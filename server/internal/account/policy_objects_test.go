package account

import (
	"errors"
	"fmt"
	"testing"
)

func mustCreatePolicy(t *testing.T, store *Store, name, mode string, ceiling uint32, users, groups []string) PolicyObject {
	t.Helper()
	policy, err := store.CreatePolicyObject(name, mode, ceiling, users, groups, "root")
	if err != nil {
		t.Fatalf("create policy %s: %v", name, err)
	}
	return policy
}

func mustAssign(t *testing.T, store *Store, policyID, scopeKind, scopeValue string, priority int) PolicyAssignment {
	t.Helper()
	assignment, err := store.CreatePolicyAssignment(policyID, scopeKind, scopeValue, priority, "root")
	if err != nil {
		t.Fatalf("assign %s/%s: %v", scopeKind, scopeValue, err)
	}
	return assignment
}

func TestPolicyObjectLifecycle(t *testing.T) {
	store := openGroupStore(t)
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}

	if _, err := store.CreatePolicyObject(" ", DevicePolicyOpen, 0, nil, nil, "root"); !errors.Is(err, ErrInvalidPolicyObjectName) {
		t.Fatalf("blank name = %v", err)
	}
	if _, err := store.CreatePolicyObject("bad", "closed", 0, nil, nil, "root"); !errors.Is(err, ErrInvalidDevicePolicyMode) {
		t.Fatalf("bad mode = %v", err)
	}
	if _, err := store.CreatePolicyObject("bad", DevicePolicyRestricted, 0, []string{"ghost"}, nil, "root"); !errors.Is(err, ErrDevicePolicyUser) {
		t.Fatalf("unknown user = %v", err)
	}
	if _, err := store.CreatePolicyObject("bad", DevicePolicyRestricted, 0, nil, []string{"missing"}, "root"); !errors.Is(err, ErrDevicePolicyGroup) {
		t.Fatalf("unknown group = %v", err)
	}
	many := make([]string, maxDevicePolicyUsers+1)
	for i := range many {
		many[i] = "alice"
	}
	if _, err := store.CreatePolicyObject("bad", DevicePolicyOpen, 0, many, nil, "root"); !errors.Is(err, ErrDevicePolicyTooMany) {
		t.Fatalf("oversized users = %v", err)
	}

	defaults := mustCreatePolicy(t, store, "defaults", DevicePolicyOpen, 0, nil, nil)
	if defaults.PolicyVersion != 1 || defaults.Mode != DevicePolicyOpen || len(defaults.AllowedUsers) != 0 {
		t.Fatalf("defaults = %+v", defaults)
	}
	profile := mustCreatePolicy(t, store, "lab profile", DevicePolicyRestricted, 3, []string{"alice"}, []string{"lab"})
	if profile.PolicyVersion != 1 || len(profile.AllowedUsers) != 1 || profile.AllowedUsers[0] != "alice" ||
		len(profile.AllowedGroups) != 1 || profile.AllowedGroups[0] != "lab" {
		t.Fatalf("profile = %+v", profile)
	}
	if _, err := store.CreatePolicyObject("defaults", DevicePolicyOpen, 0, nil, nil, "root"); !errors.Is(err, ErrPolicyObjectExists) {
		t.Fatalf("duplicate name = %v", err)
	}
	policies, err := store.PolicyObjects()
	if err != nil || len(policies) != 2 || policies[0].Name != "defaults" || policies[1].Name != "lab profile" {
		t.Fatalf("list = %+v %v", policies, err)
	}

	updated, err := store.UpdatePolicyObject(profile.ID, "lab profile", DevicePolicyOpen, 5, nil, nil, "root")
	if err != nil {
		t.Fatalf("update profile: %v", err)
	}
	if updated.PolicyVersion != 2 || updated.Mode != DevicePolicyOpen ||
		updated.PermissionCeiling != 5 || len(updated.AllowedUsers) != 0 || len(updated.AllowedGroups) != 0 {
		t.Fatalf("updated profile = %+v", updated)
	}
	if _, err := store.UpdatePolicyObject("nope", "x", DevicePolicyOpen, 0, nil, nil, "root"); !errors.Is(err, ErrUnknownPolicyObject) {
		t.Fatalf("update unknown = %v", err)
	}
	if err := store.DeletePolicyObject("nope"); !errors.Is(err, ErrUnknownPolicyObject) {
		t.Fatalf("delete unknown = %v", err)
	}

	assignment := mustAssign(t, store, profile.ID, PolicyScopeGroup, "lab", 0)
	if err := store.DeletePolicyObject(profile.ID); !errors.Is(err, ErrPolicyObjectInUse) {
		t.Fatalf("delete in use = %v", err)
	}
	if err := store.DeletePolicyAssignment(assignment.ID); err != nil {
		t.Fatalf("delete assignment: %v", err)
	}
	if err := store.DeletePolicyObject(profile.ID); err != nil {
		t.Fatalf("delete policy: %v", err)
	}
	if _, err := store.PolicyObject(profile.ID); !errors.Is(err, ErrUnknownPolicyObject) {
		t.Fatalf("policy after delete = %v", err)
	}
	if err := store.DeletePolicyObject(defaults.ID); err != nil {
		t.Fatalf("delete unused policy: %v", err)
	}
}

