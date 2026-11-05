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

type accountGroupReply struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	MemberQuota int    `json:"member_quota"`
	DeviceQuota int    `json:"device_quota"`
	MemberCount int    `json:"member_count"`
	DeviceCount int    `json:"device_count"`
	Role        string `json:"role"`
}

type accountGroupListReply struct {
	Groups     []accountGroupReply `json:"groups"`
	GlobalRole string              `json:"global_role"`
}

type accountGroupDetailReply struct {
	Group   accountGroupReply `json:"group"`
	Members []struct {
		Username string `json:"username"`
		Role     string `json:"role"`
	} `json:"members"`
	Devices []struct {
		PeerID  string `json:"peer_id"`
		AddedBy string `json:"added_by"`
	} `json:"devices"`
}

// TestAccountGroupHTTP covers the console permission ladder, both quota
// refusals and group-scoped history reads over HTTP. Accounts without a global
// console role reach only the groups they belong to.
func TestAccountGroupHTTP(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
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
	bob, err := s.Accounts.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.CreateUser("carol", "carol password"); err != nil {
		t.Fatalf("create carol: %v", err)
	}
	if _, err := s.Accounts.CreateUser("dave", "dave password"); err != nil {
		t.Fatalf("create dave: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "op", "operator pass").Token
	readonly := login(t, s.URL, "ro", "readonly pass").Token
	bobToken := login(t, s.URL, "bob", "battery staple").Token
	carolToken := login(t, s.URL, "carol", "carol password").Token
	daveToken := login(t, s.URL, "dave", "dave password").Token

	for _, tc := range []struct {
		name  string
		token string
	}{
		{"operator", operator},
		{"readonly", readonly},
		{"viewer", bobToken},
	} {
		status, body := request(t, http.MethodPost, s.URL+"/v1/groups",
			`{"id":"nope","name":"Nope"}`, tc.token)
		if status != http.StatusForbidden {
			t.Fatalf("%s group create: %d %s", tc.name, status, body)
		}
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/groups",
		`{"id":"lab","name":"Lab","description":"bench","member_quota":2,"device_quota":1}`, admin)
	if status != http.StatusCreated {
		t.Fatalf("create group: %d %s", status, body)
	}
	var group accountGroupReply
	if err := json.Unmarshal(body, &group); err != nil || group.ID != "lab" || group.Role != account.RoleAdmin {
		t.Fatalf("created group = %+v (%v)", group, err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups",
		`{"id":"lab","name":"Again"}`, admin); status != http.StatusConflict {
		t.Fatalf("duplicate group: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups",
		`{"id":"bad id","name":"Bad"}`, admin); status != http.StatusBadRequest {
		t.Fatalf("invalid group id: %d %s", status, body)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/groups/lab/members",
		`{"username":"bob","role":"readonly"}`, admin)
	if status != http.StatusCreated {
		t.Fatalf("add bob: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups/lab/members",
		`{"username":"carol","role":"operator"}`, admin); status != http.StatusCreated {
		t.Fatalf("add carol: %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/groups/lab/members",
		`{"username":"dave","role":"readonly"}`, admin)
	if status != http.StatusConflict || !strings.Contains(string(body), "limit 2") {
		t.Fatalf("member quota: %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/groups", "", bobToken)
	if status != http.StatusOK {
		t.Fatalf("bob group list: %d %s", status, body)
	}
	var list accountGroupListReply
	if err := json.Unmarshal(body, &list); err != nil || len(list.Groups) != 1 ||
		list.Groups[0].Role != account.RoleReadonly || list.GlobalRole != "" {
		t.Fatalf("bob groups = %+v (%v)", list, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/groups", "", daveToken)
	if status != http.StatusOK {
		t.Fatalf("dave group list: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &list); err != nil || len(list.Groups) != 0 {
		t.Fatalf("dave groups = %+v (%v)", list, err)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/groups/lab", "", bobToken); status != http.StatusOK {
		t.Fatalf("bob group detail: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/groups/lab", "", daveToken); status != http.StatusForbidden {
		t.Fatalf("dave group detail: %d %s", status, body)
	}

	// Readonly membership reads, operator membership writes devices, and only
	// admin membership manages members or the group itself.
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups/lab/devices",
		`{"peer_id":"devbox"}`, bobToken); status != http.StatusForbidden {
		t.Fatalf("readonly add device: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups/lab/devices",
		`{"peer_id":"devbox"}`, carolToken); status != http.StatusCreated {
		t.Fatalf("operator add device: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups/lab/members",
		`{"username":"dave","role":"readonly"}`, carolToken); status != http.StatusForbidden {
		t.Fatalf("operator add member: %d %s", status, body)
	}
	// Global admin and operator rights still apply across every group.
	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups/lab/devices",
		`{"peer_id":"devbox"}`, admin); status != http.StatusConflict {
		t.Fatalf("duplicate device: %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/groups/lab/devices",
		`{"peer_id":"devbox2"}`, admin)
	if status != http.StatusConflict || !strings.Contains(string(body), "limit 1") {
		t.Fatalf("device quota: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/groups/lab",
		`{"name":"Renamed"}`, bobToken); status != http.StatusForbidden {
		t.Fatalf("readonly rename: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/groups/lab",
		`{"name":"Lab"}`, admin); status != http.StatusOK {
		t.Fatalf("admin rename: %d %s", status, body)
	}

	// Both scoped history endpoints read only the group's accounts and devices.
	now := time.Now().UTC()
	if _, err := s.Accounts.RecordConnection(bob.ID, bob.Username, "peer-bob",
		"viewer", "127.0.0.1", "", now.Add(-time.Minute), now, true, ""); err != nil {
		t.Fatalf("record bob connection: %v", err)
	}
	if _, err := s.Accounts.RecordConnection(alice.ID, alice.Username, "peer-alice",
		"viewer", "127.0.0.1", "", now.Add(-time.Minute), now, true, ""); err != nil {
		t.Fatalf("record alice connection: %v", err)
	}
	if _, err := s.Accounts.RecordHostConnection("devbox", "",
		now.Add(-time.Minute), now, true, ""); err != nil {
		t.Fatalf("record host connection: %v", err)
	}
	for _, name := range []string{"alice", "bob", "carol"} {
		if err := s.Accounts.AppendAudit(account.AuditEvent{
			Username: name, Action: "group_scope_probe", Detail: name,
		}); err != nil {
			t.Fatalf("append audit %s: %v", name, err)
		}
	}

	status, body = request(t, http.MethodGet,
		s.URL+"/v1/audit/connections?group=lab", "", bobToken)
	if status != http.StatusOK {
		t.Fatalf("bob group connections: %d %s", status, body)
	}
	text := string(body)
	if !strings.Contains(text, `"username":"bob"`) || !strings.Contains(text, `"username":"-"`) ||
		strings.Contains(text, `"username":"alice"`) {
		t.Fatalf("group connections leaked: %s", body)
	}
	if status, body := request(t, http.MethodGet,
		s.URL+"/v1/audit/connections?group=lab", "", daveToken); status != http.StatusForbidden {
		t.Fatalf("non-member group connections: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet,
		s.URL+"/v1/audit?group=lab&action=group_scope_probe", "", bobToken)
	if status != http.StatusOK {
		t.Fatalf("bob group audit: %d %s", status, body)
	}
	text = string(body)
	if !strings.Contains(text, `"username":"bob"`) || !strings.Contains(text, `"username":"carol"`) ||
		strings.Contains(text, `"username":"alice"`) {
		t.Fatalf("group audit leaked: %s", body)
	}
	status, body = request(t, http.MethodGet,
		s.URL+"/v1/audit?format=csv&group=lab&action=group_scope_probe", "", bobToken)
	if status != http.StatusOK {
		t.Fatalf("bob group audit csv: %d %s", status, body)
	}
	text = string(body)
	if !strings.Contains(text, "bob") || strings.Contains(text, "alice") {
		t.Fatalf("group audit csv leaked: %s", body)
	}
	if status, body := request(t, http.MethodGet,
		s.URL+"/v1/audit?format=csv&group=lab", "", daveToken); status != http.StatusForbidden {
		t.Fatalf("non-member group audit csv: %d %s", status, body)
	}

	// The unfiltered trail keeps the stricter console rule.
	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit", "", bobToken); status != http.StatusForbidden {
		t.Fatalf("bob unfiltered audit: %d %s", status, body)
	}

	status, body = request(t, http.MethodPatch, s.URL+"/v1/groups/lab/members/bob",
		`{"role":"admin"}`, admin)
	if status != http.StatusOK {
		t.Fatalf("promote bob: %d %s", status, body)
	}
	status, body = request(t, http.MethodDelete, s.URL+"/v1/groups/lab/members/bob", "", admin)
	if status != http.StatusConflict || !strings.Contains(string(body), "group admin") {
		t.Fatalf("remove last group admin: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/groups/lab/members/carol",
		`{"role":"admin"}`, bobToken); status != http.StatusOK {
		t.Fatalf("group admin promotes carol: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/groups/lab/members/bob", "", admin); status != http.StatusNoContent {
		t.Fatalf("remove second group admin: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/groups/lab", "", carolToken); status != http.StatusNoContent {
		t.Fatalf("group admin delete: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/groups/lab", "", admin); status != http.StatusNotFound {
		t.Fatalf("deleted group detail: %d %s", status, body)
	}
}
