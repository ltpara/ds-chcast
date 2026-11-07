package account

import (
	"database/sql"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func TestHostAuditIdentityMigrationAndRetries(t *testing.T) {
	path := filepath.Join(t.TempDir(), "audit.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	seedAuditEventTables(t, db)
	if _, err := db.Exec(`CREATE TABLE schema_migrations (
		version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at INTEGER NOT NULL)`); err != nil {
		t.Fatal(err)
	}
	for version := 1; version <= 8; version++ {
		if _, err := db.Exec(`INSERT INTO schema_migrations VALUES (?, 'legacy', 0)`, version); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := db.Exec(`INSERT INTO audit_events (created_at, username, action, detail)
		VALUES (1, 'alice', 'login', 'legacy')`); err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if store != nil {
			store.Close()
		}
	}()
	legacy, err := store.AuditEvents(10)
	if err != nil || len(legacy) != 1 || legacy[0].HostID != "" || legacy[0].EventKey != "" || legacy[0].Detail != "legacy" {
		t.Fatalf("legacy audit migration: %+v, %v", legacy, err)
	}
	if err := applyHostAuditIdentity(store.db); err != nil {
		t.Fatalf("repeat migration: %v", err)
	}
	event := AuditEvent{Action: AuditHostDenied, HostID: "host-a", EventKey: "stable-event-a",
		CreatedAt: time.UnixMilli(1700000000123), ClientIP: "203.0.113.9", Detail: "host-a: refused"}
	var wg sync.WaitGroup
	errors := make(chan error, 16)
	for i := 0; i < cap(errors); i++ {
		wg.Add(1)
		go func() { defer wg.Done(); errors <- store.AppendAudit(event) }()
	}
	wg.Wait()
	close(errors)
	for err := range errors {
		if err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.AppendAudit(event); err != nil {
		t.Fatal(err)
	}
	rows, err := store.AuditEventsFor(AuditHostDenied, 10)
	if err != nil || len(rows) != 1 || rows[0].HostID != event.HostID || rows[0].EventKey != event.EventKey {
		t.Fatalf("persistent retry deduplication: %+v, %v", rows, err)
	}
	archive, err := store.AuditEventsAfter(0, 10)
	if err != nil || len(archive) != 2 || archive[1].HostID != event.HostID || archive[1].EventKey != event.EventKey {
		t.Fatalf("archive identity: %+v, %v", archive, err)
	}
	// Conflict races may leave sequence gaps; retention must count rows, not
	// treat the next large sequence value as thousands of new events.
	if _, err := store.db.Exec(`UPDATE sqlite_sequence SET seq = ? WHERE name = 'audit_events'`, auditRetention*2); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if err := store.AppendAudit(AuditEvent{Action: AuditLogin, Detail: "ordinary"}); err != nil {
			t.Fatal(err)
		}
	}
	rows, err = store.AuditEvents(10)
	if err != nil || len(rows) != 4 {
		t.Fatalf("ordinary events and retention after retries: %+v, %v", rows, err)
	}
}