func TestPolicyAssignmentValidation(t *testing.T) {
	store := openGroupStore(t)
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}
	policy := mustCreatePolicy(t, store, "profile", DevicePolicyRestricted, 0, []string{"alice"}, nil)

	invalid := []struct {
		name       string
		policyID   string
		scopeKind  string
		scopeValue string
		priority   int
		want       error
	}{
		{"unknown policy", "nope", PolicyScopeAll, "", 0, ErrUnknownPolicyObject},
		{"bad scope", "x", "team", "lab", 0, ErrInvalidPolicyScope},
		{"all with value", policy.ID, PolicyScopeAll, "lab", 0, ErrInvalidPolicyScope},
		{"missing group", policy.ID, PolicyScopeGroup, "missing", 0, ErrUnknownPolicyScope},
		{"bad device id", policy.ID, PolicyScopeDevice, "bad id", 0, ErrInvalidPolicyScope},
		{"priority too high", policy.ID, PolicyScopeAll, "", maxPolicyPriority + 1, ErrInvalidPolicyPriority},
		{"priority too low", policy.ID, PolicyScopeAll, "", -maxPolicyPriority - 1, ErrInvalidPolicyPriority},
	}
	for _, tc := range invalid {
		policyID := tc.policyID
		if policyID == "x" {
			policyID = policy.ID
		}
		if _, err := store.CreatePolicyAssignment(policyID, tc.scopeKind, tc.scopeValue, tc.priority, "root"); !errors.Is(err, tc.want) {
			t.Fatalf("%s = %v, want %v", tc.name, err, tc.want)
		}
	}

	all := mustAssign(t, store, policy.ID, PolicyScopeAll, "", 0)
	device := mustAssign(t, store, policy.ID, PolicyScopeDevice, "devbox", 5)
	if _, err := store.CreatePolicyAssignment(policy.ID, PolicyScopeAll, "", 9, "root"); !errors.Is(err, ErrPolicyAssignmentExists) {
		t.Fatalf("duplicate scope = %v", err)
	}
	assignments, err := store.PolicyAssignments()
	if err != nil || len(assignments) != 2 || assignments[0].ID != device.ID || assignments[0].PolicyName != "profile" {
		t.Fatalf("list assignments = %+v %v", assignments, err)
	}
	if err := store.DeletePolicyAssignment("nope"); !errors.Is(err, ErrUnknownPolicyAssignment) {
		t.Fatalf("delete unknown assignment = %v", err)
	}
	if err := store.DeletePolicyAssignment(all.ID); err != nil {
		t.Fatalf("delete assignment: %v", err)
	}
}

