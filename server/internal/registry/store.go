package registry

import (
	"bytes"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	_ "modernc.org/sqlite"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

const sqliteHeader = "SQLite format 3\x00"

// Store is the on-disk peer database. It holds the permanent ID -> public-key
// binding plus the last metadata report, mirroring RustDesk's SQLite peer
// table; leases and relay assignments stay in memory and are rebuilt when a
// host reconnects.
type Store struct {
	path string
	db   *sql.DB
	mu   sync.Mutex
}

func NewStore(path string) (*Store, error) {
	if path == "" {
		return nil, errors.New("identity store path is empty")
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("state directory %s: %w", dir, err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("open state %s: %w", path, err)
	}
	// A single connection keeps per-connection pragmas effective and matches
	// the registry's own serialisation.
	db.SetMaxOpenConns(1)
	for _, pragma := range []string{
		"PRAGMA busy_timeout=5000",
		"PRAGMA synchronous=NORMAL",
	} {
		if _, err := db.Exec(pragma); err != nil {
			db.Close()
			return nil, fmt.Errorf("state %s: %s: %w", path, pragma, err)
		}
	}
	if err := enableWAL(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("state %s: PRAGMA journal_mode=WAL: %w", path, err)
	}
	const schema = `CREATE TABLE IF NOT EXISTS peers (
		id          TEXT PRIMARY KEY,
		public_key  TEXT NOT NULL,
		fingerprint TEXT NOT NULL DEFAULT '',
		hostname    TEXT NOT NULL DEFAULT '',
		platform    TEXT NOT NULL DEFAULT '',
		version     TEXT NOT NULL DEFAULT '',
		alias       TEXT NOT NULL DEFAULT '',
		tags        TEXT NOT NULL DEFAULT '',
		created_at  INTEGER NOT NULL,
		last_seen   INTEGER NOT NULL DEFAULT 0
	)`
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, fmt.Errorf("state %s: create schema: %w", path, err)
	}
	if err := ensurePeerColumns(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("state %s: upgrade schema: %w", path, err)
	}
	return &Store{path: path, db: db}, nil
}

// enableWAL retries while another instance is switching a fresh database to
// WAL; SQLite returns SQLITE_BUSY for the mode change without invoking the
// busy handler, so a simultaneous cold start needs an explicit wait.
func enableWAL(db *sql.DB) error {
	var err error
	for attempt := 0; attempt < 100; attempt++ {
		if _, err = db.Exec("PRAGMA journal_mode=WAL"); err == nil {
			return nil
		}
		if !strings.Contains(err.Error(), "SQLITE_BUSY") {
			return err
		}
		time.Sleep(50 * time.Millisecond)
	}
	return err
}

// ensurePeerColumns adds operator labels to databases created before the
// columns existed. CREATE TABLE IF NOT EXISTS leaves an old schema untouched,
// so an upgrade without this would silently lose alias/tag writes.
func ensurePeerColumns(db *sql.DB) error {
	rows, err := db.Query(`PRAGMA table_info(peers)`)
	if err != nil {
		return err
	}
	columns := make(map[string]bool)
	for rows.Next() {
		var (
			cid, notNull, pk int
			name, columnType string
			defaultValue     sql.NullString
		)
		if err := rows.Scan(&cid, &name, &columnType, &notNull, &defaultValue, &pk); err != nil {
			rows.Close()
			return err
		}
		columns[name] = true
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return err
	}
	for _, column := range []string{"alias", "tags"} {
		if columns[column] {
			continue
		}
		if _, err := db.Exec(fmt.Sprintf(
			`ALTER TABLE peers ADD COLUMN %s TEXT NOT NULL DEFAULT ''`, column)); err != nil {
			if strings.Contains(err.Error(), "duplicate column name") {
				continue
			}
			return err
		}
	}
	return nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) Load() ([]Identity, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT id, public_key, fingerprint, hostname, platform, version,
		alias, tags, created_at, last_seen FROM peers ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("state %s: load peers: %w", s.path, err)
	}
	defer rows.Close()
	var out []Identity
	for rows.Next() {
		var (
			record              Identity
			tags                string
			createdAt, lastSeen int64
		)
		if err := rows.Scan(&record.ID, &record.PublicKey, &record.Fingerprint,
			&record.Hostname, &record.Platform, &record.Version, &record.Alias, &tags,
			&createdAt, &lastSeen); err != nil {
			return nil, fmt.Errorf("state %s: read peer: %w", s.path, err)
		}
		if !ValidID(record.ID) {
			return nil, fmt.Errorf("state %s contains invalid id %q", s.path, record.ID)
		}
		if _, err := identity.ParsePublicKey(record.PublicKey); err != nil {
			return nil, fmt.Errorf("state %s: host %s: %w", s.path, record.ID, err)
		}
		if err := validFingerprint(record.Fingerprint); err != nil {
			return nil, fmt.Errorf("state %s: host %s: %w", s.path, record.ID, err)
		}
		record.CreatedAt = timeFromMillis(createdAt)
		record.LastSeen = timeFromMillis(lastSeen)
		record.Tags = decodeTags(tags)
		out = append(out, record)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("state %s: read peers: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) Save(record Identity) error {
	if !ValidID(record.ID) {
		return fmt.Errorf("invalid peer id %q", record.ID)
	}
	if _, err := identity.ParsePublicKey(record.PublicKey); err != nil {
		return err
	}
	if err := validFingerprint(record.Fingerprint); err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec(`INSERT INTO peers
		(id, public_key, fingerprint, hostname, platform, version, alias, tags, created_at, last_seen)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
			public_key = excluded.public_key,
			fingerprint = excluded.fingerprint,
			hostname = excluded.hostname,
			platform = excluded.platform,
			version = excluded.version,
			alias = excluded.alias,
			tags = excluded.tags,
			last_seen = excluded.last_seen`,
		record.ID, record.PublicKey, record.Fingerprint, record.Hostname,
		record.Platform, record.Version, record.Alias, encodeTags(record.Tags),
		unixMillis(record.CreatedAt), unixMillis(record.LastSeen))
	if err != nil {
		return fmt.Errorf("state %s: save peer %s: %w", s.path, record.ID, err)
	}
	return nil
}

func (s *Store) Delete(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(`DELETE FROM peers WHERE id = ?`, id); err != nil {
		return fmt.Errorf("state %s: delete peer %s: %w", s.path, id, err)
	}
	return nil
}

func unixMillis(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.UTC().UnixMilli()
}

func encodeTags(tags []string) string {
	if len(tags) == 0 {
		return ""
	}
	raw, err := json.Marshal(tags)
	if err != nil {
		return ""
	}
	return string(raw)
}

func decodeTags(raw string) []string {
	if raw == "" {
		return nil
	}
	var tags []string
	if err := json.Unmarshal([]byte(raw), &tags); err != nil {
		return nil
	}
	return tags
}

func timeFromMillis(ms int64) time.Time {
	if ms == 0 {
		return time.Time{}
	}
	return time.UnixMilli(ms).UTC()
}

// prepareStore upgrades a pre-SQLite JSON state file in place, or imports the
// sibling <state>.json when the database does not exist yet. The JSON file is
// kept next to the database so an operator can still recover the old data.
func prepareStore(path string) error {
	file, err := os.Open(path)
	if err == nil {
		data, readErr := readStateHead(file)
		file.Close()
		if readErr != nil {
			return fmt.Errorf("read state %s: %w", path, readErr)
		}
		if len(data) == 0 || bytes.Equal(data, []byte(sqliteHeader)) {
			return nil
		}
		ids, err := decodeLegacyState(path, data)
		if err != nil {
			return err
		}
		backup := path + ".legacy"
		if _, statErr := os.Stat(backup); statErr == nil {
			backup = fmt.Sprintf("%s.legacy-%d", path, time.Now().Unix())
		}
		if err := os.Rename(path, backup); err != nil {
			return fmt.Errorf("migrate state %s: %w", path, err)
		}
		if err := saveAll(path, ids); err != nil {
			_ = os.Rename(backup, path)
			return err
		}
		return nil
	}
	if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("read state %s: %w", path, err)
	}

	ext := filepath.Ext(path)
	legacy := strings.TrimSuffix(path, ext) + ".json"
	if legacy == path {
		return nil
	}
	data, err := os.ReadFile(legacy)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read legacy state %s: %w", legacy, err)
	}
	ids, err := decodeLegacyState(legacy, data)
	if err != nil {
		return err
	}
	if err := saveAll(path, ids); err != nil {
		return err
	}
	if err := os.Rename(legacy, legacy+".imported"); err != nil {
		return fmt.Errorf("migrate state %s: %w", legacy, err)
	}
	return nil
}

