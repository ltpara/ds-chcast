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

func TestRegistrySchemaMigrationRecordsBaseline(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := NewStore(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	var version int
	var name string
	if err := store.db.QueryRow(
		`SELECT version, name FROM registry_schema_migrations`,
	).Scan(&version, &name); err != nil {
		store.Close()
		t.Fatalf("read migration record: %v", err)
	}
	if version != registrySchemaVersion || name != "baseline" {
		store.Close()
		t.Fatalf("migration record = %d %q", version, name)
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
	if records != 1 {
		t.Fatalf("migration records = %d, want 1", records)
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
