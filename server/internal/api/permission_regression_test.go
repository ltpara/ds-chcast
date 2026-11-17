package api_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/pgtest"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// Run the same HTTP permission matrix on SQLite by default, and on a fresh
// PostgreSQL schema for each case when the integration DSN is configured.
func permissionRegressionServer(t *testing.T, opts testutil.Options) *testutil.Server {
	t.Helper()
	if os.Getenv("CHAST_TEST_POSTGRES_DSN") != "" {
		opts.PostgresDSN = pgtest.Schema(t)
	}
	return testutil.StartWith(t, opts)
}

// Regression coverage for the least-privilege console contract.
func TestPermissionRegressionNamedManagerBindingVisibility(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	for _, username := range []string{"root", "manager", "reader", "ordinary"} {
		if _, err := s.Accounts.CreateUser(username, "correct horse"); err != nil {
			t.Fatal(err)
		}
	}
	root := login(t, s.URL, "root", "correct horse").Token
	createNamedRole(t, s.URL, root, "role-manager", account.PermissionRolesRead, account.PermissionRolesWrite, account.PermissionDevicesRead)
	createNamedRole(t, s.URL, root, "role-reader", account.PermissionRolesRead)
	createNamedRole(t, s.URL, root, "device-reader", account.PermissionDevicesRead)
	managerBinding := bindNamedRole(t, s.URL, root, "manager", "role-manager", "global", "", "")
	readerBinding := bindNamedRole(t, s.URL, root, "reader", "role-reader", "global", "", "")
	deviceBinding := bindNamedRole(t, s.URL, root, "ordinary", "device-reader", "object", "devbox", "device")
	strongBinding := bindNamedRole(t, s.URL, root, "ordinary", account.RoleAdmin, "global", "", "")
	manager := login(t, s.URL, "manager", "correct horse").Token
	reader := login(t, s.URL, "reader", "correct horse").Token
	assertBindings := func(token, query string, wanted ...string) {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/role-bindings"+query, "", token)
		if status != http.StatusOK {
			t.Fatalf("list bindings: %d %s", status, body)
		}
		var listed roleBindingListReply
		if err := json.Unmarshal(body, &listed); err != nil {
			t.Fatal(err)
		}
		ids := make(map[string]bool)
		for _, binding := range listed.Bindings {
			ids[binding.ID] = true
		}
		if len(ids) != len(wanted) {
			t.Fatalf("bindings count: got %d want %d: %+v", len(ids), len(wanted), listed.Bindings)
		}
		for _, id := range wanted {
			if !ids[id] {
				t.Fatalf("missing binding %s", id)
			}
		}
	}
	assertBindings(manager, "", managerBinding.ID, readerBinding.ID, deviceBinding.ID)
	assertBindings(manager, "?username=ordinary", deviceBinding.ID)
	assertBindings(reader, "", readerBinding.ID)
	assertBindings(reader, "?username=ordinary")
	assertBindings(root, "", managerBinding.ID, readerBinding.ID, deviceBinding.ID, strongBinding.ID)
	// The admin owner's fixed role must not bypass a narrower token ceiling.
	limitedToken := createToken(t, s.URL, root, `{"name":"binding manager","role":"admin","resources":["accounts"]}`).Secret
	assertBindings(limitedToken, "", readerBinding.ID)
	readonlyToken := createToken(t, s.URL, root, `{"name":"binding reader","role":"readonly","resources":["accounts"]}`).Secret
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/role-bindings", "", readonlyToken); status != http.StatusForbidden {
		t.Fatalf("token without roles.read: %d", status)
	}
	if status, _ := request(t, http.MethodDelete, s.URL+"/v1/role-bindings/"+strongBinding.ID, "", manager); status != http.StatusForbidden {
		t.Fatalf("strong binding unbind: %d", status)
	}
	if status, _ := request(t, http.MethodDelete, s.URL+"/v1/role-bindings/"+deviceBinding.ID, "", manager); status != http.StatusNoContent {
		t.Fatalf("manageable binding unbind: %d", status)
	}
	assertBindings(manager, "", managerBinding.ID, readerBinding.ID)
}