// readStateHead returns the whole file unless it already starts with the
// SQLite header, in which case only the header is read.
func readStateHead(file *os.File) ([]byte, error) {
	head := make([]byte, len(sqliteHeader))
	n, err := io.ReadFull(file, head)
	if err != nil && !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, io.EOF) {
		return nil, err
	}
	head = head[:n]
	if n == len(sqliteHeader) && bytes.Equal(head, []byte(sqliteHeader)) {
		return head, nil
	}
	rest, err := io.ReadAll(file)
	if err != nil {
		return nil, err
	}
	return append(head, rest...), nil
}

func saveAll(path string, ids []Identity) error {
	store, err := NewStore(path)
	if err != nil {
		removeSQLiteFiles(path)
		return err
	}
	for _, record := range ids {
		if err := store.Save(record); err != nil {
			store.Close()
			removeSQLiteFiles(path)
			return err
		}
	}
	if err := store.Close(); err != nil {
		removeSQLiteFiles(path)
		return fmt.Errorf("state %s: close after import: %w", path, err)
	}
	return nil
}

func removeSQLiteFiles(path string) {
	for _, name := range []string{path, path + "-wal", path + "-shm"} {
		_ = os.Remove(name)
	}
}

type legacyStateFile struct {
	Version    int        `json:"version"`
	Identities []Identity `json:"identities"`
}

func decodeLegacyState(path string, data []byte) ([]Identity, error) {
	var state legacyStateFile
	if err := json.Unmarshal(data, &state); err != nil {
		return nil, fmt.Errorf("parse legacy state %s: %w", path, err)
	}
	if state.Version != 1 {
		return nil, fmt.Errorf("legacy state %s has unsupported version %d", path, state.Version)
	}
	seen := make(map[string]struct{}, len(state.Identities))
	for _, record := range state.Identities {
		if !ValidID(record.ID) {
			return nil, fmt.Errorf("legacy state %s contains invalid id %q", path, record.ID)
		}
		if _, err := identity.ParsePublicKey(record.PublicKey); err != nil {
			return nil, fmt.Errorf("legacy state %s: host %s: %w", path, record.ID, err)
		}
		if err := validFingerprint(record.Fingerprint); err != nil {
			return nil, fmt.Errorf("legacy state %s: host %s: %w", path, record.ID, err)
		}
		if _, ok := seen[record.ID]; ok {
			return nil, fmt.Errorf("legacy state %s contains duplicate id %q", path, record.ID)
		}
		seen[record.ID] = struct{}{}
	}
	return state.Identities, nil
}
