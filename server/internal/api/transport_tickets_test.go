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

type transportTicketReply struct {
	Ticket        string `json:"ticket"`
	Audience      string `json:"audience"`
	IssuedAtMS    int64  `json:"issued_at_ms"`
	ExpiresAtMS   int64  `json:"expires_at_ms"`
	ExpiresInMS   int64  `json:"expires_in_ms"`
	PolicyVersion uint32 `json:"policy_version"`
	Issuer        string `json:"issuer"`
	KeyID         string `json:"kid"`
	Subject       string `json:"subject"`
}

type lookupTransportReply struct {
	TransportPunch   *transportTicketReply `json:"transport_punch"`
	TransportRelay   *transportTicketReply `json:"transport_relay"`
	TransportCarrier *transportTicketReply `json:"transport_carrier"`
}

func verifyTransportReply(t *testing.T, issuer *acl.Issuer, reply *transportTicketReply,
	audience, hostID string, clientKey []byte) acl.TransportTicket {
	t.Helper()
	if reply == nil {
		t.Fatalf("lookup did not return a %s transport ticket", audience)
	}
	if reply.Audience != audience || reply.Issuer != issuer.Name() ||
		reply.KeyID != issuer.KeyID() || reply.Subject == "" {
		t.Fatalf("transport ticket = %+v", reply)
	}
	if reply.ExpiresInMS <= 0 || reply.ExpiresInMS > 60000 ||
		reply.ExpiresAtMS-reply.IssuedAtMS != reply.ExpiresInMS {
		t.Fatalf("transport ticket lifetime = %+v", reply)
	}
	signed, err := base64.RawURLEncoding.DecodeString(reply.Ticket)
	if err != nil {
		t.Fatalf("transport ticket is not base64url: %v", err)
	}
	verifier := acl.NewTransportVerifier(issuer.Name(), audience)
	if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	ticket, err := verifier.Verify(signed, time.Now())
	if err != nil {
		t.Fatalf("verify %s transport ticket: %v", audience, err)
	}
	if ticket.HostID != hostID || !bytes.Equal(ticket.ClientPublicKey[:], clientKey) {
		t.Fatalf("transport ticket binding = %+v", ticket)
	}
	return ticket
}

// TestLookupTransportTickets proves lookup mints a separate short-lived
// credential per transport, bound to the agent's key and the current policy.
func TestLookupTransportTickets(t *testing.T) {
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	clientKey := bytes.Repeat([]byte{0x51}, 32)
	keyParam := base64.RawURLEncoding.EncodeToString(clientKey)
	lookupURL := s.URL + "/v1/hosts/devbox"

	// Without the agent key lookup keeps the historical shape.
	status, body := request(t, http.MethodGet, lookupURL, "", alice)
	if status != http.StatusOK {
		t.Fatalf("plain lookup = %d %s", status, body)
	}
	var plain lookupTransportReply
	if err := json.Unmarshal(body, &plain); err != nil {
		t.Fatalf("plain lookup reply: %v", err)
	}
	if plain.TransportPunch != nil || plain.TransportRelay != nil || plain.TransportCarrier != nil {
		t.Fatalf("plain lookup returned transport tickets: %s", body)
	}
	if status, body := request(t, http.MethodGet, lookupURL+"?transport_public_key=short",
		"", alice); status != http.StatusBadRequest {
		t.Fatalf("short transport key = %d %s", status, body)
	}

	// A shared deployment credential still gets transport admission, but the
	// subject names the shared caller instead of an account.
	status, body = request(t, http.MethodGet, lookupURL+"?transport_public_key="+keyParam,
		"", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("shared transport lookup = %d %s", status, body)
	}
	var shared lookupTransportReply
	if err := json.Unmarshal(body, &shared); err != nil {
		t.Fatalf("shared transport reply: %v", err)
	}
	if shared.TransportPunch == nil || shared.TransportPunch.Subject != "management" ||
		shared.TransportPunch.PolicyVersion != 0 {
		t.Fatalf("shared transport ticket = %+v", shared.TransportPunch)
	}

	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 5,
		[]string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set restricted policy: %v", err)
	}
	if status, body := request(t, http.MethodGet, lookupURL+"?transport_public_key="+keyParam,
		"", login(t, s.URL, "bob", "battery staple").Token); status != http.StatusForbidden {
		t.Fatalf("unlisted transport lookup = %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, lookupURL+"?transport_public_key="+keyParam,
		"", testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("shared restricted transport lookup = %d %s", status, body)
	}

	status, body = request(t, http.MethodGet, lookupURL+"?transport_public_key="+keyParam, "", alice)
	if status != http.StatusOK {
		t.Fatalf("managed transport lookup = %d %s", status, body)
	}
	var managed lookupTransportReply
	if err := json.Unmarshal(body, &managed); err != nil {
		t.Fatalf("managed transport reply: %v", err)
	}
	punch := verifyTransportReply(t, issuer, managed.TransportPunch,
		acl.TransportAudiencePunch, "devbox", clientKey)
	relay := verifyTransportReply(t, issuer, managed.TransportRelay,
		acl.TransportAudienceRelay, "devbox", clientKey)
	carrier := verifyTransportReply(t, issuer, managed.TransportCarrier,
		acl.TransportAudienceCarrier, "devbox", clientKey)
	if punch.ID == relay.ID || relay.ID == carrier.ID || punch.ID == carrier.ID {
		t.Fatalf("transport tickets reused an id: %s %s %s", punch.ID, relay.ID, carrier.ID)
	}
	if punch.Subject != managed.TransportPunch.Subject ||
		managed.TransportPunch.Subject == "alice" || punch.PolicyVersion != 1 {
		t.Fatalf("transport binding = %+v", managed.TransportPunch)
	}
}

// TestLookupTransportRequiresIssuerForManagedDevice keeps a managed device
// from silently falling back to the shared punch credential.
func TestLookupTransportRequiresIssuerForManagedDevice(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", "open", 3, []string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set policy: %v", err)
	}
	keyParam := base64.RawURLEncoding.EncodeToString(bytes.Repeat([]byte{0x52}, 32))
	status, body := request(t, http.MethodGet,
		s.URL+"/v1/hosts/devbox?transport_public_key="+keyParam, "", alice)
	if status != http.StatusServiceUnavailable {
		t.Fatalf("managed device without issuer = %d %s", status, body)
	}
}

// TestDevicePolicyAssignmentUpdatesRegistryAdmission proves the punch port
// learns about a policy change immediately, without waiting for the host to
// register again.
func TestDevicePolicyAssignmentUpdatesRegistryAdmission(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if host, ok := s.Registry.Lookup("devbox"); !ok || host.Managed {
		t.Fatalf("fresh registration = %+v", host)
	}

	status, body := request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		`{"mode":"open"}`, admin)
	if status != http.StatusOK {
		t.Fatalf("set open = %d %s", status, body)
	}
	if host, _ := s.Registry.Lookup("devbox"); !host.Managed || host.PolicyVersion != 1 {
		t.Fatalf("registry after assignment = %+v", host)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/device-policies/devbox",
		"", admin); status != http.StatusNoContent {
		t.Fatalf("delete policy = %d %s", status, body)
	}
	if host, _ := s.Registry.Lookup("devbox"); host.Managed {
		t.Fatalf("registry after delete = %+v", host)
	}
}