func TestPermissionRegressionDisconnectDoesNotGrantEmergency(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{AccessIssuer: newTestIssuer(t)})
	root, err := s.Accounts.CreateUser("root", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	limited, err := s.Accounts.CreateUser("limited", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	role, err := s.Accounts.CreateAdminRole("disconnect-only", "Disconnect", "", []string{account.PermissionConnectionsDisconnect})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.BindRole(limited.Username, role.ID, "global", "", ""); err != nil {
		t.Fatal(err)
	}
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 6, []string{root.Username}, nil, root.Username); err != nil {
		t.Fatal(err)
	}
	token := login(t, s.URL, limited.Username, "correct horse").Token
	status, data := request(t, http.MethodPost, s.URL+"/v1/emergency-access",
		emergencyRequestBody(t, "devbox", bytes.Repeat([]byte{0x42}, 32), bytes.Repeat([]byte{0x43}, 32), 6, "local review"), token)
	var result emergencyAccessReply
	if err := json.Unmarshal(data, &result); err != nil {
		t.Fatal(err)
	}
	t.Logf("disconnect-only emergency status=%d, ticket issued=%t", status, result.AccessTicket != nil)
	if status != http.StatusForbidden {
		t.Errorf("want 403 for an account with only connections.disconnect, got %d", status)
	}
}

