package rendezvous

import (
	"crypto/ed25519"
	"net"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func listenOn(t *testing.T, ip string) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP(ip)})
	if err != nil {
		t.Skipf("cannot bind %s: %v", ip, err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

// keepaliveSigned lets a test sign the challenge with a key that does not
// match the registered host, exercising failures on the audit path.
func keepaliveSigned(t *testing.T, conn *net.UDPConn, meetAddr *net.UDPAddr, signer ed25519.PrivateKey) wire.Message {
	t.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, conn)
	if challenge.Type != wire.TypeKeepaliveChallenge || challenge.Nonce == "" {
		t.Fatalf("challenge: %+v", challenge)
	}
	message := identity.KeepaliveMessage("devbox", challenge.Candidate, challenge.Nonce)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(signer, message),
	}), meetAddr)
	return recv(t, conn)
}

func TestKeepaliveIPChangeAudit(t *testing.T) {
	meet, _, meetAddr, priv := startMeet(t)
	tracker := ipchange.New(0)
	meet.SetIPChanges(tracker)

	first := listenOn(t, "127.0.0.1")
	if ack := keepalive(t, first, meetAddr, priv); !ack.OK {
		t.Fatalf("first keepalive: %+v", ack)
	}
	if stats := tracker.Stats(); stats.Tracked != 0 || stats.Changes != 0 {
		t.Fatalf("baseline should not be a change: %+v", stats)
	}

	second := listenOn(t, "127.0.0.2")
	if ack := keepalive(t, second, meetAddr, priv); !ack.OK {
		t.Fatalf("second keepalive: %+v", ack)
	}
	if stats := tracker.Stats(); stats.Tracked != 1 || stats.Changes != 1 {
		t.Fatalf("after the address change: %+v", stats)
	}
	entry, ok := tracker.Lookup("devbox")
	if !ok || len(entry.IPs) != 2 {
		t.Fatalf("audit entry: %+v (found=%v)", entry, ok)
	}

	third := listenOn(t, "127.0.0.3")
	bad := keepaliveSigned(t, third, meetAddr, identity.PrivateKeyFromSecret("someone else"))
	if bad.OK || bad.Error == "" {
		t.Fatalf("bad signature was accepted: %+v", bad)
	}
	if stats := tracker.Stats(); stats.Tracked != 1 || stats.Changes != 1 {
		t.Fatalf("failed keepalive polluted the audit: %+v", stats)
	}
	if ack := keepalive(t, third, meetAddr, priv); !ack.OK {
		t.Fatalf("third keepalive: %+v", ack)
	}
	if stats := tracker.Stats(); stats.Tracked != 1 || stats.Changes != 2 {
		t.Fatalf("after the third address: %+v", stats)
	}
}

func TestKeepaliveUsesAssignedRelayHost(t *testing.T) {
	_, reg, meetAddr, priv := startMeet(t)
	reg.SetRelay("devbox", 41000, "relay-b.example")
	ack := keepalive(t, listenUDP(t), meetAddr, priv)
	if !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}
	if ack.Relay != "relay-b.example:41000" {
		t.Fatalf("relay = %q, want relay-b.example:41000", ack.Relay)
	}
}
