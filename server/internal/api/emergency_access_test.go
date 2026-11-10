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
	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type emergencyAccessReply struct {
	ID           string `json:"id"`
	Punch        string `json:"punch"`
	Fingerprint  string `json:"fingerprint"`
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
	TransportPunch   *transportTicketReply `json:"transport_punch"`
	TransportRelay   *transportTicketReply `json:"transport_relay"`
	TransportCarrier *transportTicketReply `json:"transport_carrier"`
}

func emergencyRequestBody(t *testing.T, peerID string, clientKey, transportKey []byte,
	permissions uint32, reason string) string {
	t.Helper()
	data, err := json.Marshal(map[string]any{
		"peer_id":              peerID,
		"client_public_key":    base64.RawURLEncoding.EncodeToString(clientKey),
		"transport_public_key": base64.RawURLEncoding.EncodeToString(transportKey),
		"permissions":          permissions,
		"reason":               reason,
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// TestEmergencyAccessIssuesShortLivedTickets proves an explicit override
// bypasses the allow list for a managed device while still producing ordinary
// short-lived credentials and a dedicated audit record.
func TestEmergencyAccessIssuesShortLivedTickets(t *testing.T) {
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	if _, err := s.Accounts.CreateUser("rootadmin", "correct horse"); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "rootadmin", "correct horse").Token
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 6,
		[]string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("set policy: %v", err)
	}
	clientKey := bytes.Repeat([]byte{0x42}, 32)
	transportKey := bytes.Repeat([]byte{0x43}, 32)
	reason := "incident 4711: console unreachable"

	status, body := request(t, http.MethodPost, s.URL+"/v1/emergency-access",
		emergencyRequestBody(t, "devbox", clientKey, transportKey, 0xffffffff, reason), admin)
	if status != http.StatusCreated {
		t.Fatalf("emergency access = %d %s", status, body)
	}
	var reply emergencyAccessReply
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("emergency reply: %v", err)
	}
	// The override must carry the rendezvous data a normal lookup would, or
	// the caller holds tickets it cannot use on a restricted device.
	if reply.ID != "devbox" || reply.Punch == "" || reply.Fingerprint != "AB12" {
		t.Fatalf("emergency reply lacks host endpoints: %s", body)
	}
	if reply.AccessTicket == nil || reply.AccessTicket.Ticket == "" {
		t.Fatalf("emergency reply has no access ticket: %s", body)
	}
	if reply.AccessTicket.Subject != "emergency:rootadmin" ||
		reply.AccessTicket.PolicyVersion != 1 || reply.AccessTicket.Permissions != 6 ||
		reply.AccessTicket.Issuer != issuer.Name() || reply.AccessTicket.KeyID != issuer.KeyID() {
		t.Fatalf("emergency access ticket = %+v", reply.AccessTicket)
	}
	signed, err := base64.RawURLEncoding.DecodeString(reply.AccessTicket.Ticket)
	if err != nil {
		t.Fatalf("ticket is not base64url: %v", err)
	}
	verifier := acl.NewVerifier(issuer.Name(), "device:devbox")
	if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	ticket, err := verifier.Verify(signed, time.Now())
	if err != nil {
		t.Fatalf("verify emergency ticket: %v", err)
	}
	if ticket.HostID != "devbox" || ticket.HostPublicKeyFingerprint != "AB12" ||
		ticket.Subject != "emergency:rootadmin" || ticket.Permissions != 6 ||
		ticket.PolicyVersion != 1 || !bytes.Equal(ticket.ClientPublicKey[:], clientKey) {
		t.Fatalf("emergency ticket binding = %+v", ticket)
	}
	if lifetime := ticket.ExpiresAt - ticket.IssuedAt; lifetime <= 0 || lifetime > 60 {
		t.Fatalf("emergency ticket lifetime = %d", lifetime)
	}
	for _, tc := range []struct {
		reply    *transportTicketReply
		audience string
	}{
		{reply.TransportPunch, acl.TransportAudiencePunch},
		{reply.TransportRelay, acl.TransportAudienceRelay},
		{reply.TransportCarrier, acl.TransportAudienceCarrier},
	} {
		verified := verifyTransportReply(t, issuer, tc.reply, tc.audience, "devbox", transportKey)
		if verified.PolicyVersion != 1 {
			t.Fatalf("%s policy version = %d", tc.audience, verified.PolicyVersion)
		}
		if tc.reply.Subject != "emergency:rootadmin" {
			t.Fatalf("%s subject = %q", tc.audience, tc.reply.Subject)
		}
	}

	// The management root key is also an explicit emergency principal; its
	// audit trail names it instead of an account.
	status, body = request(t, http.MethodPost, s.URL+"/v1/emergency-access",
		emergencyRequestBody(t, "devbox", clientKey, transportKey, 4, reason), testutil.ServerKey)
	if status != http.StatusCreated {
		t.Fatalf("server-key emergency access = %d %s", status, body)
	}
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatalf("server-key emergency reply: %v", err)
	}
	if reply.AccessTicket.Subject != "emergency:server-key" || reply.AccessTicket.Permissions != 4 {
		t.Fatalf("server-key emergency ticket = %+v", reply.AccessTicket)
	}

	status, data := request(t, http.MethodGet, s.URL+"/v1/audit?action="+account.AuditEmergencyAccess,
		"", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("audit = %d %s", status, data)
	}
	var audit struct {
		Events []struct {
			Username string `json:"username"`
			Action   string `json:"action"`
			Detail   string `json:"detail"`
			Success  bool   `json:"success"`
		} `json:"events"`
	}
	if err := json.Unmarshal(data, &audit); err != nil {
		t.Fatalf("audit reply: %v", err)
	}
	if len(audit.Events) != 2 {
		t.Fatalf("emergency audit events = %d: %s", len(audit.Events), data)
	}
	actors := make(map[string]string)
	for _, event := range audit.Events {
		actors[event.Username] = event.Detail
		if event.Action != account.AuditEmergencyAccess || !event.Success ||
			!strings.Contains(event.Detail, "devbox") || !strings.Contains(event.Detail, reason) {
			t.Fatalf("emergency audit event = %+v", event)
		}
	}
	if _, ok := actors["rootadmin"]; !ok {
		t.Fatalf("audit did not name the admin: %+v", audit.Events)
	}
	if _, ok := actors["server-key"]; !ok {
		t.Fatalf("audit did not name the server key: %+v", audit.Events)
	}
}

