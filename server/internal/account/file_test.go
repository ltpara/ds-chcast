package account

import (
	"errors"
	"path/filepath"
	"testing"
	"time"
)

func TestFileTransferBatch(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	occurred := time.Date(2026, 10, 25, 11, 0, 0, 0, time.UTC)
	events, err := store.RecordFileTransfers(
		alice.ID, alice.Username, "devbox", "203.0.113.5", "Testland", RouteRelay,
		"0011aabb",
		[]FileTransferReport{
			{Direction: "upload", Name: "a.txt", Size: 1, Success: true, OccurredAt: occurred},
			{Direction: "download", Name: "b.txt", Size: 2, Success: false,
				Detail: "disk full", OccurredAt: occurred.Add(time.Second)},
		},
	)
	if err != nil {
		t.Fatalf("record batch: %v", err)
	}
	if len(events) != 2 || events[0].Name != "a.txt" || events[1].Name != "b.txt" {
		t.Fatalf("batch events = %+v", events)
	}
	for _, event := range events {
		if event.Location != "Testland" || event.Route != RouteRelay ||
			event.AuditRef != "0011aabb" || event.Files != 1 ||
			event.ClientIP != "203.0.113.5" {
			t.Fatalf("batch context = %+v", event)
		}
	}
	if events[0].Success != true || events[1].Success != false ||
		events[1].Detail != "disk full" {
		t.Fatalf("batch results = %+v", events)
	}

	// One bad entry rejects the whole batch before anything is written.
	if _, err := store.RecordFileTransfers(
		alice.ID, alice.Username, "devbox", "", "", "", "",
		[]FileTransferReport{
			{Direction: "upload", Name: "ok.txt", Size: 1},
			{Direction: "viewer", Name: "bad.txt", Size: 1},
		},
	); !errors.Is(err, ErrInvalidTransfer) {
		t.Fatalf("bad batch error = %v", err)
	}
	if _, err := store.RecordFileTransfers(alice.ID, alice.Username, "devbox",
		"", "", "", "", nil); !errors.Is(err, ErrInvalidTransfer) {
		t.Fatalf("empty batch error = %v", err)
	}
	if rows, err := store.FileTransfers(alice.ID, 10); err != nil || len(rows) != 2 {
		t.Fatalf("rows after rejected batch = %+v, %v", rows, err)
	}

	// The route whitelist drops unknown values instead of storing them.
	if route := normalizeRoute(" PUNCH "); route != RoutePunch {
		t.Fatalf("normalizeRoute = %q", route)
	}
	if route := normalizeRoute("bogus"); route != "" {
		t.Fatalf("unknown route = %q", route)
	}
}

func TestFileTransferBatchLimit(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	reports := make([]FileTransferReport, maxTransferBatch+1)
	for i := range reports {
		reports[i] = FileTransferReport{Direction: "upload", Name: "f", Size: 1}
	}
	if _, err := store.RecordFileTransfers(alice.ID, alice.Username, "devbox",
		"", "", "", "", reports); !errors.Is(err, ErrInvalidTransfer) {
		t.Fatalf("oversized batch error = %v", err)
	}
}

func TestFileTransferEventsLifecycle(t *testing.T) {
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

	occurred := time.Date(2026, 10, 25, 11, 0, 0, 0, time.UTC)
	first, err := store.RecordFileTransfer(
		alice.ID, alice.Username, " devbox\x00 ", "upload", "report\x00.pdf",
		"127.0.0.1", "", "", "", 4096, 1, occurred, true, "",
	)
	if err != nil {
		t.Fatalf("record upload: %v", err)
	}
	if first.ID == 0 || first.PeerID != "devbox" || first.Direction != "upload" ||
		first.Name != "report.pdf" || first.Size != 4096 || first.Files != 1 ||
		!first.Success || !first.OccurredAt.Equal(occurred) {
		t.Fatalf("unexpected upload event: %+v", first)
	}
	second, err := store.RecordFileTransfer(
		alice.ID, alice.Username, "laptop", "download", "photos.zip", "10.0.0.5", "", "", "",
		900, 3, occurred.Add(time.Minute), false, "disk full",
	)
	if err != nil {
		t.Fatalf("record download: %v", err)
	}
	if _, err := store.RecordFileTransfer(
		bob.ID, bob.Username, "server", "upload", "", "192.0.2.7", "", "", "", 0, 2, occurred, true, "",
	); err != nil {
		t.Fatalf("record bob transfer: %v", err)
	}

	aliceEvents, err := store.FileTransfers(alice.ID, 10)
	if err != nil {
		t.Fatalf("list alice transfers: %v", err)
	}
	if len(aliceEvents) != 2 || aliceEvents[0].ID != second.ID || aliceEvents[1].ID != first.ID {
		t.Fatalf("alice transfers = %+v", aliceEvents)
	}
	if aliceEvents[0].Success || aliceEvents[0].Files != 3 || aliceEvents[0].Detail != "disk full" {
		t.Fatalf("second event = %+v", aliceEvents[0])
	}
	if bobEvents, err := store.FileTransfersFor("BOB", 10); err != nil || len(bobEvents) != 1 ||
		bobEvents[0].Username != "bob" || bobEvents[0].Name != "" {
		t.Fatalf("bob filter = %+v, %v", bobEvents, err)
	}
	all, err := store.FileTransfersFor("", 10)
	if err != nil || len(all) != 3 {
		t.Fatalf("all transfers = %+v, %v", all, err)
	}
	limited, err := store.FileTransfersFor("", 2)
	if err != nil || len(limited) != 2 || limited[0].ID != all[0].ID {
		t.Fatalf("limited transfers = %+v, %v", limited, err)
	}

	for _, tc := range []struct {
		name      string
		userID    string
		peer      string
		direction string
		size      int64
		files     int
	}{
		{"empty user", "", "devbox", "upload", 1, 1},
		{"empty peer", alice.ID, "  \x00 ", "upload", 1, 1},
		{"unknown direction", alice.ID, "devbox", "viewer", 1, 1},
		{"negative size", alice.ID, "devbox", "upload", -1, 1},
		{"no files", alice.ID, "devbox", "upload", 1, 0},
		{"too many files", alice.ID, "devbox", "upload", 1, 2000},
	} {
		if _, err := store.RecordFileTransfer(
			tc.userID, "alice", tc.peer, tc.direction, "x", "", "", "", "", tc.size, tc.files,
			occurred, true, "",
		); !errors.Is(err, ErrInvalidTransfer) {
			t.Fatalf("%s: error = %v", tc.name, err)
		}
	}

	// A missing timestamp uses the store clock instead of 1970.
	now, err := store.RecordFileTransfer(alice.ID, alice.Username, "instant", "download", "a", "", "", "", "", 1, 1, time.Time{}, true, "")
	if err != nil {
		t.Fatalf("record without timestamp: %v", err)
	}
	if now.OccurredAt.IsZero() || now.OccurredAt.Year() < 2020 {
		t.Fatalf("fallback timestamp = %+v", now)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen account store: %v", err)
	}
	defer reopened.Close()
	persisted, err := reopened.FileTransfers(alice.ID, 10)
	if err != nil {
		t.Fatalf("list after reopen: %v", err)
	}
	if len(persisted) != 3 || persisted[0].PeerID != "instant" {
		t.Fatalf("transfers after reopen = %+v", persisted)
	}
}
