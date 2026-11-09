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

type apiTokenReply struct {
	ID           string   `json:"id"`
	Name         string   `json:"name"`
	Role         string   `json:"role"`
	Resources    []string `json:"resources"`
	GroupID      string   `json:"group_id"`
	ExpiresAtMS  int64    `json:"expires_at_ms"`
	LastUsedAtMS int64    `json:"last_used_at_ms"`
	Revoked      bool     `json:"revoked"`
}

type apiTokenCreateReply struct {
	Token  apiTokenReply `json:"token"`
	Secret string        `json:"secret"`
}

type apiTokenListReply struct {
	Tokens []apiTokenReply `json:"tokens"`
}

func createToken(t *testing.T, base, adminToken, body string) apiTokenCreateReply {
	t.Helper()
	status, data := request(t, http.MethodPost, base+"/v1/api-tokens", body, adminToken)
	if status != http.StatusCreated {
		t.Fatalf("create token: %d %s", status, data)
	}
	var reply apiTokenCreateReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("create token reply: %v", err)
	}
	if !strings.HasPrefix(reply.Secret, account.APITokenPrefix) || reply.Token.ID == "" {
		t.Fatalf("unexpected token reply: %+v", reply)
	}
	return reply
}

func TestAPITokenScopesAndLifecycle(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUserWithRole("bob", "battery staple", "", "", account.RoleOperator); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatalf("create east: %v", err)
	}
	if _, err := s.Accounts.CreateGroup("west", "West", "", 0, 0); err != nil {
		t.Fatalf("create west: %v", err)
	}
	adminToken := login(t, s.URL, "alice", "correct horse").Token
	bobToken := login(t, s.URL, "bob", "battery staple").Token

	devices := createToken(t, s.URL, adminToken,
		`{"name":"devices","role":"readonly","resources":["devices"],"expires_in_seconds":3600}`)

	if status, body := request(t, http.MethodGet, s.URL+"/v1/identities", "", devices.Secret); status != http.StatusOK {
		t.Fatalf("devices token read identities: %d %s", status, body)
	}
	for _, path := range []string{"/v1/audit", "/v1/accounts", "/v1/recordings", "/v1/stats"} {
		if status, body := request(t, http.MethodGet, s.URL+path, "", devices.Secret); status != http.StatusForbidden {
			t.Fatalf("%s with device token: %d %s", path, status, body)
		}
	}

	grouped := createToken(t, s.URL, adminToken,
		`{"name":"east ops","role":"operator","resources":["groups","audit"],"group_id":"east","expires_in_seconds":3600}`)
	if grouped.Token.GroupID != "east" {
		t.Fatalf("group = %q", grouped.Token.GroupID)
	}
	for _, path := range []string{"/v1/groups/east", "/v1/groups/east/members", "/v1/audit?group=east"} {
		if status, body := request(t, http.MethodGet, s.URL+path, "", grouped.Secret); status != http.StatusOK {
			t.Fatalf("%s with group token: %d %s", path, status, body)
		}
	}
	for _, path := range []string{"/v1/groups/west", "/v1/groups/west/members", "/v1/audit?group=west"} {
		if status, body := request(t, http.MethodGet, s.URL+path, "", grouped.Secret); status != http.StatusForbidden {
			t.Fatalf("%s with group token: %d %s", path, status, body)
		}
	}
	status, data := request(t, http.MethodGet, s.URL+"/v1/groups", "", grouped.Secret)
	if status != http.StatusOK {
		t.Fatalf("list groups with group token: %d %s", status, data)
	}
	var groups accountGroupListReply
	if err := json.Unmarshal(data, &groups); err != nil {
		t.Fatalf("group list: %v", err)
	}
	if len(groups.Groups) != 1 || groups.Groups[0].ID != "east" || groups.Groups[0].Role != "operator" {
		t.Fatalf("group-scoped listing = %+v", groups.Groups)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/groups/west", "", grouped.Secret); status != http.StatusForbidden {
		t.Fatalf("cross-group read: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit?group=west", "", grouped.Secret); status != http.StatusForbidden {
		t.Fatalf("cross-group audit: %d %s", status, body)
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/identities", "", "chast_api_unknown"); status != http.StatusUnauthorized {
		t.Fatalf("unknown token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/api-tokens",
		`{"name":"nope","role":"admin","resources":["all"]}`, bobToken); status != http.StatusForbidden {
		t.Fatalf("operator token create: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/api-tokens",
		`{"name":"root","role":"admin","resources":["all"]}`, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("server key token create: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/api-tokens",
		`{"name":"bad","role":"admin","resources":["devices","accounts"],"group_id":"east"}`, adminToken); status != http.StatusBadRequest {
		t.Fatalf("group-scoped account token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/api-tokens",
		`{"name":"wide","role":"admin","resources":["devices"],"group_id":"east"}`, adminToken); status != http.StatusBadRequest {
		t.Fatalf("group-scoped device token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/api-tokens",
		`{"name":"weak","role":"admin","resources":["devices"]}`, bobToken); status != http.StatusForbidden {
		t.Fatalf("outranking token: %d %s", status, body)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/api-tokens", "", adminToken)
	if status != http.StatusOK {
		t.Fatalf("list tokens: %d %s", status, data)
	}
	var listing apiTokenListReply
	if err := json.Unmarshal(data, &listing); err != nil {
		t.Fatalf("list tokens reply: %v", err)
	}
	if len(listing.Tokens) != 2 {
		t.Fatalf("token listing = %+v", listing.Tokens)
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit?action="+account.AuditAPITokenCreate, "", adminToken); status != http.StatusOK ||
		!strings.Contains(string(body), devices.Token.ID) || !strings.Contains(string(body), "alice") {
		t.Fatalf("token create audit: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/api-tokens/"+devices.Token.ID, "", adminToken); status != http.StatusNoContent {
		t.Fatalf("revoke token: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/identities", "", devices.Secret); status != http.StatusUnauthorized {
		t.Fatalf("revoked token read: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/api-tokens/missing", "", adminToken); status != http.StatusNotFound {
		t.Fatalf("revoke unknown: %d %s", status, body)
	}
}

func TestAPITokenExpiryAndOwnerDemotion(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUserWithRole("carol", "carol password", "", "", account.RoleAdmin); err != nil {
		t.Fatalf("create carol: %v", err)
	}
	adminToken := login(t, s.URL, "alice", "correct horse").Token
	token := createToken(t, s.URL, adminToken,
		`{"name":"nightly","role":"admin","resources":["all"],"expires_in_seconds":60}`)

	if status, body := request(t, http.MethodGet, s.URL+"/v1/accounts", "", token.Secret); status != http.StatusOK {
		t.Fatalf("admin token read accounts: %d %s", status, body)
	}

	if _, err := s.Accounts.SetRole("alice", account.RoleReadonly); err != nil {
		t.Fatalf("demote alice: %v", err)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/accounts", "", token.Secret); status != http.StatusForbidden {
		t.Fatalf("demoted token read accounts: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/identities", "", token.Secret); status != http.StatusOK {
		t.Fatalf("demoted token read devices: %d %s", status, body)
	}
	if _, err := s.Accounts.SetRole("alice", account.RoleAdmin); err != nil {
		t.Fatalf("restore alice: %v", err)
	}

	now := time.Now().UTC()
	s.Accounts.SetClock(func() time.Time { return now.Add(2 * time.Minute) })
	if status, body := request(t, http.MethodGet, s.URL+"/v1/identities", "", token.Secret); status != http.StatusUnauthorized {
		t.Fatalf("expired token read: %d %s", status, body)
	}
}