// TestDevicePolicyAssignmentBatch covers the transactional bulk path: input is
// bounded, duplicate ids collapse, existing assignments are reported as
// skipped instead of failing the call, and an over-quota batch rolls back
// completely so no partial list is left behind.
func TestDevicePolicyAssignmentBatch(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "profile", DevicePolicyOpen, 4, nil, nil)

	if _, err := store.CreateDevicePolicyAssignments(policy.ID, nil, 0, "root"); !errors.Is(err, ErrInvalidPolicyAssignmentBatch) {
		t.Fatalf("empty batch = %v", err)
	}
	tooMany := make([]string, MaxPolicyAssignmentBatch+1)
	for i := range tooMany {
		tooMany[i] = fmt.Sprintf("device%04d", i)
	}
	if _, err := store.CreateDevicePolicyAssignments(policy.ID, tooMany, 0, "root"); !errors.Is(err, ErrInvalidPolicyAssignmentBatch) {
		t.Fatalf("oversized batch = %v", err)
	}
	if _, err := store.CreateDevicePolicyAssignments(policy.ID, []string{"devbox"}, maxPolicyPriority+1, "root"); !errors.Is(err, ErrInvalidPolicyPriority) {
		t.Fatalf("bad priority = %v", err)
	}
	if _, err := store.CreateDevicePolicyAssignments("nope", []string{"devbox"}, 0, "root"); !errors.Is(err, ErrUnknownPolicyObject) {
		t.Fatalf("unknown policy = %v", err)
	}
	if _, err := store.CreateDevicePolicyAssignments(policy.ID, []string{"devbox", "bad id"}, 0, "root"); !errors.Is(err, ErrInvalidPolicyScope) {
		t.Fatalf("bad device id = %v", err)
	}
	if assignments, err := store.PolicyAssignments(); err != nil || len(assignments) != 0 {
		t.Fatalf("failed batches wrote rows = %+v %v", assignments, err)
	}

	batch, err := store.CreateDevicePolicyAssignments(policy.ID, []string{"devbox", "laptop", "devbox"}, 7, "root")
	if err != nil {
		t.Fatalf("batch: %v", err)
	}
	if batch.PolicyName != "profile" || len(batch.Created) != 2 || len(batch.Skipped) != 0 ||
		batch.Created[0].ScopeValue != "devbox" || batch.Created[1].ScopeValue != "laptop" ||
		batch.Created[0].Priority != 7 || batch.Created[0].CreatedBy != "root" ||
		batch.Created[0].ScopeKind != PolicyScopeDevice {
		t.Fatalf("batch = %+v", batch)
	}
	second, err := store.CreateDevicePolicyAssignments(policy.ID, []string{"devbox", "desk"}, 7, "root")
	if err != nil || len(second.Created) != 1 || len(second.Skipped) != 1 || second.Skipped[0] != "devbox" {
		t.Fatalf("second batch = %+v %v", second, err)
	}
	assignments, err := store.PolicyAssignments()
	if err != nil || len(assignments) != 3 {
		t.Fatalf("assignments = %+v %v", assignments, err)
	}

	tx, err := store.db.Begin()
	if err != nil {
		t.Fatalf("begin quota seed: %v", err)
	}
	for i := 0; i < maxPolicyAssignments-3; i++ {
		if _, err := tx.Exec(
			`INSERT INTO policy_assignments (id, policy_id, scope_kind, scope_value, priority, created_at)
			 VALUES (?, ?, 'device', ?, 0, 0)`,
			fmt.Sprintf("quota%04d", i), policy.ID, fmt.Sprintf("seed%04d", i),
		); err != nil {
			tx.Rollback()
			t.Fatalf("seed assignment %d: %v", i, err)
		}
	}
	if err := tx.Commit(); err != nil {
		t.Fatalf("commit quota seed: %v", err)
	}
	if _, err := store.CreateDevicePolicyAssignments(policy.ID, []string{"alpha", "beta"}, 0, "root"); !errors.Is(err, ErrPolicyAssignmentQuota) {
		t.Fatalf("batch quota = %v", err)
	}
	if assignments, err := store.PolicyAssignments(); err != nil || len(assignments) != maxPolicyAssignments {
		t.Fatalf("quota rollback left %d rows (%v)", len(assignments), err)
	}
}

