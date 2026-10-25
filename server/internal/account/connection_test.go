package account

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestConnectionEventsLifecycle(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	started := time.Date(2026, 10, 25, 9, 0, 0, 0, time.UTC)
	ended := started.Add(42 * time.Minute)
	first, err := store.RecordConnection(
		alice.ID, alice.Username, " devbox\x00 ", "", "127.0.0.1",
		started, ended, true, "session ended\x00",
	)
	if err != nil {
		t.Fatalf("record connection: %v", err)
	}
	if first.ID == 0 || first.PeerID != "devbox" || first.Direction != "viewer" ||
		first.Detail != "session ended" || !first.Success {
		t.Fatalf("unexpected event: %+v", first)
	}
	if first.Duration() != 42*time.Minute {
		t.Fatalf("duration = %v", first.Duration())
	}
	second, err := store.RecordConnection(
		alice.ID, alice.Username, "laptop", "viewer", "10.0.0.5",
		ended, ended.Add(time.Minute), false, "viewer closed",
	)
	if err != nil {
		t.Fatalf("record second connection: %v", err)
	}
	if _, err := store.RecordConnection(
		bob.ID, bob.Username, "server", "viewer", "192.0.2.7",
		started, started.Add(5*time.Minute), true, "",
	); err != nil {
		t.Fatalf("record bob connection: %v", err)
	}

	aliceEvents, err := store.Connections(alice.ID, 10)
	if err != nil {
		t.Fatalf("list alice connections: %v", err)
	}
	if len(aliceEvents) != 2 || aliceEvents[0].ID != second.ID || aliceEvents[1].ID != first.ID {
		t.Fatalf("alice connections = %+v", aliceEvents)
	}
	if aliceEvents[0].Success || aliceEvents[0].Duration() != time.Minute {
		t.Fatalf("second event = %+v", aliceEvents[0])
	}
	if bobEvents, err := store.ConnectionsFor("BOB", 10); err != nil || len(bobEvents) != 1 ||
		bobEvents[0].Username != "bob" {
		t.Fatalf("bob filter = %+v, %v", bobEvents, err)
	}
	all, err := store.ConnectionsFor("", 10)
	if err != nil || len(all) != 3 {
		t.Fatalf("all connections = %+v, %v", all, err)
	}
	limited, err := store.ConnectionsFor("", 2)
	if err != nil || len(limited) != 2 || limited[0].ID != all[0].ID {
		t.Fatalf("limited connections = %+v, %v", limited, err)
	}

	for _, tc := range []struct {
		name      string
		userID    string
		peer      string
		direction string
		started   time.Time
		ended     time.Time
	}{
		{"empty user", "", "devbox", "viewer", started, ended},
		{"empty peer", alice.ID, "  \x00 ", "viewer", started, ended},
		{"unknown direction", alice.ID, "devbox", "host", started, ended},
		{"ends before start", alice.ID, "devbox", "viewer", ended, started},
		{"over 30 days", alice.ID, "devbox", "viewer", started, started.Add(31 * 24 * time.Hour)},
	} {
		if _, err := store.RecordConnection(
			tc.userID, "alice", tc.peer, tc.direction, "", tc.started, tc.ended, true, "",
		); !errors.Is(err, ErrInvalidConnection) {
			t.Fatalf("%s: error = %v", tc.name, err)
		}
	}

	// Missing timestamps fall back to the store clock instead of 1970.
	now, err := store.RecordConnection(alice.ID, alice.Username, "instant", "viewer", "", time.Time{}, time.Time{}, true, "")
	if err != nil {
		t.Fatalf("record without timestamps: %v", err)
	}
	if now.StartedAt.IsZero() || now.EndedAt.Before(now.StartedAt) {
		t.Fatalf("fallback timestamps = %+v", now)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen account store: %v", err)
	}
	defer reopened.Close()
	persisted, err := reopened.Connections(alice.ID, 10)
	if err != nil {
		t.Fatalf("list after reopen: %v", err)
	}
	if len(persisted) != 3 || persisted[0].PeerID != "instant" {
		t.Fatalf("events after reopen = %+v", persisted)
	}
	if !strings.Contains(persisted[2].PeerID, "devbox") {
		t.Fatalf("oldest event after reopen = %+v", persisted[2])
	}
}
