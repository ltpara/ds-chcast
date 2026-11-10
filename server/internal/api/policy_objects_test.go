package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type policyObjectReply struct {
	ID                string   `json:"id"`
	Name              string   `json:"name"`
	Mode              string   `json:"mode"`
	PermissionCeiling uint32   `json:"permission_ceiling"`
	PolicyVersion     uint32   `json:"policy_version"`
	AllowedUsers      []string `json:"allowed_users"`
	AllowedGroups     []string `json:"allowed_groups"`
	UpdatedAtMS       int64    `json:"updated_at_ms"`
}

type policyAssignmentReply struct {
	ID         string `json:"id"`
	PolicyID   string `json:"policy_id"`
	PolicyName string `json:"policy_name"`
	ScopeKind  string `json:"scope_kind"`
	ScopeValue string `json:"scope_value"`
	Priority   int    `json:"priority"`
}

type managedPolicyReply struct {
	Managed bool `json:"managed"`
	Policy  *struct {
		PolicyVersion     uint32   `json:"policy_version"`
		Mode              string   `json:"mode"`
		PermissionCeiling uint32   `json:"permission_ceiling"`
		AllowedUsers      []string `json:"allowed_users"`
		Source            *struct {
			Kind     string `json:"kind"`
			ID       string `json:"id"`
			Name     string `json:"name"`
			Version  uint32 `json:"version"`
			Priority int    `json:"priority"`
		} `json:"source"`
	} `json:"policy"`
}

type registrationPolicyReply struct {
	DevicePolicy *struct {
		Mode              string `json:"mode"`
		PermissionCeiling uint32 `json:"permission_ceiling"`
		PolicyVersion     uint32 `json:"policy_version"`
	} `json:"device_policy"`
}

func decodeManagedPolicy(t *testing.T, body []byte) managedPolicyReply {
	t.Helper()
	var reply managedPolicyReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("policy reply: %v (%s)", err, body)
	}
	return reply
}

