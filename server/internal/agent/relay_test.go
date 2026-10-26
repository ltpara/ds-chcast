package agent

import (
	"crypto/ed25519"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func testHostAgent(t *testing.T) (*HostAgent, ed25519.PrivateKey) {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	priv := identity.PrivateKeyFromSecret("host-secret")
	return &HostAgent{
		conn: conn,
		log:  quietLogger(),
		cfg:  HostConfig{ID: "devbox"},
		priv: priv,
	}, priv
}

func TestApplyRegistrationKeepsRelaySet(t *testing.T) {
	h, _ := testHostAgent(t)
	h.applyRegistration(registerResult{
		Relay:        "127.0.0.1:41000",
		Relays:       []string{"127.0.0.1:41000", "127.0.0.2:41000", "127.0.0.1:41000"},
		ExpiresInSec: 60,
	})
	if len(h.relayEndpoints()) != 2 {
		t.Fatalf("relay endpoints: %+v", h.relayEndpoints())
	}
	for _, allowed := range []string{"127.0.0.1:41000", "127.0.0.2:41000"} {
		addr, err := net.ResolveUDPAddr("udp", allowed)
		if err != nil {
			t.Fatal(err)
		}
		if !h.relayAllowed(addr) {
			t.Fatalf("advertised relay %s was rejected", allowed)
		}
	}
	if h.relayAllowed(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 3), Port: 41000}) {
		t.Fatal("unadvertised relay address was accepted")
	}
	if h.relayAllowed(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 2), Port: 41001}) {
		t.Fatal("advertised host on a different port was accepted")
	}

	// A later reply that only carries the primary endpoint drops the stale set.
	h.applyRegistration(registerResult{Relay: "127.0.0.1:41000", ExpiresInSec: 60})
	if h.relayAllowed(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 2), Port: 41000}) {
		t.Fatal("stale relay endpoint survived the next registration")
	}
}

func TestRelayChallengeAcceptedFromAdvertisedAddress(t *testing.T) {
	h, priv := testHostAgent(t)

	// The relay challenges from the lease's port, so bind the alternate
	// advertised address on that same port.
	alt, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP("127.0.0.2"), Port: 41000})
	if err != nil {
		t.Skipf("cannot bind the alternate loopback address: %v", err)
	}
	defer alt.Close()

	h.applyRegistration(registerResult{
		Relay:  "127.0.0.1:41000",
		Relays: []string{"127.0.0.1:41000", "127.0.0.2:41000"},
	})
	src := alt.LocalAddr().(*net.UDPAddr)
	challenge := wire.Message{
		Type:      wire.TypeRelayChallenge,
		HostID:    "devbox",
		Nonce:     "nonce-1",
		Candidate: "198.51.100.7:6000",
	}
	h.handleControl(wire.Encode(challenge), src)

	alt.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, _, err := alt.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("relay auth reply: %v", err)
	}
	reply, err := wire.Decode(buf[:n])
	if err != nil {
		t.Fatal(err)
	}
	if reply.Type != wire.TypeRelayAuth || reply.Nonce != challenge.Nonce || reply.Candidate != challenge.Candidate {
		t.Fatalf("relay auth: %+v", reply)
	}
	message := identity.RelayMessage("devbox", src.Port, challenge.Candidate, challenge.Nonce)
	if !identity.Verify(priv.Public().(ed25519.PublicKey), message, reply.Signature) {
		t.Fatal("relay auth signature does not cover the alternate source port")
	}

	// A challenge from an address outside the advertised set is ignored.
	h.handleControl(wire.Encode(challenge), &net.UDPAddr{IP: net.IPv4(127, 0, 0, 3), Port: 5000})
	alt.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, _, err := alt.ReadFromUDP(buf); err == nil {
		t.Fatal("challenge from an unadvertised address produced a reply")
	}
}
