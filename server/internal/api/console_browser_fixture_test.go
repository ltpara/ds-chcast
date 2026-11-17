package api_test

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/http/httputil"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// TestConsoleBrowserFixture is an opt-in manual browser fixture, not an
// automated acceptance result. The directory must be a new, empty directory.
// Read fixture.json for its loopback URL and synthetic accounts. Create stop
// in that directory to shut down. Tests normally skip this fixture.
func TestConsoleBrowserFixture(t *testing.T) {
	dir := os.Getenv("CHAST_BROWSER_FIXTURE_DIR")
	if dir == "" {
		t.Skip("CHAST_BROWSER_FIXTURE_DIR is not set (manual browser fixture)")
	}
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) != 0 {
		t.Fatalf("fixture directory must exist and be empty: %v", err)
	}
	s := testutil.Start(t)
	const password = "console-test-password"
	users := []string{"root", "manager", "reader", "east-user", "west-user", "ordinary"}
	for _, name := range users {
		if _, err := s.Accounts.CreateUser(name, password); err != nil {
			t.Fatal(err)
		}
	}
	for _, id := range []string{"east", "west"} {
		if _, err := s.Accounts.CreateGroup(id, strings.ToUpper(id), "browser fixture", 0, 0); err != nil {
			t.Fatal(err)
		}
		if _, err := s.Accounts.AddGroupDevice(id, id+"box", "root"); err != nil {
			t.Fatal(err)
		}
		registerLiveHost(t, s.URL, id+"box", id+"box secret")
	}
	for _, role := range []struct {
		id   string
		keys []string
	}{
		{"named-manager", account.AllPermissions()},
		{"console-reader", []string{account.PermissionRolesRead, account.PermissionDevicesRead, account.PermissionConnectionsRead}},
		{"scoped-reader", []string{account.PermissionDevicesRead, account.PermissionConnectionsRead}},
	} {
		if _, err := s.Accounts.CreateAdminRole(role.id, role.id, "browser fixture", role.keys); err != nil {
			t.Fatal(err)
		}
	}
	for _, binding := range []struct{ user, role, scope, value, kind string }{
		{"manager", "named-manager", "global", "", ""},
		{"reader", "console-reader", "global", "", ""},
		{"east-user", "scoped-reader", "group", "east", ""},
		{"west-user", "scoped-reader", "object", "westbox", "device"},
	} {
		if _, err := s.Accounts.BindRole(binding.user, binding.role, binding.scope, binding.value, binding.kind); err != nil {
			t.Fatal(err)
		}
	}
	refreshHosts := func(sequence uint64) {
		for _, id := range []string{"eastbox", "westbox"} {
			registerLiveHost(t, s.URL, id, id+" secret")
			if _, err := s.Accounts.ApplyLiveSnapshot(id, liveSnapshot(sequence, apiLiveConnA, apiLiveConnB)); err != nil {
				t.Fatal(err)
			}
		}
	}
	refreshHosts(1)
	var managementTick func(uint64)
	if os.Getenv("CHAST_BROWSER_MANAGEMENT") == "1" {
		managementTick = setupManagementBrowserFixture(t, s, dir)
	}
	target, err := url.Parse(s.URL)
	if err != nil {
		t.Fatal(err)
	}
	proxy := httputil.NewSingleHostReverseProxy(target)
	exists := func(name string) bool {
		_, err := os.Stat(filepath.Join(dir, name))
		return err == nil
	}
	// Fault injection is confined to this loopback test proxy. Production
	// handlers and embedded assets remain unchanged.
	front := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if managementTick != nil {
			isStrategy := strings.Contains(r.URL.Path, "client-strategy") || strings.HasPrefix(r.URL.Path, "/v1/device-strategies") || r.URL.Path == "/v1/policies"
			isDetail := strings.HasPrefix(r.URL.Path, "/v1/live-connections/") && r.Method == http.MethodGet
			isDisconnect := strings.HasSuffix(r.URL.Path, "/disconnect")
			if (isDetail && exists("fail-connection-detail")) || (isStrategy && r.Method != http.MethodGet && exists("fail-strategy-save")) {
				http.Error(w, `{"error":"fixture: management unavailable"}`, http.StatusServiceUnavailable)
				return
			}
			if (isDetail && exists("delay-connection-detail")) || (isDisconnect && exists("delay-disconnect")) || (isStrategy && exists("delay-strategy")) {
				select {
				case <-r.Context().Done():
					return
				case <-time.After(4 * time.Second):
				}
			}
			if isDisconnect && exists("lose-disconnect-response") {
				// Execute the real mutation but lose the response at the proxy.
				recorded := httptest.NewRecorder()
				proxy.ServeHTTP(recorded, r)
				http.Error(w, `{"error":"fixture: disconnect response lost"}`, http.StatusBadGateway)
				return
			}
		}
		if strings.HasPrefix(r.URL.Path, "/v1/admin-roles") || strings.HasPrefix(r.URL.Path, "/v1/role-bindings") {
			if r.Method != http.MethodGet && exists("fail-role-save") {
				http.Error(w, `{"error":"fixture: role save unavailable"}`, http.StatusServiceUnavailable)
				return
			}
			if (r.Method == http.MethodGet && exists("delay-role-list")) ||
				(r.Method != http.MethodGet && exists("delay-role-save")) {
				select {
				case <-r.Context().Done():
					return
				case <-time.After(4 * time.Second):
				}
			}
		}
		proxy.ServeHTTP(w, r)
	}))
	defer front.Close()
	manifest, err := json.MarshalIndent(map[string]any{
		"url": front.URL + "/admin/", "api_url": s.URL,
		"users": users, "password": password,
		"note": "Synthetic loopback fixture; no production data. Create stop to shut down.",
	}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "fixture.json"), manifest, 0600); err != nil {
		t.Fatal(err)
	}
	t.Logf("manual browser fixture ready: %s/admin/", front.URL)
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for sequence := uint64(2); ; sequence++ {
		<-ticker.C
		if exists("stop") {
			return
		}
		if managementTick != nil {
			managementTick(sequence)
		} else if sequence%10 == 0 {
			refreshHosts(sequence)
		}
	}
}
