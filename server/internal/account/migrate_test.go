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
	if err := store.db.QueryRow(`SELECT version, name FROM schema_migrations`).Scan(&version, &name); err != nil {
		store.Close()
		t.Fatalf("read migration record: %v", err)
	}
	if version != accountSchemaVersion || name != "baseline" {
		store.Close()
		t.Fatalf("migration record = %d %q", version, name)
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
	if records != 1 {
		t.Fatalf("migration records = %d, want 1", records)
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
