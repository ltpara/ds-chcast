package account

import (
	"database/sql"
	"path/filepath"
	"strings"
	"testing"
)

func TestSchemaVersionMatchesMigrations(t *testing.T) {
	if last := accountMigrations[len(accountMigrations)-1].Version; last != accountSchemaVersion {
		t.Fatalf("last migration = %d, accountSchemaVersion = %d", last, accountSchemaVersion)
	}
}

func TestSchemaMigrationRecordsBaselineOnce(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	var version int
	var name string
	if err := store.db.QueryRow(
		`SELECT version, name FROM schema_migrations ORDER BY version DESC LIMIT 1`,
	).Scan(&version, &name); err != nil {
		store.Close()
		t.Fatalf("read migration record: %v", err)
	}
	latest := accountMigrations[len(accountMigrations)-1]
	if version != accountSchemaVersion || name != latest.Name {
		store.Close()
		t.Fatalf("migration record = %d %q, want %d %q",
			version, name, accountSchemaVersion, latest.Name)
	}
	user, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		store.Close()
		t.Fatalf("create user: %v", err)
	}
	// A stale revision would be bumped by the baseline backfill, so leaving
	// it at zero proves that reopening skips the recorded migration.
	if _, err := store.db.Exec(`UPDATE users SET book_revision = 0 WHERE id = ?`, user.ID); err != nil {
		store.Close()
		t.Fatalf("reset revision: %v", err)
	}
	if _, err := store.db.Exec(
		`INSERT INTO address_book (user_id, peer_id, name, tags, updated_at, revision)
		 VALUES (?, 'peer1', '', '', 1, 0)`, user.ID,
	); err != nil {
		store.Close()
		t.Fatalf("seed address book row: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	defer reopened.Close()
	var revision int64
	if err := reopened.db.QueryRow(`SELECT book_revision FROM users WHERE id = ?`, user.ID).Scan(&revision); err != nil {
		t.Fatalf("read revision: %v", err)
	}
	if revision != 0 {
		t.Fatalf("baseline re-ran on reopen: book_revision = %d", revision)
	}
	var records int
	if err := reopened.db.QueryRow(`SELECT COUNT(*) FROM schema_migrations`).Scan(&records); err != nil {
		t.Fatalf("count migration records: %v", err)
	}
	if records != len(accountMigrations) {
		t.Fatalf("migration records = %d, want %d", records, len(accountMigrations))
	}
}

func TestSchemaMigrationAddsAddressBookNesting(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open raw database: %v", err)
	}
	for _, statement := range []string{
		`CREATE TABLE schema_migrations (
			version    INTEGER PRIMARY KEY,
			name       TEXT NOT NULL DEFAULT '',
			applied_at INTEGER NOT NULL
		)`,
		`INSERT INTO schema_migrations (version, name, applied_at) VALUES
			(1, 'baseline', 0), (2, 'archive checkpoints', 0)`,
		`CREATE TABLE address_book (
			user_id    TEXT NOT NULL,
			peer_id    TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			tags       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			revision   INTEGER NOT NULL DEFAULT 0,
			deleted    INTEGER NOT NULL DEFAULT 0,
			group_id   TEXT NOT NULL DEFAULT '',
			PRIMARY KEY (user_id, peer_id)
		)`,
		`CREATE TABLE address_book_groups (
			user_id    TEXT NOT NULL,
			group_id   TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			revision   INTEGER NOT NULL DEFAULT 0,
			deleted    INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, group_id)
		)`,
		`INSERT INTO address_book_groups (user_id, group_id, name, updated_at, revision, deleted)
		 VALUES ('user1', 'legacy', 'Legacy', 0, 1, 0)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			db.Close()
			t.Fatalf("seed pre-v3 database: %v", err)
		}
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close raw database: %v", err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer store.Close()
	var (
		parent   string
		position int64
	)
	if err := store.db.QueryRow(
		`SELECT parent_id, position FROM address_book_groups WHERE group_id = 'legacy'`,
	).Scan(&parent, &position); err != nil {
		t.Fatalf("read migrated group: %v", err)
	}
	if parent != "" || position != 0 {
		t.Fatalf("migrated group placement = %q/%d", parent, position)
	}
	for _, column := range []struct {
		table string
		name  string
	}{
		{"address_book", "position"},
		{"address_book_groups", "parent_id"},
		{"address_book_groups", "position"},
	} {
		found, err := columnExists(store.db, column.table, column.name)
		if err != nil || !found {
			t.Fatalf("column %s.%s missing after migration: %v", column.table, column.name, err)
		}
	}
	var version int
	if err := store.db.QueryRow(`SELECT MAX(version) FROM schema_migrations`).Scan(&version); err != nil {
		t.Fatalf("read schema version: %v", err)
	}
	if version != accountSchemaVersion {
		t.Fatalf("schema version = %d, want %d", version, accountSchemaVersion)
	}
}

func TestSchemaMigrationAddsRecordingArchive(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open raw database: %v", err)
	}
	for _, statement := range []string{
		`CREATE TABLE schema_migrations (
			version    INTEGER PRIMARY KEY,
			name       TEXT NOT NULL DEFAULT '',
			applied_at INTEGER NOT NULL
		)`,
		`INSERT INTO schema_migrations (version, name, applied_at) VALUES
			(1, 'baseline', 0), (2, 'archive checkpoints', 0), (3, 'address book nesting', 0)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			db.Close()
			t.Fatalf("seed pre-v4 database: %v", err)
		}
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close raw database: %v", err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	defer store.Close()
	var tables int
	if err := store.db.QueryRow(
		`SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'recordings'`,
	).Scan(&tables); err != nil || tables != 1 {
		t.Fatalf("recordings table after migration: %d %v", tables, err)
	}
	var version int
	if err := store.db.QueryRow(`SELECT MAX(version) FROM schema_migrations`).Scan(&version); err != nil {
		t.Fatalf("read schema version: %v", err)
	}
	if version != accountSchemaVersion {
		t.Fatalf("schema version = %d, want %d", version, accountSchemaVersion)
	}
}

func TestSchemaMigrationRejectsNewerDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open raw database: %v", err)
	}
	if _, err := db.Exec(
		`INSERT INTO schema_migrations (version, name, applied_at) VALUES (?, 'future', 0)`,
		accountSchemaVersion+1,
	); err != nil {
		db.Close()
		t.Fatalf("seed future version: %v", err)
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close raw database: %v", err)
	}

	if _, err := Open(path); err == nil || !strings.Contains(err.Error(), "newer than this binary supports") {
		t.Fatalf("open newer database error = %v", err)
	}
}
