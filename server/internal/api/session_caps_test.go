package api_test

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type accountCapReply struct {
	ID                string `json:"id"`
	Username          string `json:"username"`
	PermissionCeiling uint32 `json:"permission_ceiling"`
}

// TestAccountSessionCapEndpoint proves an admin can cap one account's sessions,
// the console lists the cap, and the next access ticket for that account is
// clamped while another account stays uncapped.
func TestAccountSessionCapEndpoint(t *testing.T) {
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	bob := login(t, s.URL, "bob", "battery staple").Token

	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/permissions",
		`{"permission_ceiling":1}`, bob); status != http.StatusForbidden {
		t.Fatalf("ordinary user cap = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/ghost/permissions",
		`{"permission_ceiling":1}`, alice); status != http.StatusNotFound {
		t.Fatalf("unknown account cap = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/permissions",
		`{"permission_ceiling":-1}`, alice); status != http.StatusBadRequest {
		t.Fatalf("negative cap = %d %s", status, body)
	}

	status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/permissions",
		`{"permission_ceiling":3}`, alice)
	if status != http.StatusOK {
		t.Fatalf("set alice cap = %d %s", status, body)
	}
	var set accountCapReply
	if err := json.Unmarshal(body, &set); err != nil {
		t.Fatalf("set reply: %v", err)
	}
	if set.Username != "alice" || set.PermissionCeiling != 3 {
		t.Fatalf("set reply = %+v", set)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/accounts", "", alice)
	if status != http.StatusOK {
		t.Fatalf("list accounts = %d %s", status, body)
	}
	var list struct {
		Users []accountCapReply `json:"users"`
	}
	if err := json.Unmarshal(body, &list); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	caps := make(map[string]uint32, len(list.Users))
	for _, user := range list.Users {
		caps[user.Username] = user.PermissionCeiling
	}
	if caps["alice"] != 3 || caps["bob"] != 0 {
		t.Fatalf("listed caps = %v", caps)
	}

	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", "open", 0, nil, nil, "root"); err != nil {
		t.Fatalf("set open policy: %v", err)
	}

	clientKey := bytes.Repeat([]byte{0x42}, 32)
	permissionsOf := func(token string) uint32 {
		t.Helper()
		status, body := request(t, http.MethodPost, s.URL+"/v1/access-tickets",
			ticketRequestBody(t, "devbox", clientKey, 0xffffffff), token)
		if status != http.StatusCreated {
			t.Fatalf("issue ticket = %d %s", status, body)
		}
		reply := decodeAccessTicket(t, body)
		signed, err := base64.RawURLEncoding.DecodeString(reply.Ticket)
		if err != nil {
			t.Fatalf("ticket is not base64url: %v", err)
		}
		verifier := acl.NewVerifier(issuer.Name(), "device:devbox")
		if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
			t.Fatalf("pin key: %v", err)
		}
		ticket, err := verifier.Verify(signed, time.Now())
		if err != nil {
			t.Fatalf("verify ticket: %v", err)
		}
		return ticket.Permissions
	}
	if got := permissionsOf(alice); got != 3 {
		t.Fatalf("capped ticket permissions = %#x", got)
	}
	if got := permissionsOf(bob); got != 0xffffffff {
		t.Fatalf("uncapped ticket permissions = %#x", got)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/permissions",
		`{"permission_ceiling":0}`, alice); status != http.StatusOK {
		t.Fatalf("clear alice cap = %d %s", status, body)
	}
	if got := permissionsOf(alice); got != 0xffffffff {
		t.Fatalf("cleared ticket permissions = %#x", got)
	}
}