func mustJSON(t *testing.T, value any) string {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// TestPolicyObjectRoutes covers the console lifecycle of named policies and
// their assignments: admin-only access, validation, materialisation into the
// existing device-policy row, event pushes, manual overrides and the monotonic
// per-device version across a manual delete.
func TestPolicyObjectRoutes(t *testing.T) {
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
	admin := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "bob", "battery staple").Token

	now := time.Now().Unix()
	for _, peer := range []string{"devbox", "laptop"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, peer, peer+" secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("register %s: %d %s", peer, status, body)
		}
	}
	base := pollHostEvents(t, s.URL, "devbox", "devbox secret", 0)

	for _, tc := range []struct {
		method string
		path   string
	}{
		{http.MethodGet, "/v1/policies"},
		{http.MethodPost, "/v1/policies"},
		{http.MethodGet, "/v1/policy-assignments"},
		{http.MethodPost, "/v1/policy-assignments"},
	} {
		if status, body := request(t, tc.method, s.URL+tc.path, `{"name":"x"}`, operator); status != http.StatusForbidden {
			t.Fatalf("operator %s %s = %d %s", tc.method, tc.path, status, body)
		}
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/policies", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("anonymous list = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies", `{"name":""}`, admin); status != http.StatusBadRequest {
		t.Fatalf("blank name = %d %s", status, body)
	}

	status, body := request(t, http.MethodPost, s.URL+"/v1/policies",
		`{"name":"lab profile","mode":"restricted","permission_ceiling":5,"allowed_users":["alice"]}`, admin)
	if status != http.StatusCreated {
		t.Fatalf("create policy: %d %s", status, body)
	}
	var profile policyObjectReply
	if err := json.Unmarshal(body, &profile); err != nil {
		t.Fatalf("policy reply: %v", err)
	}
	if profile.ID == "" || profile.PolicyVersion != 1 || profile.Mode != "restricted" ||
		profile.PermissionCeiling != 5 || len(profile.AllowedUsers) != 1 || profile.UpdatedAtMS == 0 {
		t.Fatalf("created policy = %+v", profile)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies",
		`{"name":"lab profile"}`, admin); status != http.StatusConflict {
		t.Fatalf("duplicate policy = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, s.URL+"/v1/policies/"+profile.ID,
		`{"name":"lab profile","mode":"open","permission_ceiling":7}`, admin); status != http.StatusOK {
		t.Fatalf("update policy: %d %s", status, body)
	} else if updated := json.Unmarshal(body, &profile); updated != nil || profile.PolicyVersion != 2 || profile.PermissionCeiling != 7 {
		t.Fatalf("updated policy = %+v", profile)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/policies/nope", "", admin); status != http.StatusNotFound {
		t.Fatalf("unknown policy = %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/policies", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list policies: %d %s", status, body)
	}
	var list struct {
		Policies []policyObjectReply `json:"policies"`
	}
	if err := json.Unmarshal(body, &list); err != nil || len(list.Policies) != 1 || list.Policies[0].ID != profile.ID {
		t.Fatalf("policy list = %+v (%v)", list.Policies, err)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/groups",
		`{"id":"lab","name":"Lab","device_quota":4}`, admin); status != http.StatusCreated {
		t.Fatalf("create group: %d %s", status, body)
	}
	for _, peer := range []string{"devbox", "laptop"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/groups/lab/devices",
			mustJSON(t, map[string]any{"peer_id": peer}), admin); status != http.StatusCreated {
			t.Fatalf("add %s to group: %d %s", peer, status, body)
		}
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		`{"policy_id":"nope","scope_kind":"all"}`, admin); status != http.StatusNotFound {
		t.Fatalf("unknown policy assignment = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		mustJSON(t, map[string]any{"policy_id": profile.ID, "scope_kind": "group", "scope_value": "missing"}), admin); status != http.StatusNotFound {
		t.Fatalf("unknown group assignment = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		mustJSON(t, map[string]any{"policy_id": profile.ID, "scope_kind": "team"}), admin); status != http.StatusBadRequest {
		t.Fatalf("bad scope assignment = %d %s", status, body)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		mustJSON(t, map[string]any{"policy_id": profile.ID, "scope_kind": "group", "scope_value": "lab", "priority": 10}), admin)
	if status != http.StatusCreated {
		t.Fatalf("create assignment: %d %s", status, body)
	}
	var assignment policyAssignmentReply
	if err := json.Unmarshal(body, &assignment); err != nil {
		t.Fatalf("assignment reply: %v", err)
	}
	if assignment.PolicyName != "lab profile" || assignment.ScopeKind != "group" ||
		assignment.ScopeValue != "lab" || assignment.Priority != 10 {
		t.Fatalf("assignment = %+v", assignment)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		mustJSON(t, map[string]any{"policy_id": profile.ID, "scope_kind": "group", "scope_value": "lab"}), admin); status != http.StatusConflict {
		t.Fatalf("duplicate assignment = %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/policy-assignments", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list assignments: %d %s", status, body)
	}
	var assignments struct {
		Assignments []policyAssignmentReply `json:"assignments"`
	}
	if err := json.Unmarshal(body, &assignments); err != nil || len(assignments.Assignments) != 1 ||
		assignments.Assignments[0].ID != assignment.ID {
		t.Fatalf("assignment list = %+v (%v)", assignments.Assignments, err)
	}

	// The assignment materialises into the row admission already reads, and the
	// host event stream carries the new version.
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("materialised policy: %d %s", status, body)
	}
	managed := decodeManagedPolicy(t, body)
	if !managed.Managed || managed.Policy.PolicyVersion != 1 || managed.Policy.Mode != "open" ||
		managed.Policy.PermissionCeiling != 7 || managed.Policy.Source == nil ||
		managed.Policy.Source.ID != profile.ID || managed.Policy.Source.Name != "lab profile" ||
		managed.Policy.Source.Version != 2 || managed.Policy.Source.Priority != 10 {
		t.Fatalf("materialised policy = %+v source %+v", managed.Policy, managed.Policy)
	}
	pushed := pollHostEvents(t, s.URL, "devbox", "devbox secret", base.Cursor)
	if len(pushed.Events) != 1 || pushed.Events[0].Kind != "policy" ||
		pushed.Events[0].Managed == nil || !*pushed.Events[0].Managed || pushed.Events[0].PolicyVersion != 1 {
		t.Fatalf("assignment push = %+v", pushed)
	}
	base = pushed

	// A device whose row was never materialised still learns the assignment
	// when it registers: registration reconciles before reading the policy.
	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "laptop", "laptop secret", "CD34", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("re-register laptop: %d %s", status, body)
	}
	var registration registrationPolicyReply
	if err := json.Unmarshal(body, &registration); err != nil {
		t.Fatalf("register reply: %v", err)
	}
	if registration.DevicePolicy == nil || registration.DevicePolicy.PolicyVersion == 0 ||
		registration.DevicePolicy.PermissionCeiling != 7 || registration.DevicePolicy.Mode != "open" {
		t.Fatalf("registration policy = %+v", registration.DevicePolicy)
	}

	// A manual per-device policy wins over the assignment and is not pushed
	// over by later reconciles.
	if status, body := request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		`{"mode":"restricted","allowed_users":["bob"]}`, admin); status != http.StatusOK {
		t.Fatalf("manual policy: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, s.URL+"/v1/policies/"+profile.ID,
		`{"name":"lab profile","mode":"restricted","permission_ceiling":3,"allowed_users":["alice"]}`, admin); status != http.StatusOK {
		t.Fatalf("edit assigned policy: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("manual detail: %d %s", status, body)
	}
	managed = decodeManagedPolicy(t, body)
	if !managed.Managed || managed.Policy.Mode != "restricted" || managed.Policy.Source != nil ||
		managed.Policy.AllowedUsers[0] != "bob" {
		t.Fatalf("manual override = %+v", managed.Policy)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/policies/"+profile.ID, "", admin); status != http.StatusConflict {
		t.Fatalf("delete in-use policy = %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/policy-assignments/"+assignment.ID, "", admin); status != http.StatusNoContent {
		t.Fatalf("delete assignment: %d %s", status, body)
	}
	// desk loses the assignment; devbox keeps the manual policy.
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/desk", "", admin)
	if status != http.StatusOK {
		t.Fatalf("desk after assignment delete: %d %s", status, body)
	}
	if reply := decodeManagedPolicy(t, body); reply.Managed {
		t.Fatalf("desk still managed: %+v", reply)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/policy-assignments",
		mustJSON(t, map[string]any{"policy_id": profile.ID, "scope_kind": "device", "scope_value": "devbox", "priority": 1}), admin)
	if status != http.StatusCreated {
		t.Fatalf("device assignment: %d %s", status, body)
	}
	var deviceAssignment policyAssignmentReply
	if err := json.Unmarshal(body, &deviceAssignment); err != nil {
		t.Fatalf("device assignment reply: %v", err)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/device-policies/devbox", "", admin); status != http.StatusNoContent {
		t.Fatalf("delete manual: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("assigned after manual delete: %d %s", status, body)
	}
	managed = decodeManagedPolicy(t, body)
	if !managed.Managed || managed.Policy.Source == nil || managed.Policy.Source.ID != profile.ID ||
		managed.Policy.PolicyVersion <= 2 {
		t.Fatalf("assigned after manual delete = %+v source %+v", managed.Policy, managed.Policy)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/policy-assignments/"+deviceAssignment.ID, "", admin); status != http.StatusNoContent {
		t.Fatalf("delete device assignment: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/policies/"+profile.ID, "", admin); status != http.StatusNoContent {
		t.Fatalf("delete policy: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/policies", "", admin)
	if status != http.StatusOK {
		t.Fatalf("policy list after delete: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &list); err != nil || len(list.Policies) != 0 {
		t.Fatalf("policies after delete = %+v (%v)", list.Policies, err)
	}
}
