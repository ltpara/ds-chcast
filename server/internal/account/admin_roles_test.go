package account

import (
	"errors"
	"testing"
	"time"
)

func containsPermission(keys []string, want string) bool {
	for _, key := range keys {
		if key == want {
			return true
		}
	}
	return false
}

func TestNamedRolesAreSeededAndBuiltinsAreImmutable(t *testing.T) {
	store := openTemp(t, "named-roles.db")
	roles, err := store.ListAdminRoles()
	if err != nil {
		t.Fatalf("list built-in roles: %v", err)
	}
	byID := make(map[string]AdminRole, len(roles))
	for _, role := range roles {
		byID[role.ID] = role
	}
	for _, fixedRole := range []string{RoleAdmin, RoleOperator, RoleReadonly} {
		role, ok := byID[fixedRole]
		if !ok || !role.Builtin {
			t.Fatalf("built-in role %s missing: %+v", fixedRole, roles)
		}
		want, _ := BuiltinRolePermissions(fixedRole)
		if len(role.Permissions) != len(want) {
			t.Fatalf("built-in %s permissions = %v, want %v", fixedRole, role.Permissions, want)
		}
	}
	if _, err := store.UpdateAdminRole(RoleAdmin, "Changed", "", nil); !errors.Is(err, ErrBuiltinRoleImmutable) {
		t.Fatalf("update built-in role = %v", err)
	}
	if err := store.DeleteAdminRole(RoleAdmin); !errors.Is(err, ErrBuiltinRoleImmutable) {
		t.Fatalf("delete built-in role = %v", err)
	}
}

func TestUpdateAdminRolePreservesCreatedAt(t *testing.T) {
	store := openTemp(t, "update-role-times.db")
	createdAt := time.Date(2026, 11, 16, 13, 50, 54, 0, time.UTC)
	store.SetClock(func() time.Time { return createdAt })
	role, err := store.CreateAdminRole("device-reader", "Device reader", "", []string{PermissionDevicesRead})
	if err != nil {
		t.Fatalf("create role: %v", err)
	}
	updatedAt := createdAt.Add(time.Second)
	store.SetClock(func() time.Time { return updatedAt })
	updated, err := store.UpdateAdminRole(role.ID, "Device viewer", "updated", []string{PermissionAuditRead})
	if err != nil {
		t.Fatalf("update role: %v", err)
	}
	if !updated.CreatedAt.Equal(role.CreatedAt) || updated.CreatedAt.IsZero() {
		t.Fatalf("updated role creation time = %v, want %v", updated.CreatedAt, role.CreatedAt)
	}
	if !updated.UpdatedAt.Equal(updatedAt) {
		t.Fatalf("updated role modification time = %v, want %v", updated.UpdatedAt, updatedAt)
	}
	persisted, err := store.AdminRole(role.ID)
	if err != nil {
		t.Fatalf("read updated role: %v", err)
	}
	if !persisted.CreatedAt.Equal(role.CreatedAt) || !persisted.UpdatedAt.Equal(updatedAt) {
		t.Fatalf("persisted role times = created %v, updated %v", persisted.CreatedAt, persisted.UpdatedAt)
	}
}

