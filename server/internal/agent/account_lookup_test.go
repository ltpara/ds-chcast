package agent

import (
	"context"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// TestRestrictedDeviceLookupWithAccountToken covers the viewer path that the
// shared link key cannot open: a restricted device admits only an account
// subject, so the tunnel must present the signed-in account's bearer token
// while the transport credential is bound to that same subject.
func TestRestrictedDeviceLookupWithAccountToken(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: transportTestIssuer(t)})
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	_, token, _, err := s.Accounts.Login("alice", "correct horse")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if _, err := s.Accounts.SetDevicePolicy("devbox", account.DevicePolicyRestricted, 0,
		[]string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("device policy: %v", err)
	}
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	shared := newAPIClient(s.URL, testutil.ServerKey, nil, nil)
	if _, err := shared.lookup(ctx, "devbox", ""); err == nil {
		t.Fatal("the shared link key looked up a restricted device")
	}

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		AccountToken:  token,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeDirect,
		DirectTimeout: 2 * time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	if viewer.transportPunch == nil {
		t.Fatal("restricted device lookup issued no punch credential")
	}
	if viewer.transportPunch.Subject != alice.ID {
		t.Fatalf("punch subject = %q, want %q", viewer.transportPunch.Subject, alice.ID)
	}
	assertRoundTrip(t, viewer, ModeDirect)
}

// TestAccountLookupKeepsLegacyPunchFallback proves the split between the
// lookup bearer token and the punch HMAC key: an unmanaged deployment without
// an issuer still receives no transport credential, and the host only accepts
// the link-key proof even though lookup authenticated as the account.
func TestAccountLookupKeepsLegacyPunchFallback(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	_, token, _, err := s.Accounts.Login("alice", "correct horse")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		LinkKey:       testutil.ServerKey,
		AccountToken:  token,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeDirect,
		DirectTimeout: 2 * time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	if viewer.transportPunch != nil {
		t.Fatal("an unmanaged deployment issued a transport credential")
	}
	if viewerAPIKey(viewer.cfg) != token {
		t.Fatal("lookup did not authenticate as the account")
	}
	if punchAPIKey(viewer.cfg) != testutil.ServerKey {
		t.Fatal("punch fallback did not keep the link key")
	}
	assertRoundTrip(t, viewer, ModeDirect)
}
