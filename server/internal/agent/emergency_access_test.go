package agent

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// TestEmergencyAccessViewerReachesRestrictedDevice covers the whole viewer
// side of the management override: the ordinary lookup is refused for a
// restricted device, an administrator token plus a stated reason obtains both
// the rendezvous data and short-lived tickets, and the resulting transport
// credential actually carries a direct session. The access ticket is verified
// against the deployment issuer to prove it is bound to this viewer's client
// key rather than merely echoed back.
func TestEmergencyAccessViewerReachesRestrictedDevice(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	issuer := transportTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	if _, err := s.Accounts.CreateUser("rootadmin", "correct horse"); err != nil {
		t.Fatalf("create admin: %v", err)
	}
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	_, adminToken, _, err := s.Accounts.Login("rootadmin", "correct horse")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", account.DevicePolicyRestricted, 6,
		[]string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("device policy: %v", err)
	}
	echo := startEcho(t)
	host, err := StartHost(ctx, HostConfig{
		ServerURL:   s.URL,
		ServerKey:   testutil.ServerKey,
		ID:          "devbox",
		Key:         "host-secret",
		Fingerprint: "AB12",
		LocalAddr:   echo.LocalAddr().String(),
		ListenAddr:  "127.0.0.1:0",
		Keepalive:   300 * time.Millisecond,
		Logger:      quietLogger(),
	})
	if err != nil {
		t.Fatalf("start host agent: %v", err)
	}
	t.Cleanup(host.Close)
	readyCtx, readyCancel := context.WithTimeout(ctx, 5*time.Second)
	if err := host.WaitReady(readyCtx); err != nil {
		readyCancel()
		t.Fatalf("host agent readiness: %v", err)
	}
	readyCancel()

	shared := newAPIClient(s.URL, testutil.ServerKey, nil, nil)
	if _, err := shared.lookup(ctx, "devbox", ""); err == nil {
		t.Fatal("the shared link key looked up a restricted device")
	}

	clientPub, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatalf("client key: %v", err)
	}
	reason := "incident 4711: console unreachable"
	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeDirect,
		DirectTimeout: 2 * time.Second,
		Logger:        quietLogger(),
		Emergency: &EmergencyAccess{
			AdminToken:      adminToken,
			ClientPublicKey: base64.RawURLEncoding.EncodeToString(clientPub),
			Reason:          reason,
		},
	})
	if err != nil {
		t.Fatalf("start emergency viewer: %v", err)
	}
	t.Cleanup(viewer.Close)

	grant := viewer.AccessTicket()
	if grant == nil || grant.Ticket == "" {
		t.Fatal("emergency viewer received no access ticket")
	}
	if grant.Subject != "emergency:rootadmin" || grant.PolicyVersion != 1 || grant.Permissions != 6 {
		t.Fatalf("access ticket = %+v", grant)
	}
	signed, err := base64.RawURLEncoding.DecodeString(grant.Ticket)
	if err != nil {
		t.Fatalf("ticket is not base64url: %v", err)
	}
	verifier := acl.NewVerifier(issuer.Name(), "device:devbox")
	if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
		t.Fatalf("pin issuer: %v", err)
	}
	verified, err := verifier.Verify(signed, time.Now())
	if err != nil {
		t.Fatalf("verify access ticket: %v", err)
	}
	if verified.Subject != "emergency:rootadmin" || verified.HostID != "devbox" ||
		verified.Permissions != 6 || !bytes.Equal(verified.ClientPublicKey[:], clientPub) {
		t.Fatalf("access ticket binding = %+v", verified)
	}

	if viewer.transportPunch == nil || viewer.transportPunch.Subject != "emergency:rootadmin" {
		t.Fatalf("punch credential = %+v", viewer.transportPunch)
	}
	assertRoundTrip(t, viewer, ModeDirect)
}

// TestEmergencyAccessNeedsAdminCredentialAndReason pins the client-side
// validation: the tunnel refuses a partial override instead of silently
// falling back to an ordinary lookup the device will reject.
func TestEmergencyAccessNeedsAdminCredentialAndReason(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	pub, _, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatalf("client key: %v", err)
	}
	clientKey := base64.RawURLEncoding.EncodeToString(pub)
	cases := []struct {
		name     string
		override EmergencyAccess
	}{
		{"no token", EmergencyAccess{ClientPublicKey: clientKey, Reason: "incident"}},
		{"no reason", EmergencyAccess{AdminToken: "token", ClientPublicKey: clientKey}},
		{"no client key", EmergencyAccess{AdminToken: "token", Reason: "incident"}},
		{"bad client key", EmergencyAccess{AdminToken: "token", Reason: "incident", ClientPublicKey: "not-a-key"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			override := tc.override
			if _, err := StartViewer(ctx, ViewerConfig{
				ServerURL:  "http://127.0.0.1:1",
				HostID:     "devbox",
				ListenAddr: "127.0.0.1:0",
				Logger:     quietLogger(),
				Emergency:  &override,
			}); err == nil {
				t.Fatal("invalid emergency override started a viewer")
			}
		})
	}
}
