package schemamig

import (
	"database/sql"
	"errors"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"
)

func openTestDB(t *testing.T) *sql.DB {
	t.Helper()
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open test database: %v", err)
	}
	db.SetMaxOpenConns(1)
	t.Cleanup(func() { db.Close() })
	return db
}

func testMigrations(applied *int) []Migration {
	count := func() { *applied = *applied + 1 }
	return []Migration{
		{Version: 1, Name: "create widgets", Apply: func(db *sql.DB) error {
			count()
			_, err := db.Exec(`CREATE TABLE widgets (
				id INTEGER PRIMARY KEY,
				label TEXT NOT NULL DEFAULT ''
			)`)
			return err
		}},
		{Version: 2, Name: "seed widget", Apply: func(db *sql.DB) error {
			count()
			_, err := db.Exec(`INSERT INTO widgets (id, label) VALUES (1, 'seeded')`)
			return err
		}},
	}
}

func TestRunAppliesPendingMigrationsOnce(t *testing.T) {
	db := openTestDB(t)
	var applied int
	version, err := Run(db, "schema_migrations", testMigrations(&applied))
	if err != nil {
		t.Fatalf("first run: %v", err)
	}
	if version != 2 || applied != 2 {
		t.Fatalf("first run = version %d, %d applied", version, applied)
	}
	version, err = Run(db, "schema_migrations", testMigrations(&applied))
	if err != nil {
		t.Fatalf("second run: %v", err)
	}
	if version != 2 || applied != 2 {
		t.Fatalf("second run reapplied migrations: version %d, %d applied", version, applied)
	}
	var rows, records int
	if err := db.QueryRow(`SELECT COUNT(*) FROM widgets`).Scan(&rows); err != nil {
		t.Fatalf("count widgets: %v", err)
	}
	if err := db.QueryRow(`SELECT COUNT(*) FROM schema_migrations`).Scan(&records); err != nil {
		t.Fatalf("count migration records: %v", err)
	}
	if rows != 1 || records != 2 {
		t.Fatalf("widgets = %d, migration records = %d", rows, records)
	}
}

func TestRunAppliesOnlyMissingMigrations(t *testing.T) {
	db := openTestDB(t)
	var applied int
	all := testMigrations(&applied)
	if _, err := Run(db, "schema_migrations", all[:1]); err != nil {
		t.Fatalf("baseline run: %v", err)
	}
	version, err := Run(db, "schema_migrations", all)
	if err != nil {
		t.Fatalf("upgrade run: %v", err)
	}
	if version != 2 || applied != 2 {
		t.Fatalf("upgrade run = version %d, %d applied", version, applied)
	}
}

func TestRunRejectsNewerSchema(t *testing.T) {
	db := openTestDB(t)
	var applied int
	if _, err := Run(db, "schema_migrations", testMigrations(&applied)); err != nil {
		t.Fatalf("baseline run: %v", err)
	}
	if _, err := db.Exec(
		`INSERT INTO schema_migrations (version, name, applied_at) VALUES (3, 'future', 0)`,
	); err != nil {
		t.Fatalf("seed future version: %v", err)
	}
	version, err := Run(db, "schema_migrations", testMigrations(&applied))
	var tooNew *TooNewError
	if !errors.As(err, &tooNew) {
		t.Fatalf("newer schema error = %v, want TooNewError", err)
	}
	if version != 3 || tooNew.Found != 3 || tooNew.Supported != 2 {
		t.Fatalf("newer schema = version %d, %+v", version, tooNew)
	}
	if applied != 2 {
		t.Fatalf("newer schema still applied migrations: %d", applied)
	}
}

func TestRunValidatesMigrationList(t *testing.T) {
	db := openTestDB(t)
	noop := func(*sql.DB) error { return nil }
	for name, testCase := range map[string]struct {
		table      string
		migrations []Migration
	}{
		"empty":         {"schema_migrations", nil},
		"bad table":     {"Schema Migrations", []Migration{{Version: 1, Apply: noop}}},
		"gap":           {"schema_migrations", []Migration{{Version: 2, Apply: noop}}},
		"missing apply": {"schema_migrations", []Migration{{Version: 1}}},
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := Run(db, testCase.table, testCase.migrations); err == nil {
				t.Fatal("expected a validation error")
			}
		})
	}
}
