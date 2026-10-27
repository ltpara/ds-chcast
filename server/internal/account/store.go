// Package account stores user accounts, access tokens and per-user address
// books. It mirrors the role of hbbs's users/peers tables: a viewer signs in
// once and sees the same saved peers on every machine.
package account

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	_ "modernc.org/sqlite"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const (
	// TokenTTL is the lifetime of a viewer session: the refresh token may be
	// rotated for this long after the last use, and revocation is explicit
	// through logout, a password change or a used refresh token replay.
	TokenTTL = 720 * time.Hour
	// AccessTokenTTL keeps the bearer token short so a copied token ages out
	// quickly; viewers renew it with the refresh token without a password.
	AccessTokenTTL = 30 * time.Minute

	maxName           = 128
	maxTag            = 32
	maxTags           = 16
	maxDeviceID       = 64
	maxDeviceName     = 128
	sessionIDBytes    = 12
	tokenBytes        = 32
	recoveryCodeCount = 10
	recoveryCodeBytes = 10
	recoveryCodeGroup = 4
	saltBytes         = 16
	passwordKeyBytes  = 32
	passwordIteration = 210_000
)

var (
	ErrInvalidCredentials = errors.New("invalid credentials")
	ErrInvalidToken       = errors.New("invalid account token")
	ErrRefreshReused      = errors.New("refresh token was already used")
	ErrInvalidDeviceName  = errors.New("device name is required")
	ErrUserExists         = errors.New("user already exists")
	ErrUnknownUser        = errors.New("unknown user")
	ErrInvalidUsername    = errors.New("username must match [A-Za-z0-9][A-Za-z0-9_.@-]{0,63}")
	ErrWeakPassword       = errors.New("password must be 8..128 bytes")
	ErrInvalidPeerID      = errors.New("invalid peer id")
	ErrInvalidGroupID     = errors.New("invalid address book group id")
	ErrInvalidGroupName   = errors.New("address book group name is required")
	ErrUnknownGroup       = errors.New("unknown address book group")
	ErrUnknownPeer        = errors.New("unknown address book peer")
	ErrPeerConflict       = errors.New("address book peer changed elsewhere")
	ErrGroupConflict      = errors.New("address book group changed elsewhere")
	ErrSelfShare          = errors.New("cannot share an address book with yourself")
	ErrUnknownShare       = errors.New("address book is not shared")
	ErrInvalidPermission  = errors.New("permission must be read, write or control")
	ErrPermissionDenied   = errors.New("address book permission is insufficient")

	ErrTwoFactorRequired    = errors.New("two-factor code required")
	ErrInvalidTwoFactorCode = errors.New("two-factor code was not accepted")
	ErrTwoFactorEnabled     = errors.New("two-factor is already enabled")
	ErrTwoFactorNotStarted  = errors.New("two-factor enrollment has not started")
	ErrTwoFactorNotEnabled  = errors.New("two-factor is not enabled")
)

// Shared address-book permissions, weakest first. Read may list the book,
// write may add/edit/delete its entries and control may also manage its grants.
const (
	PermissionRead    = "read"
	PermissionWrite   = "write"
	PermissionControl = "control"
)

var usernamePattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.@-]{0,63}$`)

type User struct {
	ID        string    `json:"id"`
	Username  string    `json:"username"`
	CreatedAt time.Time `json:"created_at"`
}

// Session describes one live login. The ID is random and reveals nothing
// about the bearer token, so a viewer can revoke another device without ever
// seeing its secret.
type Session struct {
	ID         string    `json:"id"`
	DeviceID   string    `json:"device_id,omitempty"`
	DeviceName string    `json:"device_name,omitempty"`
	CreatedAt  time.Time `json:"created_at"`
	ExpiresAt  time.Time `json:"expires_at"`
	Current    bool      `json:"current"`
	// LoginIP and Location describe where the session signed in. Location is
	// empty without a GeoIP table or outside its coverage.
	LoginIP  string `json:"login_ip,omitempty"`
	Location string `json:"location,omitempty"`
}

type Peer struct {
	ID        string    `json:"id"`
	Name      string    `json:"name,omitempty"`
	Tags      []string  `json:"tags,omitempty"`
	GroupID   string    `json:"group_id,omitempty"`
	UpdatedAt time.Time `json:"updated_at"`
	// Revision is the per-user sequence number of the last write. Clients
	// pass the newest revision they have seen back through Snapshot to fetch
	// only later changes, including deletion tombstones.
	Revision int64 `json:"revision,omitempty"`
	// Deleted marks a tombstone. Peers never returns them; Snapshot does so
	// an offline viewer can learn about deletions it missed.
	Deleted bool `json:"deleted,omitempty"`
}

// Group organizes peers inside one account's address book. Groups share the
// owner's revision stream with peers so a single cursor covers both.
type Group struct {
	ID        string    `json:"id"`
	Name      string    `json:"name,omitempty"`
	UpdatedAt time.Time `json:"updated_at"`
	Revision  int64     `json:"revision,omitempty"`
	Deleted   bool      `json:"deleted,omitempty"`
}

// Snapshot is a complete alive address book plus the changes after the
// caller's last seen revision. Peers is always authoritative for the current
// state, while Changes carries tombstones that Peers omits.
type Snapshot struct {
	Revision     int64   `json:"revision"`
	Peers        []Peer  `json:"peers"`
	Changes      []Peer  `json:"changes"`
	Groups       []Group `json:"groups"`
	GroupChanges []Group `json:"group_changes"`
}

// Grant names a user who may access the owner's address book at Permission.
// GroupID scopes the grant to one group; an empty value means the whole book.
type Grant struct {
	Username   string    `json:"username"`
	Permission string    `json:"permission"`
	GroupID    string    `json:"group_id,omitempty"`
	CreatedAt  time.Time `json:"created_at"`
}

// SharedScope is one grant the signed-in grantee holds on another account's
// book. A whole-book grant appears with an empty GroupID.
type SharedScope struct {
	GroupID    string `json:"group_id,omitempty"`
	Permission string `json:"permission"`
}

// SharedBook is another user's address book as seen by a grantee. Revision
// lets the viewer show how fresh the snapshot is, but shared books are fetched
// whole rather than through the owner's incremental cursor. Peers and Groups
// only contain what the grants expose.
type SharedBook struct {
	Owner      string        `json:"owner"`
	Permission string        `json:"permission"`
	Revision   int64         `json:"revision"`
	Peers      []Peer        `json:"peers"`
	Groups     []Group       `json:"groups,omitempty"`
	Scopes     []SharedScope `json:"scopes,omitempty"`
}

// Store keeps accounts in the same SQLite file as the peer registry. A single
// connection serialises writes and the WAL journal lets registry traffic keep
// reading while a login or address-book write is in flight.
type Store struct {
	path      string
	db        *sql.DB
	mu        sync.Mutex
	now       func() time.Time
	tokenTTL  time.Duration
	accessTTL time.Duration
}

func Open(path string) (*Store, error) {
	if path == "" {
		return nil, errors.New("account store path is empty")
	}
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("account directory %s: %w", dir, err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("open account store %s: %w", path, err)
	}
	db.SetMaxOpenConns(1)
	for _, pragma := range []string{
		"PRAGMA busy_timeout=5000",
		"PRAGMA synchronous=NORMAL",
		"PRAGMA foreign_keys=ON",
	} {
		if _, err := db.Exec(pragma); err != nil {
			db.Close()
			return nil, fmt.Errorf("account store %s: %s: %w", path, pragma, err)
		}
	}
	if err := enableWAL(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("account store %s: PRAGMA journal_mode=WAL: %w", path, err)
	}
	for _, schema := range []string{
		`CREATE TABLE IF NOT EXISTS users (
			id            TEXT PRIMARY KEY,
			username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
			password_hash  TEXT NOT NULL,
			created_at     INTEGER NOT NULL,
			book_revision  INTEGER NOT NULL DEFAULT 0,
			totp_secret    TEXT NOT NULL DEFAULT '',
			totp_enabled   INTEGER NOT NULL DEFAULT 0,
			totp_last_step INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE TABLE IF NOT EXISTS user_tokens (
			token_hash  TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at  INTEGER NOT NULL,
			expires_at  INTEGER NOT NULL,
			session_id  TEXT NOT NULL DEFAULT '',
			device_id   TEXT NOT NULL DEFAULT '',
			device_name TEXT NOT NULL DEFAULT '',
			login_ip    TEXT NOT NULL DEFAULT '',
			login_location TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS user_tokens_expiry ON user_tokens(expires_at)`,
		`CREATE TABLE IF NOT EXISTS refresh_tokens (
			token_hash  TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			session_id  TEXT NOT NULL,
			created_at  INTEGER NOT NULL,
			expires_at  INTEGER NOT NULL,
			rotated_at  INTEGER NOT NULL DEFAULT 0,
			replaced_by TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS refresh_tokens_expiry ON refresh_tokens(expires_at)`,
		`CREATE INDEX IF NOT EXISTS refresh_tokens_session ON refresh_tokens(session_id)`,
		`CREATE TABLE IF NOT EXISTS recovery_codes (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at INTEGER NOT NULL,
			used_at    INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, code_hash)
		)`,
		`CREATE TABLE IF NOT EXISTS address_book (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			peer_id    TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			tags       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			revision   INTEGER NOT NULL DEFAULT 0,
			deleted    INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, peer_id)
		)`,
		`CREATE TABLE IF NOT EXISTS address_book_grants (
			owner_user_id   TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			grantee_user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id        TEXT NOT NULL DEFAULT '',
			permission      TEXT NOT NULL DEFAULT 'read',
			created_at      INTEGER NOT NULL,
			PRIMARY KEY (owner_user_id, grantee_user_id, group_id),
			CHECK (owner_user_id <> grantee_user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS address_book_grants_grantee
			ON address_book_grants(grantee_user_id)`,
		`CREATE TABLE IF NOT EXISTS address_book_groups (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id   TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			revision   INTEGER NOT NULL DEFAULT 0,
			deleted    INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, group_id)
		)`,
		`CREATE TABLE IF NOT EXISTS audit_events (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			created_at INTEGER NOT NULL,
			username   TEXT NOT NULL,
			action     TEXT NOT NULL,
			detail     TEXT NOT NULL DEFAULT '',
			client_ip  TEXT NOT NULL DEFAULT '',
			location   TEXT NOT NULL DEFAULT '',
			success    INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS audit_events_created ON audit_events(created_at)`,
		`CREATE TABLE IF NOT EXISTS connection_events (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			user_id    TEXT NOT NULL,
			username   TEXT NOT NULL,
			peer_id    TEXT NOT NULL,
			direction  TEXT NOT NULL DEFAULT 'viewer',
			client_ip  TEXT NOT NULL DEFAULT '',
			started_at INTEGER NOT NULL,
			ended_at   INTEGER NOT NULL,
			success    INTEGER NOT NULL DEFAULT 1,
			detail     TEXT NOT NULL DEFAULT '',
			host_id    TEXT NOT NULL DEFAULT '',
			audit_ref  TEXT NOT NULL DEFAULT '',
			source     TEXT NOT NULL DEFAULT 'viewer'
		)`,
		`CREATE INDEX IF NOT EXISTS connection_events_started
			ON connection_events(started_at)`,
		`CREATE INDEX IF NOT EXISTS connection_events_audit
			ON connection_events(audit_ref) WHERE audit_ref <> ''`,
		`CREATE TABLE IF NOT EXISTS file_events (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			user_id     TEXT NOT NULL,
			username    TEXT NOT NULL,
			peer_id     TEXT NOT NULL,
			direction   TEXT NOT NULL,
			name        TEXT NOT NULL DEFAULT '',
			size        INTEGER NOT NULL DEFAULT 0,
			files       INTEGER NOT NULL DEFAULT 1,
			client_ip   TEXT NOT NULL DEFAULT '',
			occurred_at INTEGER NOT NULL,
			success     INTEGER NOT NULL DEFAULT 1,
			detail      TEXT NOT NULL DEFAULT '',
			host_id     TEXT NOT NULL DEFAULT '',
			audit_ref   TEXT NOT NULL DEFAULT '',
			source      TEXT NOT NULL DEFAULT 'viewer'
		)`,
		`CREATE INDEX IF NOT EXISTS file_events_occurred
			ON file_events(occurred_at)`,
		`CREATE TABLE IF NOT EXISTS login_locks (
			login_key    TEXT PRIMARY KEY,
			failures     INTEGER NOT NULL,
			window_start INTEGER NOT NULL,
			locked_until INTEGER NOT NULL DEFAULT 0,
			last_seen    INTEGER NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS login_locks_seen ON login_locks(last_seen)`,
	} {
		if _, err := db.Exec(schema); err != nil {
			db.Close()
			return nil, fmt.Errorf("account store %s: create schema: %w", path, err)
		}
	}
	if err := migrateGrantScopes(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("account store %s: migrate address book grants: %w", path, err)
	}
	for _, column := range []struct {
		table string
		name  string
		ddl   string
	}{
		{"users", "book_revision", "book_revision INTEGER NOT NULL DEFAULT 0"},
		{"users", "totp_secret", "totp_secret TEXT NOT NULL DEFAULT ''"},
		{"users", "totp_enabled", "totp_enabled INTEGER NOT NULL DEFAULT 0"},
		{"users", "totp_last_step", "totp_last_step INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "revision", "revision INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "deleted", "deleted INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "group_id", "group_id TEXT NOT NULL DEFAULT ''"},
		{"address_book_grants", "permission", "permission TEXT NOT NULL DEFAULT 'read'"},
		{"user_tokens", "session_id", "session_id TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "device_id", "device_id TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "device_name", "device_name TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "login_ip", "login_ip TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "login_location", "login_location TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "access_expires_at", "access_expires_at INTEGER NOT NULL DEFAULT 0"},
		{"audit_events", "location", "location TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "host_id", "host_id TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "audit_ref", "audit_ref TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "source", "source TEXT NOT NULL DEFAULT 'viewer'"},
		{"file_events", "host_id", "host_id TEXT NOT NULL DEFAULT ''"},
		{"file_events", "audit_ref", "audit_ref TEXT NOT NULL DEFAULT ''"},
		{"file_events", "source", "source TEXT NOT NULL DEFAULT 'viewer'"},
	} {
		if err := ensureColumn(db, column.table, column.name, column.ddl); err != nil {
			db.Close()
			return nil, fmt.Errorf("account store %s: migrate %s.%s: %w", path, column.table, column.name, err)
		}
	}
	if err := backfillSessionIDs(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("account store %s: migrate session ids: %w", path, err)
	}
	if _, err := db.Exec(
		`CREATE INDEX IF NOT EXISTS connection_events_audit
		 ON connection_events(audit_ref) WHERE audit_ref <> ''`,
	); err != nil {
		db.Close()
		return nil, fmt.Errorf("account store %s: index audit refs: %w", path, err)
	}
	if _, err := db.Exec(
		`CREATE UNIQUE INDEX IF NOT EXISTS user_tokens_session
		 ON user_tokens(session_id) WHERE session_id <> ''`,
	); err != nil {
		db.Close()
		return nil, fmt.Errorf("account store %s: index session ids: %w", path, err)
	}
	if err := backfillRevisions(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("account store %s: migrate address book revisions: %w", path, err)
	}
	return &Store{
		path:      path,
		db:        db,
		now:       time.Now,
		tokenTTL:  TokenTTL,
		accessTTL: AccessTokenTTL,
	}, nil
}

func (s *Store) Close() error { return s.db.Close() }

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

// ensureColumn upgrades databases created before revision tracking existed.
// Table and column names are internal constants, never user input.
func ensureColumn(db *sql.DB, table, column, ddl string) error {
	found, err := columnExists(db, table, column)
	if err != nil || found {
		return err
	}
	if _, err := db.Exec(`ALTER TABLE ` + table + ` ADD COLUMN ` + ddl); err != nil {
		// Another instance may have added the column between the check and the
		// ALTER; that is the same outcome as winning the race ourselves.
		if strings.Contains(err.Error(), "duplicate column name") {
			if exists, checkErr := columnExists(db, table, column); checkErr == nil && exists {
				return nil
			}
		}
		return err
	}
	return nil
}

func columnExists(db *sql.DB, table, column string) (bool, error) {
	rows, err := db.Query(`PRAGMA table_info(` + table + `)`)
	if err != nil {
		return false, err
	}
	defer rows.Close()
	for rows.Next() {
		var (
			cid     int
			name    string
			kind    string
			notNull int
			dflt    sql.NullString
			pk      int
		)
		if err := rows.Scan(&cid, &name, &kind, &notNull, &dflt, &pk); err != nil {
			return false, err
		}
		if name == column {
			return true, nil
		}
	}
	return false, rows.Err()
}

// migrateGrantScopes rebuilds address_book_grants so one owner/grantee pair
// can hold separate grants per group. The original primary key allowed only
// one row per pair, so adding a column is not enough. An empty group id keeps
// the old whole-book meaning.
func migrateGrantScopes(db *sql.DB) error {
	found, err := columnExists(db, "address_book_grants", "group_id")
	if err != nil || found {
		return err
	}
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	// A concurrent instance may have completed the rebuild between the check
	// above and this transaction; in that case there is nothing left to do.
	rows, err := tx.Query(`PRAGMA table_info(address_book_grants)`)
	if err != nil {
		return err
	}
	for rows.Next() {
		var (
			cid     int
			name    string
			kind    string
			notNull int
			dflt    sql.NullString
			pk      int
		)
		if err := rows.Scan(&cid, &name, &kind, &notNull, &dflt, &pk); err != nil {
			rows.Close()
			return err
		}
		if name == "group_id" {
			rows.Close()
			return nil
		}
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	rows.Close()
	for _, statement := range []string{
		`DROP INDEX IF EXISTS address_book_grants_grantee`,
		`CREATE TABLE address_book_grants_scoped (
			owner_user_id   TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			grantee_user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id        TEXT NOT NULL DEFAULT '',
			permission      TEXT NOT NULL DEFAULT 'read',
			created_at      INTEGER NOT NULL,
			PRIMARY KEY (owner_user_id, grantee_user_id, group_id),
			CHECK (owner_user_id <> grantee_user_id)
		)`,
		`INSERT INTO address_book_grants_scoped
			(owner_user_id, grantee_user_id, group_id, permission, created_at)
		 SELECT owner_user_id, grantee_user_id, '', permission, created_at
		 FROM address_book_grants`,
		`DROP TABLE address_book_grants`,
		`ALTER TABLE address_book_grants_scoped RENAME TO address_book_grants`,
		`CREATE INDEX IF NOT EXISTS address_book_grants_grantee
			ON address_book_grants(grantee_user_id)`,
	} {
		if _, err := tx.Exec(statement); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// backfillRevisions assigns every pre-migration row a revision. Without it a
// legacy row would look like revision zero and an incremental client could
// never tell it apart from history it already has.
func backfillRevisions(db *sql.DB) error {
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`UPDATE users SET book_revision = book_revision + 1
		 WHERE EXISTS (
			SELECT 1 FROM address_book
			WHERE address_book.user_id = users.id AND address_book.revision = 0
		 )`,
	); err != nil {
		return err
	}
	if _, err := tx.Exec(
		`UPDATE address_book
		 SET revision = (SELECT book_revision FROM users WHERE users.id = address_book.user_id)
		 WHERE revision = 0`,
	); err != nil {
		return err
	}
	return tx.Commit()
}

// backfillSessionIDs gives tokens created before session metadata existed a
// public id, so they appear in the session list and can be revoked. The rows
// are read before the transaction because the pool has one connection.
func backfillSessionIDs(db *sql.DB) error {
	rows, err := db.Query(`SELECT token_hash FROM user_tokens WHERE session_id = ''`)
	if err != nil {
		return err
	}
	var hashes []string
	for rows.Next() {
		var hash string
		if err := rows.Scan(&hash); err != nil {
			rows.Close()
			return err
		}
		hashes = append(hashes, hash)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return err
	}
	rows.Close()
	if len(hashes) == 0 {
		return nil
	}
	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for _, hash := range hashes {
		id, err := newRandomHex(sessionIDBytes)
		if err != nil {
			return err
		}
		if _, err := tx.Exec(`UPDATE user_tokens SET session_id = ? WHERE token_hash = ?`, id, hash); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// SetClock lets tests advance token expiry without sleeping.
func (s *Store) SetClock(now func() time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.now = now
}

func (s *Store) CreateUser(username, password string) (User, error) {
	username = strings.TrimSpace(username)
	if !usernamePattern.MatchString(username) {
		return User{}, ErrInvalidUsername
	}
	if err := validatePassword(password); err != nil {
		return User{}, err
	}
	hash, err := hashPassword(password)
	if err != nil {
		return User{}, fmt.Errorf("hash password: %w", err)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var exists int
	err = s.db.QueryRow(`SELECT 1 FROM users WHERE username = ?`, username).Scan(&exists)
	if err == nil {
		return User{}, ErrUserExists
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return User{}, fmt.Errorf("account store %s: look up user: %w", s.path, err)
	}
	id, err := newRandomHex(16)
	if err != nil {
		return User{}, err
	}
	created := s.now().UTC()
	if _, err := s.db.Exec(
		`INSERT INTO users (id, username, password_hash, created_at) VALUES (?, ?, ?, ?)`,
		id, username, hash, unixMillis(created),
	); err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			return User{}, ErrUserExists
		}
		return User{}, fmt.Errorf("account store %s: create user: %w", s.path, err)
	}
	return User{ID: id, Username: username, CreatedAt: created}, nil
}

// SetPassword replaces the hash and revokes every existing token, so a
// compromised session cannot outlive the password reset.
func (s *Store) SetPassword(username, password string) error {
	username = strings.TrimSpace(username)
	if err := validatePassword(password); err != nil {
		return err
	}
	hash, err := hashPassword(password)
	if err != nil {
		return fmt.Errorf("hash password: %w", err)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin password update: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(`UPDATE users SET password_hash = ? WHERE username = ?`, hash, username)
	if err != nil {
		return fmt.Errorf("account store %s: update password: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownUser
	}
	if _, err := tx.Exec(
		`DELETE FROM user_tokens WHERE user_id = (SELECT id FROM users WHERE username = ?)`,
		username,
	); err != nil {
		return fmt.Errorf("account store %s: revoke tokens: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`DELETE FROM refresh_tokens WHERE user_id = (SELECT id FROM users WHERE username = ?)`,
		username,
	); err != nil {
		return fmt.Errorf("account store %s: revoke refresh tokens: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit password update: %w", s.path, err)
	}
	return nil
}

// ChangePassword verifies the caller's current password and replaces it,
// revoking every other session while keeping the caller signed in. Other
// devices must log in again with the new password.
func (s *Store) ChangePassword(userID, currentToken, currentPassword, newPassword string) error {
	if err := validatePassword(newPassword); err != nil {
		return err
	}
	hash, err := hashPassword(newPassword)
	if err != nil {
		return fmt.Errorf("hash password: %w", err)
	}
	current := tokenHash(strings.TrimSpace(currentToken))
	s.mu.Lock()
	defer s.mu.Unlock()
	var stored string
	err = s.db.QueryRow(
		`SELECT password_hash FROM users WHERE id = ?`,
		userID,
	).Scan(&stored)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownUser
	}
	if err != nil {
		return fmt.Errorf("account store %s: change password lookup: %w", s.path, err)
	}
	ok, err := verifyPassword(stored, currentPassword)
	if err != nil || !ok {
		return ErrInvalidCredentials
	}
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin password change: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`UPDATE users SET password_hash = ? WHERE id = ?`, hash, userID); err != nil {
		return fmt.Errorf("account store %s: update password: %w", s.path, err)
	}
	if current != "" {
		_, err = tx.Exec(
			`DELETE FROM user_tokens WHERE user_id = ? AND token_hash <> ?`,
			userID, current,
		)
	} else {
		_, err = tx.Exec(`DELETE FROM user_tokens WHERE user_id = ?`, userID)
	}
	if err != nil {
		return fmt.Errorf("account store %s: revoke other sessions: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`DELETE FROM refresh_tokens WHERE user_id = ?
		 AND session_id NOT IN (SELECT session_id FROM user_tokens WHERE user_id = ?)`,
		userID, userID,
	); err != nil {
		return fmt.Errorf("account store %s: revoke other refresh tokens: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit password change: %w", s.path, err)
	}
	return nil
}

func (s *Store) DeleteUser(username string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(`DELETE FROM users WHERE username = ?`, strings.TrimSpace(username))
	if err != nil {
		return fmt.Errorf("account store %s: delete user: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownUser
	}
	return nil
}

func (s *Store) ListUsers() ([]User, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT id, username, created_at FROM users ORDER BY username`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list users: %w", s.path, err)
	}
	defer rows.Close()
	var out []User
	for rows.Next() {
		var (
			user    User
			created int64
		)
		if err := rows.Scan(&user.ID, &user.Username, &created); err != nil {
			return nil, err
		}
		user.CreatedAt = timeFromMillis(created)
		out = append(out, user)
	}
	return out, rows.Err()
}

func (s *Store) FindUser(username string) (User, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		user    User
		created int64
	)
	err := s.db.QueryRow(
		`SELECT id, username, created_at FROM users WHERE username = ?`,
		strings.TrimSpace(username),
	).Scan(&user.ID, &user.Username, &created)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrUnknownUser
	}
	if err != nil {
		return User{}, fmt.Errorf("account store %s: find user: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	return user, nil
}

// Login verifies the password and issues a fresh bearer token. Unknown users
// still pay for a password verification so response timing does not reveal
// which usernames exist.
func (s *Store) Login(username, password string) (User, string, time.Time, error) {
	return s.LoginWithDevice(username, password, "", "")
}

// LoginWithDevice is Login plus the viewer's device label, shown by the
// account session list so a user can recognize and revoke other machines.
func (s *Store) LoginWithDevice(username, password, deviceID, deviceName string) (User, string, time.Time, error) {
	return s.LoginWithDeviceCode(username, password, deviceID, deviceName, "")
}

// LoginWithDeviceCode is LoginWithDevice plus the current TOTP code for
// accounts that enabled two-factor authentication. A missing code reports
// ErrTwoFactorRequired only after the password matched, so the extra step
// never reveals whether a username exists. Succeeding steps are remembered so
// the same code cannot be replayed by another login within its window.
func (s *Store) LoginWithDeviceCode(username, password, deviceID, deviceName, code string) (User, string, time.Time, error) {
	return s.LoginWithDeviceRecovery(username, password, deviceID, deviceName, code, "")
}

// LoginWithDeviceRecovery is LoginWithDeviceCode plus single-use recovery
// codes. A recovery code is accepted only when no TOTP code was supplied and
// is consumed in the same transaction that issues the token, so two concurrent
// logins cannot spend the same backup code. Wrong codes report
// ErrInvalidTwoFactorCode and are counted like a wrong TOTP code.
func (s *Store) LoginWithDeviceRecovery(username, password, deviceID, deviceName, code, recoveryCode string) (User, string, time.Time, error) {
	user, token, expires, _, _, err := s.LoginWithRefresh(
		username, password, deviceID, deviceName, code, recoveryCode)
	return user, token, expires, err
}

// LoginWithRefresh issues a short-lived access token plus a refresh token for
// the same session. RefreshSession rotates the pair without a password; the
// session lives until the refresh token expires or is revoked.
func (s *Store) LoginWithRefresh(username, password, deviceID, deviceName, code, recoveryCode string) (User, string, time.Time, string, time.Time, error) {
	username = strings.TrimSpace(username)
	code = strings.TrimSpace(code)
	recoveryCode = normalizeRecoveryCode(recoveryCode)
	s.mu.Lock()
	var (
		user         User
		hash         string
		created      int64
		totpSecret   string
		totpEnabled  bool
		totpLastStep int64
	)
	err := s.db.QueryRow(
		`SELECT id, username, password_hash, created_at,
		        totp_secret, totp_enabled, totp_last_step
		 FROM users WHERE username = ?`,
		username,
	).Scan(&user.ID, &user.Username, &hash, &created, &totpSecret, &totpEnabled, &totpLastStep)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		verifyDummy(password)
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidCredentials
	}
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: login lookup: %w", s.path, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidCredentials
	}
	if totpEnabled && code == "" && recoveryCode == "" {
		return User{}, "", time.Time{}, "", time.Time{}, ErrTwoFactorRequired
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	useRecovery := false
	if totpEnabled {
		if code != "" {
			step, matched := totpMatch(totpSecret, code, now)
			if !matched || step <= totpLastStep {
				return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidTwoFactorCode
			}
			totpLastStep = step
		} else {
			useRecovery = true
		}
	}
	token, err := newToken()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	refresh, err := newToken()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	sessionID, err := newRandomHex(sessionIDBytes)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	deviceID = normalizeText(deviceID, maxDeviceID)
	deviceName = normalizeText(deviceName, maxDeviceName)
	accessExpires := now.Add(s.accessTTL)
	sessionExpires := now.Add(s.tokenTTL)
	if _, err := s.db.Exec(`DELETE FROM user_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: prune tokens: %w", s.path, err)
	}
	if _, err := s.db.Exec(`DELETE FROM refresh_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: prune refresh tokens: %w", s.path, err)
	}
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: begin login: %w", s.path, err)
	}
	defer tx.Rollback()
	if useRecovery {
		res, err := tx.Exec(
			`UPDATE recovery_codes SET used_at = ?
			 WHERE user_id = ? AND code_hash = ? AND used_at = 0`,
			unixMillis(now), user.ID, recoveryCodeHash(recoveryCode),
		)
		if err != nil {
			return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: consume recovery code: %w", s.path, err)
		}
		if rows, err := res.RowsAffected(); err != nil {
			return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: consume recovery code: %w", s.path, err)
		} else if rows == 0 {
			return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidTwoFactorCode
		}
	}
	if totpEnabled && !useRecovery {
		res, err := tx.Exec(
			`UPDATE users SET totp_last_step = ? WHERE id = ? AND totp_last_step < ?`,
			totpLastStep, user.ID, totpLastStep,
		)
		if err != nil {
			return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: record two-factor step: %w", s.path, err)
		}
		if rows, err := res.RowsAffected(); err != nil {
			return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: record two-factor step: %w", s.path, err)
		} else if rows == 0 {
			return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidTwoFactorCode
		}
	}
	if _, err := tx.Exec(
		`INSERT INTO user_tokens
			(token_hash, user_id, created_at, expires_at, access_expires_at,
			 session_id, device_id, device_name)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		tokenHash(token), user.ID, unixMillis(now), unixMillis(sessionExpires),
		unixMillis(accessExpires), sessionID, deviceID, deviceName,
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: save token: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO refresh_tokens
			(token_hash, user_id, session_id, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?)`,
		tokenHash(refresh), user.ID, sessionID, unixMillis(now), unixMillis(sessionExpires),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: save refresh token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: commit login: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	return user, token, accessExpires, refresh, sessionExpires, nil
}

// TwoFactorEnabled reports whether logins for userID require a TOTP code.
func (s *Store) TwoFactorEnabled(userID string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var enabled bool
	err := s.db.QueryRow(`SELECT totp_enabled FROM users WHERE id = ?`, userID).Scan(&enabled)
	if errors.Is(err, sql.ErrNoRows) {
		return false, ErrUnknownUser
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read two-factor state: %w", s.path, err)
	}
	return enabled, nil
}

// StartTwoFactor creates a pending secret. It is not required at login until
// EnableTwoFactor accepts a code from it. An account that already enabled 2FA
// must disable it (password plus a current code) or be reset by an operator,
// so a stolen token cannot silently replace the authenticator.
func (s *Store) StartTwoFactor(userID string) (string, error) {
	secret, err := newTOTPSecret()
	if err != nil {
		return "", err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var enabled bool
	err = s.db.QueryRow(`SELECT totp_enabled FROM users WHERE id = ?`, userID).Scan(&enabled)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrUnknownUser
	}
	if err != nil {
		return "", fmt.Errorf("account store %s: start two-factor: %w", s.path, err)
	}
	if enabled {
		return "", ErrTwoFactorEnabled
	}
	if _, err := s.db.Exec(
		`UPDATE users SET totp_secret = ?, totp_enabled = 0, totp_last_step = 0 WHERE id = ?`,
		secret, userID,
	); err != nil {
		return "", fmt.Errorf("account store %s: save two-factor secret: %w", s.path, err)
	}
	return secret, nil
}

// EnableTwoFactor confirms a pending secret with a code from the caller's
// authenticator. The matched step is recorded so the enrollment code cannot
// also be used for the next login.
func (s *Store) EnableTwoFactor(userID, code string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		secret  string
		enabled bool
	)
	err := s.db.QueryRow(`SELECT totp_secret, totp_enabled FROM users WHERE id = ?`, userID).
		Scan(&secret, &enabled)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownUser
	}
	if err != nil {
		return fmt.Errorf("account store %s: read two-factor secret: %w", s.path, err)
	}
	if secret == "" {
		return ErrTwoFactorNotStarted
	}
	if enabled {
		return nil
	}
	step, matched := totpMatch(secret, code, s.now().UTC())
	if !matched {
		return ErrInvalidTwoFactorCode
	}
	if _, err := s.db.Exec(
		`UPDATE users SET totp_enabled = 1, totp_last_step = ? WHERE id = ?`,
		step, userID,
	); err != nil {
		return fmt.Errorf("account store %s: enable two-factor: %w", s.path, err)
	}
	return nil
}