func TestPolicyObjectAndAssignmentQuotas(t *testing.T) {
	store := openGroupStore(t)
	tx, err := store.db.Begin()
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	for i := 0; i < maxPolicyObjects; i++ {
		if _, err := tx.Exec(
			`INSERT INTO policy_objects (id, name, mode, permission_ceiling, policy_version, created_at, updated_at)
			 VALUES (?, ?, 'open', 0, 1, 0, 0)`,
			fmt.Sprintf("seed%04d", i), fmt.Sprintf("seed %04d", i),
		); err != nil {
			tx.Rollback()
			t.Fatalf("seed policy %d: %v", i, err)
		}
	}
	if err := tx.Commit(); err != nil {
		t.Fatalf("commit seed: %v", err)
	}
	if _, err := store.CreatePolicyObject("overflow", DevicePolicyOpen, 0, nil, nil, "root"); !errors.Is(err, ErrPolicyObjectQuota) {
		t.Fatalf("policy quota = %v", err)
	}

	tx, err = store.db.Begin()
	if err != nil {
		t.Fatalf("begin assignments: %v", err)
	}
	for i := 0; i < maxPolicyAssignments; i++ {
		if _, err := tx.Exec(
			`INSERT INTO policy_assignments (id, policy_id, scope_kind, scope_value, priority, created_at)
			 VALUES (?, 'seed0000', 'device', ?, 0, 0)`,
			fmt.Sprintf("assign%04d", i), fmt.Sprintf("quota%04d", i),
		); err != nil {
			tx.Rollback()
			t.Fatalf("seed assignment %d: %v", i, err)
		}
	}
	if err := tx.Commit(); err != nil {
		t.Fatalf("commit assignment seed: %v", err)
	}
	if _, err := store.CreatePolicyAssignment("seed0000", PolicyScopeAll, "", 0, "root"); !errors.Is(err, ErrPolicyAssignmentQuota) {
		t.Fatalf("assignment quota = %v", err)
	}
}

