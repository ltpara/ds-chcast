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

type policyImportReply struct {
	Created   []policyObjectReply `json:"created"`
	Updated   []policyObjectReply `json:"updated"`
	Unchanged []policyObjectReply `json:"unchanged"`
}

type policyExportReply struct {
	Format   string `json:"format"`
	Version  int    `json:"version"`
	Policies []struct {
		Name              string   `json:"name"`
		Mode              string   `json:"mode"`
		PermissionCeiling uint32   `json:"permission_ceiling"`
		AllowedUsers      []string `json:"allowed_users"`
		AllowedGroups     []string `json:"allowed_groups"`
	} `json:"policies"`
}

func TestPolicyObjectImportExportRoutes(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.SetRole("bob", account.RoleOperator); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if _, err := s.Accounts.CreateGroup("lab", "Lab", "", 0, 0); err != nil {
		t.Fatalf("create lab: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "bob", "battery staple").Token

	for _, tc := range []struct {
		method string
		path   string
		body   string
	}{
		{http.MethodGet, "/v1/policies/export", ""},
		{http.MethodPost, "/v1/policies/import", `{"format":"chast-policy-objects","version":1,"policies":[]}`},
	} {
		if status, body := request(t, tc.method, s.URL+tc.path, tc.body, operator); status != http.StatusForbidden {
			t.Fatalf("operator %s %s = %d %s", tc.method, tc.path, status, body)
		}
		if status, body := request(t, tc.method, s.URL+tc.path, tc.body, ""); status != http.StatusUnauthorized {
			t.Fatalf("anonymous %s %s = %d %s", tc.method, tc.path, status, body)
		}
	}

	importBody := mustJSON(t, map[string]any{
		"format":  "chast-policy-objects",
		"version": 1,
		"policies": []map[string]any{
			{
				"name": "lab profile", "mode": "restricted", "permission_ceiling": 5,
				"allowed_users": []string{"alice"}, "allowed_groups": []string{"lab"},
			},
			{"name": "defaults", "mode": "open", "permission_ceiling": 0},
		},
	})
	status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import", importBody, admin)
	if status != http.StatusOK {
		t.Fatalf("create import: %d %s", status, body)
	}
	var imported policyImportReply
	if err := json.Unmarshal(body, &imported); err != nil {
		t.Fatalf("import reply: %v (%s)", err, body)
	}
	if len(imported.Created) != 2 || len(imported.Updated) != 0 || len(imported.Unchanged) != 0 {
		t.Fatalf("created import = %+v", imported)
	}
	var labProfile policyObjectReply
	for _, policy := range imported.Created {
		if policy.Name == "lab profile" {
			labProfile = policy
		}
	}
	if labProfile.ID == "" || labProfile.PolicyVersion != 1 || labProfile.PermissionCeiling != 5 ||
		len(labProfile.AllowedUsers) != 1 || len(labProfile.AllowedGroups) != 1 {
		t.Fatalf("imported lab profile = %+v", labProfile)
	}

	status, exportBody := request(t, http.MethodGet, s.URL+"/v1/policies/export", "", admin)
	if status != http.StatusOK {
		t.Fatalf("export policies: %d %s", status, exportBody)
	}
	if strings.Contains(string(exportBody), `"id"`) ||
		strings.Contains(string(exportBody), `"policy_version"`) ||
		strings.Contains(string(exportBody), `"updated_at"`) {
		t.Fatalf("export leaked local metadata: %s", exportBody)
	}
	var exported policyExportReply
	if err := json.Unmarshal(exportBody, &exported); err != nil {
		t.Fatalf("export reply: %v (%s)", err, exportBody)
	}
	if exported.Format != "chast-policy-objects" || exported.Version != 1 || len(exported.Policies) != 2 ||
		exported.Policies[0].Name != "defaults" || exported.Policies[1].Name != "lab profile" ||
		exported.Policies[0].AllowedUsers == nil || exported.Policies[0].AllowedGroups == nil {
		t.Fatalf("export = %+v", exported)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/policies/"+labProfile.ID, "", admin); status != http.StatusOK {
		t.Fatalf("detail route after export route: %d %s", status, body)
	}

	roundTrip := mustJSON(t, map[string]any{
		"format": "chast-policy-objects", "version": 1, "conflict": "update",
		"policies": exported.Policies,
	})
	status, body = request(t, http.MethodPost, s.URL+"/v1/policies/import", roundTrip, admin)
	if status != http.StatusOK {
		t.Fatalf("round-trip import: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &imported); err != nil ||
		len(imported.Created) != 0 || len(imported.Updated) != 0 || len(imported.Unchanged) != 2 {
		t.Fatalf("round-trip reply = %+v (%v)", imported, err)
	}
	for _, policy := range imported.Unchanged {
		if policy.Name == "lab profile" && policy.PolicyVersion != 1 {
			t.Fatalf("unchanged round-trip advanced version: %+v", policy)
		}
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import", string(exportBody), admin); status != http.StatusConflict {
		t.Fatalf("default reject conflict = %d %s", status, body)
	}

	invalid := []struct {
		name string
		body string
	}{
		{"unknown top-level field", `{"format":"chast-policy-objects","version":1,"policies":[],"extra":true}`},
		{"unknown policy field", `{"format":"chast-policy-objects","version":1,"policies":[{"name":"x","id":"local"}]}`},
		{"wrong format", `{"format":"other","version":1,"policies":[]}`},
		{"wrong version", `{"format":"chast-policy-objects","version":2,"policies":[]}`},
		{"missing policies", `{"format":"chast-policy-objects","version":1}`},
		{"trailing data", `{"format":"chast-policy-objects","version":1,"policies":[]} {}`},
		{"duplicate names", `{"format":"chast-policy-objects","version":1,"conflict":"update","policies":[{"name":"dup"},{"name":"dup"}]}`},
		{"invalid conflict", `{"format":"chast-policy-objects","version":1,"conflict":"merge","policies":[]}`},
	}
	for _, tc := range invalid {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import", tc.body, admin); status != http.StatusBadRequest {
			t.Fatalf("%s = %d %s", tc.name, status, body)
		}
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import",
		strings.Repeat(" ", (1<<20)+1), admin); status != http.StatusBadRequest {
		t.Fatalf("oversized import = %d %s", status, body)
	}

	// Materialise the assigned policy, then prove an import update reaches the
	// existing device row through reconcileAllPolicies.
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		mustJSON(t, map[string]any{
			"policy_id": labProfile.ID, "scope_kind": "device", "scope_value": "devbox",
		}), admin); status != http.StatusCreated {
		t.Fatalf("assign lab profile: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", time.Now().Unix()), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("re-register devbox: %d %s", status, body)
	}
	changedBody := mustJSON(t, map[string]any{
		"format": "chast-policy-objects", "version": 1, "conflict": "update",
		"policies": []map[string]any{{
			"name": "lab profile", "mode": "restricted", "permission_ceiling": 6,
			"allowed_users": []string{"alice"}, "allowed_groups": []string{"lab"},
		}},
	})
	status, body = request(t, http.MethodPost, s.URL+"/v1/policies/import", changedBody, admin)
	if status != http.StatusOK {
		t.Fatalf("update import: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &imported); err != nil || len(imported.Updated) != 1 ||
		imported.Updated[0].PolicyVersion != 2 {
		t.Fatalf("update import reply = %+v (%v)", imported, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("materialised policy: %d %s", status, body)
	}
	managed := decodeManagedPolicy(t, body)
	if !managed.Managed || managed.Policy == nil || managed.Policy.PermissionCeiling != 6 {
		t.Fatalf("reconciled policy = %+v", managed.Policy)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditPolicyObjectImport, 20)
	if err != nil || len(events) == 0 {
		t.Fatalf("import audit events = %+v, %v", events, err)
	}
	for _, event := range events {
		if event.Username != "alice" || !event.Success {
			t.Fatalf("import audit event = %+v", event)
		}
	}
}
