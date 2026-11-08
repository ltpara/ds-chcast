package agent

import (
	"context"
	"net"
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

// startCarrierOnlyTCPHost closes the host's UDP socket, not just its registry
// timestamp: neither a UDP trigger nor a UDP data path can rescue these tests.
func startCarrierOnlyTCPHost(t *testing.T, ctx context.Context) (*testutil.Server, *HostAgent) {
	t.Helper()
	s := testutil.StartWith(t, testutil.Options{RegistryOfflineAfter: 100 * time.Millisecond})
	echo := startEcho(t)
	localCarrier := startEchoCarrier(t)
	host, err := StartHost(ctx, HostConfig{
		ServerURL:   s.URL,
		ServerKey:   testutil.ServerKey,
		ID:          "devbox",
		Key:         "host-secret",
		LocalAddr:   echo.LocalAddr().String(),
		CarrierAddr: localCarrier.Addr().String(),
		ListenAddr:  "127.0.0.1:0",
		Keepalive:   10 * time.Second,
		Logger:      quietLogger(),
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
	waitForSharedHost(t, s)
	waitForHostControl(t, s)
	if err := host.conn.Close(); err != nil {
		t.Fatalf("close host UDP socket: %v", err)
	}

	deadline := time.Now().Add(5 * time.Second)
	for {
		current, ok := s.Registry.Lookup("devbox")
		if ok && !s.Registry.Online(current) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("UDP mapping never expired while the carrier stayed linked")
		}
		time.Sleep(20 * time.Millisecond)
	}
	return s, host
}

func TestTCPPunchThroughRendezvousWhenUDPExpired(t *testing.T) {
	for _, mode := range []string{ModeTCP, ModeDirect, ModeAuto} {
		t.Run(mode, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			s, _ := startCarrierOnlyTCPHost(t, ctx)
			viewer, err := StartViewer(ctx, ViewerConfig{
				ServerURL:     s.URL,
				ServerKey:     testutil.ServerKey,
				HostID:        "devbox",
				ListenAddr:    "127.0.0.1:0",
				Mode:          mode,
				DirectTimeout: 5 * time.Second,
				Logger:        quietLogger(),
			})
			if err != nil {
				t.Fatalf("start %s viewer against carrier-only host: %v", mode, err)
			}
			t.Cleanup(viewer.Close)
			viewerRoundTrip(t, viewer, []byte("tcp-punch-without-udp"))
			if info := viewer.PathInfo(); info.Path != "direct" || info.Transport != "tcp" || info.Candidates != 0 {
				t.Fatalf("viewer path = %+v, want direct over tcp without UDP candidates", info)
			}
		})
	}
}

func TestCarrierOnlyTCPTimeoutFallsBackBeforeLocalClient(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s, host := startCarrierOnlyTCPHost(t, ctx)
	// Keep the carrier usable, but prevent the host joining the TCP punch.
	host.mu.Lock()
	host.tcpPunch = nil
	host.mu.Unlock()
	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeAuto,
		DirectTimeout: 400 * time.Millisecond,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(viewer.Close)
	deadline := time.Now().Add(2 * time.Second)
	for viewer.PathInfo().Path != "carrier" {
		if time.Now().After(deadline) {
			t.Fatalf("TCP timeout did not select the carrier before a local client attached: %+v", viewer.PathInfo())
		}
		time.Sleep(10 * time.Millisecond)
	}
	viewerRoundTrip(t, viewer, []byte("carrier-after-tcp-timeout"))
	if !viewer.fellBackNow() {
		t.Fatal("carrier fallback was not latched")
	}
}

func TestCarrierOnlyTCPLossFallsBackAndStaysOnCarrier(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s, _ := startCarrierOnlyTCPHost(t, ctx)
	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeAuto,
		DirectTimeout: 5 * time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start TCP viewer against carrier-only host: %v", err)
	}
	t.Cleanup(viewer.Close)
	viewerRoundTrip(t, viewer, []byte("tcp-before-loss"))
	if info := viewer.PathInfo(); info.Path != "direct" || info.Transport != "tcp" {
		t.Fatalf("viewer path = %+v, want direct over tcp", info)
	}
	viewer.tcpMu.Lock()
	conn := viewer.tcpConn
	viewer.tcpMu.Unlock()
	if conn == nil {
		t.Fatal("TCP direct socket was not published")
	}
	conn.Close()
	viewerRoundTrip(t, viewer, []byte("carrier-after-tcp-loss"))
	if info := viewer.PathInfo(); info.Path != "carrier" {
		t.Fatalf("TCP loss did not switch to carrier: %+v", info)
	}
	local, lateTCP := net.Pipe()
	defer local.Close()
	defer lateTCP.Close()
	if viewer.attachTCP(lateTCP) {
		t.Fatal("late TCP link undid carrier fallback")
	}
	viewer.onDirectPacket(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40001})
	if info := viewer.PathInfo(); info.Path != "carrier" || viewer.directOK.Load() {
		t.Fatalf("late UDP packet undid carrier fallback: %+v", info)
	}
}