func TestPermissionRegressionAccountWriterCannotPromoteSelf(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	if _, err := s.Accounts.CreateUser("root", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateUser("limited", "correct horse"); err != nil {
		t.Fatal(err)
	}
	role, err := s.Accounts.CreateAdminRole("account-writer", "Account writer", "", []string{account.PermissionAccountsWrite})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.BindRole("limited", role.ID, "global", "", ""); err != nil {
		t.Fatal(err)
	}
	token := login(t, s.URL, "limited", "correct horse").Token
	before, _ := request(t, http.MethodGet, s.URL+"/v1/admin-roles", "", token)
	status, _ := request(t, http.MethodPatch, s.URL+"/v1/accounts/limited", `{"role":"admin"}`, token)
	after, _ := request(t, http.MethodGet, s.URL+"/v1/admin-roles", "", token)
	t.Logf("accounts.write only: roles read before=%d, self-promotion=%d, roles read after=%d", before, status, after)
	if status != http.StatusForbidden {
		t.Errorf("self-promotion should be denied, got %d", status)
	}
}

func TestPermissionRegressionDeviceDeletePermission(t *testing.T) {
	for _, tc := range []struct {
		permission string
		want       int
	}{
		{account.PermissionDevicesWrite, http.StatusForbidden},
		{account.PermissionDevicesDelete, http.StatusNoContent},
	} {
		t.Run(tc.permission, func(t *testing.T) {
			s := permissionRegressionServer(t, testutil.Options{})
			if _, err := s.Accounts.CreateUser("root", "correct horse"); err != nil {
				t.Fatal(err)
			}
			if _, err := s.Accounts.CreateUser("limited", "correct horse"); err != nil {
				t.Fatal(err)
			}
			role, err := s.Accounts.CreateAdminRole("limited-role", "Limited", "", []string{tc.permission})
			if err != nil {
				t.Fatal(err)
			}
			if _, err := s.Accounts.BindRole("limited", role.ID, "global", "", ""); err != nil {
				t.Fatal(err)
			}
			registerLiveHost(t, s.URL, "devbox", "devbox secret")
			token := login(t, s.URL, "limited", "correct horse").Token
			leaseStatus, leaseBody := request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", token)
			if leaseStatus != tc.want {
				t.Errorf("delete lease: %d %s", leaseStatus, leaseBody)
			}
			status, _ := request(t, http.MethodDelete, s.URL+"/v1/identities/devbox", "", token)
			t.Logf("permission=%s delete status=%d", tc.permission, status)
			if status != tc.want {
				t.Errorf("want %d, got %d", tc.want, status)
			}
		})
	}
}

func TestPermissionRegressionScopedConnectionListing(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	if _, err := s.Accounts.CreateUser("root", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateUser("limited", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.AddGroupDevice("east", "devbox", "root"); err != nil {
		t.Fatal(err)
	}
	role, err := s.Accounts.CreateAdminRole("connection-reader", "Reader", "", []string{account.PermissionConnectionsRead, account.PermissionConnectionsDisconnect})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.BindRole("limited", role.ID, "group", "east", ""); err != nil {
		t.Fatal(err)
	}
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	if status, _ := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/live-connections", signedLiveSnapshot(t, "devbox", "devbox secret", liveSnapshot(1, apiLiveConnA)), testutil.ServerKey); status != http.StatusOK {
		t.Fatal(status)
	}
	session := login(t, s.URL, "limited", "correct horse")
	detailStatus, _ := request(t, http.MethodGet, s.URL+"/v1/live-connections/devbox/"+apiLiveConnA, "", session.Token)
	listStatus, _ := request(t, http.MethodGet, s.URL+"/v1/live-connections", "", session.Token)
	groupStatus, data := request(t, http.MethodGet, s.URL+"/v1/groups", "", session.Token)
	var groups struct {
		Groups []json.RawMessage `json:"groups"`
	}
	if err := json.Unmarshal(data, &groups); err != nil {
		t.Fatal(err)
	}
	t.Logf("scoped connection role: login permissions=%d, groups status=%d count=%d, detail=%d list=%d", len(session.Permissions), groupStatus, len(groups.Groups), detailStatus, listStatus)
	if listStatus != http.StatusOK {
		t.Errorf("want authorized scoped list, got %d", listStatus)
	}
}

func TestPermissionRegressionMachineTokenDisconnect(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	if _, err := s.Accounts.CreateUser("root", "correct horse"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "root", "correct horse").Token
	token := createToken(t, s.URL, admin, `{"name":"review","role":"admin","resources":["all"]}`).Secret
	registerLiveHost(t, s.URL, "devbox", "devbox secret")
	if status, _ := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/live-connections", signedLiveSnapshot(t, "devbox", "devbox secret", liveSnapshot(1, apiLiveConnA)), testutil.ServerKey); status != http.StatusOK {
		t.Fatal(status)
	}
	status, _ := request(t, http.MethodPost, s.URL+"/v1/live-connections/devbox/"+apiLiveConnA+"/disconnect", `{"request_key":"review-machine-disconnect"}`, token)
	t.Logf("admin/all machine token disconnect status=%d", status)
	if status != http.StatusAccepted {
		t.Errorf("want 202 for admin/all machine token, got %d", status)
	}
}

func TestPermissionRegressionAccountMaintenanceCeiling(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	for _, name := range []string{"root", "limited", "ordinary", "scoped-admin"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatal(err)
		}
	}
	role, err := s.Accounts.CreateAdminRole("writer", "Writer", "", []string{account.PermissionAccountsWrite})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.BindRole("limited", role.ID, "global", "", ""); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.BindRole("scoped-admin", "admin", "object", "devbox", "device"); err != nil {
		t.Fatal(err)
	}
	token := login(t, s.URL, "limited", "correct horse").Token
	for _, target := range []string{"root", "scoped-admin"} {
		for _, tc := range []struct{ method, path, body string }{
			{"PATCH", "", `{"role":"user"}`}, {"DELETE", "", ""},
			{"POST", "/password", `{"password":"replacement password"}`},
			{"POST", "/email", `{"email":"new@example.test"}`},
			{"POST", "/phone", `{"phone":"+15555550123"}`},
			{"POST", "/recover", `{}`}, {"POST", "/sms", `{}`},
			{"POST", "/ban", `{}`}, {"DELETE", "/ban", ""},
			{"POST", "/permissions", `{"permission_ceiling":1}`},
			{"POST", "/2fa/reset", `{}`}, {"POST", "/sessions/revoke-all", `{}`},
			{"DELETE", "/sessions/unknown", ""},
		} {
			status, body := request(t, tc.method, s.URL+"/v1/accounts/"+target+tc.path, tc.body, token)
			if status != 403 {
				t.Errorf("%s %s%s: %d %s", tc.method, target, tc.path, status, body)
			}
		}
	}
	if status, body := request(t, "POST", s.URL+"/v1/accounts", `{"username":"newadmin","password":"correct horse","role":"admin"}`, token); status != 403 {
		t.Fatalf("create admin: %d %s", status, body)
	}
	if status, body := request(t, "POST", s.URL+"/v1/accounts", `{"username":"newuser","password":"correct horse","role":"user"}`, token); status != 201 {
		t.Fatalf("create ordinary: %d %s", status, body)
	}
	if status, body := request(t, "POST", s.URL+"/v1/accounts/ordinary/password", `{"password":"replacement password"}`, token); status != 200 {
		t.Fatalf("maintain ordinary: %d %s", status, body)
	}
	root := login(t, s.URL, "root", "correct horse").Token
	restricted := createToken(t, s.URL, root, `{"name":"accounts only","role":"admin","resources":["accounts"]}`).Secret
	for _, tc := range []struct{ path, body string }{
		{"/v1/accounts", `{"username":"anotheradmin","password":"correct horse","role":"admin"}`},
		{"/v1/accounts/root/password", `{"password":"replacement password"}`},
		{"/v1/admin-roles", `{"id":"escape","name":"Escape","permissions":["devices.write"]}`},
		{"/v1/role-bindings", `{"username":"limited","role_id":"admin","scope_kind":"global"}`},
		{"/v1/api-tokens", `{"name":"escape","role":"admin","resources":["all"]}`},
	} {
		if status, body := request(t, "POST", s.URL+tc.path, tc.body, restricted); status != 403 {
			t.Errorf("restricted token %s: %d %s", tc.path, status, body)
		}
	}
	// A full administrator may grant a fixed role within its own ceiling.
	if status, body := request(t, "PATCH", s.URL+"/v1/accounts/ordinary", `{"role":"operator"}`, root); status != 200 {
		t.Fatalf("admin role grant: %d %s", status, body)
	}
}