// TestPolicyResolutionMaterialisesDevicePolicies covers the G04 core: the
// resolver picks by priority first and specificity second, materialises the
// winner into device_policies, bumps the per-device version monotonically, and
// never lets a manual per-device policy be overwritten by an assignment.
func TestPolicyResolutionMaterialisesDevicePolicies(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "devbox", "root"); err != nil {
		t.Fatalf("add devbox to lab: %v", err)
	}

	defaults := mustCreatePolicy(t, store, "defaults", DevicePolicyOpen, 0, nil, nil)
	labProfile := mustCreatePolicy(t, store, "lab profile", DevicePolicyRestricted, 3, []string{"alice"}, nil)
	deviceProfile := mustCreatePolicy(t, store, "devbox profile", DevicePolicyRestricted, 7, []string{"bob"}, nil)
	mustAssign(t, store, defaults.ID, PolicyScopeAll, "", 0)
	labAssignment := mustAssign(t, store, labProfile.ID, PolicyScopeGroup, "lab", 10)

	changed, unmanaged, err := store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	if len(unmanaged) != 0 || len(changed) != 1 {
		t.Fatalf("reconcile = %+v unmanaged %v", changed, unmanaged)
	}
	policy := changed[0]
	if policy.PolicyVersion != 1 || policy.Mode != DevicePolicyRestricted || policy.PermissionCeiling != 3 {
		t.Fatalf("materialised policy = %+v", policy)
	}
	if policy.Source == nil || policy.Source.ID != labProfile.ID || policy.Source.Name != "lab profile" ||
		policy.Source.Version != 1 || policy.Source.Priority != 10 {
		t.Fatalf("source = %+v", policy.Source)
	}
	if len(policy.AllowedUsers) != 1 || policy.AllowedUsers[0] != "alice" {
		t.Fatalf("allowed users = %v", policy.AllowedUsers)
	}
	admission, err := store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil || !admission.Allowed || admission.PolicyVersion != 1 {
		t.Fatalf("alice admission = %+v %v", admission, err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", bob.ID)
	if err != nil || admission.Allowed {
		t.Fatalf("bob admission = %+v %v", admission, err)
	}

	// An unchanged assignment must not bump the version the host acknowledged.
	if changed, unmanaged, err = store.ReconcileDevicePolicies([]string{"devbox"}); err != nil || len(changed) != 0 || len(unmanaged) != 0 {
		t.Fatalf("steady reconcile = %+v %v %v", changed, unmanaged, err)
	}

	// A more specific device assignment wins even at equal priority because
	// priority is compared first and specificity breaks the tie.
	deviceAssignment := mustAssign(t, store, deviceProfile.ID, PolicyScopeDevice, "devbox", 10)
	changed, _, err = store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil || len(changed) != 1 {
		t.Fatalf("device assignment reconcile = %+v %v", changed, err)
	}
	policy = changed[0]
	if policy.PolicyVersion != 2 || policy.PermissionCeiling != 7 ||
		policy.Source == nil || policy.Source.ID != deviceProfile.ID {
		t.Fatalf("device policy = %+v source %+v", policy, policy.Source)
	}
	admission, err = store.CheckDeviceAdmission("devbox", bob.ID)
	if err != nil || !admission.Allowed {
		t.Fatalf("bob under device policy = %+v %v", admission, err)
	}

	// Editing a losing policy must not touch the device row.
	labProfile, err = store.UpdatePolicyObject(labProfile.ID, "lab profile", DevicePolicyRestricted, 4, []string{"alice", "bob"}, nil, "root")
	if err != nil || labProfile.PolicyVersion != 2 {
		t.Fatalf("update lab profile = %+v %v", labProfile, err)
	}
	if changed, _, err = store.ReconcileDevicePolicies([]string{"devbox"}); err != nil || len(changed) != 0 {
		t.Fatalf("reconcile after losing edit = %+v %v", changed, err)
	}

	// Dropping the device assignment falls back to the group policy and bumps
	// the device version even though the template version is lower than the
	// previous source version.
	if err := store.DeletePolicyAssignment(deviceAssignment.ID); err != nil {
		t.Fatalf("delete device assignment: %v", err)
	}
	changed, _, err = store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil || len(changed) != 1 {
		t.Fatalf("fallback reconcile = %+v %v", changed, err)
	}
	policy = changed[0]
	if policy.PolicyVersion != 3 || policy.PermissionCeiling != 4 ||
		policy.Source == nil || policy.Source.ID != labProfile.ID || policy.Source.Version != 2 {
		t.Fatalf("fallback policy = %+v source %+v", policy, policy.Source)
	}
	admission, err = store.CheckDeviceAdmission("devbox", bob.ID)
	if err != nil || !admission.Allowed || admission.PermissionCeiling != 4 {
		t.Fatalf("bob under group policy = %+v %v", admission, err)
	}

	// A manual per-device policy wins and survives every reconcile.
	manual, err := store.SetDevicePolicy("devbox", DevicePolicyOpen, 0, nil, nil, "root")
	if err != nil {
		t.Fatalf("manual policy: %v", err)
	}
	if manual.Source != nil || manual.PolicyVersion != 4 {
		t.Fatalf("manual policy = %+v source %+v", manual, manual.Source)
	}
	if changed, _, err = store.ReconcileDevicePolicies([]string{"devbox"}); err != nil || len(changed) != 0 {
		t.Fatalf("reconcile over manual = %+v %v", changed, err)
	}
	if policy, _, err = store.DevicePolicy("devbox"); err != nil || policy.Mode != DevicePolicyOpen || policy.Source != nil {
		t.Fatalf("manual after reconcile = %+v %v", policy, err)
	}
	if err := store.DeleteDevicePolicy("devbox"); err != nil {
		t.Fatalf("delete manual: %v", err)
	}
	changed, _, err = store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil || len(changed) != 1 {
		t.Fatalf("reconcile after manual delete = %+v %v", changed, err)
	}
	if changed[0].PolicyVersion != 5 || changed[0].Source == nil || changed[0].Source.ID != labProfile.ID {
		t.Fatalf("assigned after manual delete = %+v", changed[0])
	}

	// Removing the device from the group falls back to the global assignment.
	if err := store.RemoveGroupDevice("lab", "devbox"); err != nil {
		t.Fatalf("remove group device: %v", err)
	}
	changed, _, err = store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil || len(changed) != 1 {
		t.Fatalf("global fallback = %+v %v", changed, err)
	}
	if changed[0].PolicyVersion != 6 || changed[0].Mode != DevicePolicyOpen ||
		changed[0].Source == nil || changed[0].Source.ID != defaults.ID {
		t.Fatalf("global policy = %+v", changed[0])
	}

	// Dropping every assignment returns the device to unmanaged.
	if err := store.DeletePolicyAssignment(labAssignment.ID); err != nil {
		t.Fatalf("delete group assignment: %v", err)
	}
	assignments, err := store.PolicyAssignments()
	if err != nil || len(assignments) != 1 {
		t.Fatalf("assignments after deletes = %+v %v", assignments, err)
	}
	if err := store.DeletePolicyAssignment(assignments[0].ID); err != nil {
		t.Fatalf("delete global assignment: %v", err)
	}
	changed, unmanaged, err = store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil || len(changed) != 0 || len(unmanaged) != 1 || unmanaged[0] != "devbox" {
		t.Fatalf("unmanage = %+v %v %v", changed, unmanaged, err)
	}
	if _, managed, err := store.DevicePolicy("devbox"); err != nil || managed {
		t.Fatalf("device after unmanage = %v %v", managed, err)
	}
	if admission, err = store.CheckDeviceAdmission("devbox", ""); err != nil || admission.Managed || !admission.Allowed {
		t.Fatalf("unmanaged admission = %+v %v", admission, err)
	}
}

// TestPolicyReconcileCacheTracksPolicyWrites proves the registration fast path:
// an unchanged device is remembered at the current generation, and any write
// that can change assignment resolution makes the next reconcile run again.
func TestPolicyReconcileCacheTracksPolicyWrites(t *testing.T) {
	store := openGroupStore(t)
	if _, _, err := store.ReconcileDevicePolicies([]string{"devbox"}); err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	if gen, ok := store.policyClean["devbox"]; !ok || gen != store.policyGen {
		t.Fatalf("steady reconcile cached = %v at %d, want generation %d", ok, gen, store.policyGen)
	}

	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "devbox", "root"); err != nil {
		t.Fatalf("add devbox to lab: %v", err)
	}
	if _, ok := store.policyClean["devbox"]; ok {
		t.Fatal("group membership write left the reconciled device cached")
	}
	if _, _, err := store.ReconcileDevicePolicies([]string{"devbox"}); err != nil {
		t.Fatalf("reconcile after membership write: %v", err)
	}

	if err := store.DeleteDevicePolicy("devbox"); !errors.Is(err, ErrUnknownDevicePolicy) {
		t.Fatalf("delete absent policy = %v", err)
	}
	if gen, ok := store.policyClean["devbox"]; !ok || gen != store.policyGen {
		t.Fatalf("failed write dropped cache = %v at %d", ok, gen)
	}
}

