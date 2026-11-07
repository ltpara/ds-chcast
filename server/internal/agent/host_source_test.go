package agent

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// Control messages are only meaningful from the server address that issued
// them; the relay is not allowed to acknowledge the rendezvous handshake.
func TestHostControlSourceGating(t *testing.T) {
	h, _ := testHostAgent(t)
	h.ctx = context.Background()
	h.punch = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 41000}
	h.relay = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 42000}
	h.keepaliveReady = make(chan struct{})
	h.relayReady = make(chan struct{})
	stranger := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 9), Port: 9}

	h.handleControl(wire.Encode(wire.Message{Type: wire.TypeKeepaliveAck, OK: true}), stranger)
	h.handleControl(wire.Encode(wire.Message{Type: wire.TypeRelayAck, OK: true}), stranger)
	select {
	case <-h.keepaliveReady:
		t.Fatal("keepalive_ack from an unrelated source was accepted")
	default:
	}
	select {
	case <-h.relayReady:
		t.Fatal("relay_ack from an unrelated source was accepted")
	default:
	}

	h.handleControl(wire.Encode(wire.Message{Type: wire.TypeKeepaliveAck, OK: true}), h.punch)
	select {
	case <-h.keepaliveReady:
	default:
		t.Fatal("keepalive_ack from the rendezvous address was rejected")
	}

	h.handleControl(wire.Encode(wire.Message{Type: wire.TypeRelayAck, OK: true}), h.relay)
	select {
	case <-h.relayReady:
	default:
		t.Fatal("relay_ack from the relay address was rejected")
	}
}

// RustDesk's responder relays v4 as soon as either side reports a symmetric
// NAT; punching the observed candidate cannot work because the viewer's NAT
// allocates a port per destination. The v6 path must still be attempted.
func TestHostSkipsV4PunchForSymmetricNAT(t *testing.T) {
	const nonce = "0123456789abcdef"
	cases := []struct {
		name      string
		ownNAT    string
		viewerNAT string
		wantPunch bool
	}{
		{"both asymmetric", NATAsymmetric, NATAsymmetric, true},
		{"host symmetric", NATSymmetric, NATAsymmetric, false},
		{"viewer symmetric", NATAsymmetric, NATSymmetric, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			h, _ := testHostAgent(t)
			h.ctx = context.Background()
			server := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 41000}
			h.punch = server
			h.natType = tc.ownNAT
			peer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err != nil {
				t.Fatal(err)
			}
			defer peer.Close()
			h.handleControl(wire.Encode(wire.Message{
				Type:      wire.TypePunchPeer,
				Candidate: peer.LocalAddr().String(),
				Nonce:     nonce,
				NATType:   tc.viewerNAT,
			}), server)
			_ = peer.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
			buf := make([]byte, 256)
			_, _, err = peer.ReadFromUDP(buf)
			if tc.wantPunch && err != nil {
				t.Fatalf("expected a v4 punch: %v", err)
			}
			if !tc.wantPunch && err == nil {
				t.Fatal("punched a v4 candidate that a symmetric NAT cannot answer")
			}
		})
	}
}

// Symmetry on v4 says nothing about v6, which bypasses the NAT entirely.
// A viewer's LAN candidate never traverses the NAT, so a symmetric mapping
// that rules out the public v4 punch does not apply. The address must also be
// admitted as a viewer source, or the path the punch opens would be dropped.
func TestHostPunchesViewerLocalCandidatesUnderSymmetricNAT(t *testing.T) {
	h, _ := testHostAgent(t)
	h.ctx = context.Background()
	server := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 41000}
	h.punch = server
	h.natType = NATSymmetric
	peer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer peer.Close()
	h.handleControl(wire.Encode(wire.Message{
		Type:       wire.TypePunchPeer,
		Nonce:      "0123456789abcdef",
		LocalAddrs: []string{peer.LocalAddr().String()},
	}), server)
	_ = peer.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, wire.MaxControlSize)
	n, _, err := peer.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("LAN candidate was not punched: %v", err)
	}
	if msg, err := wire.Decode(buf[:n]); err != nil || msg.Type != wire.TypePunchProbe {
		t.Fatalf("punch = %+v, err=%v", msg, err)
	}
	h.mu.Lock()
	accepted := h.acceptViewerSourceLocked(peer.LocalAddr().(*net.UDPAddr))
	h.mu.Unlock()
	if !accepted {
		t.Fatal("the punched LAN viewer source was not admitted")
	}
}

