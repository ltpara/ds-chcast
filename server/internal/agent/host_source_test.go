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
