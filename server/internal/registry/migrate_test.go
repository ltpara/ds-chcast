package registry

import (
	"database/sql"
	"path/filepath"
	"strings"
	"testing"
)

func TestRegistrySchemaVersionMatchesMigrations(t *testing.T) {
	if last := registryMigrations[len(registryMigrations)-1].Version; last != registrySchemaVersion {
		t.Fatalf("last migration = %d, registrySchemaVersion = %d", last, registrySchemaVersion)
	}
}

func TestRegistrySchemaMigrationRecordsLatest(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := NewStore(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	var version int
	var name string
	if err := store.db.QueryRow(
		`SELECT version, name FROM registry_schema_migrations ORDER BY version DESC LIMIT 1`,
	).Scan(&version, &name); err != nil {
		store.Close()
		t.Fatalf("read migration record: %v", err)
	}
	latest := registryMigrations[len(registryMigrations)-1]
	if version != latest.Version || name != latest.Name {
		store.Close()
		t.Fatalf("migration record = %d %q, want %d %q", version, name, latest.Version, latest.Name)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := NewStore(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	defer reopened.Close()
	var records int
	if err := reopened.db.QueryRow(`SELECT COUNT(*) FROM registry_schema_migrations`).Scan(&records); err != nil {
		t.Fatalf("count migration records: %v", err)
	}
	if records != len(registryMigrations) {
		t.Fatalf("migration records = %d, want %d", records, len(registryMigrations))
	}
}

func TestRegistrySchemaMigrationAddsWakeTargets(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	statements := []string{
		`CREATE TABLE peers (
			id TEXT PRIMARY KEY,
			public_key TEXT NOT NULL,
			fingerprint TEXT NOT NULL DEFAULT '',
			hostname TEXT NOT NULL DEFAULT '',
			platform TEXT NOT NULL DEFAULT '',
			version TEXT NOT NULL DEFAULT '',
			metadata_signature TEXT NOT NULL DEFAULT '',
			alias TEXT NOT NULL DEFAULT '',
			tags TEXT NOT NULL DEFAULT '',
			created_at INTEGER NOT NULL,
			last_seen INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE TABLE registry_schema_migrations (
			version INTEGER PRIMARY KEY,
			name TEXT NOT NULL DEFAULT '',
			applied_at BIGINT NOT NULL
		)`,
		`INSERT INTO registry_schema_migrations (version, name, applied_at) VALUES (1, 'baseline', 0)`,
	}
	for _, statement := range statements {
		if _, err := db.Exec(statement); err != nil {
			db.Close()
			t.Fatalf("seed v1 database: %v", err)
		}
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}

	store, err := NewStore(path)
	if err != nil {
		t.Fatalf("upgrade store: %v", err)
	}
	defer store.Close()
	var version int
	if err := store.db.QueryRow(
		`SELECT COALESCE(MAX(version), 0) FROM registry_schema_migrations`,
	).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != registrySchemaVersion {
		t.Fatalf("schema version = %d, want %d", version, registrySchemaVersion)
	}
	rows, err := store.db.Query(`PRAGMA table_info(peers)`)
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	found := false
	for rows.Next() {
		var (
			cid, notNull, pk int
			name, columnType string
			defaultValue     sql.NullString
		)
		if err := rows.Scan(&cid, &name, &columnType, &notNull, &defaultValue, &pk); err != nil {
			t.Fatal(err)
		}
		if name == "wake_targets" {
			found = true
		}
	}
	if err := rows.Err(); err != nil {
		t.Fatal(err)
	}
	if !found {
		t.Fatal("wake_targets column was not added to the v1 schema")
	}
}

func TestRegistrySchemaMigrationRejectsNewerDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := NewStore(path)
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
		`INSERT INTO registry_schema_migrations (version, name, applied_at) VALUES (?, 'future', 0)`,
		registrySchemaVersion+1,
	); err != nil {
		db.Close()
		t.Fatalf("seed future version: %v", err)
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close raw database: %v", err)
	}

	if _, err := NewStore(path); err == nil || !strings.Contains(err.Error(), "newer than this binary supports") {
		t.Fatalf("open newer database error = %v", err)
	}
}