// TestPolicyReconcileAllCoversGroupDevices proves that devices that joined a
// group before any assignment existed are reached by the bulk reconcile, and
// that deleting the group removes its group-scoped assignment.
func TestPolicyReconcileAllCoversGroupDevices(t *testing.T) {
	store := openGroupStore(t)
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "devbox", "root"); err != nil {
		t.Fatalf("add group device: %v", err)
	}
	if _, err := store.AddGroupDevice("lab", "laptop", "root"); err != nil {
		t.Fatalf("add second group device: %v", err)
	}
	policy := mustCreatePolicy(t, store, "lab profile", DevicePolicyRestricted, 2, []string{"alice"}, nil)
	mustAssign(t, store, policy.ID, PolicyScopeGroup, "lab", 0)

	changed, unmanaged, err := store.ReconcileAllDevicePolicies()
	if err != nil || len(unmanaged) != 0 || len(changed) != 2 {
		t.Fatalf("reconcile all = %+v %v %v", changed, unmanaged, err)
	}
	for _, device := range changed {
		if device.Source == nil || device.Source.ID != policy.ID || device.PermissionCeiling != 2 {
			t.Fatalf("materialised %s = %+v", device.PeerID, device)
		}
	}
	if policies, err := store.DevicePolicies(); err != nil || len(policies) != 2 {
		t.Fatalf("managed devices = %+v %v", policies, err)
	}

	if err := store.DeleteAccountGroup("lab"); err != nil {
		t.Fatalf("delete group: %v", err)
	}
	assignments, err := store.PolicyAssignments()
	if err != nil || len(assignments) != 0 {
		t.Fatalf("assignments after group delete = %+v %v", assignments, err)
	}
	changed, unmanaged, err = store.ReconcileAllDevicePolicies()
	if err != nil || len(changed) != 0 || len(unmanaged) != 2 {
		t.Fatalf("reconcile after group delete = %+v %v %v", changed, unmanaged, err)
	}
	if policies, err := store.DevicePolicies(); err != nil || len(policies) != 0 {
		t.Fatalf("managed devices after group delete = %+v %v", policies, err)
	}
}