// DisableTwoFactor requires both the account password and a current code, so
// neither a stolen token nor a stolen password alone can remove the second
// factor.
func (s *Store) DisableTwoFactor(userID, password, code string) error {
	s.mu.Lock()
	var (
		hash    string
		secret  string
		enabled bool
		now     = s.now().UTC()
	)
	err := s.db.QueryRow(
		`SELECT password_hash, totp_secret, totp_enabled FROM users WHERE id = ?`, userID,
	).Scan(&hash, &secret, &enabled)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownUser
	}
	if err != nil {
		return fmt.Errorf("account store %s: read two-factor state: %w", s.path, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return ErrInvalidCredentials
	}
	if !enabled {
		return ErrTwoFactorNotEnabled
	}
	if _, matched := totpMatch(secret, code, now); !matched {
		return ErrInvalidTwoFactorCode
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin disable two-factor: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`UPDATE users SET totp_secret = '', totp_enabled = 0, totp_last_step = 0 WHERE id = ?`,
		userID,
	); err != nil {
		return fmt.Errorf("account store %s: disable two-factor: %w", s.path, err)
	}
	// Backup codes are bound to the authenticator they were issued for, so a
	// later re-enrollment must not inherit codes from the old secret.
	if _, err := tx.Exec(`DELETE FROM recovery_codes WHERE user_id = ?`, userID); err != nil {
		return fmt.Errorf("account store %s: clear recovery codes: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit disable two-factor: %w", s.path, err)
	}
	return nil
}

