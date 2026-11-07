package agent

import (
	"context"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// TestTCPPunchPathThroughAgents drives the coordination server, the host's
// punched listener and the viewer's dial end to end. The TCP path has to carry
// the same opaque datagrams as UDP, because QUIC above the tunnel cannot tell
// which transport moved them.
func TestTCPPunchPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	host, err := StartHost(ctx, HostConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		ID:         "devbox",
		Key:        "host-secret",
		LocalAddr:  echo.LocalAddr().String(),
		ListenAddr: "127.0.0.1:0",
		Keepalive:  300 * time.Millisecond,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start host agent: %v", err)
	}
	t.Cleanup(host.Close)
	readyCtx, readyCancel := context.WithTimeout(ctx, 5*time.Second)
	defer readyCancel()
	if err := host.WaitReady(readyCtx); err != nil {
		t.Fatalf("host agent readiness: %v", err)
	}

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeTCP,
		DirectTimeout: 1 * time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)

	assertRoundTrip(t, viewer, ModeTCP)
	info := viewer.PathInfo()
	if info.Path != "direct" || info.Transport != "tcp" {
		t.Fatalf("viewer path = %+v, want direct over tcp", info)
	}
}

// TestTCPPunchRejectsRelayOnly checks that a server which forces the relay does
// not hand out a punch endpoint the host would refuse to serve.
func TestTCPPunchRejectsRelayOnly(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{RelayOnly: true})
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	_, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeTCP,
		Logger:     quietLogger(),
	})
	if err == nil {
		t.Fatal("relay-only server accepted a TCP-only viewer")
	}
}
