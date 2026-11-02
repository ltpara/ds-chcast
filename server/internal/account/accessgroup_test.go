package account

import (
	"errors"
	"path/filepath"
	"testing"
	"time"
)

func newAccessGroupStore(t *testing.T) (*Store, User, User, User, User) {
	t.Helper()
	store, err := Open(filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	carol, err := store.CreateUser("carol", "silent river")
	if err != nil {
		t.Fatalf("create carol: %v", err)
	}
	dave, err := store.CreateUser("dave", "quiet meadow")
	if err != nil {
		t.Fatalf("create dave: %v", err)
	}
	return store, alice, bob, carol, dave
}

func TestAccessGroupRolesMembersAndDeviceQuotas(t *testing.T) {
	store, alice, bob, carol, dave := newAccessGroupStore(t)

	group, err := store.CreateAccessGroup("ops", "core hosts", 2, 3, alice.ID)
	if err != nil {
		t.Fatalf("create group: %v", err)
	}
	if group.MemberCount != 1 || group.DeviceCount != 0 || group.MaxDevices != 2 {
		t.Fatalf("created group = %+v", group)
	}
	if role, ok, err := store.AccessGroupRole(group.ID, alice.ID); err != nil || !ok || role != GroupRoleAdmin {
		t.Fatalf("creator role = %q %v %v", role, ok, err)
	}
	if role, ok, err := store.AccessGroupRole(group.ID, bob.ID); err != nil || ok || role != "" {
		t.Fatalf("non-member role = %q %v %v", role, ok, err)
	}

	if _, err := store.SetAccessGroupMember(group.ID, bob.Username, GroupRoleWrite); err != nil {
		t.Fatalf("add writer: %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, carol.Username, GroupRoleReadonly); err != nil {
		t.Fatalf("add reader: %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, dave.Username, GroupRoleReadonly); !errors.Is(err, ErrAccessGroupMemberQuota) {
		t.Fatalf("member over quota = %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, dave.Username, "superuser"); !errors.Is(err, ErrInvalidGroupRole) {
		t.Fatalf("invalid role = %v", err)
	}
	members, err := store.ListAccessGroupMembers(group.ID)
	if err != nil || len(members) != 3 {
		t.Fatalf("members = %+v %v", members, err)
	}
	groups, err := store.AccessGroupsForUser(bob.ID)
	if err != nil || len(groups) != 1 || groups[0].Role != GroupRoleWrite || groups[0].MemberCount != 3 {
		t.Fatalf("bob groups = %+v %v", groups, err)
	}

	if _, added, err := store.AddAccessGroupDevice(group.ID, "host-a"); err != nil || !added {
		t.Fatalf("add host-a: %v %v", added, err)
	}
	if _, added, err := store.AddAccessGroupDevice(group.ID, "host-b"); err != nil || !added {
		t.Fatalf("add host-b: %v %v", added, err)
	}
	if _, _, err := store.AddAccessGroupDevice(group.ID, "host-c"); !errors.Is(err, ErrAccessGroupDeviceQuota) {
		t.Fatalf("device over quota = %v", err)
	}
	if _, added, err := store.AddAccessGroupDevice(group.ID, "host-a"); err != nil || added {
		t.Fatalf("re-add host-a = %v %v", added, err)
	}
	if _, _, err := store.AddAccessGroupDevice(group.ID, "bad id!"); err == nil {
		t.Fatal("invalid device id was accepted")
	}

	lower := 1
	if _, err := store.UpdateAccessGroup(group.ID, AccessGroupUpdate{MaxDevices: &lower}); !errors.Is(err, ErrAccessGroupQuotaBelow) {
		t.Fatalf("quota below usage = %v", err)
	}
	three := 3
	if group, err = store.UpdateAccessGroup(group.ID, AccessGroupUpdate{MaxDevices: &three}); err != nil || group.MaxDevices != 3 {
		t.Fatalf("raise quota = %+v %v", group, err)
	}
	if _, added, err := store.AddAccessGroupDevice(group.ID, "host-c"); err != nil || !added {
		t.Fatalf("add host-c after raising quota: %v %v", added, err)
	}
	if err := store.RemoveAccessGroupDevice(group.ID, "host-b"); err != nil {
		t.Fatalf("remove host-b: %v", err)
	}
	if err := store.RemoveAccessGroupDevice(group.ID, "host-b"); err == nil {
		t.Fatal("removing an unassigned device succeeded")
	}
	devices, err := store.ListAccessGroupDevices(group.ID)
	if err != nil || len(devices) != 2 {
		t.Fatalf("devices = %+v %v", devices, err)
	}

	other, err := store.CreateAccessGroup("ops2", "", 0, 0, "")
	if err != nil {
		t.Fatalf("create second group: %v", err)
	}
	name := "ops"
	if _, err := store.UpdateAccessGroup(other.ID, AccessGroupUpdate{Name: &name}); !errors.Is(err, ErrAccessGroupExists) {
		t.Fatalf("duplicate rename = %v", err)
	}
	if _, err := store.CreateAccessGroup("OPS", "", 0, 0, ""); !errors.Is(err, ErrAccessGroupExists) {
		t.Fatalf("case-insensitive duplicate = %v", err)
	}
	if _, err := store.CreateAccessGroup("", "", 0, 0, ""); !errors.Is(err, ErrInvalidAccessGroupName) {
		t.Fatalf("empty name = %v", err)
	}
	negative := -1
	if _, err := store.UpdateAccessGroup(other.ID, AccessGroupUpdate{MaxMembers: &negative}); err == nil {
		t.Fatal("negative quota was accepted")
	}
	if err := store.DeleteAccessGroup(other.ID); err != nil {
		t.Fatalf("delete group: %v", err)
	}
	if _, _, err := store.AccessGroupRole(other.ID, alice.ID); !errors.Is(err, ErrUnknownAccessGroup) {
		t.Fatalf("deleted group lookup = %v", err)
	}
}

func TestAccessGroupProtectsLastAdmin(t *testing.T) {
	store, alice, bob, carol, _ := newAccessGroupStore(t)
	group, err := store.CreateAccessGroup("ops", "", 0, 0, bob.ID)
	if err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, bob.Username, GroupRoleWrite); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("demote last admin = %v", err)
	}
	if err := store.RemoveAccessGroupMember(group.ID, bob.Username); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("remove last admin = %v", err)
	}
	if err := store.DeleteUser(bob.Username); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("delete last group admin = %v", err)
	}
	if _, err := store.SetBanned(bob.Username, true); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("ban last group admin = %v", err)
	}

	if _, err := store.SetAccessGroupMember(group.ID, carol.Username, GroupRoleAdmin); err != nil {
		t.Fatalf("add second admin: %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, bob.Username, GroupRoleWrite); err != nil {
		t.Fatalf("demote with another admin: %v", err)
	}
	if _, err := store.SetBanned(carol.Username, true); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("ban only usable admin = %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, bob.Username, GroupRoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if _, err := store.SetBanned(carol.Username, true); err != nil {
		t.Fatalf("ban carol with bob as admin: %v", err)
	}
	// carol is banned, so bob is again the only usable admin.
	if _, err := store.SetAccessGroupMember(group.ID, bob.Username, GroupRoleWrite); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("demote only usable admin = %v", err)
	}
	// A banned admin can be removed because it was not usable in the first place.
	if err := store.RemoveAccessGroupMember(group.ID, carol.Username); err != nil {
		t.Fatalf("remove banned admin: %v", err)
	}
	if err := store.DeleteUser(bob.Username); !errors.Is(err, ErrLastGroupAdmin) {
		t.Fatalf("delete last usable admin = %v", err)
	}
	if _, err := store.SetAccessGroupMember(group.ID, alice.Username, GroupRoleAdmin); err != nil {
		t.Fatalf("add alice as admin: %v", err)
	}
	if err := store.DeleteUser(bob.Username); err != nil {
		t.Fatalf("delete bob with another admin: %v", err)
	}
}

