package account

import (
	"errors"
	"testing"
	"time"
)

func TestAuditEventsForFilterBeforeCombinesGroupUserHostAndRange(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "carol password")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := store.AddGroupMember("lab", alice.Username, RoleReadonly); err != nil {
		t.Fatalf("add alice: %v", err)
	}
	if _, err := store.AddGroupMember("lab", bob.Username, RoleAdmin); err != nil {
		t.Fatalf("add bob: %v", err)
	}

	now := time.Date(2026, 11, 14, 10, 0, 0, 0, time.UTC)
	for _, event := range []AuditEvent{
		{Username: "alice", Action: "probe", HostID: "alpha", Detail: "old", CreatedAt: now.Add(-2 * time.Minute)},
		{Username: "alice", Action: "probe", HostID: "alpha", Detail: "new", CreatedAt: now.Add(-30 * time.Second)},
		{Username: "alice", Action: "other", HostID: "alpha", Detail: "other action", CreatedAt: now.Add(-30 * time.Second)},
		{Username: "alice", Action: "probe", HostID: "beta", Detail: "other host", CreatedAt: now.Add(-30 * time.Second)},
		{Username: "bob", Action: "probe", HostID: "ALPHA", Detail: "bob", CreatedAt: now.Add(-30 * time.Second)},
		{Username: "carol", Action: "probe", HostID: "alpha", Detail: "outside group", CreatedAt: now.Add(-30 * time.Second)},
	} {
		if err := store.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}

	events, err := store.AuditEventsForFilterBefore("", AuditFilter{
		Action: "probe", Username: "alice", HostID: "ALPHA",
	}, 0, 50)
	if err != nil || len(events) != 2 {
		t.Fatalf("global combined filter = %+v, %v", events, err)
	}
	events, err = store.AuditEventsForFilterBefore("lab", AuditFilter{
		Action: "probe", Username: "alice", HostID: "alpha",
	}, 0, 50)
	if err != nil || len(events) != 2 {
		t.Fatalf("group combined filter = %+v, %v", events, err)
	}
	events, err = store.AuditEventsForFilterBefore("lab", AuditFilter{
		Action: "probe", Username: "alice", HostID: "alpha",
		Bounds: TimeBounds{Since: now.Add(-time.Minute).UnixMilli()},
	}, 0, 50)
	if err != nil || len(events) != 1 || events[0].Detail != "new" {
		t.Fatalf("range combined filter = %+v, %v", events, err)
	}
	events, err = store.AuditEventsForFilterBefore("lab", AuditFilter{HostID: "alpha"}, 0, 50)
	if err != nil || len(events) != 4 {
		t.Fatalf("group host filter = %+v, %v", events, err)
	}
	for _, event := range events {
		if event.Username == carol.Username {
			t.Fatalf("group host filter leaked carol: %+v", event)
		}
	}
	if _, err := store.AuditEventsForFilterBefore("bad id", AuditFilter{}, 0, 50); !errors.Is(err, ErrInvalidAccountGroupID) {
		t.Fatalf("invalid group filter id = %v", err)
	}
}

func TestOperatorEventFiltersCombineUsernameAndHost(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "carol password")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	now := time.Now().UTC()
	for _, item := range []struct {
		user User
		peer string
	}{
		{alice, "alpha"},
		{bob, "alpha"},
		{alice, "beta"},
		{carol, "alpha"},
	} {
		if _, err := store.RecordConnection(item.user.ID, item.user.Username,
			item.peer, "viewer", "127.0.0.1", "", "", "",
			now.Add(-time.Minute), now, true, item.user.Username+"-"+item.peer); err != nil {
			t.Fatalf("record connection: %v", err)
		}
		if _, err := store.RecordFileTransfer(item.user.ID, item.user.Username,
			item.peer, "upload", item.user.Username+"-"+item.peer+".txt",
			"127.0.0.1", "", "", "", 1, 1, now, true, ""); err != nil {
			t.Fatalf("record transfer: %v", err)
		}
	}

	connections, err := store.ConnectionsForOperatorBeforeRange("ALICE", "ALPHA", TimeBounds{}, 0, 50)
	if err != nil || len(connections) != 1 || connections[0].Detail != "alice-alpha" {
		t.Fatalf("combined connection filter = %+v, %v", connections, err)
	}
	connections, err = store.ConnectionsForOperatorBeforeRange("", "alpha", TimeBounds{}, 0, 50)
	if err != nil || len(connections) != 3 {
		t.Fatalf("host connection filter = %+v, %v", connections, err)
	}

	transfers, err := store.FileTransfersForOperatorBeforeRange("ALICE", "ALPHA", TimeBounds{}, 0, 50)
	if err != nil || len(transfers) != 1 || transfers[0].Name != "alice-alpha.txt" {
		t.Fatalf("combined transfer filter = %+v, %v", transfers, err)
	}
	transfers, err = store.FileTransfersForOperatorBeforeRange("", "alpha", TimeBounds{}, 0, 50)
	if err != nil || len(transfers) != 3 {
		t.Fatalf("host transfer filter = %+v, %v", transfers, err)
	}
}