// GenerateRecoveryCodes replaces every unused backup code with a fresh set and
// returns the plaintext once; only hashes are stored. The password and a
// current TOTP code are both required so neither a stolen session nor a stolen
// password alone can mint codes that bypass the second factor.
func (s *Store) GenerateRecoveryCodes(userID, password, code string) ([]string, error) {
	s.mu.Lock()
	var (
		hash    string
		secret  string
		enabled bool
	)
	err := s.db.QueryRow(
		`SELECT password_hash, totp_secret, totp_enabled FROM users WHERE id = ?`, userID,
	).Scan(&hash, &secret, &enabled)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrUnknownUser
	}
	if err != nil {
		return nil, fmt.Errorf("account store %s: read two-factor state: %w", s.path, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return nil, ErrInvalidCredentials
	}
	if !enabled {
		return nil, ErrTwoFactorNotEnabled
	}
	if _, matched := totpMatch(secret, code, s.now().UTC()); !matched {
		return nil, ErrInvalidTwoFactorCode
	}
	codes := make([]string, 0, recoveryCodeCount)
	for i := 0; i < recoveryCodeCount; i++ {
		code, err := newRecoveryCode()
		if err != nil {
			return nil, err
		}
		codes = append(codes, code)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return nil, fmt.Errorf("account store %s: begin recovery codes: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM recovery_codes WHERE user_id = ?`, userID); err != nil {
		return nil, fmt.Errorf("account store %s: replace recovery codes: %w", s.path, err)
	}
	now := unixMillis(s.now().UTC())
	for _, code := range codes {
		if _, err := tx.Exec(
			`INSERT INTO recovery_codes (user_id, code_hash, created_at) VALUES (?, ?, ?)`,
			userID, recoveryCodeHash(code), now,
		); err != nil {
			return nil, fmt.Errorf("account store %s: save recovery code: %w", s.path, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, fmt.Errorf("account store %s: commit recovery codes: %w", s.path, err)
	}
	return codes, nil
}

// RecoveryCodeCount reports how many unused backup codes remain.
func (s *Store) RecoveryCodeCount(userID string) (int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var count int
	if err := s.db.QueryRow(
		`SELECT COUNT(*) FROM recovery_codes WHERE user_id = ? AND used_at = 0`, userID,
	).Scan(&count); err != nil {
		return 0, fmt.Errorf("account store %s: count recovery codes: %w", s.path, err)
	}
	return count, nil
}

// ResetTwoFactor is the operator escape hatch for a lost authenticator. It
// clears the secret so the next login only needs the password.
func (s *Store) ResetTwoFactor(username string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(
		`UPDATE users SET totp_secret = '', totp_enabled = 0, totp_last_step = 0 WHERE username = ?`,
		strings.TrimSpace(username),
	)
	if err != nil {
		return fmt.Errorf("account store %s: reset two-factor: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return fmt.Errorf("account store %s: reset two-factor: %w", s.path, err)
	} else if rows == 0 {
		return ErrUnknownUser
	}
	// A reset removes the authenticator the backup codes belonged to; keeping
	// them would let an old sheet of codes survive the next enrollment.
	if _, err := s.db.Exec(
		`DELETE FROM recovery_codes WHERE user_id = (SELECT id FROM users WHERE username = ?)`,
		strings.TrimSpace(username),
	); err != nil {
		return fmt.Errorf("account store %s: clear recovery codes: %w", s.path, err)
	}
	return nil
}

func (s *Store) Authenticate(token string) (User, error) {
	token = strings.TrimSpace(token)
	if token == "" {
		return User{}, ErrInvalidToken
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		user    User
		created int64
		expires int64
	)
	// access_expires_at is zero on rows issued before refresh tokens existed;
	// those legacy tokens keep their original long expiry until they age out.
	err := s.db.QueryRow(
		`SELECT u.id, u.username, u.created_at,
		        CASE WHEN t.access_expires_at > 0 THEN t.access_expires_at
		             ELSE t.expires_at END
		 FROM user_tokens t JOIN users u ON u.id = t.user_id
		 WHERE t.token_hash = ?`,
		tokenHash(token),
	).Scan(&user.ID, &user.Username, &created, &expires)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrInvalidToken
	}
	if err != nil {
		return User{}, fmt.Errorf("account store %s: authenticate: %w", s.path, err)
	}
	if s.now().UTC().UnixMilli() >= expires {
		// An expired access token must not take the session row with it:
		// RefreshSession rotates that row, so only drop it once the session
		// itself has ended.
		_, _ = s.db.Exec(
			`DELETE FROM user_tokens WHERE token_hash = ? AND expires_at <= ?`,
			tokenHash(token), unixMillis(s.now().UTC()),
		)
		return User{}, ErrInvalidToken
	}
	user.CreatedAt = timeFromMillis(created)
	return user, nil
}

// RefreshSession rotates a refresh token and returns a fresh access/refresh
// pair for the same session. Presenting a token that was already rotated
// revokes the whole session: either the viewer lost the rotation response or
// somebody else replayed a copied token, and neither should keep access.
func (s *Store) RefreshSession(refreshToken string) (User, string, time.Time, string, time.Time, error) {
	refreshToken = strings.TrimSpace(refreshToken)
	if refreshToken == "" {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidToken
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: begin refresh: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		userID    string
		sessionID string
		expires   int64
		rotated   int64
	)
	err = tx.QueryRow(
		`SELECT user_id, session_id, expires_at, rotated_at
		 FROM refresh_tokens WHERE token_hash = ?`,
		tokenHash(refreshToken),
	).Scan(&userID, &sessionID, &expires, &rotated)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidToken
	}
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: refresh lookup: %w", s.path, err)
	}
	if rotated != 0 {
		if err := revokeSessionLocked(tx, userID, sessionID); err != nil {
			return User{}, "", time.Time{}, "", time.Time{}, err
		}
		if err := tx.Commit(); err != nil {
			return User{}, "", time.Time{}, "", time.Time{},
				fmt.Errorf("account store %s: commit refresh replay: %w", s.path, err)
		}
		return User{}, "", time.Time{}, "", time.Time{}, ErrRefreshReused
	}
	if expires <= unixMillis(now) {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidToken
	}
	var (
		user    User
		created int64
	)
	err = tx.QueryRow(`SELECT username, created_at FROM users WHERE id = ?`, userID).
		Scan(&user.Username, &created)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidToken
	}
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: refresh user lookup: %w", s.path, err)
	}
	token, err := newToken()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	refresh, err := newToken()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	accessExpires := now.Add(s.accessTTL)
	sessionExpires := now.Add(s.tokenTTL)
	res, err := tx.Exec(
		`UPDATE user_tokens
		 SET token_hash = ?, created_at = ?, expires_at = ?, access_expires_at = ?
		 WHERE user_id = ? AND session_id = ?`,
		tokenHash(token), unixMillis(now), unixMillis(sessionExpires),
		unixMillis(accessExpires), userID, sessionID,
	)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: rotate access token: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	} else if rows == 0 {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidToken
	}
	if _, err := tx.Exec(
		`UPDATE refresh_tokens SET rotated_at = ?, replaced_by = ? WHERE token_hash = ?`,
		unixMillis(now), tokenHash(refresh), tokenHash(refreshToken),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: retire refresh token: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO refresh_tokens
			(token_hash, user_id, session_id, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?)`,
		tokenHash(refresh), userID, sessionID, unixMillis(now), unixMillis(sessionExpires),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: save refresh token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{},
			fmt.Errorf("account store %s: commit refresh: %w", s.path, err)
	}
	user.ID = userID
	user.CreatedAt = timeFromMillis(created)
	return user, token, accessExpires, refresh, sessionExpires, nil
}

// revokeSessionLocked removes both the access and refresh rows of one login.
func revokeSessionLocked(tx *sql.Tx, userID, sessionID string) error {
	if _, err := tx.Exec(
		`DELETE FROM user_tokens WHERE user_id = ? AND session_id = ?`,
		userID, sessionID,
	); err != nil {
		return fmt.Errorf("account store: revoke session tokens: %w", err)
	}
	if _, err := tx.Exec(
		`DELETE FROM refresh_tokens WHERE user_id = ? AND session_id = ?`,
		userID, sessionID,
	); err != nil {
		return fmt.Errorf("account store: revoke session refresh tokens: %w", err)
	}
	return nil
}

// Logout drops the session that owns this access token, including its refresh
// token, so a copied refresh token cannot outlive an explicit sign-out.
func (s *Store) Logout(token string) error {
	token = strings.TrimSpace(token)
	if token == "" {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var userID, sessionID string
	err := s.db.QueryRow(
		`SELECT user_id, session_id FROM user_tokens WHERE token_hash = ?`,
		tokenHash(token),
	).Scan(&userID, &sessionID)
	if errors.Is(err, sql.ErrNoRows) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("account store %s: logout lookup: %w", s.path, err)
	}
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin logout: %w", s.path, err)
	}
	defer tx.Rollback()
	if err := revokeSessionLocked(tx, userID, sessionID); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit logout: %w", s.path, err)
	}
	return nil
}

// Sessions lists the user's live logins, newest first. currentToken marks the
// caller's own session; expired rows are pruned so the list only shows tokens
// that still authenticate.
func (s *Store) Sessions(userID, currentToken string) ([]Session, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	if _, err := s.db.Exec(`DELETE FROM user_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return nil, fmt.Errorf("account store %s: prune sessions: %w", s.path, err)
	}
	if _, err := s.db.Exec(`DELETE FROM refresh_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return nil, fmt.Errorf("account store %s: prune refresh tokens: %w", s.path, err)
	}
	rows, err := s.db.Query(
		`SELECT session_id, device_id, device_name, created_at, expires_at, token_hash,
		        login_ip, login_location
		 FROM user_tokens WHERE user_id = ?
		 ORDER BY created_at DESC, session_id`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list sessions: %w", s.path, err)
	}
	defer rows.Close()
	current := ""
	if strings.TrimSpace(currentToken) != "" {
		current = tokenHash(strings.TrimSpace(currentToken))
	}
	out := make([]Session, 0)
	for rows.Next() {
		var (
			session   Session
			created   int64
			expires   int64
			tokenHash string
		)
		if err := rows.Scan(&session.ID, &session.DeviceID, &session.DeviceName,
			&created, &expires, &tokenHash, &session.LoginIP, &session.Location); err != nil {
			return nil, err
		}
		session.CreatedAt = timeFromMillis(created)
		session.ExpiresAt = timeFromMillis(expires)
		session.Current = tokenHash == current
		out = append(out, session)
	}
	return out, rows.Err()
}

// SetSessionOrigin records where a login token was issued. It runs after the
// token row exists so refresh rotation keeps the original location instead of
// rewriting it from the refreshing device.
func (s *Store) SetSessionOrigin(token, clientIP, location string) error {
	hash := tokenHash(strings.TrimSpace(token))
	if hash == "" {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`UPDATE user_tokens SET login_ip = ?, login_location = ? WHERE token_hash = ?`,
		strings.TrimSpace(clientIP), strings.TrimSpace(location), hash,
	); err != nil {
		return fmt.Errorf("account store %s: record session origin: %w", s.path, err)
	}
	return nil
}

// RevokeOtherSessions deletes every login of the user except the caller's
// token and reports how many were removed. The caller must already be
// authenticated, so an empty token is rejected instead of removing everything.
func (s *Store) RevokeOtherSessions(userID, currentToken string) (int, error) {
	currentToken = strings.TrimSpace(currentToken)
	if currentToken == "" {
		return 0, ErrInvalidToken
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, fmt.Errorf("account store %s: begin revoke other sessions: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`DELETE FROM refresh_tokens WHERE user_id = ?
		 AND session_id IN (SELECT session_id FROM user_tokens WHERE user_id = ? AND token_hash <> ?)`,
		userID, userID, tokenHash(currentToken),
	); err != nil {
		return 0, fmt.Errorf("account store %s: revoke other refresh tokens: %w", s.path, err)
	}
	res, err := tx.Exec(
		`DELETE FROM user_tokens WHERE user_id = ? AND token_hash <> ?`,
		userID, tokenHash(currentToken),
	)
	if err != nil {
		return 0, fmt.Errorf("account store %s: revoke other sessions: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("account store %s: revoke other sessions: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("account store %s: commit revoke other sessions: %w", s.path, err)
	}
	return int(affected), nil
}

// RenameSession updates the display name of one live login. false means the
// id does not belong to this user or the session already expired. An empty or
// control-only name is rejected instead of silently clearing the device label.
func (s *Store) RenameSession(userID, sessionID, deviceName string) (bool, error) {
	sessionID = strings.TrimSpace(sessionID)
	if sessionID == "" {
		return false, nil
	}
	deviceName = normalizeText(deviceName, maxDeviceName)
	if deviceName == "" {
		return false, ErrInvalidDeviceName
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(
		`UPDATE user_tokens SET device_name = ?
		 WHERE user_id = ? AND session_id = ? AND expires_at > ?`,
		deviceName, userID, sessionID, unixMillis(s.now().UTC()),
	)
	if err != nil {
		return false, fmt.Errorf("account store %s: rename session: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return false, err
	}
	return affected > 0, nil
}

// RevokeSession deletes one login by its public id. false means the id does
// not belong to this user or the session already expired.
func (s *Store) RevokeSession(userID, sessionID string) (bool, error) {
	sessionID = strings.TrimSpace(sessionID)
	if sessionID == "" {
		return false, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return false, fmt.Errorf("account store %s: begin revoke session: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(
		`DELETE FROM user_tokens WHERE user_id = ? AND session_id = ?`,
		userID, sessionID,
	)
	if err != nil {
		return false, fmt.Errorf("account store %s: revoke session: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return false, err
	}
	if _, err := tx.Exec(
		`DELETE FROM refresh_tokens WHERE user_id = ? AND session_id = ?`,
		userID, sessionID,
	); err != nil {
		return false, fmt.Errorf("account store %s: revoke session refresh token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return false, fmt.Errorf("account store %s: commit revoke session: %w", s.path, err)
	}
	return affected > 0, nil
}

// Peers lists live address-book entries only. Tombstones stay visible through
// Snapshot so incremental clients can apply deletions.
func (s *Store) Peers(userID string) ([]Peer, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.peersLocked(userID)
}

func (s *Store) peersLocked(userID string) ([]Peer, error) {
	rows, err := s.db.Query(
		`SELECT peer_id, name, tags, group_id, updated_at, revision FROM address_book
		 WHERE user_id = ? AND deleted = 0 ORDER BY peer_id`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list address book: %w", s.path, err)
	}
	defer rows.Close()
	var out []Peer
	for rows.Next() {
		var (
			peer      Peer
			tags      string
			updatedAt int64
		)
		if err := rows.Scan(&peer.ID, &peer.Name, &tags, &peer.GroupID, &updatedAt, &peer.Revision); err != nil {
			return nil, err
		}
		peer.Tags = decodeTags(tags)
		peer.UpdatedAt = timeFromMillis(updatedAt)
		out = append(out, peer)
	}
	return out, rows.Err()
}

func (s *Store) groupsLocked(userID string) ([]Group, error) {
	rows, err := s.db.Query(
		`SELECT group_id, name, updated_at, revision FROM address_book_groups
		 WHERE user_id = ? AND deleted = 0 ORDER BY name, group_id`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list address book groups: %w", s.path, err)
	}
	defer rows.Close()
	var out []Group
	for rows.Next() {
		var (
			group     Group
			updatedAt int64
		)
		if err := rows.Scan(&group.ID, &group.Name, &updatedAt, &group.Revision); err != nil {
			return nil, err
		}
		group.UpdatedAt = timeFromMillis(updatedAt)
		out = append(out, group)
	}
	return out, rows.Err()
}

// Snapshot returns the live address book, the current per-user revision and
// every row written after since, tombstones included. A since value outside
// the stored range (negative, or from a recreated database) falls back to a
// full history so the caller cannot miss deletions.
func (s *Store) Snapshot(userID string, since int64) (Snapshot, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var revision int64
	err := s.db.QueryRow(`SELECT book_revision FROM users WHERE id = ?`, userID).Scan(&revision)
	if errors.Is(err, sql.ErrNoRows) {
		return Snapshot{}, ErrUnknownUser
	}
	if err != nil {
		return Snapshot{}, fmt.Errorf("account store %s: read address book revision: %w", s.path, err)
	}
	if since < 0 || since > revision {
		since = 0
	}
	peers, err := s.peersLocked(userID)
	if err != nil {
		return Snapshot{}, err
	}
	groups, err := s.groupsLocked(userID)
	if err != nil {
		return Snapshot{}, err
	}
	rows, err := s.db.Query(
		`SELECT peer_id, name, tags, group_id, updated_at, revision, deleted FROM address_book
		 WHERE user_id = ? AND revision > ? ORDER BY revision, peer_id`,
		userID, since,
	)
	if err != nil {
		return Snapshot{}, fmt.Errorf("account store %s: list address book changes: %w", s.path, err)
	}
	defer rows.Close()
	changes := make([]Peer, 0)
	for rows.Next() {
		var (
			peer      Peer
			tags      string
			updatedAt int64
			deleted   int
		)
		if err := rows.Scan(&peer.ID, &peer.Name, &tags, &peer.GroupID, &updatedAt, &peer.Revision, &deleted); err != nil {
			return Snapshot{}, err
		}
		peer.Tags = decodeTags(tags)
		peer.UpdatedAt = timeFromMillis(updatedAt)
		peer.Deleted = deleted != 0
		changes = append(changes, peer)
	}
	if err := rows.Err(); err != nil {
		return Snapshot{}, err
	}
	rows.Close()
	groupRows, err := s.db.Query(
		`SELECT group_id, name, updated_at, revision, deleted FROM address_book_groups
		 WHERE user_id = ? AND revision > ? ORDER BY revision, group_id`,
		userID, since,
	)
	if err != nil {
		return Snapshot{}, fmt.Errorf("account store %s: list address book group changes: %w", s.path, err)
	}
	defer groupRows.Close()
	groupChanges := make([]Group, 0)
	for groupRows.Next() {
		var (
			group     Group
			updatedAt int64
			deleted   int
		)
		if err := groupRows.Scan(&group.ID, &group.Name, &updatedAt, &group.Revision, &deleted); err != nil {
			return Snapshot{}, err
		}
		group.UpdatedAt = timeFromMillis(updatedAt)
		group.Deleted = deleted != 0
		groupChanges = append(groupChanges, group)
	}
	if err := groupRows.Err(); err != nil {
		return Snapshot{}, err
	}
	if peers == nil {
		peers = []Peer{}
	}
	if groups == nil {
		groups = []Group{}
	}
	return Snapshot{
		Revision:     revision,
		Peers:        peers,
		Changes:      changes,
		Groups:       groups,
		GroupChanges: groupChanges,
	}, nil
}

// nextRevision advances the per-user counter inside the caller's transaction.
func nextRevision(tx *sql.Tx, userID string) (int64, error) {
	res, err := tx.Exec(`UPDATE users SET book_revision = book_revision + 1 WHERE id = ?`, userID)
	if err != nil {
		return 0, err
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return 0, err
	}
	if affected == 0 {
		return 0, ErrUnknownUser
	}
	var revision int64
	if err := tx.QueryRow(`SELECT book_revision FROM users WHERE id = ?`, userID).Scan(&revision); err != nil {
		return 0, err
	}
	return revision, nil
}

// UpdatePeer applies a partial address-book edit. Nil fields keep the previous
// value; an empty name or tag list clears that field. Editing a tombstoned
// peer revives it with a new revision.
func (s *Store) UpdatePeer(userID, peerID string, name *string, tags *[]string) (Peer, error) {
	return s.UpdatePeerWithGroup(userID, peerID, name, tags, nil)
}

// UpdatePeerWithGroup is UpdatePeer plus an optional group move, applied in the
// same revision. An empty group id removes the peer from its group.
func (s *Store) UpdatePeerWithGroup(userID, peerID string, name *string, tags *[]string, groupID *string) (Peer, error) {
	return s.UpdatePeerWithGroupAt(userID, peerID, name, tags, groupID, 0)
}

// UpdatePeerWithGroupAt is UpdatePeerWithGroup with optimistic concurrency
// control. A positive ifRevision must equal the live row's current revision
// inside the write transaction; otherwise ErrPeerConflict is returned and
// nothing changes. Zero keeps the previous last-writer-wins behavior.
func (s *Store) UpdatePeerWithGroupAt(userID, peerID string, name *string, tags *[]string, groupID *string, ifRevision int64) (Peer, error) {
	if !registry.ValidID(peerID) {
		return Peer{}, ErrInvalidPeerID
	}
	if groupID != nil && *groupID != "" && !registry.ValidID(*groupID) {
		return Peer{}, ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.updatePeerLocked(userID, peerID, name, tags, groupID, ifRevision)
}

func (s *Store) updatePeerLocked(userID, peerID string, name *string, tags *[]string, groupID *string, ifRevision int64) (Peer, error) {
	var cleanName string
	var cleanTags []string
	if name != nil {
		cleanName = normalizeText(*name, maxName)
	}
	if tags != nil {
		cleanTags = normalizeTags(*tags)
	}
	tx, err := s.db.Begin()
	if err != nil {
		return Peer{}, fmt.Errorf("account store %s: begin address book update: %w", s.path, err)
	}
	defer tx.Rollback()
	if groupID != nil && *groupID != "" {
		live, err := liveGroupLocked(tx, userID, *groupID)
		if err != nil {
			return Peer{}, err
		}
		if !live {
			return Peer{}, ErrUnknownGroup
		}
	}
	updatedName := ""
	var existingTags string
	updatedGroup := ""
	var (
		currentRevision int64
		deleted         bool
	)
	err = tx.QueryRow(
		`SELECT name, tags, group_id, revision, deleted FROM address_book WHERE user_id = ? AND peer_id = ?`,
		userID, peerID,
	).Scan(&updatedName, &existingTags, &updatedGroup, &currentRevision, &deleted)
	found := err == nil
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return Peer{}, fmt.Errorf("account store %s: read address book entry: %w", s.path, err)
	}
	if ifRevision > 0 && (!found || deleted || currentRevision != ifRevision) {
		return Peer{}, ErrPeerConflict
	}
	updatedTags := decodeTags(existingTags)
	if name != nil {
		updatedName = cleanName
	}
	if tags != nil {
		updatedTags = cleanTags
	}
	if groupID != nil {
		updatedGroup = *groupID
	}
	revision, err := nextRevision(tx, userID)
	if err != nil {
		return Peer{}, fmt.Errorf("account store %s: advance address book revision: %w", s.path, err)
	}
	now := s.now().UTC()
	if _, err := tx.Exec(
		`INSERT INTO address_book (user_id, peer_id, name, tags, group_id, updated_at, revision, deleted)
		 VALUES (?, ?, ?, ?, ?, ?, ?, 0)
		 ON CONFLICT(user_id, peer_id) DO UPDATE SET
			name = excluded.name, tags = excluded.tags, group_id = excluded.group_id,
			updated_at = excluded.updated_at, revision = excluded.revision, deleted = 0`,
		userID, peerID, updatedName, encodeTags(updatedTags), updatedGroup, unixMillis(now), revision,
	); err != nil {
		return Peer{}, fmt.Errorf("account store %s: update address book entry: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return Peer{}, fmt.Errorf("account store %s: commit address book entry: %w", s.path, err)
	}
	return Peer{
		ID:        peerID,
		Name:      updatedName,
		Tags:      updatedTags,
		GroupID:   updatedGroup,
		UpdatedAt: now,
		Revision:  revision,
	}, nil
}

// SetPeerGroup moves a live peer between groups without touching its name or
// tags. Assigning the current group is a no-op and does not advance revisions.
func (s *Store) SetPeerGroup(userID, peerID, groupID string) (Peer, error) {
	return s.SetPeerGroupAt(userID, peerID, groupID, 0)
}

// SetPeerGroupAt is SetPeerGroup with the same optimistic concurrency contract
// as UpdatePeerWithGroupAt. Zero ifRevision keeps last-writer-wins.
func (s *Store) SetPeerGroupAt(userID, peerID, groupID string, ifRevision int64) (Peer, error) {
	if !registry.ValidID(peerID) {
		return Peer{}, ErrInvalidPeerID
	}
	if groupID != "" && !registry.ValidID(groupID) {
		return Peer{}, ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return Peer{}, fmt.Errorf("account store %s: begin address book group move: %w", s.path, err)
	}
	defer tx.Rollback()
	if groupID != "" {
		live, err := liveGroupLocked(tx, userID, groupID)
		if err != nil {
			return Peer{}, err
		}
		if !live {
			return Peer{}, ErrUnknownGroup
		}
	}
	var (
		peer      Peer
		tags      string
		updatedAt int64
	)
	err = tx.QueryRow(
		`SELECT peer_id, name, tags, group_id, updated_at, revision FROM address_book
		 WHERE user_id = ? AND peer_id = ? AND deleted = 0`,
		userID, peerID,
	).Scan(&peer.ID, &peer.Name, &tags, &peer.GroupID, &updatedAt, &peer.Revision)
	if errors.Is(err, sql.ErrNoRows) {
		return Peer{}, ErrUnknownPeer
	}
	if err != nil {
		return Peer{}, fmt.Errorf("account store %s: read address book entry: %w", s.path, err)
	}
	if ifRevision > 0 && peer.Revision != ifRevision {
		return Peer{}, ErrPeerConflict
	}
	peer.Tags = decodeTags(tags)
	peer.UpdatedAt = timeFromMillis(updatedAt)
	if peer.GroupID == groupID {
		return peer, nil
	}
	revision, err := nextRevision(tx, userID)
	if err != nil {
		return Peer{}, fmt.Errorf("account store %s: advance address book revision: %w", s.path, err)
	}
	now := s.now().UTC()
	if _, err := tx.Exec(
		`UPDATE address_book SET group_id = ?, updated_at = ?, revision = ?
		 WHERE user_id = ? AND peer_id = ?`,
		groupID, unixMillis(now), revision, userID, peerID,
	); err != nil {
		return Peer{}, fmt.Errorf("account store %s: move address book entry: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return Peer{}, fmt.Errorf("account store %s: commit address book group move: %w", s.path, err)
	}
	peer.GroupID = groupID
	peer.UpdatedAt = now
	peer.Revision = revision
	return peer, nil
}

// UpsertGroup creates or renames one of the user's groups. Reusing a
// tombstoned id revives it with a new revision.
func (s *Store) UpsertGroup(userID, groupID, name string) (Group, error) {
	return s.UpsertGroupAt(userID, groupID, name, 0)
}

// UpsertGroupAt is UpsertGroup with an optional group revision token. A
// positive ifRevision must match the live group; a missing or tombstoned
// group, or one written since the client read it, reports ErrGroupConflict.
// Zero keeps last-writer-wins for older clients.
func (s *Store) UpsertGroupAt(userID, groupID, name string, ifRevision int64) (Group, error) {
	if !registry.ValidID(groupID) {
		return Group{}, ErrInvalidGroupID
	}
	name = normalizeText(name, maxName)
	if name == "" {
		return Group{}, ErrInvalidGroupName
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return Group{}, fmt.Errorf("account store %s: begin address book group write: %w", s.path, err)
	}
	defer tx.Rollback()
	if ifRevision > 0 {
		var (
			live    int64
			deleted int
		)
		err := tx.QueryRow(
			`SELECT revision, deleted FROM address_book_groups
			 WHERE user_id = ? AND group_id = ?`,
			userID, groupID,
		).Scan(&live, &deleted)
		if errors.Is(err, sql.ErrNoRows) ||
			(err == nil && (deleted != 0 || live != ifRevision)) {
			return Group{}, ErrGroupConflict
		}
		if err != nil {
			return Group{}, fmt.Errorf("account store %s: read address book group: %w", s.path, err)
		}
	}
	revision, err := nextRevision(tx, userID)
	if err != nil {
		return Group{}, fmt.Errorf("account store %s: advance address book revision: %w", s.path, err)
	}
	now := s.now().UTC()
	if _, err := tx.Exec(
		`INSERT INTO address_book_groups (user_id, group_id, name, updated_at, revision, deleted)
		 VALUES (?, ?, ?, ?, ?, 0)
		 ON CONFLICT(user_id, group_id) DO UPDATE SET
			name = excluded.name, updated_at = excluded.updated_at,
			revision = excluded.revision, deleted = 0`,
		userID, groupID, name, unixMillis(now), revision,
	); err != nil {
		return Group{}, fmt.Errorf("account store %s: write address book group: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return Group{}, fmt.Errorf("account store %s: commit address book group: %w", s.path, err)
	}
	return Group{ID: groupID, Name: name, UpdatedAt: now, Revision: revision}, nil
}

// DeleteGroup tombstones a group and removes its live peers from it. Every
// moved peer gets its own revision so incremental clients see the new group.
// false means the group was already absent.
func (s *Store) DeleteGroup(userID, groupID string) (bool, error) {
	return s.DeleteGroupAt(userID, groupID, 0)
}

// DeleteGroupAt is DeleteGroup with an optional group revision token. A
// positive ifRevision that no longer matches the live group reports
// ErrGroupConflict before anything is written.
func (s *Store) DeleteGroupAt(userID, groupID string, ifRevision int64) (bool, error) {
	if !registry.ValidID(groupID) {
		return false, ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return false, fmt.Errorf("account store %s: begin address book group delete: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		deleted int
		live    int64
	)
	err = tx.QueryRow(
		`SELECT deleted, revision FROM address_book_groups WHERE user_id = ? AND group_id = ?`,
		userID, groupID,
	).Scan(&deleted, &live)
	if errors.Is(err, sql.ErrNoRows) || (err == nil && deleted != 0) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read address book group: %w", s.path, err)
	}
	if ifRevision > 0 && live != ifRevision {
		return false, ErrGroupConflict
	}
	revision, err := nextRevision(tx, userID)
	if err != nil {
		return false, fmt.Errorf("account store %s: advance address book revision: %w", s.path, err)
	}
	now := s.now().UTC()
	if _, err := tx.Exec(
		`UPDATE address_book_groups SET name = '', updated_at = ?, revision = ?, deleted = 1
		 WHERE user_id = ? AND group_id = ?`,
		unixMillis(now), revision, userID, groupID,
	); err != nil {
		return false, fmt.Errorf("account store %s: delete address book group: %w", s.path, err)
	}
	rows, err := tx.Query(
		`SELECT peer_id FROM address_book
		 WHERE user_id = ? AND group_id = ? AND deleted = 0 ORDER BY peer_id`,
		userID, groupID,
	)
	if err != nil {
		return false, fmt.Errorf("account store %s: list grouped peers: %w", s.path, err)
	}
	var peerIDs []string
	for rows.Next() {
		var peerID string
		if err := rows.Scan(&peerID); err != nil {
			rows.Close()
			return false, fmt.Errorf("account store %s: list grouped peers: %w", s.path, err)
		}
		peerIDs = append(peerIDs, peerID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return false, fmt.Errorf("account store %s: list grouped peers: %w", s.path, err)
	}
	rows.Close()
	for _, peerID := range peerIDs {
		peerRevision, err := nextRevision(tx, userID)
		if err != nil {
			return false, fmt.Errorf("account store %s: advance address book revision: %w", s.path, err)
		}
		if _, err := tx.Exec(
			`UPDATE address_book SET group_id = '', updated_at = ?, revision = ?
			 WHERE user_id = ? AND peer_id = ?`,
			unixMillis(now), peerRevision, userID, peerID,
		); err != nil {
			return false, fmt.Errorf("account store %s: ungroup address book entry: %w", s.path, err)
		}
	}
	// A scoped grant for a tombstoned group would otherwise linger and cover
	// nothing; revoke it with the group it was created for.
	if _, err := tx.Exec(
		`DELETE FROM address_book_grants WHERE owner_user_id = ? AND group_id = ?`,
		userID, groupID,
	); err != nil {
		return false, fmt.Errorf("account store %s: revoke group grants: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return false, fmt.Errorf("account store %s: commit address book group delete: %w", s.path, err)
	}
	return true, nil
}

func liveGroupLocked(tx *sql.Tx, userID, groupID string) (bool, error) {
	var deleted int
	err := tx.QueryRow(
		`SELECT deleted FROM address_book_groups WHERE user_id = ? AND group_id = ?`,
		userID, groupID,
	).Scan(&deleted)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return deleted == 0, nil
}

// DeletePeer writes a tombstone instead of dropping the row. false means
// there was no live entry, preserving the previous 404 behaviour.
func (s *Store) DeletePeer(userID, peerID string) (bool, error) {
	if !registry.ValidID(peerID) {
		return false, ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.deletePeerLocked(userID, peerID, 0)
}

func (s *Store) deletePeerLocked(userID, peerID string, ifRevision int64) (bool, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return false, fmt.Errorf("account store %s: begin address book delete: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		deleted         int
		currentRevision int64
	)
	err = tx.QueryRow(
		`SELECT revision, deleted FROM address_book WHERE user_id = ? AND peer_id = ?`,
		userID, peerID,
	).Scan(&currentRevision, &deleted)
	if errors.Is(err, sql.ErrNoRows) {
		if ifRevision > 0 {
			return false, ErrPeerConflict
		}
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read address book entry: %w", s.path, err)
	}
	if deleted != 0 {
		if ifRevision > 0 {
			return false, ErrPeerConflict
		}
		return false, nil
	}
	if ifRevision > 0 && currentRevision != ifRevision {
		return false, ErrPeerConflict
	}
	revision, err := nextRevision(tx, userID)
	if err != nil {
		return false, fmt.Errorf("account store %s: advance address book revision: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`UPDATE address_book SET name = '', tags = '', updated_at = ?, revision = ?, deleted = 1
		 WHERE user_id = ? AND peer_id = ?`,
		unixMillis(s.now().UTC()), revision, userID, peerID,
	); err != nil {
		return false, fmt.Errorf("account store %s: delete address book entry: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return false, fmt.Errorf("account store %s: commit address book delete: %w", s.path, err)
	}
	return true, nil
}

// ShareBook lets granteeUsername access the owner's live address book at the
// requested permission. An empty permission means read. Sharing the same user
// again updates the permission and keeps the original creation time.
func (s *Store) ShareBook(ownerID, granteeUsername, permission string) (Grant, error) {
	return s.ShareBookScoped(ownerID, granteeUsername, permission, "")
}

// ShareBookScoped shares the whole book when groupID is empty, otherwise only
// that group. Grants accumulate, so a user can hold whole-book read plus write
// on a single group.
func (s *Store) ShareBookScoped(ownerID, granteeUsername, permission, groupID string) (Grant, error) {
	permission, err := normalizePermission(permission)
	if err != nil {
		return Grant{}, err
	}
	groupID = strings.TrimSpace(groupID)
	if groupID != "" && !registry.ValidID(groupID) {
		return Grant{}, ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.shareBookLocked(ownerID, granteeUsername, permission, groupID)
}

func (s *Store) shareBookLocked(ownerID, granteeUsername, permission, groupID string) (Grant, error) {
	granteeUsername = strings.TrimSpace(granteeUsername)
	var (
		granteeID string
		grantee   string
	)
	err := s.db.QueryRow(
		`SELECT id, username FROM users WHERE username = ?`, granteeUsername,
	).Scan(&granteeID, &grantee)
	if errors.Is(err, sql.ErrNoRows) {
		return Grant{}, ErrUnknownUser
	}
	if err != nil {
		return Grant{}, fmt.Errorf("account store %s: look up shared user: %w", s.path, err)
	}
	if granteeID == ownerID {
		return Grant{}, ErrSelfShare
	}
	if groupID != "" {
		live, err := s.groupLiveLocked(ownerID, groupID)
		if err != nil {
			return Grant{}, err
		}
		if !live {
			return Grant{}, ErrUnknownGroup
		}
	}
	if _, err := s.db.Exec(
		`INSERT INTO address_book_grants (owner_user_id, grantee_user_id, group_id, permission, created_at)
		 VALUES (?, ?, ?, ?, ?)
		 ON CONFLICT(owner_user_id, grantee_user_id, group_id) DO UPDATE SET permission = excluded.permission`,
		ownerID, granteeID, groupID, permission, unixMillis(s.now().UTC()),
	); err != nil {
		return Grant{}, fmt.Errorf("account store %s: share address book: %w", s.path, err)
	}
	var created int64
	if err := s.db.QueryRow(
		`SELECT created_at FROM address_book_grants
		 WHERE owner_user_id = ? AND grantee_user_id = ? AND group_id = ?`,
		ownerID, granteeID, groupID,
	).Scan(&created); err != nil {
		return Grant{}, fmt.Errorf("account store %s: read shared grant: %w", s.path, err)
	}
	return Grant{
		Username:   grantee,
		Permission: permission,
		GroupID:    groupID,
		CreatedAt:  timeFromMillis(created),
	}, nil
}

func (s *Store) groupLiveLocked(userID, groupID string) (bool, error) {
	var deleted int
	err := s.db.QueryRow(
		`SELECT deleted FROM address_book_groups WHERE user_id = ? AND group_id = ?`,
		userID, groupID,
	).Scan(&deleted)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read address book group: %w", s.path, err)
	}
	return deleted == 0, nil
}

// UnshareBook revokes every grant between the pair, matching the original
// whole-book semantics. Unsharing a user who was never granted the book
// reports ErrUnknownShare so the API can answer 404.
func (s *Store) UnshareBook(ownerID, granteeUsername string) error {
	granteeUsername = strings.TrimSpace(granteeUsername)
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.unshareBookLocked(ownerID, granteeUsername)
}

// UnshareBookScope revokes one grant row: the whole-book grant when groupID is
// empty, otherwise that group's grant. Other scopes stay in place.
func (s *Store) UnshareBookScope(ownerID, granteeUsername, groupID string) error {
	groupID = strings.TrimSpace(groupID)
	if groupID != "" && !registry.ValidID(groupID) {
		return ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.unshareBookScopeLocked(ownerID, granteeUsername, groupID)
}

func (s *Store) unshareBookScopeLocked(ownerID, granteeUsername, groupID string) error {
	res, err := s.db.Exec(
		`DELETE FROM address_book_grants
		 WHERE owner_user_id = ?
		   AND grantee_user_id = (SELECT id FROM users WHERE username = ?)
		   AND group_id = ?`,
		ownerID, granteeUsername, groupID,
	)
	if err != nil {
		return fmt.Errorf("account store %s: unshare address book scope: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownShare
	}
	return nil
}

func (s *Store) unshareBookLocked(ownerID, granteeUsername string) error {
	res, err := s.db.Exec(
		`DELETE FROM address_book_grants
		 WHERE owner_user_id = ?
		   AND grantee_user_id = (SELECT id FROM users WHERE username = ?)`,
		ownerID, granteeUsername,
	)
	if err != nil {
		return fmt.Errorf("account store %s: unshare address book: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownShare
	}
	return nil
}

// Grants lists both directions: users with access to this book, and owners
// whose books this user can access.
func (s *Store) Grants(userID string) (outgoing, incoming []Grant, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if outgoing, err = s.grantsLocked(
		`SELECT u.username, g.permission, g.group_id, g.created_at FROM address_book_grants g
		 JOIN users u ON u.id = g.grantee_user_id
		 WHERE g.owner_user_id = ? ORDER BY u.username COLLATE NOCASE, g.group_id`,
		userID,
	); err != nil {
		return nil, nil, err
	}
	if incoming, err = s.grantsLocked(
		`SELECT u.username, g.permission, g.group_id, g.created_at FROM address_book_grants g
		 JOIN users u ON u.id = g.owner_user_id
		 WHERE g.grantee_user_id = ? ORDER BY u.username COLLATE NOCASE, g.group_id`,
		userID,
	); err != nil {
		return nil, nil, err
	}
	return outgoing, incoming, nil
}

func (s *Store) grantsLocked(query, userID string) ([]Grant, error) {
	rows, err := s.db.Query(query, userID)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list address book grants: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]Grant, 0)
	for rows.Next() {
		var (
			grant   Grant
			created int64
		)
		if err := rows.Scan(&grant.Username, &grant.Permission, &grant.GroupID, &created); err != nil {
			return nil, err
		}
		grant.CreatedAt = timeFromMillis(created)
		out = append(out, grant)
	}
	return out, rows.Err()
}

// SharedBooks returns every address book shared with userID. Each book lists
// only the peers and groups its grants expose; Scopes carries the individual
// whole-book and group-level permissions. Owners are resolved first so the
// single database connection is free before each peer query runs.
func (s *Store) SharedBooks(userID string) ([]SharedBook, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT u.id, u.username, g.group_id, g.permission, u.book_revision
		 FROM address_book_grants g
		 JOIN users u ON u.id = g.owner_user_id
		 WHERE g.grantee_user_id = ? ORDER BY u.username COLLATE NOCASE, g.group_id`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list shared books: %w", s.path, err)
	}
	type ownerBook struct {
		id       string
		username string
		revision int64
		scopes   []SharedScope
	}
	var (
		owners []*ownerBook
		index  = make(map[string]*ownerBook)
	)
	for rows.Next() {
		var (
			id         string
			username   string
			groupID    string
			permission string
			revision   int64
		)
		if err := rows.Scan(&id, &username, &groupID, &permission, &revision); err != nil {
			rows.Close()
			return nil, err
		}
		book, ok := index[id]
		if !ok {
			book = &ownerBook{id: id, username: username, revision: revision}
			index[id] = book
			owners = append(owners, book)
		}
		book.scopes = append(book.scopes, SharedScope{GroupID: groupID, Permission: permission})
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	rows.Close()
	out := make([]SharedBook, 0, len(owners))
	for _, item := range owners {
		peers, err := s.peersLocked(item.id)
		if err != nil {
			return nil, err
		}
		if peers == nil {
			peers = []Peer{}
		}
		groups, err := s.groupsLocked(item.id)
		if err != nil {
			return nil, err
		}
		whole := ""
		permission := ""
		for _, scope := range item.scopes {
			if permissionRank(scope.Permission) > permissionRank(permission) {
				permission = scope.Permission
			}
			if scope.GroupID != "" {
				continue
			}
			whole = scope.Permission
		}
		visiblePeers := make([]Peer, 0, len(peers))
		for _, peer := range peers {
			if whole != "" || scopePermission(item.scopes, peer.GroupID) != "" {
				visiblePeers = append(visiblePeers, peer)
			}
		}
		visibleGroups := make([]Group, 0, len(groups))
		for _, group := range groups {
			if whole != "" || scopePermission(item.scopes, group.ID) != "" {
				visibleGroups = append(visibleGroups, group)
			}
		}
		out = append(out, SharedBook{
			Owner:      item.username,
			Permission: permission,
			Revision:   item.revision,
			Peers:      visiblePeers,
			Groups:     visibleGroups,
			Scopes:     item.scopes,
		})
	}
	return out, nil
}

