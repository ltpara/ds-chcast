package sqlitex_test

import (
	"database/sql"
	"path/filepath"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/sqlitex"
)

func opCount(stats sqlitex.StoreStats, op string) (queries, errs uint64) {
	for _, entry := range stats.Ops {
		if entry.Op == op {
			return entry.Queries, entry.Errors
		}
	}
	return 0, 0
}

func TestDriverMeasuresStatements(t *testing.T) {
	before := sqlitex.StoreStatsFor(sqlitex.Registry)
	db, err := sql.Open(sqlitex.RegistryDriver, filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	db.SetMaxOpenConns(1)

	mustExec := func(query string, args ...any) {
		t.Helper()
		if _, err := db.Exec(query, args...); err != nil {
			t.Fatalf("%s: %v", query, err)
		}
	}
	mustExec(`CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, body TEXT NOT NULL)`)
	mustExec(`INSERT INTO messages (body) VALUES (?)`, "hello")

	rows, err := db.Query(`SELECT body FROM messages`)
	if err != nil {
		t.Fatal(err)
	}
	for rows.Next() {
		var body string
		if err := rows.Scan(&body); err != nil {
			t.Fatal(err)
		}
	}
	if err := rows.Close(); err != nil {
		t.Fatal(err)
	}

	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(`INSERT INTO messages (body) VALUES (?)`, "in-tx"); err != nil {
		t.Fatal(err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatal(err)
	}

	if _, err := db.Exec(`SELECT * FROM missing_table`); err == nil {
		t.Fatal("expected an error for a missing table")
	}

	after := sqlitex.StoreStatsFor(sqlitex.Registry)
	if after.Queries <= before.Queries {
		t.Fatalf("queries did not grow: %d -> %d", before.Queries, after.Queries)
	}
	if after.InFlight != 0 {
		t.Fatalf("in-flight = %d, want 0", after.InFlight)
	}
	for _, op := range []string{"create messages", "insert messages", "select messages", "begin", "commit"} {
		got, _ := opCount(after, op)
		was, _ := opCount(before, op)
		if got <= was {
			t.Errorf("op %q did not grow: %d -> %d", op, was, got)
		}
	}
	if _, errs := opCount(after, "select missing_table"); errs == 0 {
		t.Error("missing-table error was not attributed to select missing_table")
	}

	pool := sqlitex.Pool(db)
	if pool.MaxOpenConnections != 1 {
		t.Errorf("max open = %d, want 1", pool.MaxOpenConnections)
	}
	if pool.OpenConnections != 1 {
		t.Errorf("open connections = %d, want 1 while a connection is pooled", pool.OpenConnections)
	}
}

func TestProductionStoresUseInstrumentedDriver(t *testing.T) {
	regBefore := sqlitex.StoreStatsFor(sqlitex.Registry)
	reg, err := registry.NewStore(filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatal(err)
	}
	if pool := reg.SQLitePoolStats(); pool.MaxOpenConnections != 1 {
		t.Errorf("registry max open = %d, want 1", pool.MaxOpenConnections)
	}
	if err := reg.Close(); err != nil {
		t.Fatal(err)
	}
	if after := sqlitex.StoreStatsFor(sqlitex.Registry); after.Queries <= regBefore.Queries {
		t.Fatalf("registry store queries did not grow: %d -> %d", regBefore.Queries, after.Queries)
	}

	accBefore := sqlitex.StoreStatsFor(sqlitex.Account)
	acc, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatal(err)
	}
	if pool := acc.SQLitePoolStats(); pool.MaxOpenConnections != 1 {
		t.Errorf("account max open = %d, want 1", pool.MaxOpenConnections)
	}
	if err := acc.Close(); err != nil {
		t.Fatal(err)
	}
	if after := sqlitex.StoreStatsFor(sqlitex.Account); after.Queries <= accBefore.Queries {
		t.Fatalf("account store queries did not grow: %d -> %d", accBefore.Queries, after.Queries)
	}
}
