package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type batchLookupReply struct {
	Hosts []struct {
		ID string `json:"id"`
	} `json:"hosts"`
	Missing []string `json:"missing"`
	Denied  []string `json:"denied"`
}

func decodeBatchLookup(t *testing.T, data []byte) batchLookupReply {
	t.Helper()
	var reply batchLookupReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("batch lookup reply: %v", err)
	}
	return reply
}

// TestLookupHonorsDevicePolicy proves that a restricted device denies shared
// viewer credentials and accounts outside its ACL at lookup time. The host
// still performs local authentication; this only covers server-side admission.
func TestLookupHonorsDevicePolicy(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	bob := login(t, s.URL, "bob", "battery staple").Token

	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	lookup := func(token string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", token)
	}
	batch := func(token string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodGet, s.URL+"/v1/hosts?ids=devbox", "", token)
	}

	if status, body := lookup(testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("unmanaged shared lookup: %d %s", status, body)
	}
	if status, body := lookup(bob); status != http.StatusOK {
		t.Fatalf("unmanaged account lookup: %d %s", status, body)
	}

	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 0, []string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set restricted policy: %v", err)
	}
	if status, body := lookup(testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("restricted shared lookup = %d %s", status, body)
	}
	if status, body := lookup(alice); status != http.StatusOK {
		t.Fatalf("allowed account lookup = %d %s", status, body)
	}
	if status, body := lookup(bob); status != http.StatusForbidden {
		t.Fatalf("unlisted account lookup = %d %s", status, body)
	}

	status, body := batch(testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("restricted shared batch: %d %s", status, body)
	}
	shared := decodeBatchLookup(t, body)
	if len(shared.Hosts) != 0 || len(shared.Denied) != 1 || shared.Denied[0] != "devbox" {
		t.Fatalf("restricted shared batch = %+v", shared)
	}
	status, body = batch(alice)
	if status != http.StatusOK {
		t.Fatalf("allowed batch: %d %s", status, body)
	}
	allowed := decodeBatchLookup(t, body)
	if len(allowed.Hosts) != 1 || allowed.Hosts[0].ID != "devbox" || len(allowed.Denied) != 0 {
		t.Fatalf("allowed batch = %+v", allowed)
	}
	status, body = batch(bob)
	if status != http.StatusOK {
		t.Fatalf("unlisted batch: %d %s", status, body)
	}
	if denied := decodeBatchLookup(t, body); len(denied.Hosts) != 0 || len(denied.Denied) != 1 {
		t.Fatalf("unlisted batch = %+v", denied)
	}

	if _, err := s.Accounts.SetBanned("bob", true); err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	// Banning revokes the account's sessions, so the old token never reaches
	// the policy check. A still-valid token presented after a ban would be
	// denied by CheckDeviceAdmission as well.
	if status, body := lookup(bob); status != http.StatusUnauthorized {
		t.Fatalf("banned account lookup = %d %s", status, body)
	}
	if _, err := s.Accounts.SetBanned("bob", false); err != nil {
		t.Fatalf("unban bob: %v", err)
	}
	bob = login(t, s.URL, "bob", "battery staple").Token

	if _, err := s.Accounts.SetDevicePolicy("devbox", "open", 0, nil, nil, "root"); err != nil {
		t.Fatalf("set open policy: %v", err)
	}
	if status, body := lookup(testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("open shared lookup: %d %s", status, body)
	}
	if status, body := lookup(bob); status != http.StatusOK {
		t.Fatalf("open account lookup: %d %s", status, body)
	}
}