func scopePermission(scopes []SharedScope, groupID string) string {
	best := ""
	for _, scope := range scopes {
		if scope.GroupID == groupID && permissionRank(scope.Permission) > permissionRank(best) {
			best = scope.Permission
		}
	}
	return best
}

// sharedGrants collects every grant a grantee holds on one owner. Unknown
// owners and missing grants both report ErrUnknownShare so the API does not
// leak usernames.
type sharedGrants struct {
	ownerID string
	whole   string
	groups  map[string]string
}

func (s *Store) sharedGrantsLocked(granteeID, ownerUsername string) (sharedGrants, error) {
	var ownerID string
	err := s.db.QueryRow(
		`SELECT id FROM users WHERE username = ?`, strings.TrimSpace(ownerUsername),
	).Scan(&ownerID)
	if errors.Is(err, sql.ErrNoRows) {
		return sharedGrants{}, ErrUnknownShare
	}
	if err != nil {
		return sharedGrants{}, fmt.Errorf("account store %s: look up shared owner: %w", s.path, err)
	}
	rows, err := s.db.Query(
		`SELECT group_id, permission FROM address_book_grants
		 WHERE owner_user_id = ? AND grantee_user_id = ?`,
		ownerID, granteeID,
	)
	if err != nil {
		return sharedGrants{}, fmt.Errorf("account store %s: read shared grants: %w", s.path, err)
	}
	defer rows.Close()
	out := sharedGrants{ownerID: ownerID, groups: make(map[string]string)}
	found := false
	for rows.Next() {
		var (
			groupID    string
			permission string
		)
		if err := rows.Scan(&groupID, &permission); err != nil {
			return sharedGrants{}, err
		}
		found = true
		if groupID == "" {
			out.whole = permission
		} else {
			out.groups[groupID] = permission
		}
	}
	if err := rows.Err(); err != nil {
		return sharedGrants{}, err
	}
	if !found {
		return sharedGrants{}, ErrUnknownShare
	}
	return out, nil
}