func TestHostStillPunchesIPv6ForSymmetricNAT(t *testing.T) {
	peer, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Skipf("IPv6 loopback unavailable: %v", err)
	}
	defer peer.Close()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{})
	if err != nil {
		t.Skipf("dual-stack socket unavailable: %v", err)
	}
	defer conn.Close()
	h := &HostAgent{
		conn: conn,
		ctx:  context.Background(),
		log:  quietLogger(),
		cfg:  HostConfig{ID: "devbox"},
	}
	server := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 41000}
	h.punch = server
	h.natType = NATSymmetric
	h.handleControl(wire.Encode(wire.Message{
		Type:        wire.TypePunchPeer,
		CandidateV6: peer.LocalAddr().String(),
		Nonce:       "0123456789abcdef",
		NATType:     NATSymmetric,
	}), server)
	_ = peer.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
	buf := make([]byte, 256)
	if _, _, err := peer.ReadFromUDP(buf); err != nil {
		t.Fatalf("IPv6 punch was skipped with a symmetric v4 NAT: %v", err)
	}
}

// One punch notification may carry both families. The host must probe both in
// parallel, which is RustDesk's punch_v6 running beside the v4 punch instead of
// replacing it.
func TestHostPunchesBothFamiliesInParallel(t *testing.T) {
	v6peer, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Skipf("IPv6 loopback unavailable: %v", err)
	}
	defer v6peer.Close()
	v4peer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer v4peer.Close()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{})
	if err != nil {
		t.Skipf("dual-stack socket unavailable: %v", err)
	}
	defer conn.Close()
	h := &HostAgent{
		conn: conn,
		ctx:  context.Background(),
		log:  quietLogger(),
		cfg:  HostConfig{ID: "devbox"},
	}
	server := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 41000}
	h.punch = server
	h.handleControl(wire.Encode(wire.Message{
		Type:        wire.TypePunchPeer,
		Candidate:   v4peer.LocalAddr().String(),
		CandidateV6: v6peer.LocalAddr().String(),
		Nonce:       "0123456789abcdef",
		NATType:     NATAsymmetric,
	}), server)
	for _, tc := range []struct {
		name string
		peer *net.UDPConn
	}{{"IPv4", v4peer}, {"IPv6", v6peer}} {
		_ = tc.peer.SetReadDeadline(time.Now().Add(2 * time.Second))
		buf := make([]byte, 256)
		if _, _, err := tc.peer.ReadFromUDP(buf); err != nil {
			t.Fatalf("%s peer was not punched: %v", tc.name, err)
		}
	}
}

func TestConcurrentDirectViewersSurvivePunchExpiry(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	server := testutil.Start(t)
	echo := startEcho(t)
	host := startHostAgent(t, ctx, server.URL, echo.LocalAddr().String())
	connect := func() *ViewerAgent {
		v, err := StartViewer(ctx, ViewerConfig{
			ServerURL: server.URL, ServerKey: testutil.ServerKey, HostID: "devbox",
			ListenAddr: "127.0.0.1:0", Mode: ModeDirect, Logger: quietLogger(),
		})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(v.Close)
		assertRoundTrip(t, v, ModeDirect)
		return v
	}
	a, b := connect(), connect()
	host.mu.Lock()
	for source := range host.punchPeers {
		host.punchPeers[source] = time.Now().Add(-time.Second)
	}
	host.mu.Unlock()
	assertRoundTrip(t, b, "last direct viewer after punch expiry")
	assertRoundTrip(t, a, "first direct viewer after punch expiry")
}

func TestHostRouteExpiryRevokesViewerSource(t *testing.T) {
	for _, closeRoutes := range []bool{false, true} {
		t.Run(map[bool]string{false: "timeout", true: "close"}[closeRoutes], func(t *testing.T) {
			h, _ := testHostAgent(t)
			backend, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err != nil {
				t.Fatal(err)
			}
			defer backend.Close()
			defer h.routes.close()
			viewer := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 7), Port: 50000}
			h.remote = viewer // Being the last sender must not grant permanent access.
			if err := h.routes.forward(viewer.String(), backend.LocalAddr().(*net.UDPAddr), []byte("hello"), func([]byte) error { return nil }); err != nil {
				t.Fatal(err)
			}
			if !h.acceptViewerSourceLocked(viewer) {
				t.Fatal("live route was rejected")
			}
			// Repeat the deadline update to cover the reader's startup ordering.
			if closeRoutes {
				h.routes.close()
			}
			deadline := time.Now().Add(2 * time.Second)
			for h.routes.contains(viewer.String()) && time.Now().Before(deadline) {
				h.routes.mu.Lock()
				if conn := h.routes.peers[viewer.String()]; conn != nil {
					_ = conn.SetReadDeadline(time.Now().Add(-time.Second))
				}
				h.routes.mu.Unlock()
				time.Sleep(time.Millisecond)
			}
			if h.acceptViewerSourceLocked(viewer) {
				t.Fatal("closed or expired route retained viewer authorization")
			}
		})
	}
}