func TestPermissionRegressionScopedCollections(t *testing.T) {
	for _, kind := range []string{"group", "object"} {
		t.Run(kind, func(t *testing.T) {
			s := permissionRegressionServer(t, testutil.Options{})
			for _, name := range []string{"root", "limited"} {
				if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
					t.Fatal(err)
				}
			}
			if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
				t.Fatal(err)
			}
			if _, err := s.Accounts.AddGroupDevice("east", "eastbox", "root"); err != nil {
				t.Fatal(err)
			}
			keys := []string{account.PermissionDevicesRead, account.PermissionDevicesWrite, account.PermissionDevicesPolicyRead, account.PermissionConnectionsRead, account.PermissionConnectionsDisconnect}
			role, err := s.Accounts.CreateAdminRole("scoped", "Scoped", "", keys)
			if err != nil {
				t.Fatal(err)
			}
			value, objectKind := "east", ""
			if kind == "object" {
				value, objectKind = "eastbox", "device"
			}
			binding, err := s.Accounts.BindRole("limited", role.ID, kind, value, objectKind)
			if err != nil {
				t.Fatal(err)
			}
			for _, id := range []string{"eastbox", "westbox"} {
				registerLiveHost(t, s.URL, id, id+" secret")
				if _, err := s.Accounts.SetDevicePolicy(id, "open", 7, nil, nil, "root"); err != nil {
					t.Fatal(err)
				}
				if status, body := request(t, "POST", s.URL+"/v1/hosts/"+id+"/live-connections", signedLiveSnapshot(t, id, id+" secret", liveSnapshot(1, apiLiveConnA)), testutil.ServerKey); status != 200 {
					t.Fatalf("snapshot: %d %s", status, body)
				}
			}
			session := login(t, s.URL, "limited", "correct horse")
			if len(session.Permissions) != 0 {
				t.Fatal("scoped grant leaked globally")
			}
			for _, path := range []string{"/v1/account", "/v1/account/refresh"} {
				method, body := "GET", ""
				if strings.HasSuffix(path, "refresh") {
					method = "POST"
					body = `{"refresh_token":"` + session.RefreshToken + `"}`
				}
				status, data := request(t, method, s.URL+path, body, session.Token)
				var payload struct {
					Permissions []string `json:"permissions"`
					Scoped      []struct {
						Kind        string   `json:"scope_kind"`
						Permissions []string `json:"permissions"`
					} `json:"scoped_permissions"`
				}
				if err := json.Unmarshal(data, &payload); err != nil {
					t.Fatal(err)
				}
				if status != 200 || len(payload.Permissions) != 0 || len(payload.Scoped) != 1 || payload.Scoped[0].Kind != kind || len(payload.Scoped[0].Permissions) != len(keys) {
					t.Fatalf("session grants: %d %s", status, data)
				}
				if method == "POST" {
					if err := json.Unmarshal(data, &session); err != nil {
						t.Fatal(err)
					}
				}
			}
			for _, path := range []string{"/v1/identities?limit=1", "/v1/identities?format=csv", "/v1/device-policies", "/v1/live-connections?limit=1"} {
				status, data := request(t, "GET", s.URL+path, "", session.Token)
				if status != 200 || !strings.Contains(string(data), "eastbox") || strings.Contains(string(data), "westbox") {
					t.Errorf("scoped %s: %d %s", path, status, data)
				}
				if strings.Contains(path, "limit=1") && !strings.Contains(string(data), "console_permissions") {
					t.Errorf("row permissions missing: %s", data)
				}
			}
			status, data := request(t, "GET", s.URL+"/v1/identities?offset=1&limit=1", "", session.Token)
			var page struct {
				Total      int               `json:"total"`
				Identities []json.RawMessage `json:"identities"`
			}
			if err := json.Unmarshal(data, &page); err != nil {
				t.Fatal(err)
			}
			if status != 200 || page.Total != 1 || len(page.Identities) != 0 {
				t.Fatalf("scope before offset: %d %s", status, data)
			}
			for _, path := range []string{"/v1/stats", "/v1/accounts", "/v1/admin-roles", "/v1/policies", "/v1/live-connections/westbox/" + apiLiveConnA} {
				if status, data := request(t, "GET", s.URL+path, "", session.Token); status != 403 {
					t.Errorf("global/cross-scope %s: %d %s", path, status, data)
				}
			}
			if status, data := request(t, "POST", s.URL+"/v1/identities/bulk", `{"ids":["eastbox","westbox"],"disabled":true}`, session.Token); status != 403 {
				t.Fatalf("cross-scope bulk: %d %s", status, data)
			}
			if record, _ := s.Registry.Identity("eastbox"); record.Disabled {
				t.Fatal("rejected bulk partially applied")
			}
			if status, data := request(t, "POST", s.URL+"/v1/identities/bulk", `{"ids":["eastbox"],"disabled":false}`, session.Token); status != 200 {
				t.Fatalf("scoped bulk: %d %s", status, data)
			}
			if _, err := s.Accounts.UnbindRole(binding.ID); err != nil {
				t.Fatal(err)
			}
			for _, path := range []string{"/v1/identities", "/v1/live-connections"} {
				if status, data := request(t, "GET", s.URL+path, "", session.Token); status != 403 {
					t.Fatalf("revoked list %s: %d %s", path, status, data)
				}
			}
			if status, data := request(t, "POST", s.URL+"/v1/live-connections/eastbox/"+apiLiveConnA+"/disconnect", `{"request_key":"revoked-disconnect"}`, session.Token); status != 403 {
				t.Fatalf("revoked action: %d %s", status, data)
			}
		})
	}
}