func TestAttachTCPDoesNotUndoRelayFallback(t *testing.T) {
	local, peer := net.Pipe()
	defer local.Close()
	defer peer.Close()
	v := &ViewerAgent{
		ctx:      context.Background(),
		log:      quietLogger(),
		fellBack: true,
	}
	if v.attachTCP(peer) {
		t.Fatal("TCP link was attached after the session had fallen back to relay")
	}
	if v.tcpActive.Load() || v.directOK.Load() {
		t.Fatal("rejected TCP link still changed the active path")
	}
}

func TestTCPLinkStatusKeepsRemainingViewer(t *testing.T) {
	h := &HostAgent{}
	first, firstPeer := net.Pipe()
	defer first.Close()
	defer firstPeer.Close()
	second, secondPeer := net.Pipe()
	defer second.Close()
	defer secondPeer.Close()

	h.mu.Lock()
	if !h.addTCPLinkLocked(first, "198.51.100.10:4000") ||
		!h.addTCPLinkLocked(second, "198.51.100.11:4000") {
		h.mu.Unlock()
		t.Fatal("TCP links were rejected below the limit")
	}
	if h.tcpViewer != "198.51.100.10:4000" || h.viewerTransport != "tcp" {
		h.mu.Unlock()
		t.Fatalf("initial TCP status = viewer %q transport %q", h.tcpViewer, h.viewerTransport)
	}
	h.removeTCPLinkLocked(first, "198.51.100.10:4000")
	if h.tcpViewer != "198.51.100.11:4000" || h.viewerTransport != "tcp" {
		h.mu.Unlock()
		t.Fatalf("remaining TCP status = viewer %q transport %q", h.tcpViewer, h.viewerTransport)
	}
	h.removeTCPLinkLocked(second, "198.51.100.11:4000")
	if h.tcpViewer != "" || h.viewerTransport != "" {
		h.mu.Unlock()
		t.Fatalf("empty TCP status = viewer %q transport %q", h.tcpViewer, h.viewerTransport)
	}
	h.mu.Unlock()
}

func waitForHostControl(t *testing.T, s *testutil.Server) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for !s.Rendezvous.HostControlLinked("devbox") {
		if time.Now().After(deadline) {
			t.Fatal("host rendezvous control never connected")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestTCPPunchWithoutCarrierWhenUDPExpired(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{RegistryOfflineAfter: 100 * time.Millisecond})
	echo := startEcho(t)
	host, err := StartHost(ctx, HostConfig{
		ServerURL: s.URL, ServerKey: testutil.ServerKey, ID: "devbox", Key: "host-secret",
		LocalAddr: echo.LocalAddr().String(), ListenAddr: "127.0.0.1:0", Logger: quietLogger(),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(host.Close)
	waitForHostControl(t, s)
	host.conn.Close()
	deadline := time.Now().Add(3 * time.Second)
	for {
		current, ok := s.Registry.Lookup("devbox")
		if ok && !s.Registry.Online(current) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("UDP registration never expired")
		}
		time.Sleep(10 * time.Millisecond)
	}
	if s.Relay.HostLinked("devbox") {
		t.Fatal("test unexpectedly has a carrier host link")
	}
	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL: s.URL, ServerKey: testutil.ServerKey, HostID: "devbox", ListenAddr: "127.0.0.1:0",
		Mode: ModeAuto, DirectTimeout: 5 * time.Second, Logger: quietLogger(),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(viewer.Close)
	viewerRoundTrip(t, viewer, []byte("rendezvous-control-without-udp-or-carrier"))
	if info := viewer.PathInfo(); info.Path != "direct" || info.Transport != "tcp" || info.Candidates != 0 {
		t.Fatalf("independent TCP rendezvous path = %+v", info)
	}
}

func TestTCPPunchAdaptiveBudgetWaitsForCoordinationRetry(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s, host := startCarrierOnlyTCPHost(t, ctx)
	host.mu.Lock()
	endpoint := host.tcpPunch
	host.tcpPunch = nil
	host.mu.Unlock()
	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL: s.URL, ServerKey: testutil.ServerKey, HostID: "devbox", ListenAddr: "127.0.0.1:0",
		Mode: ModeAuto, Logger: quietLogger(),
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(viewer.Close)
	// Drop the first host trigger. The same request must reach it at t=3s;
	// the local connect budget (1s) must not expire during coordination.
	time.Sleep(1500 * time.Millisecond)
	if info := viewer.PathInfo(); info.Path != "connecting" || viewer.fellBackNow() {
		t.Fatalf("coordination was preempted by the connect timeout: %+v", info)
	}
	host.mu.Lock()
	host.tcpPunch = endpoint
	host.mu.Unlock()
	viewerRoundTrip(t, viewer, []byte("after-retransmitted-coordination"))
	if info := viewer.PathInfo(); info.Path != "direct" || info.Transport != "tcp" {
		t.Fatalf("retry failed to establish TCP: %+v", info)
	}
}