// effective is the strongest permission covering groupID. The whole-book
// grant covers every group; a group grant only covers its own group.
func (g sharedGrants) effective(groupID string) string {
	best := g.whole
	if permission := g.groups[groupID]; permissionRank(permission) > permissionRank(best) {
		best = permission
	}
	return best
}

func (g sharedGrants) require(required, groupID string) error {
	if !permissionAllows(g.effective(groupID), required) {
		return ErrPermissionDenied
	}
	return nil
}

// peerGroupLocked reports the group a peer currently belongs to, including
// tombstones, so a scoped grantee can revive an entry without escaping its
// group. Unknown peers map to the ungrouped scope.
func (s *Store) peerGroupLocked(userID, peerID string) (string, error) {
	var groupID string
	err := s.db.QueryRow(
		`SELECT group_id FROM address_book WHERE user_id = ? AND peer_id = ?`,
		userID, peerID,
	).Scan(&groupID)
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("account store %s: read shared peer group: %w", s.path, err)
	}
	return groupID, nil
}

func permissionAllows(granted, required string) bool {
	return permissionRank(granted) >= permissionRank(required)
}

func permissionRank(permission string) int {
	switch permission {
	case PermissionControl:
		return 3
	case PermissionWrite:
		return 2
	case PermissionRead:
		return 1
	}
	return 0
}

