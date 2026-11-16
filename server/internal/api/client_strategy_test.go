package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestClientStrategyRoutesAndExport(t *testing.T) {
	s := testutil.Start(t)
	for _, name := range []string{"admin", "operator", "reader"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := s.Accounts.SetRole("operator", account.RoleOperator); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.SetRole("reader", account.RoleReadonly); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "admin", "correct horse").Token
	operator := login(t, s.URL, "operator", "correct horse").Token
	reader := login(t, s.URL, "reader", "correct horse").Token
	policy, err := s.Accounts.CreatePolicyObject("managed", "open", 0, nil, nil, "root")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreatePolicyAssignment(policy.ID, "all", "", 0, "root"); err != nil {
		t.Fatal(err)
	}
	endpoint := s.URL + "/v1/policies/" + policy.ID + "/client-strategy"
	for _, tc := range []struct {
		token  string
		status int
	}{{"", http.StatusUnauthorized}, {operator, http.StatusForbidden}, {reader, http.StatusForbidden}} {
		for _, method := range []string{http.MethodGet, http.MethodPut} {
			if status, body := request(t, method, endpoint, `{}`, tc.token); status != tc.status {
				t.Fatalf("unauthorized config: %d %s", status, body)
			}
		}
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/client-strategies/catalog", "", admin); status != http.StatusOK {
		t.Fatalf("catalog: %d %s", status, body)
	}
	for _, invalid := range []string{
		`{}`, `{"if_revision":0}`, `{"settings":{}}`, `{"if_revision":0,"settings":null}`,
		`{"if_revision":-1,"settings":{}}`,
		`{"if_revision":0,"settings":{"host.keep_awake":{"mode":"locked","value":"false"}}}`,
		`{"if_revision":0,"settings":{"unknown":{"mode":"locked","value":true}}}`,
		`{"if_revision":0,"settings":{"host.keep_awake":{"mode":"locked","value":false,"value":true}}}`,
	} {
		if status, body := request(t, http.MethodPut, endpoint, invalid, admin); status != http.StatusBadRequest {
			t.Fatalf("invalid %s: %d %s", invalid, status, body)
		}
	}
	valid := `{"if_revision":0,"settings":{"host.keep_awake":{"mode":"locked","value":false},"viewer.tunnel_mode":{"mode":"default","value":"relay"}}}`
	status, body := request(t, http.MethodPut, endpoint, valid, admin)
	if status != http.StatusOK {
		t.Fatalf("save: %d %s", status, body)
	}
	var saved account.ClientStrategy
	if err := json.Unmarshal(body, &saved); err != nil || saved.Revision != 1 || len(saved.Settings) != 2 {
		t.Fatalf("saved: %+v %v", saved, err)
	}
	if status, body := request(t, http.MethodPut, endpoint, valid, admin); status != http.StatusConflict {
		t.Fatalf("stale save: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", registerBody(t, "devbox", "secret", "AB12", time.Now().Unix()), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-strategies/devbox", "", reader)
	if status != http.StatusOK {
		t.Fatalf("preview: %d %s", status, body)
	}
	var snapshot account.ClientStrategySnapshot
	if err := json.Unmarshal(body, &snapshot); err != nil || snapshot.Revision != 1 || len(snapshot.Settings) != 1 {
		t.Fatalf("preview: %+v %v", snapshot, err)
	}
	if _, err := s.Accounts.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	grouped := createToken(t, s.URL, admin, `{"name":"east config","role":"admin","resources":["devices"],"group_id":"east","expires_in_seconds":3600}`)
	if status, body := request(t, http.MethodGet, s.URL+"/v1/device-strategies/devbox", "", grouped.Secret); status != http.StatusForbidden {
		t.Fatalf("cross-group preview: %d %s", status, body)
	}
	if _, err := s.Accounts.AddGroupDevice("east", "devbox", "root"); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/device-strategies/devbox", "", grouped.Secret); status != http.StatusOK {
		t.Fatalf("in-group preview: %d %s", status, body)
	}
	for _, method := range []string{http.MethodGet, http.MethodPut} {
		if status, body := request(t, method, endpoint, valid, grouped.Secret); status != http.StatusForbidden {
			t.Fatalf("group token accessed global strategy: %d %s", status, body)
		}
	}
	wrongResource := createToken(t, s.URL, admin, `{"name":"audit only","role":"admin","resources":["audit"],"expires_in_seconds":3600}`)
	if status, body := request(t, http.MethodGet, s.URL+"/v1/device-strategies/devbox", "", wrongResource.Secret); status != http.StatusForbidden {
		t.Fatalf("wrong resource preview: %d %s", status, body)
	}
	for _, tc := range []struct {
		path string
		code int
	}{
		{"/v1/device-strategies/unknown", http.StatusNotFound},
		{"/v1/device-strategies/devbox?target=bad", http.StatusBadRequest},
		{"/v1/policies/missing/client-strategy", http.StatusNotFound},
	} {
		if status, body := request(t, http.MethodGet, s.URL+tc.path, "", admin); status != tc.code {
			t.Fatalf("%s: %d %s", tc.path, status, body)
		}
	}
	status, exported := request(t, http.MethodGet, s.URL+"/v1/policies/export", "", admin)
	if status != http.StatusOK {
		t.Fatalf("export: %d %s", status, exported)
	}
	var doc map[string]any
	if err := json.Unmarshal(exported, &doc); err != nil || doc["version"] != float64(2) {
		t.Fatalf("export lost config: %s %v", exported, err)
	}
	// Reset and restore the exported overlay. Admission revision must not move.
	if status, body := request(t, http.MethodPut, endpoint, `{"if_revision":1,"settings":{}}`, admin); status != http.StatusOK {
		t.Fatalf("clear: %d %s", status, body)
	}
	doc["conflict"] = "update"
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import", mustJSON(t, doc), admin); status != http.StatusOK {
		t.Fatalf("import: %d %s", status, body)
	}
	current, err := s.Accounts.ClientStrategy(policy.ID)
	if err != nil || current.Revision != 3 || len(current.Settings) != 2 {
		t.Fatalf("restored: %+v %v", current, err)
	}
	unchanged, err := s.Accounts.PolicyObject(policy.ID)
	if err != nil || unchanged.PolicyVersion != policy.PolicyVersion {
		t.Fatalf("config import changed admission: %+v %v", unchanged, err)
	}
	legacy := `{"format":"chast-policy-objects","version":1,"conflict":"update","policies":[{"name":"managed","mode":"open","permission_ceiling":0}]}`
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import", legacy, admin); status != http.StatusOK {
		t.Fatalf("legacy import: %d %s", status, body)
	}
	current, err = s.Accounts.ClientStrategy(policy.ID)
	if err != nil || current.Revision != 3 || len(current.Settings) != 2 {
		t.Fatalf("legacy import erased config: %+v %v", current, err)
	}
	events, err := s.Accounts.AuditEventsFor(account.AuditClientStrategyUpdate, 20)
	if err != nil || len(events) != 2 {
		t.Fatalf("strategy audit = %d %v", len(events), err)
	}
}

func TestClientStrategyImportValidationIsAtomic(t *testing.T) {
	s := testutil.Start(t)
	invalid := `{"format":"chast-policy-objects","version":2,"policies":[{"name":"valid","client_settings":{}},{"name":"invalid","client_settings":{"host.max_viewers":{"mode":"locked","value":100}}}]}`
	if status, body := request(t, http.MethodPost, s.URL+"/v1/policies/import", invalid, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("invalid import: %d %s", status, body)
	}
	policies, err := s.Accounts.PolicyObjects()
	if err != nil || len(policies) != 0 {
		t.Fatalf("partial import committed: %+v %v", policies, err)
	}
}
