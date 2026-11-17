package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type adminRoleReply struct {
	ID          string    `json:"id"`
	Revision    string    `json:"revision"`
	Name        string    `json:"name"`
	Description string    `json:"description"`
	Permissions []string  `json:"permissions"`
	Builtin     bool      `json:"builtin"`
	CreatedAt   time.Time `json:"created_at"`
	UpdatedAt   time.Time `json:"updated_at"`
}

type adminRoleListReply struct {
	Roles       []adminRoleReply `json:"roles"`
	Permissions []string         `json:"permissions"`
}

type roleBindingReply struct {
	ID         string `json:"id"`
	RoleID     string `json:"role_id"`
	UserID     string `json:"user_id"`
	Username   string `json:"username"`
	ScopeKind  string `json:"scope_kind"`
	ScopeValue string `json:"scope_value"`
	ObjectKind string `json:"object_kind"`
	RoleName   string `json:"role_name"`
}

type roleBindingListReply struct {
	Bindings []roleBindingReply `json:"bindings"`
}

func hasPermission(permissions []string, want string) bool {
	for _, permission := range permissions {
		if permission == want {
			return true
		}
	}
	return false
}

func createNamedRole(t *testing.T, base, token, id string, permissions ...string) adminRoleReply {
	t.Helper()
	status, body := request(t, http.MethodPost, base+"/v1/admin-roles",
		mustJSON(t, map[string]any{
			"id": id, "name": id, "description": "test role", "permissions": permissions,
		}), token)
	if status != http.StatusCreated {
		t.Fatalf("create role %s: %d %s", id, status, body)
	}
	var role adminRoleReply
	if err := json.Unmarshal(body, &role); err != nil {
		t.Fatalf("create role reply: %v", err)
	}
	return role
}

func bindNamedRole(t *testing.T, base, token, username, roleID, scopeKind, scopeValue, objectKind string) roleBindingReply {
	t.Helper()
	status, body := request(t, http.MethodPost, base+"/v1/role-bindings",
		mustJSON(t, map[string]any{
			"username": username, "role_id": roleID, "scope_kind": scopeKind,
			"scope_value": scopeValue, "object_kind": objectKind,
		}), token)
	if status != http.StatusCreated {
		t.Fatalf("bind role %s to %s: %d %s", roleID, username, status, body)
	}
	var binding roleBindingReply
	if err := json.Unmarshal(body, &binding); err != nil {
		t.Fatalf("bind role reply: %v", err)
	}
	return binding
}

