package account

import (
	"errors"
	"path/filepath"
	"testing"
)

// TestDevicePolicyUnmanagedDefault proves that a device without a policy row
// keeps the v61 local-authentication behavior instead of silently becoming
// restricted when the schema upgrade runs.
func TestDevicePolicyUnmanagedDefault(t *testing.T) {
	store := openGroupStore(t)
	if _, managed, err := store.DevicePolicy("devbox"); err != nil || managed {
		t.Fatalf("unmanaged policy = %v %v", managed, err)
	}
	admission, err := store.CheckDeviceAdmission("devbox", "")
	if err != nil {
		t.Fatalf("unmanaged admission: %v", err)
	}
	if admission.Managed || !admission.Allowed || admission.Reason != "unmanaged" {
		t.Fatalf("unmanaged admission = %+v", admission)
	}
	if policies, err := store.DevicePolicies(); err != nil || len(policies) != 0 {
		t.Fatalf("unmanaged list = %+v %v", policies, err)
	}
}

func TestDevicePolicyValidation(t *testing.T) {
	store := openGroupStore(t)
	if _, err := store.SetDevicePolicy("bad id", DevicePolicyOpen, 0, nil, nil, "root"); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("invalid peer = %v", err)
	}
	if _, err := store.SetDevicePolicy("devbox", "closed", 0, nil, nil, "root"); !errors.Is(err, ErrInvalidDevicePolicyMode) {
		t.Fatalf("invalid mode = %v", err)
	}
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 0, []string{"ghost"}, nil, "root"); !errors.Is(err, ErrDevicePolicyUser) {
		t.Fatalf("unknown user = %v", err)
	}
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 0, nil, []string{"missing"}, "root"); !errors.Is(err, ErrDevicePolicyGroup) {
		t.Fatalf("unknown group = %v", err)
	}
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 0, []string{" "}, nil, "root"); !errors.Is(err, ErrDevicePolicyUser) {
		t.Fatalf("blank user = %v", err)
	}
	many := make([]string, maxDevicePolicyUsers+1)
	for i := range many {
		many[i] = "alice"
	}
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyOpen, 0, many, nil, "root"); !errors.Is(err, ErrDevicePolicyTooMany) {
		t.Fatalf("oversized list = %v", err)
	}
	if _, _, err := store.DevicePolicy("bad id"); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatalf("read invalid peer = %v", err)
	}
	if err := store.DeleteDevicePolicy("devbox"); !errors.Is(err, ErrUnknownDevicePolicy) {
		t.Fatalf("delete unmanaged = %v", err)
	}
}

