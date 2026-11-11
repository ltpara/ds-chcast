package rendezvous

import (
	"net"
	"strconv"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/lanmask"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// A LAN host and a WAN viewer cannot punch to each other; hbbs reports the
// peer as symmetric in both directions so both sides start on the relay. The
// LAN half is also handed the server's local relay address.
func TestLANXORForcesRelayOnUDPPunch(t *testing.T) {
	mask, err := lanmask.Parse("127.0.0.0/8")
	if err != nil {
		t.Fatal(err)
	}
	_, reg, meetAddr, priv := startMeet(t, WithLAN(mask, "192.168.50.10"))
	host := listenUDP(t)
	viewer := listenUDP(t)
	ack := keepalive(t, host, meetAddr, priv)
	if ack.Relay != "192.168.50.10:41000" {
		t.Fatalf("LAN host relay = %q, want the local address", ack.Relay)
	}
	// Move the host's observed mapping to a WAN address while the viewer stays
	// on loopback: the pair is LAN/WAN even though both sockets are local.
	if err := reg.Touch("devbox", "198.51.100.7:5000", nil, "asymmetric"); err != nil {
		t.Fatal(err)
	}

	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
		LocalAddrs: []string{"192.168.50.20:55000"},
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.NATType != "symmetric" {
		t.Fatalf("viewer was not forced onto the relay: %+v", reply)
	}
	if reply.Relay != "192.168.50.10:41000" {
		t.Fatalf("viewer relay = %q, want the local address", reply.Relay)
	}
	if reply.IsLocal {
		t.Fatalf("a cross-network punch was marked local: %+v", reply)
	}
}

// The LAN/WAN classification is what both punch legs exchange.
func TestPunchNATForcesSymmetricAcrossLAN(t *testing.T) {
	mask, err := lanmask.Parse("10.0.0.0/8")
	if err != nil {
		t.Fatal(err)
	}
	s := &Server{lanMask: mask}
	lanHost := registry.Host{Candidate: "10.1.2.3:5000", NATType: "asymmetric"}
	wanHost := registry.Host{Candidate: "198.51.100.7:5000", NATType: "asymmetric"}
	lanViewer := net.ParseIP("10.9.8.7")
	wanViewer := net.ParseIP("203.0.113.9")
	if got := s.punchNAT(lanHost, lanHost.NATType, wanViewer); got != "symmetric" {
		t.Fatalf("LAN host / WAN viewer = %q", got)
	}
	if got := s.punchNAT(wanHost, wanHost.NATType, lanViewer); got != "symmetric" {
		t.Fatalf("WAN host / LAN viewer = %q", got)
	}
	if got := s.punchNAT(lanHost, lanHost.NATType, lanViewer); got != "asymmetric" {
		t.Fatalf("LAN pair = %q", got)
	}
	if got := s.punchNAT(wanHost, wanHost.NATType, wanViewer); got != "asymmetric" {
		t.Fatalf("WAN pair = %q", got)
	}
	if got := s.punchNAT(wanHost, "not-a-classification", wanViewer); got != "" {
		t.Fatalf("unknown classification = %q", got)
	}
}

// Both observed addresses inside the mask stay on the direct path; the mask
// only widens what counts as one network.
func TestLANPairKeepsDirectPunch(t *testing.T) {
	mask, err := lanmask.Parse("127.0.0.0/8")
	if err != nil {
		t.Fatal(err)
	}
	_, _, meetAddr, priv := startMeet(t, WithLAN(mask, "192.168.50.10"))
	host := listenUDP(t)
	viewer := listenUDP(t)
	keepalive(t, host, meetAddr, priv)

	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
		NATType:    "asymmetric",
		LocalAddrs: []string{"192.168.50.20:55000"},
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.NATType == "symmetric" {
		t.Fatalf("LAN pair was forced onto the relay: %+v", reply)
	}
	if !reply.IsLocal {
		t.Fatalf("the punch was not marked local: %+v", reply)
	}
	if reply.Relay != "192.168.50.10:41000" {
		t.Fatalf("LAN viewer relay = %q", reply.Relay)
	}
	notice := recv(t, host)
	if notice.NATType != "asymmetric" {
		t.Fatalf("viewer NAT type was rewritten: %+v", notice)
	}
	if len(notice.LocalAddrs) != 1 {
		t.Fatalf("LAN candidates were dropped: %v", notice.LocalAddrs)
	}
}

func TestRelayForPicksTheLocalAddressPerSide(t *testing.T) {
	mask, err := lanmask.Parse("10.0.0.0/8")
	if err != nil {
		t.Fatal(err)
	}
	s := &Server{
		relayAddr:  func(port int) string { return "203.0.113.5:" + strconv.Itoa(port) },
		lanMask:    mask,
		lanLocalIP: "10.1.2.3",
	}
	local := registry.Host{RelayPort: 41000}
	if got := s.relayFor(local, true); got != "10.1.2.3:41000" {
		t.Fatalf("LAN side = %q", got)
	}
	if got := s.relayFor(local, false); got != "203.0.113.5:41000" {
		t.Fatalf("WAN side = %q", got)
	}
	node := registry.Host{RelayPort: 41000, RelayNode: "relay1", RelayHost: "relay1.example.com"}
	if got := s.relayFor(node, true); got != "relay1.example.com:41000" {
		t.Fatalf("node lease was rewritten: %q", got)
	}
}
