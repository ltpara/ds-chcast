package agent

import (
	"context"
	"net"
	"testing"
	"time"

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
