package account

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func openGroupStore(t *testing.T) *Store {
	t.Helper()
	store, err := Open(filepath.Join(t.TempDir(), "groups.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	return store
}

// TestAccountGroupRolesAndQuotas covers the membership ladder, both quotas and
// the guarantee that a group never loses its last admin through a routine
// member edit.
func TestAccountGroupRolesAndQuotas(t *testing.T) {
	store := openGroupStore(t)
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "carol password")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	dave, err := store.CreateUser("dave", "dave password")
	if err != nil {
		t.Fatalf("create dave: %v", err)
	}

	if _, err := store.CreateGroup("bad id", "Lab", "", 0, 0); !errors.Is(err, ErrInvalidAccountGroupID) {
		t.Fatalf("invalid group id = %v", err)
	}
	if _, err := store.CreateGroup("lab", "  ", "", 0, 0); !errors.Is(err, ErrInvalidAccountGroupName) {
		t.Fatalf("empty group name = %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", -1, 0); !errors.Is(err, ErrInvalidGroupQuota) {
		t.Fatalf("negative quota = %v", err)
	}
	group, err := store.CreateGroup("lab", "Lab", "test bench", 2, 1)
	if err != nil {
		t.Fatalf("create group: %v", err)
	}
	if group.MemberQuota != 2 || group.DeviceQuota != 1 || group.MemberCount != 0 {
		t.Fatalf("created group = %+v", group)
	}
	if _, err := store.CreateGroup("lab", "Again", "", 0, 0); !errors.Is(err, ErrAccountGroupExists) {
		t.Fatalf("duplicate group = %v", err)
	}

	if _, err := store.AddGroupMember("lab", "bob", RoleReadonly); err != nil {
		t.Fatalf("add bob: %v", err)
	}
	if _, err := store.AddGroupMember("lab", "carol", RoleOperator); err != nil {
		t.Fatalf("add carol: %v", err)
	}
	if _, err := store.AddGroupMember("lab", "bob", RoleAdmin); !errors.Is(err, ErrGroupMemberExists) {
		t.Fatalf("duplicate member = %v", err)
	}
	if _, err := store.AddGroupMember("lab", "ghost", RoleReadonly); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("unknown member = %v", err)
	}
	if _, err := store.AddGroupMember("lab", "dave", "superuser"); !errors.Is(err, ErrInvalidGroupRole) {
		t.Fatalf("invalid role = %v", err)
	}
	_, err = store.AddGroupMember("lab", "dave", RoleReadonly)
	if !errors.Is(err, ErrGroupMemberQuota) || !strings.Contains(err.Error(), "limit 2") {
		t.Fatalf("member quota error = %v", err)
	}
	if role, member, err := store.GroupRole("lab", bob.ID); err != nil || !member || role != RoleReadonly {
		t.Fatalf("bob role = %q %v %v", role, member, err)
	}
	if _, member, err := store.GroupRole("lab", dave.ID); err != nil || member {
		t.Fatalf("dave membership = %v %v", member, err)
	}

	if _, err := store.AddGroupDevice("lab", "devbox", "alice"); err != nil {
		t.Fatalf("add device: %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "devbox", "alice"); !errors.Is(err, ErrGroupDeviceExists) {
		t.Fatalf("duplicate device = %v", err)
	}
	_, err = store.AddGroupDevice("lab", "devbox2", "alice")
	if !errors.Is(err, ErrGroupDeviceQuota) || !strings.Contains(err.Error(), "limit 1") {
		t.Fatalf("device quota error = %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "bad id", "alice"); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("invalid device id = %v", err)
	}

	if _, err := store.UpdateGroupMember("lab", "bob", RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if _, err := store.UpdateGroupMember("lab", "bob", RoleOperator); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("demote last admin = %v", err)
	}
	if err := store.RemoveGroupMember("lab", "bob"); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("remove last admin = %v", err)
	}
	if _, err := store.UpdateGroupMember("lab", "carol", RoleAdmin); err != nil {
		t.Fatalf("promote carol: %v", err)
	}
	if err := store.RemoveGroupMember("lab", "bob"); err != nil {
		t.Fatalf("remove second admin: %v", err)
	}
	if err := store.RemoveGroupMember("lab", "carol"); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("remove remaining admin = %v", err)
	}
	if err := store.RemoveGroupMember("lab", "bob"); !errors.Is(err, ErrUnknownGroupMember) {
		t.Fatalf("remove absent member = %v", err)
	}

	reduced := 0
	if _, err := store.UpdateAccountGroup("lab", nil, nil, &reduced, nil); err != nil {
		t.Fatalf("clear member quota: %v", err)
	}
	if _, err := store.AddGroupMember("lab", "dave", RoleReadonly); err != nil {
		t.Fatalf("add dave: %v", err)
	}
	one := 1
	_, err = store.UpdateAccountGroup("lab", nil, nil, &one, nil)
	if !errors.Is(err, ErrGroupQuotaTooSmall) {
		t.Fatalf("quota below usage = %v", err)
	}

	access, err := store.GroupsForUser(carol.ID)
	if err != nil || len(access) != 1 || access[0].ID != "lab" || access[0].Role != RoleAdmin {
		t.Fatalf("carol groups = %+v, %v", access, err)
	}
	group, err = store.AccountGroupByID("lab")
	if err != nil || group.MemberCount != 2 || group.DeviceCount != 1 {
		t.Fatalf("group counts = %+v, %v", group, err)
	}
	if err := store.DeleteAccountGroup("lab"); err != nil {
		t.Fatalf("delete group: %v", err)
	}
	if _, err := store.AccountGroupByID("lab"); !errors.Is(err, ErrUnknownAccountGroup) {
		t.Fatalf("group survived delete: %v", err)
	}
	if _, member, err := store.GroupRole("lab", carol.ID); err != nil || member {
		t.Fatalf("membership survived delete: %v %v", member, err)
	}
	if devices, err := store.GroupDevices("lab"); err != nil || len(devices) != 0 {
		t.Fatalf("devices survived delete: %+v, %v", devices, err)
	}
}

// TestAccountGroupEventFilters proves that the group-scoped history joins by
// member accounts and by group devices, and that unrelated accounts and hosts
// stay outside the window.
func TestAccountGroupEventFilters(t *testing.T) {
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
	if _, err := store.AddGroupMember("lab", "bob", RoleAdmin); err != nil {
		t.Fatalf("add bob: %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "devbox", "alice"); err != nil {
		t.Fatalf("add device: %v", err)
	}
	now := time.Now().UTC()
	for _, tc := range []struct{ user User }{{alice}, {bob}, {carol}} {
		if _, err := store.RecordConnection(tc.user.ID, tc.user.Username,
			"peer-"+tc.user.Username, "viewer", "127.0.0.1", "", "", "",
			now.Add(-time.Minute), now, true, "report"); err != nil {
			t.Fatalf("record connection for %s: %v", tc.user.Username, err)
		}
	}
	if _, err := store.RecordHostConnection("devbox", "", now.Add(-time.Minute), now, true, ""); err != nil {
		t.Fatalf("record host connection: %v", err)
	}
	for _, user := range []User{alice, bob, carol} {
		if err := store.AppendAudit(AuditEvent{Username: user.Username,
			Action: "probe_" + user.Username, Detail: "x"}); err != nil {
			t.Fatalf("append audit for %s: %v", user.Username, err)
		}
	}
	for _, user := range []User{bob, carol} {
		if _, err := store.RecordFileTransfer(user.ID, user.Username, "peer",
			"upload", "a.txt", "127.0.0.1", "", "", "", 10, 1, now, true, ""); err != nil {
			t.Fatalf("record transfer for %s: %v", user.Username, err)
		}
	}

	connections, err := store.ConnectionsForGroupBefore("lab", "", "", 0, 50)
	if err != nil {
		t.Fatalf("group connections: %v", err)
	}
	usernames := make(map[string]bool)
	for _, event := range connections {
		usernames[event.Username] = true
	}
	if !usernames["bob"] || !usernames["-"] || usernames["alice"] || usernames["carol"] {
		t.Fatalf("group connections = %+v", connections)
	}
	filtered, err := store.ConnectionsForGroupBefore("lab", "bob", "devbox", 0, 50)
	if err != nil || len(filtered) != 0 {
		t.Fatalf("combined group filters = %+v, %v", filtered, err)
	}
	filtered, err = store.ConnectionsForGroupBefore("lab", "bob", "", 0, 50)
	if err != nil || len(filtered) != 1 || filtered[0].Username != "bob" {
		t.Fatalf("group username filter = %+v, %v", filtered, err)
	}

	transfers, err := store.FileTransfersForGroupBefore("lab", "", "", 0, 50)
	if err != nil || len(transfers) != 1 || transfers[0].Username != "bob" {
		t.Fatalf("group transfers = %+v, %v", transfers, err)
	}

	events, err := store.AuditEventsForGroupBefore("lab", "", 0, 50)
	if err != nil {
		t.Fatalf("group audit: %v", err)
	}
	if len(events) != 1 || events[0].Username != "bob" {
		t.Fatalf("group audit = %+v", events)
	}
	events, err = store.AuditEventsForGroupBefore("lab", "probe_bob", 0, 50)
	if err != nil || len(events) != 1 {
		t.Fatalf("group action filter = %+v, %v", events, err)
	}

	if _, err := store.ConnectionsForGroupBefore("bad id", "", "", 0, 50); !errors.Is(err, ErrInvalidAccountGroupID) {
		t.Fatalf("invalid group filter id = %v", err)
	}
}