// TestDevicePolicyAdmissionAndReceipts covers the open/restricted ladder, the
// direct-user and group allow lists, version increments and stale receipts.
func TestDevicePolicyAdmissionAndReceipts(t *testing.T) {
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
	dave, err := store.CreateUser("dave", "dave password")
	if err != nil {
		t.Fatalf("create dave: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	if _, err := store.AddGroupMember("lab", "carol", RoleReadonly); err != nil {
		t.Fatalf("add carol: %v", err)
	}

	open, err := store.SetDevicePolicy("devbox", DevicePolicyOpen, 0, nil, nil, "root")
	if err != nil {
		t.Fatalf("open policy: %v", err)
	}
	if open.PolicyVersion != 1 || open.Mode != DevicePolicyOpen || len(open.AllowedUsers) != 0 {
		t.Fatalf("open policy = %+v", open)
	}
	admission, err := store.CheckDeviceAdmission("devbox", "")
	if err != nil {
		t.Fatalf("open admission: %v", err)
	}
	if !admission.Managed || !admission.Allowed || admission.PolicyVersion != 1 {
		t.Fatalf("open admission = %+v", admission)
	}

	restricted, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 3,
		[]string{"alice", "alice", "dave"}, []string{"lab"}, "root")
	if err != nil {
		t.Fatalf("restricted policy: %v", err)
	}
	if restricted.PolicyVersion != 2 {
		t.Fatalf("version after edit = %d, want 2", restricted.PolicyVersion)
	}
	if len(restricted.AllowedUsers) != 2 || restricted.AllowedUsers[0] != "alice" ||
		restricted.AllowedUsers[1] != "dave" {
		t.Fatalf("deduplicated users = %v", restricted.AllowedUsers)
	}
	if len(restricted.AllowedGroups) != 1 || restricted.AllowedGroups[0] != "lab" {
		t.Fatalf("groups = %v", restricted.AllowedGroups)
	}
	if restricted.PermissionCeiling != 3 {
		t.Fatalf("ceiling = %d", restricted.PermissionCeiling)
	}

	cases := []struct {
		name   string
		userID string
		allow  bool
		reason string
	}{
		{"direct user", alice.ID, true, "user allowed"},
		{"group member", carol.ID, true, "group allowed"},
		{"unlisted user", bob.ID, false, "not allowed by device policy"},
		{"empty subject", "", false, "no account subject"},
		{"unknown subject", "no-such-user", false, "no account subject"},
	}
	for _, tc := range cases {
		admission, err := store.CheckDeviceAdmission("devbox", tc.userID)
		if err != nil {
			t.Fatalf("%s admission: %v", tc.name, err)
		}
		if admission.Allowed != tc.allow || admission.Reason != tc.reason ||
			admission.PolicyVersion != 2 || admission.PermissionCeiling != 3 {
			t.Fatalf("%s admission = %+v", tc.name, admission)
		}
	}

	if _, err := store.SetBanned("dave", true); err != nil {
		t.Fatalf("ban dave: %v", err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", dave.ID)
	if err != nil {
		t.Fatalf("banned admission: %v", err)
	}
	if admission.Allowed || admission.Reason != "account disabled" {
		t.Fatalf("banned admission = %+v", admission)
	}
	if _, err := store.SetBanned("dave", false); err != nil {
		t.Fatalf("unban dave: %v", err)
	}

	if _, err := store.RecordDevicePolicyReceipt("devbox", 1, true, "stale"); !errors.Is(err, ErrStaleDevicePolicyVersion) {
		t.Fatalf("stale receipt = %v", err)
	}
	if _, err := store.RecordDevicePolicyReceipt("unmanaged", 1, true, ""); !errors.Is(err, ErrUnknownDevicePolicy) {
		t.Fatalf("receipt for unmanaged = %v", err)
	}
	receipt, err := store.RecordDevicePolicyReceipt("devbox", 2, false, "apply failed: card busy")
	if err != nil {
		t.Fatalf("receipt: %v", err)
	}
	if receipt.PolicyVersion != 2 || receipt.Applied || receipt.Detail == "" {
		t.Fatalf("receipt = %+v", receipt)
	}
	policy, managed, err := store.DevicePolicy("devbox")
	if err != nil || !managed {
		t.Fatalf("read policy: %v %v", managed, err)
	}
	if policy.Receipt == nil || policy.Receipt.Applied || policy.Receipt.PolicyVersion != 2 {
		t.Fatalf("stored receipt = %+v", policy.Receipt)
	}
	// The receipt survives an unrelated edit but is reported for its old
	// version until the host acknowledges the new one.
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyOpen, 3, nil, nil, "root"); err != nil {
		t.Fatalf("second edit: %v", err)
	}
	policy, _, err = store.DevicePolicy("devbox")
	if err != nil {
		t.Fatalf("read policy after edit: %v", err)
	}
	if policy.PolicyVersion != 3 || policy.Receipt == nil || policy.Receipt.PolicyVersion != 2 {
		t.Fatalf("policy after edit = %+v receipt %+v", policy, policy.Receipt)
	}

	if err := store.DeleteDevicePolicy("devbox"); err != nil {
		t.Fatalf("delete policy: %v", err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", bob.ID)
	if err != nil {
		t.Fatalf("admission after delete: %v", err)
	}
	if admission.Managed || !admission.Allowed {
		t.Fatalf("admission after delete = %+v", admission)
	}
	if err := store.DeleteDevicePolicy("devbox"); !errors.Is(err, ErrUnknownDevicePolicy) {
		t.Fatalf("second delete = %v", err)
	}
}

// TestDevicePolicyPersistsAcrossReopen checks that the policy, its allow lists
// and the receipt all live in the durable store rather than in memory.
func TestDevicePolicyPersistsAcrossReopen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "policy.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		store.Close()
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 7, []string{"alice"}, nil, "root"); err != nil {
		store.Close()
		t.Fatalf("set policy: %v", err)
	}
	if _, err := store.RecordDevicePolicyReceipt("devbox", 1, true, ""); err != nil {
		store.Close()
		t.Fatalf("receipt: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}

	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	defer reopened.Close()
	policy, managed, err := reopened.DevicePolicy("devbox")
	if err != nil || !managed {
		t.Fatalf("policy after reopen = %v %v", managed, err)
	}
	if policy.Mode != DevicePolicyRestricted || policy.PermissionCeiling != 7 ||
		len(policy.AllowedUsers) != 1 || policy.AllowedUsers[0] != "alice" {
		t.Fatalf("policy after reopen = %+v", policy)
	}
	if policy.Receipt == nil || !policy.Receipt.Applied || policy.Receipt.PolicyVersion != 1 {
		t.Fatalf("receipt after reopen = %+v", policy.Receipt)
	}
	if policies, err := reopened.DevicePolicies(); err != nil || len(policies) != 1 || policies[0].PeerID != "devbox" {
		t.Fatalf("list after reopen = %+v %v", policies, err)
	}
}

func TestDevicePolicySchemaTables(t *testing.T) {
	store := openGroupStore(t)
	for _, table := range []string{
		"device_policies",
		"device_policy_users",
		"device_policy_groups",
		"device_policy_receipts",
	} {
		var found int
		if err := store.db.QueryRow(
			`SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?`, table,
		).Scan(&found); err != nil || found == 0 {
			t.Fatalf("table %s missing: %v", table, err)
		}
	}
	var version int
	if err := store.db.QueryRow(`SELECT MAX(version) FROM schema_migrations`).Scan(&version); err != nil {
		t.Fatalf("read schema version: %v", err)
	}
	if version != accountSchemaVersion || version < 10 {
		t.Fatalf("schema version = %d, want %d", version, accountSchemaVersion)
	}
}