func normalizePermission(permission string) (string, error) {
	switch normalized := strings.ToLower(strings.TrimSpace(permission)); normalized {
	case "", PermissionRead:
		return PermissionRead, nil
	case PermissionWrite, PermissionControl:
		return normalized, nil
	}
	return "", ErrInvalidPermission
}

// UpdateSharedPeer writes into another user's book through a write or control
// grant. The owner's revision advances so their own incremental clients see
// the change like any other edit.
func (s *Store) UpdateSharedPeer(granteeID, ownerUsername, peerID string, name *string, tags *[]string) (Peer, error) {
	return s.UpdateSharedPeerAt(granteeID, ownerUsername, peerID, name, tags, 0)
}

// UpdateSharedPeerAt is UpdateSharedPeer with an optional revision token.
// A positive ifRevision must match the owner's live row; zero keeps
// last-writer-wins for grantee writes. A group-scoped grant may only touch
// peers that currently live in that group.
func (s *Store) UpdateSharedPeerAt(granteeID, ownerUsername, peerID string, name *string, tags *[]string, ifRevision int64) (Peer, error) {
	if !registry.ValidID(peerID) {
		return Peer{}, ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(granteeID, ownerUsername)
	if err != nil {
		return Peer{}, err
	}
	groupID, err := s.peerGroupLocked(grants.ownerID, peerID)
	if err != nil {
		return Peer{}, err
	}
	if err := grants.require(PermissionWrite, groupID); err != nil {
		return Peer{}, err
	}
	return s.updatePeerLocked(grants.ownerID, peerID, name, tags, nil, ifRevision)
}

func (s *Store) DeleteSharedPeer(granteeID, ownerUsername, peerID string) (bool, error) {
	return s.DeleteSharedPeerAt(granteeID, ownerUsername, peerID, 0)
}

// DeleteSharedPeerAt is DeleteSharedPeer with an optional revision token.
// Deleting a row that changed, vanished or was already tombstoned under a
// positive token returns ErrPeerConflict instead of reporting success.
func (s *Store) DeleteSharedPeerAt(granteeID, ownerUsername, peerID string, ifRevision int64) (bool, error) {
	if !registry.ValidID(peerID) {
		return false, ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(granteeID, ownerUsername)
	if err != nil {
		return false, err
	}
	groupID, err := s.peerGroupLocked(grants.ownerID, peerID)
	if err != nil {
		return false, err
	}
	if err := grants.require(PermissionWrite, groupID); err != nil {
		return false, err
	}
	return s.deletePeerLocked(grants.ownerID, peerID, ifRevision)
}

// AddSharedPeer inserts a new peer into another user's book through a write or
// control grant. An existing live or tombstoned entry with the same id is
// overwritten and revived, matching the owner's own PATCH upsert behaviour.
func (s *Store) AddSharedPeer(granteeID, ownerUsername, peerID, name string, tags []string) (Peer, error) {
	return s.AddSharedPeerScoped(granteeID, ownerUsername, peerID, "", name, tags)
}

// AddSharedPeerScoped adds a peer to the owner's ungrouped list (empty
// groupID) or to one group. Group grants may only write into their own group.
func (s *Store) AddSharedPeerScoped(granteeID, ownerUsername, peerID, groupID, name string, tags []string) (Peer, error) {
	if !registry.ValidID(peerID) {
		return Peer{}, ErrInvalidPeerID
	}
	groupID = strings.TrimSpace(groupID)
	if groupID != "" && !registry.ValidID(groupID) {
		return Peer{}, ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(granteeID, ownerUsername)
	if err != nil {
		return Peer{}, err
	}
	var groupArg *string
	if groupID != "" {
		live, err := s.groupLiveLocked(grants.ownerID, groupID)
		if err != nil {
			return Peer{}, err
		}
		if !live {
			return Peer{}, ErrUnknownGroup
		}
		groupArg = &groupID
	}
	if err := grants.require(PermissionWrite, groupID); err != nil {
		return Peer{}, err
	}
	return s.updatePeerLocked(grants.ownerID, peerID, &name, &tags, groupArg, 0)
}

// ShareBookAs lets a grantee with whole-book control permission manage
// another user's grants without being the owner.
func (s *Store) ShareBookAs(actorID, ownerUsername, granteeUsername, permission string) (Grant, error) {
	return s.ShareBookScopedAs(actorID, ownerUsername, granteeUsername, permission, "")
}

// ShareBookScopedAs is ShareBookAs for one group scope. Management still
// requires whole-book control, so a group controller cannot widen its reach.
func (s *Store) ShareBookScopedAs(actorID, ownerUsername, granteeUsername, permission, groupID string) (Grant, error) {
	permission, err := normalizePermission(permission)
	if err != nil {
		return Grant{}, err
	}
	groupID = strings.TrimSpace(groupID)
	if groupID != "" && !registry.ValidID(groupID) {
		return Grant{}, ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(actorID, ownerUsername)
	if err != nil {
		return Grant{}, err
	}
	if !permissionAllows(grants.whole, PermissionControl) {
		return Grant{}, ErrPermissionDenied
	}
	return s.shareBookLocked(grants.ownerID, granteeUsername, permission, groupID)
}

func (s *Store) UnshareBookAs(actorID, ownerUsername, granteeUsername string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(actorID, ownerUsername)
	if err != nil {
		return err
	}
	if !permissionAllows(grants.whole, PermissionControl) {
		return ErrPermissionDenied
	}
	return s.unshareBookLocked(grants.ownerID, strings.TrimSpace(granteeUsername))
}

// UnshareBookScopeAs revokes one scope of a shared book for a controller.
func (s *Store) UnshareBookScopeAs(actorID, ownerUsername, granteeUsername, groupID string) error {
	groupID = strings.TrimSpace(groupID)
	if groupID != "" && !registry.ValidID(groupID) {
		return ErrInvalidGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(actorID, ownerUsername)
	if err != nil {
		return err
	}
	if !permissionAllows(grants.whole, PermissionControl) {
		return ErrPermissionDenied
	}
	return s.unshareBookScopeLocked(grants.ownerID, strings.TrimSpace(granteeUsername), groupID)
}

// SharedBookGrants lists the grants on a shared book for a controller.
func (s *Store) SharedBookGrants(actorID, ownerUsername string) ([]Grant, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	grants, err := s.sharedGrantsLocked(actorID, ownerUsername)
	if err != nil {
		return nil, err
	}
	if !permissionAllows(grants.whole, PermissionControl) {
		return nil, ErrPermissionDenied
	}
	return s.grantsLocked(
		`SELECT u.username, g.permission, g.group_id, g.created_at FROM address_book_grants g
		 JOIN users u ON u.id = g.grantee_user_id
		 WHERE g.owner_user_id = ? ORDER BY u.username COLLATE NOCASE, g.group_id`,
		grants.ownerID,
	)
}

func validatePassword(password string) error {
	if len(password) < 8 || len(password) > 128 {
		return ErrWeakPassword
	}
	return nil
}

func hashPassword(password string) (string, error) {
	salt := make([]byte, saltBytes)
	if _, err := rand.Read(salt); err != nil {
		return "", err
	}
	key := pbkdf2SHA256([]byte(password), salt, passwordIteration, passwordKeyBytes)
	return fmt.Sprintf("pbkdf2-sha256$%d$%s$%s", passwordIteration,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(key)), nil
}

func verifyPassword(encoded, password string) (bool, error) {
	parts := strings.Split(encoded, "$")
	if len(parts) != 4 || parts[0] != "pbkdf2-sha256" {
		return false, errors.New("unsupported password hash")
	}
	iterations, err := strconv.Atoi(parts[1])
	if err != nil || iterations <= 0 || iterations > 1_000_000 {
		return false, errors.New("invalid password hash")
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[2])
	if err != nil || len(salt) == 0 {
		return false, errors.New("invalid password salt")
	}
	want, err := base64.RawStdEncoding.DecodeString(parts[3])
	if err != nil || len(want) == 0 {
		return false, errors.New("invalid password hash")
	}
	got := pbkdf2SHA256([]byte(password), salt, iterations, len(want))
	return subtle.ConstantTimeCompare(got, want) == 1, nil
}

var (
	dummyOnce sync.Once
	dummyHash string
)

func verifyDummy(password string) {
	dummyOnce.Do(func() {
		salt := make([]byte, saltBytes)
		copy(salt, "chast-dummy-salt")
		key := pbkdf2SHA256([]byte("chast-dummy-password"), salt, passwordIteration, passwordKeyBytes)
		dummyHash = fmt.Sprintf("pbkdf2-sha256$%d$%s$%s", passwordIteration,
			base64.RawStdEncoding.EncodeToString(salt),
			base64.RawStdEncoding.EncodeToString(key))
	})
	_, _ = verifyPassword(dummyHash, password)
}

func pbkdf2SHA256(password, salt []byte, iterations, keyLen int) []byte {
	hashLen := sha256.Size
	blocks := (keyLen + hashLen - 1) / hashLen
	out := make([]byte, 0, blocks*hashLen)
	for block := 1; block <= blocks; block++ {
		mac := hmac.New(sha256.New, password)
		mac.Write(salt)
		mac.Write([]byte{byte(block >> 24), byte(block >> 16), byte(block >> 8), byte(block)})
		u := mac.Sum(nil)
		t := make([]byte, len(u))
		copy(t, u)
		for i := 1; i < iterations; i++ {
			mac.Reset()
			mac.Write(u)
			u = mac.Sum(nil)
			for j := range t {
				t[j] ^= u[j]
			}
		}
		out = append(out, t...)
	}
	return out[:keyLen]
}

func newToken() (string, error) {
	raw := make([]byte, tokenBytes)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(raw), nil
}

func newRandomHex(size int) (string, error) {
	raw := make([]byte, size)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw), nil
}

func tokenHash(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

// newRecoveryCode returns a grouped base32 code such as ABCD-EFGH-IJKL-MNOP.
// Every hyphen-separated group is four characters so the code is easy to read
// off paper; normalizeRecoveryCode makes dashes and case optional at login.
func newRecoveryCode() (string, error) {
	raw := make([]byte, recoveryCodeBytes)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate recovery code: %w", err)
	}
	encoded := totpBase32.EncodeToString(raw)
	var b strings.Builder
	for i := 0; i < len(encoded); i += recoveryCodeGroup {
		if i > 0 {
			b.WriteByte('-')
		}
		b.WriteString(encoded[i : i+recoveryCodeGroup])
	}
	return b.String(), nil
}

func normalizeRecoveryCode(code string) string {
	code = strings.ToUpper(code)
	var b strings.Builder
	for _, r := range code {
		if r == '-' || r == ' ' || r == '\t' || r == '\r' || r == '\n' {
			continue
		}
		b.WriteRune(r)
	}
	return b.String()
}

func recoveryCodeHash(code string) string {
	return tokenHash(normalizeRecoveryCode(code))
}

func normalizeText(value string, limit int) string {
	value = strings.TrimSpace(value)
	value = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, value)
	if runes := []rune(value); len(runes) > limit {
		value = string(runes[:limit])
	}
	return value
}

func normalizeTags(tags []string) []string {
	seen := make(map[string]struct{}, len(tags))
	out := make([]string, 0, len(tags))
	for _, tag := range tags {
		tag = normalizeText(tag, maxTag)
		if tag == "" {
			continue
		}
		if _, ok := seen[tag]; ok {
			continue
		}
		seen[tag] = struct{}{}
		out = append(out, tag)
		if len(out) == maxTags {
			break
		}
	}
	return out
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

func unixMillis(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.UTC().UnixMilli()
}

func timeFromMillis(ms int64) time.Time {
	if ms == 0 {
		return time.Time{}
	}
	return time.UnixMilli(ms).UTC()
}
