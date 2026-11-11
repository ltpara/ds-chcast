package api_test

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type deviceStateReply struct {
	ID       string   `json:"id"`
	Alias    string   `json:"alias"`
	Tags     []string `json:"tags"`
	Disabled bool     `json:"disabled"`
	Owner    string   `json:"owner"`
}

func decodeDeviceState(t *testing.T, body []byte) deviceStateReply {
	t.Helper()
	var reply deviceStateReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("device state reply: %v (%s)", err, body)
	}
	return reply
}

// TestDeviceDisableDropsLeaseAndBlocksRegistration proves a central disable is
// immediate: the lease and relay slot go away, a renewal is refused, labels
// stay, and enabling restores registration.
func TestDeviceDisableDropsLeaseAndBlocksRegistration(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"alias":"Lab","tags":["prod"]}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("labels: %d %s", status, body)
	}

	status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"disabled":true}`, testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("disable: %d %s", status, body)
	}
	if reply := decodeDeviceState(t, body); !reply.Disabled || reply.Alias != "Lab" || len(reply.Tags) != 1 {
		t.Fatalf("disable reply = %+v", reply)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey); status != http.StatusForbidden ||
		!strings.Contains(string(body), "disabled") {
		t.Fatalf("lookup after disable: %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusForbidden || !strings.Contains(string(body), "disabled") {
		t.Fatalf("registration while disabled: %d %s", status, body)
	}

	_, list := listIdentities(t, s.URL, "", testutil.ServerKey)
	if list.Total != 1 || list.Online != 0 || len(list.Identities) != 1 {
		t.Fatalf("list after disable = %+v", list)
	}
	if entry := list.Identities[0]; !entry.Disabled || entry.Online || entry.Alias != "Lab" {
		t.Fatalf("disabled entry = %+v", entry)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"disabled":false}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("enable: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", time.Now().Unix()), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("registration after enable: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/ghost",
		`{"disabled":true}`, testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("disable an unknown device: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{}`, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("empty patch: %d %s", status, body)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditDeviceDisable, 10)
	if err != nil {
		t.Fatalf("disable audit: %v", err)
	}
	if len(events) != 1 || !events[0].Success || events[0].Username != "server-key" {
		t.Fatalf("disable audit = %+v", events)
	}
	events, err = s.Accounts.AuditEventsFor(account.AuditDeviceEnable, 10)
	if err != nil {
		t.Fatalf("enable audit: %v", err)
	}
	if len(events) != 1 || !events[0].Success {
		t.Fatalf("enable audit = %+v", events)
	}
}

// TestDeviceOwnerAuthorizesRestrictedDevice proves the owner assigned to a
// device may look it up and receive an access ticket even when the policy
// allow list names somebody else, while other accounts and a cleared owner do
// not.
func TestDeviceOwnerAuthorizesRestrictedDevice(t *testing.T) {
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	for _, user := range []struct{ name, password string }{
		{"alice", "correct horse"},
		{"bob", "battery staple"},
		{"carol", "third account"},
	} {
		if _, err := s.Accounts.CreateUser(user.name, user.password); err != nil {
			t.Fatalf("create %s: %v", user.name, err)
		}
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	bob := login(t, s.URL, "bob", "battery staple").Token
	carol := login(t, s.URL, "carol", "third account").Token
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 0, []string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set restricted policy: %v", err)
	}
	lookup := func(token string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", token)
	}
	if status, body := lookup(alice); status != http.StatusOK {
		t.Fatalf("allow-listed lookup: %d %s", status, body)
	}
	if status, body := lookup(bob); status != http.StatusForbidden {
		t.Fatalf("non-owner lookup: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"owner":"bob"}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("set owner: %d %s", status, body)
	} else if reply := decodeDeviceState(t, body); reply.Owner != "bob" {
		t.Fatalf("owner reply = %+v", reply)
	}
	if status, body := lookup(bob); status != http.StatusOK {
		t.Fatalf("owner lookup: %d %s", status, body)
	}
	if status, body := lookup(carol); status != http.StatusForbidden {
		t.Fatalf("unrelated lookup: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet,
		s.URL+"/v1/hosts/devbox?client_public_key="+base64.RawURLEncoding.EncodeToString(bytes.Repeat([]byte{0x42}, 32)),
		"", bob); status != http.StatusOK || !strings.Contains(string(body), `"access_ticket"`) {
		t.Fatalf("owner lookup ticket: %d %s", status, body)
	}

	clientKey := bytes.Repeat([]byte{0x42}, 32)
	issue := func(token string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodPost, s.URL+"/v1/access-tickets",
			ticketRequestBody(t, "devbox", clientKey, 1), token)
	}
	if status, body := issue(bob); status != http.StatusCreated {
		t.Fatalf("owner ticket: %d %s", status, body)
	}
	if status, body := issue(carol); status != http.StatusForbidden {
		t.Fatalf("unrelated ticket: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"owner":"ghost"}`, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("unknown owner: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"owner":""}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("clear owner: %d %s", status, body)
	}
	if status, body := lookup(bob); status != http.StatusForbidden {
		t.Fatalf("lookup after clearing owner: %d %s", status, body)
	}

	// A disabled device refuses the owner's ticket before any policy work.
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"owner":"bob","disabled":true}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("owner plus disable: %d %s", status, body)
	}
	if status, body := issue(bob); status != http.StatusForbidden || !strings.Contains(string(body), "disabled") {
		t.Fatalf("owner ticket on a disabled device: %d %s", status, body)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditDeviceOwner, 10)
	if err != nil {
		t.Fatalf("owner audit: %v", err)
	}
	if len(events) != 3 || !events[0].Success || !strings.Contains(events[0].Detail, "bob") {
		t.Fatalf("owner audit = %+v", events)
	}
}

// TestDeviceDisableRefusesWake keeps a disabled device from being woken: the
// lease is gone but the persisted wake link would otherwise still queue a job.
func TestDeviceDisableRefusesWake(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{WakeRelays: map[string][]string{"targetbox": {"proxy"}}})
	now := time.Now().Unix()
	registerWakeAgent(t, s.URL, "targetbox", now)
	registerWakeAgentLink(t, s.URL, "proxy", now+1, true, []string{wakeLink})

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/targetbox",
		`{"disabled":true}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("disable targetbox: %d %s", status, body)
	}
	status, _, body := postWake(t, s.URL, "targetbox", "aa-bb-cc-dd-ee-ff", testutil.ServerKey)
	if status != http.StatusConflict || !strings.Contains(string(body), "disabled") {
		t.Fatalf("wake while disabled: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/targetbox",
		`{"disabled":false}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("enable targetbox: %d %s", status, body)
	}
	if status, _, body := postWake(t, s.URL, "targetbox", "aa-bb-cc-dd-ee-ff", testutil.ServerKey); status != http.StatusAccepted {
		t.Fatalf("wake after enable: %d %s", status, body)
	}
}