func TestNamedGlobalManagerProtectsLastManagementAccess(t *testing.T) {
	store := openTemp(t, "named-manager.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	role, err := store.CreateAdminRole("account-manager", "Account manager", "",
		[]string{PermissionAccountsRead, PermissionAccountsWrite})
	if err != nil {
		t.Fatalf("create named role: %v", err)
	}
	binding, err := store.BindRole(bob.Username, role.ID, RoleScopeGlobal, "", "")
	if err != nil {
		t.Fatalf("bind named manager: %v", err)
	}
	effective, err := store.EffectiveConsolePermissions(bob.ID, "", "", "")
	if err != nil || !effective[PermissionAccountsWrite] {
		t.Fatalf("effective manager permissions = %v, %v", effective, err)
	}

	// Hand over management to bob, then prove that the named assignment is a
	// real last-manager guard for every account mutation path.
	if _, err := store.SetRole(alice.Username, RoleUser); err != nil {
		t.Fatalf("demote original admin while named manager exists: %v", err)
	}
	if _, err := store.SetBanned(bob.Username, true); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("ban last named manager = %v", err)
	}
	if err := store.DeleteUser(bob.Username); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("delete last named manager = %v", err)
	}
	if user, err := store.FindUser(bob.Username); err != nil || user.Banned {
		t.Fatalf("failed mutation changed named manager: %+v, %v", user, err)
	}
	if _, err := store.SetRole(bob.Username, RoleUser); err != nil {
		t.Fatalf("fixed role change does not revoke named binding: %v", err)
	}
	if _, err := store.SetBanned(bob.Username, true); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("ban user with named manager = %v", err)
	}

	// Removing the named replacement must also fail, and the failed transaction
	// must leave the binding in place.
	if _, err := store.UnbindRole(binding.ID); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("unbind last named manager = %v", err)
	}
	if bindings, err := store.RoleBindings(bob.ID); err != nil || len(bindings) != 1 {
		t.Fatalf("binding after rejected unbind = %+v, %v", bindings, err)
	}
	if _, err := store.UpdateAdminRole(role.ID, role.Name, role.Description,
		[]string{PermissionAccountsRead}); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("remove named manager permission = %v", err)
	}
	unchanged, err := store.AdminRole(role.ID)
	if err != nil || !containsPermission(unchanged.Permissions, PermissionAccountsWrite) {
		t.Fatalf("role after rejected update = %+v, %v", unchanged, err)
	}
	if err := store.DeleteAdminRole(role.ID); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("delete last named manager role = %v", err)
	}
	if _, err := store.AdminRole(role.ID); err != nil {
		t.Fatalf("failed role deletion removed role: %v", err)
	}

	// Restore alice as a fixed manager; the named binding is then redundant and
	// can be removed, leaving a manageable deployment.
	if _, err := store.SetRole(alice.Username, RoleAdmin); err != nil {
		t.Fatalf("restore fixed admin role: %v", err)
	}
	if _, err := store.UnbindRole(binding.ID); err != nil {
		t.Fatalf("unbind with fixed admin present: %v", err)
	}
	if bindings, err := store.RoleBindings(bob.ID); err != nil || len(bindings) != 0 {
		t.Fatalf("bindings after unbind = %+v, %v", bindings, err)
	}
}

func TestGroupScopedManagerDoesNotProtectGlobalManagement(t *testing.T) {
	store := openTemp(t, "scoped-manager.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := store.CreateGroup("ops", "Operations", "", 0, 0); err != nil {
		t.Fatalf("create group: %v", err)
	}
	role, err := store.CreateAdminRole("group-manager", "Group manager", "",
		[]string{PermissionAccountsWrite})
	if err != nil {
		t.Fatalf("create scoped role: %v", err)
	}
	binding, err := store.BindRole(bob.Username, role.ID, RoleScopeGroup, "ops", "")
	if err != nil {
		t.Fatalf("bind group role: %v", err)
	}
	effective, err := store.EffectiveConsolePermissions(bob.ID, "ops", "", "")
	if err != nil || !effective[PermissionAccountsWrite] {
		t.Fatalf("group effective permissions = %v, %v", effective, err)
	}
	global, err := store.EffectiveConsolePermissions(bob.ID, "", "", "")
	if err != nil || global[PermissionAccountsWrite] {
		t.Fatalf("group role leaked globally = %v, %v", global, err)
	}
	if _, err := store.SetRole("alice", RoleUser); !errors.Is(err, ErrLastManagementAdmin) {
		t.Fatalf("group role protected global management: %v", err)
	}
	if _, err := store.UnbindRole(binding.ID); err != nil {
		t.Fatalf("unbind group role: %v", err)
	}
}

func TestObjectScopedPermissionsRequireExactTarget(t *testing.T) {
	store := openTemp(t, "object-manager.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	role, err := store.CreateAdminRole("device-viewer", "Device viewer", "",
		[]string{PermissionDevicesRead})
	if err != nil {
		t.Fatalf("create object role: %v", err)
	}
	if _, err := store.BindRole(bob.Username, role.ID, RoleScopeObject, "peer-1", "device"); err != nil {
		t.Fatalf("bind object role: %v", err)
	}
	matched, err := store.EffectiveConsolePermissions(bob.ID, "", "device", "peer-1")
	if err != nil || !matched[PermissionDevicesRead] {
		t.Fatalf("matched object permissions = %v, %v", matched, err)
	}
	unmatched, err := store.EffectiveConsolePermissions(bob.ID, "", "device", "peer-2")
	if err != nil || unmatched[PermissionDevicesRead] {
		t.Fatalf("unmatched object permissions = %v, %v", unmatched, err)
	}
}