func TestPermissionRegressionEmergencyScope(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{AccessIssuer: newTestIssuer(t)})
	for _, name := range []string{"root", "limited"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatal(err)
		}
	}
	for _, id := range []string{"eastbox", "westbox"} {
		registerLiveHost(t, s.URL, id, id+" secret")
		if _, err := s.Accounts.SetDevicePolicy(id, "restricted", 6, []string{"root"}, nil, "root"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.AddGroupDevice("east", "eastbox", "root"); err != nil {
		t.Fatal(err)
	}
	role, err := s.Accounts.CreateAdminRole("emergency", "Emergency", "", []string{account.PermissionDevicesEmergencyAccess})
	if err != nil {
		t.Fatal(err)
	}
	binding, err := s.Accounts.BindRole("limited", role.ID, "group", "east", "")
	if err != nil {
		t.Fatal(err)
	}
	limited := login(t, s.URL, "limited", "correct horse").Token
	root := login(t, s.URL, "root", "correct horse").Token
	machine := createToken(t, s.URL, root, `{"name":"east emergency","role":"admin","resources":["devices"],"group_id":"east"}`).Secret
	for _, token := range []string{limited, machine} {
		for _, id := range []string{"eastbox", "westbox"} {
			want := 201
			if id == "westbox" {
				want = 403
			}
			status, data := request(t, "POST", s.URL+"/v1/emergency-access", emergencyRequestBody(t, id, bytes.Repeat([]byte{0x42}, 32), bytes.Repeat([]byte{0x43}, 32), 6, "incident"), token)
			if status != want {
				t.Errorf("emergency %s: %d %s", id, status, data)
			}
		}
	}
	if _, err := s.Accounts.UnbindRole(binding.ID); err != nil {
		t.Fatal(err)
	}
	if status, data := request(t, "POST", s.URL+"/v1/emergency-access", emergencyRequestBody(t, "eastbox", bytes.Repeat([]byte{0x42}, 32), bytes.Repeat([]byte{0x43}, 32), 6, "incident"), limited); status != 403 {
		t.Fatalf("revoked emergency: %d %s", status, data)
	}
}

func TestPermissionRegressionMachineDisconnectCeilings(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	for _, name := range []string{"root", "owner"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := s.Accounts.SetRole("owner", account.RoleAdmin); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.AddGroupDevice("east", "eastbox", "root"); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"eastbox", "westbox"} {
		registerLiveHost(t, s.URL, id, id+" secret")
		if status, body := request(t, "POST", s.URL+"/v1/hosts/"+id+"/live-connections", signedLiveSnapshot(t, id, id+" secret", liveSnapshot(1, apiLiveConnA)), testutil.ServerKey); status != 200 {
			t.Fatalf("snapshot %d %s", status, body)
		}
	}
	owner := login(t, s.URL, "owner", "correct horse").Token
	for i, tc := range []struct {
		role, resource, group, host string
		want                        int
	}{
		{"admin", "devices", "", "eastbox", 202}, {"operator", "devices", "", "eastbox", 202},
		{"readonly", "devices", "", "eastbox", 403}, {"admin", "accounts", "", "eastbox", 403},
		{"admin", "devices", "east", "eastbox", 202}, {"admin", "devices", "east", "westbox", 403},
	} {
		created := createToken(t, s.URL, owner, fmt.Sprintf(`{"name":"case-%d","role":%q,"resources":[%q],"group_id":%q}`, i, tc.role, tc.resource, tc.group))
		status, body := request(t, "POST", s.URL+"/v1/live-connections/"+tc.host+"/"+apiLiveConnA+"/disconnect", `{"request_key":"ceiling-disconnect"}`, created.Secret)
		if status != tc.want {
			t.Errorf("%+v: %d %s", tc, status, body)
		}
	}
	created := createToken(t, s.URL, owner, `{"name":"owner-demotion","role":"admin","resources":["devices"]}`)
	if _, err := s.Accounts.SetRole("owner", account.RoleReadonly); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, "POST", s.URL+"/v1/live-connections/eastbox/"+apiLiveConnA+"/disconnect", `{"request_key":"after-demotion"}`, created.Secret); status != 403 {
		t.Fatalf("demoted token: %d %s", status, body)
	}
	user, err := s.Accounts.FindUser("owner")
	if err != nil {
		t.Fatal(err)
	}
	if err := s.Accounts.RevokeAPIToken(user.ID, created.Token.ID); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, "GET", s.URL+"/v1/live-connections", "", created.Secret); status != 401 {
		t.Fatalf("revoked token: %d %s", status, body)
	}
}
