package sqlitex

import (
	"errors"
	"testing"
	"time"
)

func TestClassify(t *testing.T) {
	cases := map[string]string{
		"SELECT id, public_key FROM peers ORDER BY id": "select peers",
		"select 1": "select",
		"INSERT INTO peers (id, public_key) VALUES (?, ?)":               "insert peers",
		"INSERT OR REPLACE INTO users (id) VALUES (?)":                   "insert users",
		"UPDATE users SET role = ? WHERE id = ?":                         "update users",
		"UPDATE OR IGNORE peers SET alias = ?":                           "update peers",
		"DELETE FROM tokens WHERE id = ?":                                "delete tokens",
		"CREATE TABLE IF NOT EXISTS peers (id TEXT PRIMARY KEY)":         "create peers",
		"CREATE UNIQUE INDEX IF NOT EXISTS peers_alias ON peers (alias)": "create peers_alias",
		"ALTER TABLE peers ADD COLUMN alias TEXT":                        "alter peers",
		"BEGIN":                    "begin",
		"COMMIT":                   "commit",
		"ROLLBACK":                 "rollback",
		"PRAGMA busy_timeout=5000": "pragma",
		"PRAGMA table_info(peers)": "pragma table_info",
		"":                         "other",
	}
	for query, want := range cases {
		if got := operation(query); got != want {
			t.Errorf("operation(%q) = %q, want %q", query, got, want)
		}
	}
}

func TestSlowAndErrorCounters(t *testing.T) {
	before := StoreStatsFor(Registry)
	observe(Registry, "SELECT * FROM peers", 2*SlowQuery, nil)
	observe(Registry, "SELECT * FROM peers", time.Millisecond, errors.New("boom"))
	after := StoreStatsFor(Registry)

	if got := after.Queries - before.Queries; got != 2 {
		t.Fatalf("query delta = %d, want 2", got)
	}
	if got := after.Slow - before.Slow; got != 1 {
		t.Fatalf("slow delta = %d, want 1", got)
	}
	if got := after.Errors - before.Errors; got != 1 {
		t.Fatalf("error delta = %d, want 1", got)
	}
	if after.Seconds <= before.Seconds {
		t.Fatalf("duration total did not grow")
	}
	var found bool
	for _, op := range after.Ops {
		if op.Op == "select peers" && op.Slow >= 1 && op.Errors >= 1 {
			found = true
		}
	}
	if !found {
		t.Fatalf("select peers op stats did not record the slow/error samples: %+v", after.Ops)
	}
}
