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

func TestRegistrySchemaMigrationRecordsEveryStep(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := NewStore(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	rows, err := store.db.Query(
		`SELECT version, name FROM registry_schema_migrations ORDER BY version`)
	if err != nil {
		store.Close()
		t.Fatalf("read migration records: %v", err)
	}
	var versions []int
	var names []string
	for rows.Next() {
		var version int
		var name string
		if err := rows.Scan(&version, &name); err != nil {
			rows.Close()
			store.Close()
			t.Fatalf("scan migration record: %v", err)
		}
		versions = append(versions, version)
		names = append(names, name)
	}
	rows.Close()
	if len(versions) != len(registryMigrations) || versions[len(versions)-1] != registrySchemaVersion ||
		names[0] != "baseline" || names[len(names)-1] != "metadata_signed" {
		store.Close()
		t.Fatalf("migration records = %v %v", versions, names)
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

// TestStoreUpgradesMetadataSignatureSchema covers a database written by the
// previous binary: schema version 1 is already recorded, so only the new
// migration runs and must add the column without touching existing rows.
func TestStoreUpgradesMetadataSignatureSchema(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`CREATE TABLE peers (
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
	)`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`CREATE TABLE registry_schema_migrations (
		version    INTEGER PRIMARY KEY,
		name       TEXT NOT NULL DEFAULT '',
		applied_at INTEGER NOT NULL
	)`); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(
		`INSERT INTO registry_schema_migrations (version, name, applied_at) VALUES (1, 'baseline', 0)`); err != nil {
		t.Fatal(err)
	}
	_, pub := hostKey("host-secret")
	if _, err := db.Exec(`INSERT INTO peers
		(id, public_key, fingerprint, hostname, platform, version, created_at, last_seen)
		VALUES (?, ?, '', 'STUDIO', 'windows/amd64', '0.35.0', 1000, 0)`, "devbox", pub); err != nil {
		t.Fatal(err)
	}
	db.Close()

	store, err := NewStore(path)
	if err != nil {
		t.Fatalf("open version 1 database: %v", err)
	}
	ids, err := store.Load()
	if err != nil || len(ids) != 1 {
		t.Fatalf("load upgraded database: %v %d", err, len(ids))
	}
	if ids[0].Hostname != "STUDIO" || ids[0].MetadataSigned {
		t.Fatalf("upgraded row = %+v", ids[0])
	}
	var version int
	if err := store.db.QueryRow(
		`SELECT MAX(version) FROM registry_schema_migrations`).Scan(&version); err != nil {
		store.Close()
		t.Fatalf("read schema version: %v", err)
	}
	if version != registrySchemaVersion {
		store.Close()
		t.Fatalf("schema version = %d, want %d", version, registrySchemaVersion)
	}
	ids[0].MetadataSigned = true
	if err := store.Save(ids[0]); err != nil {
		store.Close()
		t.Fatalf("save signed row: %v", err)
	}
	store.Close()

	reopened, err := NewStore(path)
	if err != nil {
		t.Fatalf("reopen upgraded database: %v", err)
	}
	defer reopened.Close()
	ids, err = reopened.Load()
	if err != nil || len(ids) != 1 || !ids[0].MetadataSigned {
		t.Fatalf("reload upgraded database: %v %+v", err, ids)
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
