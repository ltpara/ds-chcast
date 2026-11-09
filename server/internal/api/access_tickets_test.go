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

type accessTicketReply struct {
	Ticket        string `json:"ticket"`
	ExpiresAtMS   int64  `json:"expires_at_ms"`
	IssuedAtMS    int64  `json:"issued_at_ms"`
	PolicyVersion uint32 `json:"policy_version"`
	Issuer        string `json:"issuer"`
	KeyID         string `json:"kid"`
}

func newTestIssuer(t *testing.T) *acl.Issuer {
	t.Helper()
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(i + 1)
	}
	issuer, err := acl.NewIssuer("chast-test", "kid-2026-11", seed)
	if err != nil {
		t.Fatalf("issuer: %v", err)
	}
	return issuer
}

func decodeAccessTicket(t *testing.T, data []byte) accessTicketReply {
	t.Helper()
	var reply accessTicketReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("access ticket reply: %v", err)
	}
	if reply.Ticket == "" {
		t.Fatalf("access ticket reply has no ticket: %s", data)
	}
	return reply
}

func ticketRequestBody(t *testing.T, peerID string, clientKey []byte, permissions uint32) string {
	t.Helper()
	data, err := json.Marshal(map[string]any{
		"peer_id":           peerID,
		"client_public_key": base64.RawURLEncoding.EncodeToString(clientKey),
		"permissions":       permissions,
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// TestAccessTicketEndpointDisabled proves the endpoint stays closed unless the
// deployment configures a signing key.
func TestAccessTicketEndpointDisabled(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	alice := login(t, s.URL, "alice", "correct horse").Token
	key := bytes.Repeat([]byte{7}, 32)
	status, body := request(t, http.MethodPost, s.URL+"/v1/access-tickets",
		ticketRequestBody(t, "devbox", key, 1), alice)
	if status != http.StatusServiceUnavailable {
		t.Fatalf("unconfigured ticket endpoint = %d %s", status, body)
	}
}

// TestAccessTicketIssuance covers the account-only binding, the device policy
// ceiling and the wire format a host will verify: the ticket is signed by the
// dedicated issuer key, names the host fingerprint and the account subject, and
// never carries a shared deployment credential.
func TestAccessTicketIssuance(t *testing.T) {
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

	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	clientKey := bytes.Repeat([]byte{0x42}, 32)
	issue := func(token, body string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodPost, s.URL+"/v1/access-tickets", body, token)
	}

	if status, body := issue("", ticketRequestBody(t, "devbox", clientKey, 1)); status != http.StatusUnauthorized {
		t.Fatalf("anonymous ticket = %d %s", status, body)
	}
	if status, body := issue(testutil.ServerKey, ticketRequestBody(t, "devbox", clientKey, 1)); status != http.StatusUnauthorized {
		t.Fatalf("shared-key ticket = %d %s", status, body)
	}
	if status, body := issue(alice, ticketRequestBody(t, "bad id", clientKey, 1)); status != http.StatusBadRequest {
		t.Fatalf("invalid peer ticket = %d %s", status, body)
	}
	if status, body := issue(alice, ticketRequestBody(t, "devbox", []byte{1, 2, 3}, 1)); status != http.StatusBadRequest {
		t.Fatalf("short client key = %d %s", status, body)
	}
	if status, body := issue(alice, ticketRequestBody(t, "ghost", clientKey, 1)); status != http.StatusNotFound {
		t.Fatalf("unknown host ticket = %d %s", status, body)
	}

	status, body := issue(alice, ticketRequestBody(t, "devbox", clientKey, 0xffffffff))
	if status != http.StatusCreated {
		t.Fatalf("issue ticket = %d %s", status, body)
	}
	reply := decodeAccessTicket(t, body)
	if reply.Issuer != issuer.Name() || reply.KeyID != issuer.KeyID() || reply.PolicyVersion != 0 {
		t.Fatalf("ticket reply = %+v", reply)
	}
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
	if ticket.HostID != "devbox" || ticket.HostPublicKeyFingerprint != "AB12" {
		t.Fatalf("ticket host binding = %+v", ticket)
	}
	if ticket.Subject == "" || ticket.Subject == "alice" || ticket.ID == "" {
		t.Fatalf("ticket identity = %+v", ticket)
	}
	if ticket.Permissions != 0xffffffff {
		t.Fatalf("unmanaged permissions = %#x", ticket.Permissions)
	}
	if !bytes.Equal(ticket.ClientPublicKey[:], clientKey) {
		t.Fatalf("client key was not bound")
	}
	if lifetime := ticket.ExpiresAt - ticket.IssuedAt; lifetime <= 0 || lifetime > 60 {
		t.Fatalf("ticket lifetime = %d", lifetime)
	}
	if reply.IssuedAtMS/1000 != ticket.IssuedAt || reply.ExpiresAtMS/1000 != ticket.ExpiresAt {
		t.Fatalf("reply times %d/%d do not match ticket %d/%d",
			reply.IssuedAtMS, reply.ExpiresAtMS, ticket.IssuedAt, ticket.ExpiresAt)
	}

	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 5, []string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set restricted policy: %v", err)
	}
	if status, body := issue(bob, ticketRequestBody(t, "devbox", clientKey, 1)); status != http.StatusForbidden {
		t.Fatalf("unlisted account ticket = %d %s", status, body)
	}
	status, body = issue(alice, ticketRequestBody(t, "devbox", clientKey, 0xffffffff))
	if status != http.StatusCreated {
		t.Fatalf("restricted alice ticket = %d %s", status, body)
	}
	reply = decodeAccessTicket(t, body)
	if reply.PolicyVersion != 1 {
		t.Fatalf("restricted policy version = %d", reply.PolicyVersion)
	}
	signed, err = base64.RawURLEncoding.DecodeString(reply.Ticket)
	if err != nil {
		t.Fatalf("ticket is not base64url: %v", err)
	}
	ticket, err = verifier.Verify(signed, time.Now())
	if err != nil {
		t.Fatalf("verify restricted ticket: %v", err)
	}
	if ticket.Permissions != 5 || ticket.PolicyVersion != 1 {
		t.Fatalf("restricted ticket = %+v", ticket)
	}
}

type lookupTicketReply struct {
	AccessTicket *struct {
		Ticket        string `json:"ticket"`
		IssuedAtMS    int64  `json:"issued_at_ms"`
		ExpiresAtMS   int64  `json:"expires_at_ms"`
		ExpiresInMS   int64  `json:"expires_in_ms"`
		PolicyVersion uint32 `json:"policy_version"`
		Permissions   uint32 `json:"permissions"`
		Issuer        string `json:"issuer"`
		KeyID         string `json:"kid"`
		Subject       string `json:"subject"`
	} `json:"access_ticket"`
}

// TestLookupAccessTicket proves the address lookup can hand back the same
// short-lived credential as the exchange endpoint: only an account caller gets
// one, only for a managed device it may reach, and the granted permissions are
// the requested mask inside the policy ceiling.
func TestLookupAccessTicket(t *testing.T) {
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
	clientKey := bytes.Repeat([]byte{0x42}, 32)
	keyParam := base64.RawURLEncoding.EncodeToString(clientKey)
	lookupURL := s.URL + "/v1/hosts/devbox"

	// Without a key parameter lookup keeps its historical shape.
	status, body := request(t, http.MethodGet, lookupURL, "", alice)
	if status != http.StatusOK {
		t.Fatalf("plain lookup = %d %s", status, body)
	}
	var plain lookupTicketReply
	if err := json.Unmarshal(body, &plain); err != nil {
		t.Fatalf("plain lookup reply: %v", err)
	}
	if plain.AccessTicket != nil {
		t.Fatalf("plain lookup returned a ticket: %s", body)
	}

	// A shared deployment credential has no account subject to bind.
	if status, body := request(t, http.MethodGet, lookupURL+"?client_public_key="+keyParam,
		"", testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("shared-key lookup ticket = %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, lookupURL+"?client_public_key=short",
		"", alice); status != http.StatusBadRequest {
		t.Fatalf("short key lookup ticket = %d %s", status, body)
	}

	// An unmanaged device still authenticates locally, so lookup issues no
	// ticket even for an account caller that presents a key.
	status, body = request(t, http.MethodGet, lookupURL+"?client_public_key="+keyParam, "", alice)
	if status != http.StatusOK {
		t.Fatalf("unmanaged lookup ticket = %d %s", status, body)
	}
	var unmanaged lookupTicketReply
	if err := json.Unmarshal(body, &unmanaged); err != nil {
		t.Fatalf("unmanaged lookup reply: %v", err)
	}
	if unmanaged.AccessTicket != nil {
		t.Fatalf("unmanaged lookup returned a ticket: %s", body)
	}

	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 5,
		[]string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set restricted policy: %v", err)
	}
	if status, body := request(t, http.MethodGet, lookupURL+"?client_public_key="+keyParam,
		"", login(t, s.URL, "bob", "battery staple").Token); status != http.StatusForbidden {
		t.Fatalf("unlisted account lookup ticket = %d %s", status, body)
	}

	status, body = request(t, http.MethodGet,
		lookupURL+"?client_public_key="+keyParam+"&permissions=4294967295", "", alice)
	if status != http.StatusOK {
		t.Fatalf("managed lookup ticket = %d %s", status, body)
	}
	var managed lookupTicketReply
	if err := json.Unmarshal(body, &managed); err != nil {
		t.Fatalf("managed lookup reply: %v", err)
	}
	if managed.AccessTicket == nil {
		t.Fatalf("managed lookup has no ticket: %s", body)
	}
	granted := managed.AccessTicket
	if granted.Issuer != issuer.Name() || granted.KeyID != issuer.KeyID() ||
		granted.PolicyVersion != 1 || granted.Permissions != 5 {
		t.Fatalf("lookup ticket = %+v", granted)
	}
	if granted.ExpiresInMS <= 0 || granted.ExpiresInMS > 60000 {
		t.Fatalf("lookup ticket lifetime = %d ms", granted.ExpiresInMS)
	}
	if granted.ExpiresAtMS-granted.IssuedAtMS != granted.ExpiresInMS {
		t.Fatalf("lookup ticket times %d/%d/%d", granted.IssuedAtMS, granted.ExpiresAtMS,
			granted.ExpiresInMS)
	}
	if granted.Subject == "" || granted.Subject == "alice" {
		t.Fatalf("lookup ticket subject = %q", granted.Subject)
	}
	signed, err := base64.RawURLEncoding.DecodeString(granted.Ticket)
	if err != nil {
		t.Fatalf("lookup ticket is not base64url: %v", err)
	}
	verifier := acl.NewVerifier(issuer.Name(), "device:devbox")
	if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	ticket, err := verifier.Verify(signed, time.Now())
	if err != nil {
		t.Fatalf("verify lookup ticket: %v", err)
	}
	if ticket.HostID != "devbox" || ticket.HostPublicKeyFingerprint != "AB12" {
		t.Fatalf("lookup ticket host binding = %+v", ticket)
	}
	if !bytes.Equal(ticket.ClientPublicKey[:], clientKey) {
		t.Fatalf("lookup ticket did not bind the client key")
	}
	if ticket.Permissions != 5 || ticket.PolicyVersion != 1 || ticket.ID == "" {
		t.Fatalf("lookup ticket = %+v", ticket)
	}
	if lifetime := ticket.ExpiresAt - ticket.IssuedAt; lifetime <= 0 || lifetime > 60 {
		t.Fatalf("lookup ticket lifetime = %d", lifetime)
	}

	// The caller may narrow the grant; it can never widen it past the ceiling.
	status, body = request(t, http.MethodGet,
		lookupURL+"?client_public_key="+keyParam+"&permissions=1", "", alice)
	if status != http.StatusOK {
		t.Fatalf("narrowed lookup ticket = %d %s", status, body)
	}
	var narrowed lookupTicketReply
	if err := json.Unmarshal(body, &narrowed); err != nil {
		t.Fatalf("narrowed lookup reply: %v", err)
	}
	if narrowed.AccessTicket == nil || narrowed.AccessTicket.Permissions != 1 {
		t.Fatalf("narrowed lookup ticket = %+v", narrowed.AccessTicket)
	}
	if status, body := request(t, http.MethodGet,
		lookupURL+"?client_public_key="+keyParam+"&permissions=wide", "", alice); status != http.StatusBadRequest {
		t.Fatalf("bad permissions lookup ticket = %d %s", status, body)
	}
}
