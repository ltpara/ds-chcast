package api_test

import (
	"encoding/json"
	"net/http"
	"reflect"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// Included in the SQLite and isolated PostgreSQL permission matrix.
func TestPermissionRegressionAdminRoleRevisionConflict(t *testing.T) {
	s := permissionRegressionServer(t, testutil.Options{})
	for _, name := range []string{"root", "reader"} {
		if _, err := s.Accounts.CreateUser(name, "correct horse"); err != nil {
			t.Fatal(err)
		}
	}
	token := login(t, s.URL, "root", "correct horse").Token
	initial := createNamedRole(t, s.URL, token, "shared", account.PermissionDevicesRead)
	bindNamedRole(t, s.URL, token, "reader", initial.ID, "global", "", "")
	reader := login(t, s.URL, "reader", "correct horse").Token
	path := s.URL + "/v1/admin-roles/" + initial.ID
	update := func(revision, name string) (int, []byte) {
		return request(t, http.MethodPut, path, mustJSON(t, map[string]any{"name": name, "permissions": []string{account.PermissionAuditRead}, "if_revision": revision}), token)
	}
	checkCode := func(status int, body []byte, want int, code string) {
		t.Helper()
		var reply struct {
			Code string `json:"code"`
		}
		if err := json.Unmarshal(body, &reply); err != nil || status != want || reply.Code != code {
			t.Fatalf("got %d %s, want %d %s", status, body, want, code)
		}
	}
	if len(initial.Revision) != 32 {
		t.Fatalf("creation missing revision: %+v", initial)
	}
	status, body := update("", "Missing")
	checkCode(status, body, http.StatusPreconditionRequired, "role_revision_required")
	status, body = request(t, http.MethodDelete, path, "", token)
	checkCode(status, body, http.StatusPreconditionRequired, "role_revision_required")
	status, body = update(initial.Revision, "First editor")
	if status != http.StatusOK {
		t.Fatalf("first update: %d %s", status, body)
	}
	var winner account.AdminRole
	if err := json.Unmarshal(body, &winner); err != nil {
		t.Fatal(err)
	}
	if winner.Revision == initial.Revision {
		t.Fatal("version was not advanced")
	}
	// Another editor still has the old permissions, name and revision.
	status, body = update(initial.Revision, "Second editor")
	checkCode(status, body, http.StatusConflict, "role_revision_conflict")
	status, body = request(t, http.MethodDelete, path+"?if_revision="+initial.Revision, "", token)
	checkCode(status, body, http.StatusConflict, "role_revision_conflict")
	persisted, err := s.Accounts.AdminRole(initial.ID)
	if err != nil || persisted.Revision != winner.Revision || persisted.Name != winner.Name || !reflect.DeepEqual(persisted.Permissions, winner.Permissions) {
		t.Fatalf("conflict overwrote winner: %+v %v", persisted, err)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/account", "", reader)
	var current struct {
		Permissions []string `json:"permissions"`
	}
	if err := json.Unmarshal(body, &current); err != nil || status != 200 || !reflect.DeepEqual(current.Permissions, []string{account.PermissionAuditRead}) {
		t.Fatalf("binding permissions: %d %s", status, body)
	}
	audits, err := s.Accounts.AuditEventsFor(account.AuditRoleUpdate, 20)
	if err != nil || len(audits) != 1 || !audits[0].Success {
		t.Fatalf("conflicts recorded as successful edits: %+v %v", audits, err)
	}
	// Read-only credentials still fail authorization even with the right version.
	if status, body := request(t, http.MethodPut, path, mustJSON(t, map[string]any{"name": "forbidden", "if_revision": winner.Revision}), reader); status != http.StatusForbidden {
		t.Fatalf("readonly update: %d %s", status, body)
	}
	status, body = update(winner.Revision, "Reviewed retry")
	if status != http.StatusOK {
		t.Fatalf("retry: %d %s", status, body)
	}
	if err := json.Unmarshal(body, &winner); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodDelete, path+"?if_revision="+winner.Revision, "", token); status != http.StatusNoContent {
		t.Fatalf("current delete: %d %s", status, body)
	}
	recreated := createNamedRole(t, s.URL, token, initial.ID, account.PermissionDevicesRead)
	if recreated.Revision == winner.Revision || recreated.Revision == initial.Revision {
		t.Fatal("recreated version reused")
	}
	status, body = update(winner.Revision, "Old incarnation")
	checkCode(status, body, http.StatusConflict, "role_revision_conflict")
	status, body = request(t, http.MethodDelete, path+"?if_revision="+winner.Revision, "", token)
	checkCode(status, body, http.StatusConflict, "role_revision_conflict")
}