// Only a viewer address announced by the rendezvous service may become the
// forwarding target, and the window closes when the punch expires.
func TestHostViewerSourceGating(t *testing.T) {
	h, _ := testHostAgent(t)
	h.ctx = context.Background()
	h.punch = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 41000}
	viewer := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 7), Port: 50000}

	h.mu.Lock()
	accepted := h.acceptViewerSourceLocked(viewer)
	h.mu.Unlock()
	if accepted {
		t.Fatal("an unannounced viewer was accepted")
	}

	h.handleControl(wire.Encode(wire.Message{
		Type:      wire.TypePunchPeer,
		Candidate: viewer.String(),
		Nonce:     "0123456789abcdef",
	}), h.punch)
	h.mu.Lock()
	accepted = h.acceptViewerSourceLocked(viewer)
	h.mu.Unlock()
	if !accepted {
		t.Fatal("a punched viewer was rejected")
	}

	h.mu.Lock()
	h.punchPeers[viewer.String()] = time.Now().Add(-time.Second)
	h.mu.Unlock()
	h.mu.Lock()
	accepted = h.acceptViewerSourceLocked(viewer)
	_, kept := h.punchPeers[viewer.String()]
	h.mu.Unlock()
	if accepted || kept {
		t.Fatalf("an expired punch was accepted=%v kept=%v", accepted, kept)
	}

	h.mu.Lock()
	h.relaySet = []*net.UDPAddr{{IP: net.IPv4(127, 0, 0, 1), Port: 42000}}
	accepted = h.acceptViewerSourceLocked(h.relaySet[0])
	h.mu.Unlock()
	if !accepted {
		t.Fatal("a relay endpoint was rejected")
	}
}

// The host answers a viewer's punch probe by echoing the peer's transaction id,
// but only for an address the rendezvous service announced.
func TestHostAnswersPunchProbeOnlyForAnnouncedViewer(t *testing.T) {
	h, _ := testHostAgent(t)
	h.ctx = context.Background()
	go h.readLoop()

	viewer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer viewer.Close()
	h.rememberPunchPeer(viewer.LocalAddr().(*net.UDPAddr))
	target := h.conn.LocalAddr().(*net.UDPAddr)

	probe := wire.Encode(wire.Message{Type: wire.TypePunchProbe, TID: "0123456789abcdef"})
	viewer.WriteToUDP(probe, target)
	viewer.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, wire.MaxControlSize)
	n, _, err := viewer.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("announced viewer probe was not answered: %v", err)
	}
	ack, err := wire.Decode(buf[:n])
	if err != nil || ack.Type != wire.TypePunchAck || ack.TID != "0123456789abcdef" {
		t.Fatalf("probe answer = %+v, err=%v", ack, err)
	}

	stranger, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer stranger.Close()
	stranger.WriteToUDP(probe, target)
	stranger.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if n, _, err := stranger.ReadFromUDP(buf); err == nil {
		t.Fatalf("unannounced source got a punch answer: %q", buf[:n])
	}
}

// A probe round ends when the viewer sends real data, the listener side of
// RustDesk's punch_udp. An acknowledgement alone does not end it.
func TestHostPunchSessionStopsOnViewerData(t *testing.T) {
	h, _ := testHostAgent(t)
	peer := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 7), Port: 50000}
	stop := h.beginPunchSession(peer)
	h.confirmPunchPeer(peer.String())
	select {
	case <-stop:
	default:
		t.Fatal("viewer data did not stop the punch round")
	}
	h.mu.Lock()
	_, running := h.punchSessions[peer.String()]
	h.mu.Unlock()
	if running {
		t.Fatal("stopped punch round is still registered")
	}
	h.endPunchSession(peer.String(), stop)
}
