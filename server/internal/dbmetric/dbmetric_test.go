package dbmetric

import (
	"bytes"
	"path/filepath"
	"strings"
	"testing"
)

func TestOpenSQLiteRecordsStatements(t *testing.T) {
	db, err := OpenSQLite("test-db", filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer db.Close()
	db.SetMaxOpenConns(1)

	if _, err := db.Exec(`CREATE TABLE peers (id TEXT PRIMARY KEY, name TEXT)`); err != nil {
		t.Fatalf("create: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO peers (id, name) VALUES (?, ?)`, "devbox", "Dev Box"); err != nil {
		t.Fatalf("insert: %v", err)
	}
	var name string
	if err := db.QueryRow(`SELECT name FROM peers WHERE id = ?`, "devbox").Scan(&name); err != nil {
		t.Fatalf("select: %v", err)
	}
	if _, err := db.Exec(`INSERT INTO missing VALUES (1)`); err == nil {
		t.Fatal("insert into a missing table succeeded")
	}

	var b bytes.Buffer
	Default.WritePrometheus(&b)
	out := b.String()
	for _, want := range []string{
		`chast_db_queries_total{op="CREATE"} 1`,
		`chast_db_queries_total{op="INSERT"} 2`,
		`chast_db_query_errors_total{op="INSERT"} 1`,
		`chast_db_query_errors_total{op="SELECT"} 0`,
		`chast_db_pool_wait_total{db="test-db"}`,
		`chast_db_pool_max_open{db="test-db"} 1`,
	} {
		if !strings.Contains(out, want) {
			t.Fatalf("metrics missing %q:\n%s", want, out)
		}
	}
}

func TestOperationLabel(t *testing.T) {
	cases := map[string]string{
		"":                             "OTHER",
		"  select * from peers":        "SELECT",
		"INSERT INTO peers VALUES (1)": "INSERT",
		"with recursive x as (select 1) select * from x": "WITH",
		"PRAGMA foreign_keys=ON":                         "PRAGMA",
		"replace into peers values(1)":                   "REPLACE",
		"VACUUM":                                         "VACUUM",
	}
	for query, want := range cases {
		if got := operation(query); got != want {
			t.Fatalf("operation(%q) = %q, want %q", query, got, want)
		}
	}
}
