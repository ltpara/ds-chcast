package api_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// TestConsoleRoleMatrix pins the console authorization ladder: readonly may
// only read, operator may also change device state, admin alone may manage
// accounts, and ordinary viewer accounts get no console access at all.
func TestConsoleRoleMatrix(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("op", "operator pass"); err != nil {
		t.Fatalf("create op: %v", err)
	}
	if _, err := s.Accounts.SetRole("op", account.RoleOperator); err != nil {
		t.Fatalf("promote op: %v", err)
	}
	if _, err := s.Accounts.CreateUser("ro", "readonly pass"); err != nil {
		t.Fatalf("create ro: %v", err)
	}
	if _, err := s.Accounts.SetRole("ro", account.RoleReadonly); err != nil {
		t.Fatalf("set ro: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "viewer pass"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "op", "operator pass").Token
	readonly := login(t, s.URL, "ro", "readonly pass").Token
	viewer := login(t, s.URL, "bob", "viewer pass").Token

	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}

	for _, path := range []string{
		"/v1/identities",
		"/v1/stats",
		"/v1/audit",
		"/v1/audit/connections",
		"/v1/audit/file-transfers",
	} {
		for _, tc := range []struct {
			name string
			token string
			want int
		}{
			{"readonly", readonly, http.StatusOK},
			{"operator", operator, http.StatusOK},
			{"admin", admin, http.StatusOK},
			{"viewer", viewer, http.StatusForbidden},
			{"bearer junk", "not-a-token", http.StatusUnauthorized},
		} {
			if status, body := request(t, http.MethodGet, s.URL+path, "", tc.token); status != tc.want {
				t.Fatalf("%s as %s: %d %s", path, tc.name, status, body)
			}
		}
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"alias":"lab"}`, readonly); status != http.StatusForbidden {
		t.Fatalf("readonly label change: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"alias":"lab"}`, viewer); status != http.StatusForbidden {
		t.Fatalf("viewer label change: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"alias":"lab","tags":["vip"]}`, operator); status != http.StatusOK {
		t.Fatalf("operator label change: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", readonly); status != http.StatusForbidden {
		t.Fatalf("readonly lease removal: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", operator); status != http.StatusNoContent {
		t.Fatalf("operator lease removal: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/identities/devbox", "", readonly); status != http.StatusForbidden {
		t.Fatalf("readonly identity reset: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/identities/devbox", "", operator); status != http.StatusNoContent {
		t.Fatalf("operator identity reset: %d %s", status, body)
	}

	for _, tc := range []struct {
		name string
		token string
		want int
	}{
		{"readonly", readonly, http.StatusForbidden},
		{"operator", operator, http.StatusForbidden},
		{"admin", admin, http.StatusOK},
		{"viewer", viewer, http.StatusForbidden},
	} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/accounts", "", tc.token); status != tc.want {
			t.Fatalf("account list as %s: %d %s", tc.name, status, body)
		}
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","password":"carol pass","role":"operator"}`, operator); status != http.StatusForbidden {
		t.Fatalf("operator account create: %d %s", status, body)
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","password":"carol pass","role":"operator"}`, admin)
	if status != http.StatusCreated {
		t.Fatalf("admin account create: %d %s", status, body)
	}
	var carol adminAccountReply
	if err := json.Unmarshal(body, &carol); err != nil || carol.Role != account.RoleOperator {
		t.Fatalf("created carol = %+v (%v)", carol, err)
	}
	if login(t, s.URL, "carol", "carol pass").User.Role != account.RoleOperator {
		t.Fatal("operator role did not round-trip through login")
	}

	// Device writes by an account actor must leave a trail naming that actor.
	status, body = request(t, http.MethodGet, s.URL+"/v1/audit?action="+account.AuditDeviceLabels, "", admin)
	if status != http.StatusOK {
		t.Fatalf("device label audit: %d %s", status, body)
	}
	if !strings.Contains(string(body), `"username":"op"`) {
		t.Fatalf("device label audit does not name the operator: %s", body)
	}
}
