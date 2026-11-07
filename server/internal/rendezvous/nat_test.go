package rendezvous

import (
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func TestNatProbeReportsObservedSource(t *testing.T) {
	_, _, meetAddr, _ := startMeet(t)
	probe := listenUDP(t)
	const nonce = "nat-probe-nonce-1"
	probe.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeNatProbe, Nonce: nonce}), meetAddr)
	ack := recv(t, probe)
	if ack.Type != wire.TypeNatProbeAck || ack.Nonce != nonce {
		t.Fatalf("nat probe ack: %+v", ack)
	}
	if ack.Candidate != probe.LocalAddr().String() {
		t.Fatalf("observed = %q, want %q", ack.Candidate, probe.LocalAddr())
	}
}

// A short nonce is dropped silently: answering garbage would turn the punch
// port into a reflector.
func TestNatProbeDropsShortNonce(t *testing.T) {
	_, _, meetAddr, _ := startMeet(t)
	probe := listenUDP(t)
	probe.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeNatProbe, Nonce: "short"}), meetAddr)
	_ = probe.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, wire.MaxControlSize)
	if n, _, err := probe.ReadFromUDP(buf); err == nil {
		t.Fatalf("short nonce got a reply: %s", buf[:n])
	}
}

// The host makes the short-circuit decision from the viewer's NAT type, so the
// punch request classification must survive the trip through the server.
func TestPunchRequestForwardsViewerNATType(t *testing.T) {
	_, _, meetAddr, priv := startMeet(t)
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}
	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
		NATType: "symmetric",
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.Type != wire.TypePunchPeer {
		t.Fatalf("punch reply: %+v", reply)
	}
	notice := recv(t, host)
	if notice.Type != wire.TypePunchPeer || notice.NATType != "symmetric" {
		t.Fatalf("host notice = %+v", notice)
	}

	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
		NATType: "not-a-classification",
	}), meetAddr)
	_ = recv(t, viewer)
	if notice := recv(t, host); notice.NATType != "" {
		t.Fatalf("unexpected NAT type forwarded: %+v", notice)
	}
}
