package account

import (
	"strings"
	"testing"
	"time"
)

// TestRecordingGroupScope pins the two ways a recording enters a group scope:
// its owner is a group member, or its peer is a device in the group. The
// unscoped operator view must still include both.
func TestRecordingGroupScope(t *testing.T) {
	store, alice := openTokenStore(t)
	bob, err := store.CreateUser("bob", "correct horse battery")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := store.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatalf("create east: %v", err)
	}
	if _, err := store.AddGroupMember("east", "alice", RoleReadonly); err != nil {
		t.Fatalf("add alice: %v", err)
	}
	if _, err := store.AddGroupDevice("east", "devbox", "root"); err != nil {
		t.Fatalf("add devbox: %v", err)
	}
	now := time.Now().UTC()
	add := func(id, owner, peer string, size int64) {
		t.Helper()
		if _, err := store.AddRecording(Recording{
			ID: id, UserID: owner, Name: "session.mp4", PeerID: peer,
			Size: size, SHA256: strings.Repeat("a", 64), CreatedAtMS: now.UnixMilli(),
		}); err != nil {
			t.Fatalf("add recording %s: %v", id, err)
		}
	}
	aliceOnDevbox := strings.Repeat("1", 32)
	bobOnDevbox := strings.Repeat("2", 32)
	bobOnWestbox := strings.Repeat("3", 32)
	add(aliceOnDevbox, alice.ID, "devbox", 100)
	add(bobOnDevbox, bob.ID, "devbox", 200)
	add(bobOnWestbox, bob.ID, "westbox", 400)

	list, err := store.AllRecordings("", "east", 0, 10)
	if err != nil {
		t.Fatalf("group list: %v", err)
	}
	if len(list) != 2 {
		t.Fatalf("group list = %d rows: %+v", len(list), list)
	}
	for _, rec := range list {
		if rec.ID == bobOnWestbox {
			t.Fatalf("westbox recording leaked into east scope")
		}
	}
	if list[0].Username != "bob" || list[1].Username != "alice" {
		t.Fatalf("group list owners = %q, %q", list[0].Username, list[1].Username)
	}

	if filtered, err := store.AllRecordings("bob", "east", 0, 10); err != nil || len(filtered) != 1 || filtered[0].ID != bobOnDevbox {
		t.Fatalf("group+username list = %+v, %v", filtered, err)
	}
	if all, err := store.AllRecordings("", "", 0, 10); err != nil || len(all) != 3 {
		t.Fatalf("unscoped list = %d rows, %v", len(all), err)
	}

	bytes, files, err := store.RecordingTotals("", "east")
	if err != nil || bytes != 300 || files != 2 {
		t.Fatalf("group totals = %d bytes %d files, %v", bytes, files, err)
	}
	if bytes, files, err = store.RecordingTotals("bob", "east"); err != nil || bytes != 200 || files != 1 {
		t.Fatalf("group+username totals = %d bytes %d files, %v", bytes, files, err)
	}

	for _, tc := range []struct {
		id   string
		want bool
	}{
		{aliceOnDevbox, true},
		{bobOnDevbox, true},
		{bobOnWestbox, false},
		{strings.Repeat("9", 32), false},
	} {
		got, err := store.RecordingInGroup(tc.id, "east")
		if err != nil {
			t.Fatalf("in group %s: %v", tc.id, err)
		}
		if got != tc.want {
			t.Fatalf("in group %s = %v, want %v", tc.id, got, tc.want)
		}
	}
	if got, err := store.RecordingInGroup(bobOnWestbox, ""); err != nil || !got {
		t.Fatalf("unscoped membership = %v, %v", got, err)
	}
}