// TestPolicyStaleReceiptAfterReconcile pins the console contract: a receipt for
// the version before an assignment change is rejected, so a host cannot claim
// to have applied a policy it never saw.
func TestPolicyStaleReceiptAfterReconcile(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "defaults", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeAll, "", 0)
	if _, _, err := store.ReconcileDevicePolicies([]string{"devbox"}); err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	if _, err := store.RecordDevicePolicyReceipt("devbox", 1, true, ""); err != nil {
		t.Fatalf("current receipt: %v", err)
	}
	if _, err := store.UpdatePolicyObject(policy.ID, "defaults", DevicePolicyRestricted, 0, nil, nil, "root"); err != nil {
		t.Fatalf("disable policy: %v", err)
	}
	changed, _, err := store.ReconcileDevicePolicies([]string{"devbox"})
	if err != nil || len(changed) != 1 || changed[0].PolicyVersion != 2 {
		t.Fatalf("reconcile after edit = %+v %v", changed, err)
	}
	if _, err := store.RecordDevicePolicyReceipt("devbox", 1, true, ""); !errors.Is(err, ErrStaleDevicePolicyVersion) {
		t.Fatalf("stale receipt = %v", err)
	}
	receipt, err := store.RecordDevicePolicyReceipt("devbox", 2, true, "")
	if err != nil || receipt.PolicyVersion != 2 || !receipt.Applied {
		t.Fatalf("current receipt after edit = %+v %v", receipt, err)
	}
}

func TestPolicyObjectSchemaTables(t *testing.T) {
	store := openGroupStore(t)
	for _, table := range []string{
		"policy_objects",
		"policy_object_users",
		"policy_object_groups",
		"policy_assignments",
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
	if version != accountSchemaVersion || version < 14 {
		t.Fatalf("schema version = %d, want %d", version, accountSchemaVersion)
	}
}