// TestEmergencyAccessRequiresExplicitAdminRequest pins the refusals: only an
// admin may ask, the request must state a reason, and only managed devices can
// be reached through the override.
func TestEmergencyAccessRequiresExplicitAdminRequest(t *testing.T) {
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	if _, err := s.Accounts.CreateUser("rootadmin", "correct horse"); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "rootadmin", "correct horse").Token
	alice := login(t, s.URL, "alice", "correct horse").Token
	now := time.Now().Unix()
	for _, id := range []string{"devbox", "plainbox"} {
		if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, id, id+" secret", "", now), testutil.ServerKey); status != http.StatusOK {
			t.Fatalf("register %s: %d %s", id, status, body)
		}
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", "restricted", 6, nil, nil, "root"); err != nil {
		t.Fatalf("set policy: %v", err)
	}
	clientKey := bytes.Repeat([]byte{0x42}, 32)
	transportKey := bytes.Repeat([]byte{0x43}, 32)
	issue := func(token, body string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodPost, s.URL+"/v1/emergency-access", body, token)
	}
	valid := emergencyRequestBody(t, "devbox", clientKey, transportKey, 1, "incident")

	if status, body := issue("", valid); status != http.StatusUnauthorized {
		t.Fatalf("anonymous emergency access = %d %s", status, body)
	}
	if status, body := issue(alice, valid); status != http.StatusForbidden {
		t.Fatalf("non-admin emergency access = %d %s", status, body)
	}
	if status, body := issue(admin, emergencyRequestBody(t, "devbox", clientKey, transportKey, 1, "  ")); status != http.StatusBadRequest {
		t.Fatalf("missing reason = %d %s", status, body)
	}
	longReason := strings.Repeat("a", 513)
	if status, body := issue(admin, emergencyRequestBody(t, "devbox", clientKey, transportKey, 1, longReason)); status != http.StatusBadRequest {
		t.Fatalf("long reason = %d %s", status, body)
	}
	if status, body := issue(admin, emergencyRequestBody(t, "devbox", []byte{1, 2, 3}, transportKey, 1, "incident")); status != http.StatusBadRequest {
		t.Fatalf("short client key = %d %s", status, body)
	}
	if status, body := issue(admin, emergencyRequestBody(t, "devbox", clientKey, []byte{1, 2, 3}, 1, "incident")); status != http.StatusBadRequest {
		t.Fatalf("short transport key = %d %s", status, body)
	}
	if status, body := issue(admin, emergencyRequestBody(t, "ghost", clientKey, transportKey, 1, "incident")); status != http.StatusNotFound {
		t.Fatalf("unknown host = %d %s", status, body)
	}
	if status, body := issue(admin, emergencyRequestBody(t, "plainbox", clientKey, transportKey, 1, "incident")); status != http.StatusConflict {
		t.Fatalf("unmanaged emergency access = %d %s", status, body)
	}

	unconfigured := testutil.Start(t)
	if _, err := unconfigured.Accounts.CreateUser("rootadmin", "correct horse"); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	admin = login(t, unconfigured.URL, "rootadmin", "correct horse").Token
	if status, body := request(t, http.MethodPost, unconfigured.URL+"/v1/emergency-access",
		valid, admin); status != http.StatusServiceUnavailable {
		t.Fatalf("unconfigured emergency access = %d %s", status, body)
	}
}
