package account

import (
	"testing"
	"time"
)

func TestEventPaginationAndRetention(t *testing.T) {
	store := openTemp(t, "history.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	now := time.Date(2026, 10, 27, 12, 0, 0, 0, time.UTC)
	for _, detail := range []string{"one", "two", "three", "four", "five"} {
		if err := store.AppendAudit(AuditEvent{
			Username: "alice", Action: AuditLogin, Detail: detail, CreatedAt: now, Success: true,
		}); err != nil {
			t.Fatalf("append %s: %v", detail, err)
		}
	}
	page, err := store.AuditEventsForBefore("", 0, 2)
	if err != nil || len(page) != 2 || page[0].Detail != "five" || page[1].Detail != "four" {
		t.Fatalf("first page = %+v, %v", page, err)
	}
	page, err = store.AuditEventsForBefore("", page[len(page)-1].ID, 2)
	if err != nil || len(page) != 2 || page[0].Detail != "three" || page[1].Detail != "two" {
		t.Fatalf("second page = %+v, %v", page, err)
	}
	page, err = store.AuditEventsForBefore("", page[len(page)-1].ID, 2)
	if err != nil || len(page) != 1 || page[0].Detail != "one" {
		t.Fatalf("third page = %+v, %v", page, err)
	}
	if page, err = store.AuditEventsForBefore(AuditLogout, 0, 5); err != nil || len(page) != 0 {
		t.Fatalf("action filtered page = %+v, %v", page, err)
	}

	old := now.Add(-72 * time.Hour)
	fresh := now
	for i := 0; i < 2; i++ {
		if _, err := store.RecordConnection(alice.ID, alice.Username, "devbox", "viewer",
			"127.0.0.1", "", old, old.Add(time.Minute), true, "old"); err != nil {
			t.Fatalf("record old connection: %v", err)
		}
		if _, err := store.RecordConnection(alice.ID, alice.Username, "devbox", "viewer",
			"127.0.0.1", "", fresh, fresh.Add(time.Minute), true, "new"); err != nil {
			t.Fatalf("record new connection: %v", err)
		}
		if _, err := store.RecordFileTransfer(alice.ID, alice.Username, "devbox", "upload",
			"old.pdf", "127.0.0.1", "", 1, 1, old, true, ""); err != nil {
			t.Fatalf("record old transfer: %v", err)
		}
		if _, err := store.RecordFileTransfer(alice.ID, alice.Username, "devbox", "upload",
			"new.pdf", "127.0.0.1", "", 1, 1, fresh, true, ""); err != nil {
			t.Fatalf("record new transfer: %v", err)
		}
	}
	if err := store.AppendAudit(AuditEvent{
		Username: "alice", Action: AuditLogin, Detail: "old", CreatedAt: old, Success: true,
	}); err != nil {
		t.Fatalf("append old audit: %v", err)
	}

	connections, err := store.ConnectionsBefore(alice.ID, 0, 2)
	if err != nil || len(connections) != 2 {
		t.Fatalf("connection first page = %+v, %v", connections, err)
	}
	connections, err = store.ConnectionsForHostBefore("devbox", connections[1].ID, 5)
	if err != nil || len(connections) != 2 || connections[1].Detail != "old" {
		t.Fatalf("connection second page = %+v, %v", connections, err)
	}
	transfers, err := store.FileTransfersBefore(alice.ID, 0, 2)
	if err != nil || len(transfers) != 2 {
		t.Fatalf("transfer first page = %+v, %v", transfers, err)
	}
	if transfers, err = store.FileTransfersForHostBefore("devbox", transfers[1].ID, 5); err != nil ||
		len(transfers) != 2 || transfers[1].Name != "old.pdf" {
		t.Fatalf("transfer second page = %+v, %v", transfers, err)
	}

	if _, err := store.PruneBefore(time.Time{}); err == nil {
		t.Fatal("zero cutoff was accepted")
	}
	report, err := store.PruneBefore(now.Add(-24 * time.Hour))
	if err != nil {
		t.Fatalf("prune: %v", err)
	}
	if report.AuditEvents != 1 || report.Connections != 2 || report.FileTransfers != 2 {
		t.Fatalf("prune report = %+v", report)
	}
	events, err := store.AuditEventsFor("", 50)
	if err != nil || len(events) != 5 {
		t.Fatalf("audit after prune = %d, %v", len(events), err)
	}
	if connections, err = store.Connections(alice.ID, 50); err != nil || len(connections) != 2 {
		t.Fatalf("connections after prune = %+v, %v", connections, err)
	}
	for _, connection := range connections {
		if connection.Detail != "new" {
			t.Fatalf("pruned connection survived: %+v", connection)
		}
	}
	if transfers, err = store.FileTransfers(alice.ID, 50); err != nil || len(transfers) != 2 {
		t.Fatalf("transfers after prune = %+v, %v", transfers, err)
	}
	for _, transfer := range transfers {
		if transfer.Name != "new.pdf" {
			t.Fatalf("pruned transfer survived: %+v", transfer)
		}
	}
}

func TestPruneAddressBookTombstones(t *testing.T) {
	store := openTemp(t, "tombstones.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	old := time.Date(2026, 9, 1, 12, 0, 0, 0, time.UTC)
	now := time.Date(2026, 10, 27, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return old })
	oldName := "old peer"
	if _, err := store.UpdatePeer(alice.ID, "peer-old", &oldName, nil); err != nil {
		t.Fatalf("add old peer: %v", err)
	}
	if _, err := store.DeletePeer(alice.ID, "peer-old"); err != nil {
		t.Fatalf("delete old peer: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "group-old", "Old"); err != nil {
		t.Fatalf("add old group: %v", err)
	}
	if _, err := store.DeleteGroup(alice.ID, "group-old"); err != nil {
		t.Fatalf("delete old group: %v", err)
	}

	store.SetClock(func() time.Time { return now })
	freshName := "fresh peer"
	if _, err := store.UpdatePeer(alice.ID, "peer-fresh", &freshName, nil); err != nil {
		t.Fatalf("add fresh peer: %v", err)
	}
	if _, err := store.DeletePeer(alice.ID, "peer-fresh"); err != nil {
		t.Fatalf("delete fresh peer: %v", err)
	}
	if _, err := store.UpsertGroup(alice.ID, "group-fresh", "Fresh"); err != nil {
		t.Fatalf("add fresh group: %v", err)
	}
	if _, err := store.DeleteGroup(alice.ID, "group-fresh"); err != nil {
		t.Fatalf("delete fresh group: %v", err)
	}
	if _, err := store.UpdatePeer(alice.ID, "peer-live", &freshName, nil); err != nil {
		t.Fatalf("add live peer: %v", err)
	}

	if _, err := store.PruneTombstones(time.Time{}); err == nil {
		t.Fatal("zero cutoff was accepted")
	}
	report, err := store.PruneTombstones(now.Add(-24 * time.Hour))
	if err != nil {
		t.Fatalf("prune tombstones: %v", err)
	}
	if report.Peers != 1 || report.Groups != 1 {
		t.Fatalf("tombstone report = %+v", report)
	}
	peers, err := store.Peers(alice.ID)
	if err != nil || len(peers) != 1 || peers[0].ID != "peer-live" {
		t.Fatalf("live peers after prune = %+v, %v", peers, err)
	}
	snapshot, err := store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	deleted := map[string]bool{}
	for _, peer := range snapshot.Changes {
		if peer.Deleted {
			deleted[peer.ID] = true
		}
	}
	for _, group := range snapshot.GroupChanges {
		if group.Deleted {
			deleted[group.ID] = true
		}
	}
	if deleted["peer-old"] || deleted["group-old"] {
		t.Fatalf("expired tombstones survived: %+v", deleted)
	}
	if !deleted["peer-fresh"] || !deleted["group-fresh"] {
		t.Fatalf("recent tombstones were pruned: %+v", deleted)
	}

	if report, err = store.PruneTombstones(now.Add(time.Hour)); err != nil ||
		report.Peers != 1 || report.Groups != 1 {
		t.Fatalf("second prune = %+v, %v", report, err)
	}
	snapshot, err = store.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot after second prune: %v", err)
	}
	for _, peer := range snapshot.Changes {
		if peer.Deleted {
			t.Fatalf("tombstone survived the second prune: %+v", peer)
		}
	}
	for _, group := range snapshot.GroupChanges {
		if group.Deleted {
			t.Fatalf("group tombstone survived the second prune: %+v", group)
		}
	}
	if peers, err = store.Peers(alice.ID); err != nil || len(peers) != 1 {
		t.Fatalf("live peers after second prune = %+v, %v", peers, err)
	}
}
