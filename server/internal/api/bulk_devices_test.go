package api_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type bulkDeviceReply struct {
	Disabled bool     `json:"disabled"`
	Updated  []string `json:"updated"`
	Missing  []string `json:"missing"`
}

// TestBulkDeviceDisable covers the console's multi-select path: one call flips
// the administrative flag for every selected device, reports stale ids instead
// of failing, keeps the single-device admission rules and audits each change.
func TestBulkDeviceDisable(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	for _, id := range []string{"bulk-one", "bulk-two"} {
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, id, id+" secret", "AB12", now), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("register %s: %d %s", id, status, body)
		}
	}
	bulk := func(body, key string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodPost, s.URL+"/v1/identities/bulk", body, key)
	}

	for _, body := range []string{
		`{"ids":[],"disabled":true}`,
		`{"ids":["bulk-one"]}`,
		`{"ids":["bulk one"],"disabled":true}`,
		`{"ids":["bulk-one"],"disabled":true,"extra":1}`,
	} {
		if status, _ := bulk(body, testutil.ServerKey); status != http.StatusBadRequest {
			t.Fatalf("bulk %s = %d, want 400", body, status)
		}
	}
	ids := make([]string, 0, 201)
	for i := 0; i < 201; i++ {
		ids = append(ids, fmt.Sprintf(`"dev-%03d"`, i))
	}
	if status, _ := bulk(`{"ids":[`+strings.Join(ids, ",")+`],"disabled":true}`, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("bulk over the cap = %d, want 400", status)
	}

	// The first account of a fresh server becomes admin, so seed one explicitly
	// and keep the viewer account in the ordinary user role.
	if _, err := s.Accounts.CreateUserWithRole("rootadmin", "correct horse battery", "", "", account.RoleAdmin); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	if _, err := s.Accounts.CreateUser("viewer", "correct horse"); err != nil {
		t.Fatalf("create viewer: %v", err)
	}
	viewer := login(t, s.URL, "viewer", "correct horse").Token
	if status, body := bulk(`{"ids":["bulk-one"],"disabled":true}`, viewer); status != http.StatusForbidden {
		t.Fatalf("bulk with a user token = %d %s, want 403", status, body)
	}

	status, body := bulk(`{"ids":["bulk-one","bulk-two","ghost","bulk-one"],"disabled":true}`, testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("bulk disable: %d %s", status, body)
	}
	var disabled bulkDeviceReply
	if err := json.Unmarshal(body, &disabled); err != nil {
		t.Fatalf("bulk disable reply: %v", err)
	}
	if !disabled.Disabled || len(disabled.Updated) != 2 ||
		disabled.Updated[0] != "bulk-one" || disabled.Updated[1] != "bulk-two" ||
		len(disabled.Missing) != 1 || disabled.Missing[0] != "ghost" {
		t.Fatalf("bulk disable = %+v", disabled)
	}
	for _, id := range []string{"bulk-one", "bulk-two"} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/"+id, "", testutil.ServerKey); status != http.StatusForbidden ||
			!strings.Contains(string(body), "disabled") {
			t.Fatalf("disabled lookup %s = %d %s", id, status, body)
		}
	}
	events, err := s.Accounts.AuditEventsFor(account.AuditDeviceDisable, 10)
	if err != nil {
		t.Fatalf("list disable audit: %v", err)
	}
	if len(events) != 2 {
		t.Fatalf("disable audit = %d events, want 2: %+v", len(events), events)
	}
	seen := map[string]bool{}
	for _, event := range events {
		if !event.Success {
			t.Fatalf("disable audit marked failed: %+v", event)
		}
		for _, id := range []string{"bulk-one", "bulk-two"} {
			if strings.Contains(event.Detail, id) {
				seen[id] = true
			}
		}
	}
	if !seen["bulk-one"] || !seen["bulk-two"] {
		t.Fatalf("disable audit details = %+v", events)
	}

	status, body = bulk(`{"ids":["bulk-one","bulk-two"],"disabled":false}`, testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("bulk enable: %d %s", status, body)
	}
	var enabled bulkDeviceReply
	if err := json.Unmarshal(body, &enabled); err != nil {
		t.Fatalf("bulk enable reply: %v", err)
	}
	if enabled.Disabled || len(enabled.Updated) != 2 || len(enabled.Missing) != 0 {
		t.Fatalf("bulk enable = %+v", enabled)
	}
	for _, id := range []string{"bulk-one", "bulk-two"} {
		if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/"+id, "", testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("enabled lookup %s = %d %s", id, status, body)
		}
	}
}
