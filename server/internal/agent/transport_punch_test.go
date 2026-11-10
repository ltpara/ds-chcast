package agent

import (
	"context"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func transportTestIssuer(t *testing.T) *acl.Issuer {
	t.Helper()
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(i + 17)
	}
	issuer, err := acl.NewIssuer("chast.example", "kid-transport", seed)
	if err != nil {
		t.Fatalf("issuer: %v", err)
	}
	return issuer
}

// TestManagedDirectPathThroughAgents proves the viewer agent obtains and
// presents a transport credential: the host is managed, so the rendezvous
// service refuses the legacy link-key HMAC, and a direct punch only succeeds
// with a valid credential bound to the agent's ephemeral key.
func TestManagedDirectPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: transportTestIssuer(t)})
	if _, err := s.Accounts.SetDevicePolicy("devbox", account.DevicePolicyOpen, 0, nil, nil, "root"); err != nil {
		t.Fatalf("device policy: %v", err)
	}
	testRoundTrip(t, ctx, s, ModeDirect)
}

// TestUnmanagedDirectPathWithoutIssuer keeps the legacy deployment working:
// without an issuer the lookup carries no credential and punch stays on the
// link key.
func TestUnmanagedDirectPathWithoutIssuer(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{})
	testRoundTrip(t, ctx, s, ModeDirect)
}

// TestManagedRelayPathThroughAgents proves relay admission also moves off the
// legacy carrier token: the managed registry refuses the token, and the viewer
// agent authenticates with the relay-audience transport credential.
func TestManagedRelayPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: transportTestIssuer(t)})
	if _, err := s.Accounts.SetDevicePolicy("devbox", account.DevicePolicyOpen, 0, nil, nil, "root"); err != nil {
		t.Fatalf("device policy: %v", err)
	}
	testRoundTrip(t, ctx, s, ModeRelay)
}

// TestManagedCarrierPathThroughAgents does the same for the shared carrier:
// its hello carries a carrier-audience transport credential.
func TestManagedCarrierPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: transportTestIssuer(t)})
	if _, err := s.Accounts.SetDevicePolicy("devbox", account.DevicePolicyOpen, 0, nil, nil, "root"); err != nil {
		t.Fatalf("device policy: %v", err)
	}
	echo := startEcho(t)
	localCarrier := startEchoCarrier(t)
	startHostAgentCarrier(t, ctx, s.URL, echo.LocalAddr().String(), localCarrier.Addr().String())
	waitForSharedHost(t, s)

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeCarrier,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	viewerRoundTrip(t, viewer, []byte("managed-carrier-round-trip"))
}