func TestAccessGroupScopesConnectionAndFileAudit(t *testing.T) {
	store, alice, _, _, _ := newAccessGroupStore(t)
	group, err := store.CreateAccessGroup("ops", "", 0, 0, alice.ID)
	if err != nil {
		t.Fatalf("create group: %v", err)
	}
	for _, id := range []string{"host-a", "host-b"} {
		if _, _, err := store.AddAccessGroupDevice(group.ID, id); err != nil {
			t.Fatalf("add %s: %v", id, err)
		}
	}
	now := time.Now().UTC()
	for _, peer := range []string{"host-a", "host-b", "host-c"} {
		if _, err := store.RecordConnection(alice.ID, alice.Username, peer, "viewer", "127.0.0.1",
			"", "", "", now, now.Add(time.Second), true, ""); err != nil {
			t.Fatalf("record connection %s: %v", peer, err)
		}
		if _, err := store.RecordFileTransfer(alice.ID, alice.Username, peer, "download", "file.bin",
			"127.0.0.1", "", "", "", 10, 1, now, true, ""); err != nil {
			t.Fatalf("record transfer %s: %v", peer, err)
		}
	}
	// Host-side rows carry the host id instead of a peer id.
	if _, err := store.RecordHostConnection("host-b", "abcdef12", now, now.Add(time.Second), true, ""); err != nil {
		t.Fatalf("record host connection: %v", err)
	}
	if _, err := store.RecordHostFileTransfer("host-b", "abcdef12", "upload", "in.bin", 4, now, true, ""); err != nil {
		t.Fatalf("record host transfer: %v", err)
	}

	connections, err := store.ConnectionsForGroupBefore(group.ID, 0, 100)
	if err != nil {
		t.Fatalf("group connections: %v", err)
	}
	if len(connections) != 3 {
		t.Fatalf("group connections = %+v", connections)
	}
	for _, event := range connections {
		if event.PeerID == "host-c" || event.HostID == "host-c" {
			t.Fatalf("host-c leaked into group: %+v", event)
		}
	}
	files, err := store.FileTransfersForGroupBefore(group.ID, 0, 100)
	if err != nil {
		t.Fatalf("group files: %v", err)
	}
	if len(files) != 3 {
		t.Fatalf("group files = %+v", files)
	}
	for _, event := range files {
		if event.PeerID == "host-c" || event.HostID == "host-c" {
			t.Fatalf("host-c leaked into group files: %+v", event)
		}
	}

	if err := store.RemoveAccessGroupDevice(group.ID, "host-a"); err != nil {
		t.Fatalf("remove host-a: %v", err)
	}
	connections, err = store.ConnectionsForGroupBefore(group.ID, 0, 100)
	if err != nil || len(connections) != 2 {
		t.Fatalf("connections after removing host-a = %+v %v", connections, err)
	}
}