func TestAdminRoleAPICreateUpdateDeleteAndBuiltinGuard(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token

	status, body := request(t, http.MethodPost, s.URL+"/v1/admin-roles",
		`{"id":"device-auditor","name":"Device auditor","permissions":["devices.read","audit.read","devices.read"]}`,
		admin)
	if status != http.StatusCreated {
		t.Fatalf("create role: %d %s", status, body)
	}
	var role adminRoleReply
	if err := json.Unmarshal(body, &role); err != nil {
		t.Fatalf("create role reply: %v", err)
	}
	if role.ID != "device-auditor" || !hasPermission(role.Permissions, account.PermissionDevicesRead) ||
		!hasPermission(role.Permissions, account.PermissionAuditRead) || len(role.Permissions) != 2 {
		t.Fatalf("created role = %+v", role)
	}
	if role.CreatedAt.IsZero() || role.UpdatedAt.IsZero() {
		t.Fatalf("created role timestamps = created %v, updated %v", role.CreatedAt, role.UpdatedAt)
	}
	createdAt := role.CreatedAt

	status, body = request(t, http.MethodPut, s.URL+"/v1/admin-roles/device-auditor",
		mustJSON(t, map[string]any{"name": "Device reader", "description": "read only", "permissions": []string{"devices.read"}, "if_revision": role.Revision}), admin)
	if status != http.StatusOK {
		t.Fatalf("update role: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &role); err != nil || role.Name != "Device reader" ||
		len(role.Permissions) != 1 || role.Permissions[0] != account.PermissionDevicesRead {
		t.Fatalf("updated role = %+v, %v", role, err)
	}
	// Account timestamps are persisted at millisecond precision. The create
	// response still carries the clock's nanosecond precision, while update and
	// list responses round-trip through SQLite.
	if !role.CreatedAt.Equal(createdAt.Truncate(time.Millisecond)) || role.UpdatedAt.Before(createdAt) {
		t.Fatalf("updated role timestamps = created %v, updated %v; creation was %v",
			role.CreatedAt, role.UpdatedAt, createdAt)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/admin-roles", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list roles: %d %s", status, body)
	}
	var roles adminRoleListReply
	if err := json.Unmarshal(body, &roles); err != nil {
		t.Fatalf("list role reply: %v", err)
	}
	foundAdmin, foundRole := false, false
	for _, item := range roles.Roles {
		if item.ID == account.RoleAdmin && item.Builtin {
			foundAdmin = true
		}
		if item.ID == "device-auditor" {
			foundRole = true
		}
	}
	if !foundAdmin || !foundRole || !hasPermission(roles.Permissions, account.PermissionRolesWrite) ||
		!hasPermission(roles.Permissions, account.PermissionRecordingsDelete) {
		t.Fatalf("role catalogue = %+v", roles)
	}

	if status, body := request(t, http.MethodPut, s.URL+"/v1/admin-roles/admin",
		`{"name":"Changed","permissions":[]}`, admin); status != http.StatusConflict {
		t.Fatalf("update builtin role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/admin-roles/admin", "", admin); status != http.StatusConflict {
		t.Fatalf("delete builtin role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/admin-roles",
		`{"id":"bad role","name":"Bad","permissions":["devices.read"]}`, admin); status != http.StatusBadRequest {
		t.Fatalf("invalid role id: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/admin-roles",
		`{"id":"bad-permission","name":"Bad","permissions":["root.everything"]}`, admin); status != http.StatusBadRequest {
		t.Fatalf("invalid permission: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/admin-roles/device-auditor?if_revision="+role.Revision, "", admin); status != http.StatusNoContent {
		t.Fatalf("delete role: %d %s", status, body)
	}
}

func TestRoleBindingsApplyGlobalGroupAndObjectScopes(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	for _, group := range []string{"east", "west"} {
		if _, err := s.Accounts.CreateGroup(group, group, "", 0, 0); err != nil {
			t.Fatalf("create group %s: %v", group, err)
		}
	}
	if _, err := s.Accounts.AddGroupDevice("east", "devbox", "root"); err != nil {
		t.Fatalf("add east device: %v", err)
	}
	if _, err := s.Accounts.AddGroupDevice("west", "westbox", "root"); err != nil {
		t.Fatalf("add west device: %v", err)
	}
	now := time.Now().Unix()
	for _, id := range []string{"devbox", "westbox", "otherbox"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, id, id+" secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("register %s: %d %s", id, status, body)
		}
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	userLogin := login(t, s.URL, "bob", "battery staple")

	createNamedRole(t, s.URL, admin, "device-reader", account.PermissionDevicesRead, account.PermissionDevicesWrite)
	global := bindNamedRole(t, s.URL, admin, "bob", "device-reader", "global", "", "")
	if global.ScopeKind != "global" || global.RoleName != "device-reader" {
		t.Fatalf("global binding = %+v", global)
	}
	userLogin = login(t, s.URL, "bob", "battery staple")
	if !hasPermission(userLogin.Permissions, account.PermissionDevicesRead) ||
		!hasPermission(userLogin.Permissions, account.PermissionDevicesWrite) {
		t.Fatalf("login permissions after global binding = %v", userLogin.Permissions)
	}
	for _, id := range []string{"devbox", "westbox", "otherbox"} {
		if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/"+id, `{"alias":"`+id+`"}`, userLogin.Token); status != http.StatusOK {
			t.Fatalf("global role read %s: %d %s", id, status, body)
		}
	}

	status, body := request(t, http.MethodDelete, s.URL+"/v1/role-bindings/"+global.ID, "", admin)
	if status != http.StatusNoContent {
		t.Fatalf("unbind global role: %d %s", status, body)
	}
	userLogin = login(t, s.URL, "bob", "battery staple")
	if hasPermission(userLogin.Permissions, account.PermissionDevicesRead) {
		t.Fatalf("global permission remained after unbind: %v", userLogin.Permissions)
	}

	group := bindNamedRole(t, s.URL, admin, "bob", "device-reader", "group", "east", "")
	if group.ScopeValue != "east" {
		t.Fatalf("group binding = %+v", group)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/groups/east/devices", "", userLogin.Token); status != http.StatusOK {
		t.Fatalf("group-scoped binding read east: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/groups/west/devices", "", userLogin.Token); status != http.StatusForbidden {
		t.Fatalf("group-scoped binding read west: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox", `{"alias":"devbox"}`, userLogin.Token); status != http.StatusOK {
		t.Fatalf("group-scoped binding read east device: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/westbox", `{"alias":"westbox"}`, userLogin.Token); status != http.StatusForbidden {
		t.Fatalf("group-scoped binding read west device: %d %s", status, body)
	}

	object := bindNamedRole(t, s.URL, admin, "bob", "device-reader", "object", "otherbox", "device")
	if object.ObjectKind != "device" || object.ScopeValue != "otherbox" {
		t.Fatalf("object binding = %+v", object)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/otherbox", `{"alias":"otherbox"}`, userLogin.Token); status != http.StatusOK {
		t.Fatalf("object-scoped binding read exact device: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/westbox", `{"alias":"westbox"}`, userLogin.Token); status != http.StatusForbidden {
		t.Fatalf("object-scoped binding read different group device: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/role-bindings?username=bob", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list bindings: %d %s", status, body)
	}
	var listed roleBindingListReply
	if err := json.Unmarshal(body, &listed); err != nil {
		t.Fatalf("list bindings reply: %v", err)
	}
	if len(listed.Bindings) != 2 {
		t.Fatalf("bindings after group and object = %+v", listed.Bindings)
	}
}

func TestRolePermissionChangesConvergeForLiveSessions(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	role := createNamedRole(t, s.URL, admin, "live-reader", account.PermissionDevicesRead)
	binding := bindNamedRole(t, s.URL, admin, "bob", "live-reader", "global", "", "")
	bob := login(t, s.URL, "bob", "battery staple")
	if !hasPermission(bob.Permissions, account.PermissionDevicesRead) {
		t.Fatalf("initial login permissions = %v", bob.Permissions)
	}

	status, body := request(t, http.MethodPut, s.URL+"/v1/admin-roles/live-reader",
		mustJSON(t, map[string]any{"name": "Live reader", "permissions": []string{"audit.read"}, "if_revision": role.Revision}), admin)
	if status != http.StatusOK {
		t.Fatalf("update role: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/account", "", bob.Token)
	if status != http.StatusOK {
		t.Fatalf("account after role update: %d %s", status, body)
	}
	var current struct {
		Permissions []string `json:"permissions"`
	}
	if err := json.Unmarshal(body, &current); err != nil {
		t.Fatalf("account reply: %v", err)
	}
	if hasPermission(current.Permissions, account.PermissionDevicesRead) ||
		!hasPermission(current.Permissions, account.PermissionAuditRead) {
		t.Fatalf("live permissions after role update = %v", current.Permissions)
	}

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/refresh",
		mustJSON(t, map[string]string{"refresh_token": bob.RefreshToken}), "")
	if status != http.StatusOK {
		t.Fatalf("refresh after role update: %d %s", status, data)
	}
	var rotated loginReply
	if err := json.Unmarshal(data, &rotated); err != nil {
		t.Fatalf("refresh reply: %v", err)
	}
	if hasPermission(rotated.Permissions, account.PermissionDevicesRead) ||
		!hasPermission(rotated.Permissions, account.PermissionAuditRead) {
		t.Fatalf("refreshed permissions after role update = %v", rotated.Permissions)
	}

	status, body = request(t, http.MethodDelete, s.URL+"/v1/role-bindings/"+binding.ID, "", admin)
	if status != http.StatusNoContent {
		t.Fatalf("unbind role: %d %s", status, body)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/refresh",
		mustJSON(t, map[string]string{"refresh_token": rotated.RefreshToken}), "")
	if status != http.StatusOK {
		t.Fatalf("refresh after unbind: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &rotated); err != nil {
		t.Fatalf("refresh after unbind reply: %v", err)
	}
	if hasPermission(rotated.Permissions, account.PermissionAuditRead) ||
		hasPermission(rotated.Permissions, account.PermissionDevicesRead) {
		t.Fatalf("permissions survived unbind: %v", rotated.Permissions)
	}
}

func TestRoleManagerCannotEscalatePermissions(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	createNamedRole(t, s.URL, admin, "role-manager",
		account.PermissionRolesRead, account.PermissionRolesWrite, account.PermissionDevicesRead)
	bindNamedRole(t, s.URL, admin, "bob", "role-manager", "global", "", "")
	manager := login(t, s.URL, "bob", "battery staple")
	if _, err := s.Accounts.CreateUser("carol", "long enough"); err != nil {
		t.Fatalf("create carol: %v", err)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/admin-roles",
		`{"id":"accounts-admin","name":"Accounts admin","permissions":["accounts.write"]}`,
		manager.Token); status != http.StatusForbidden {
		t.Fatalf("manager creates stronger role: %d %s", status, body)
	}
	createNamedRole(t, s.URL, admin, "accounts-admin", account.PermissionAccountsWrite)
	if status, body := request(t, http.MethodPost, s.URL+"/v1/role-bindings",
		`{"username":"bob","role_id":"accounts-admin","scope_kind":"global"}`,
		manager.Token); status != http.StatusForbidden {
		t.Fatalf("manager binds stronger role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, s.URL+"/v1/admin-roles/accounts-admin",
		`{"name":"Accounts admin","permissions":["accounts.write"]}`, manager.Token); status != http.StatusForbidden {
		t.Fatalf("manager updates stronger role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/admin-roles/accounts-admin", "", manager.Token); status != http.StatusForbidden {
		t.Fatalf("manager deletes stronger role: %d %s", status, body)
	}
	strongBinding := bindNamedRole(t, s.URL, admin, "carol", "accounts-admin", "global", "", "")
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/role-bindings/"+strongBinding.ID, "", manager.Token); status != http.StatusForbidden {
		t.Fatalf("manager unbinds stronger role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","password":"long enough"}`, manager.Token); status != http.StatusForbidden {
		t.Fatalf("manager without accounts.write created account: %d %s", status, body)
	}
}
