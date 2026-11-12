package relay

import (
	"crypto/ed25519"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// These benchmarks cover the two costs the server owns on the relay path: the
// lease handed to a registering host and the per-datagram forwarding loop.

func benchListenUDP(b *testing.B) *net.UDPConn {
	b.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		b.Fatal(err)
	}
	b.Cleanup(func() { conn.Close() })
	return conn
}

func benchRecv(b *testing.B, conn *net.UDPConn) []byte {
	b.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 4096)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		b.Fatal(err)
	}
	return append([]byte(nil), buf[:n]...)
}

func benchRelayHandshake(b *testing.B, conn *net.UDPConn, relayAddr *net.UDPAddr, id string, priv ed25519.PrivateKey) wire.Message {
	b.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: id}), relayAddr)
	challenge, err := wire.Decode(benchRecv(b, conn))
	if err != nil || challenge.Type != wire.TypeRelayChallenge || challenge.Nonce == "" {
		b.Fatalf("relay challenge: %v %+v", err, challenge)
	}
	message := identity.RelayMessage(id, relayAddr.Port, challenge.Candidate, challenge.Nonce)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeRelayAuth,
		HostID:    id,
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, message),
	}), relayAddr)
	ack, err := wire.Decode(benchRecv(b, conn))
	if err != nil {
		b.Fatal(err)
	}
	return ack
}

func startBenchRelay(b *testing.B, opts ...Option) (*Manager, *net.UDPAddr, ed25519.PrivateKey) {
	b.Helper()
	priv := identity.PrivateKeyFromSecret("bench host secret")
	reg := registry.New(time.Minute)
	if _, err := reg.Register(registry.RegisterRequest{
		ID:        "bench-host",
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		TTL:       time.Minute,
	}); err != nil {
		b.Fatal(err)
	}
	m, err := NewManager("127.0.0.1", []int{0}, reg, testLog, opts...)
	if err != nil {
		b.Fatal(err)
	}
	b.Cleanup(m.Close)
	port, err := m.Allocate("bench-host")
	if err != nil {
		b.Fatal(err)
	}
	reg.SetRelay("bench-host", port, "")
	return m, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}, priv
}

// BenchmarkRelayLease measures handing a relay port to a host and giving it
// back, the work every registration and unregistration performs.
func BenchmarkRelayLease(b *testing.B) {
	m, _, _ := startBenchRelay(b)

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := m.Allocate("bench-host"); err != nil {
			b.Fatal(err)
		}
		m.Release("bench-host")
	}
}

// BenchmarkRelayForward measures one 1200-byte viewer datagram through the
// relay: policy charge, loopback write to the host and counters. The bandwidth
// shaper is disabled here; with the defaults a session is capped at hbbr's
// 128 Mbit/s, so leaving it on would measure the cap rather than the
// forwarding path.
func BenchmarkRelayForward(b *testing.B) {
	m, relayAddr, priv := startBenchRelay(b, WithViewerTokenRequired(false))
	if err := m.SetPolicyConfig(PolicyConfig{}); err != nil {
		b.Fatal(err)
	}
	host := benchListenUDP(b)
	viewer := benchListenUDP(b)
	if ack := benchRelayHandshake(b, host, relayAddr, "bench-host", priv); !ack.OK {
		b.Fatalf("relay handshake: %+v", ack)
	}
	payload := make([]byte, 1200)

	b.SetBytes(int64(len(payload)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := viewer.WriteToUDP(payload, relayAddr); err != nil {
			b.Fatal(err)
		}
		if got := benchRecv(b, host); len(got) != len(payload) {
			b.Fatalf("forwarded %d bytes, want %d", len(got), len(payload))
		}
	}
}

// BenchmarkRelayBurst sends a batch before reading it back, so the reported
// rate reflects forwarding throughput instead of the round-trip latency that
// bounds BenchmarkRelayForward.
func BenchmarkRelayBurst(b *testing.B) {
	const batch = 16
	m, relayAddr, priv := startBenchRelay(b, WithViewerTokenRequired(false))
	if err := m.SetPolicyConfig(PolicyConfig{}); err != nil {
		b.Fatal(err)
	}
	host := benchListenUDP(b)
	viewer := benchListenUDP(b)
	if ack := benchRelayHandshake(b, host, relayAddr, "bench-host", priv); !ack.OK {
		b.Fatalf("relay handshake: %+v", ack)
	}
	payload := make([]byte, 1200)

	b.SetBytes(batch * int64(len(payload)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		for j := 0; j < batch; j++ {
			if _, err := viewer.WriteToUDP(payload, relayAddr); err != nil {
				b.Fatal(err)
			}
		}
		for j := 0; j < batch; j++ {
			if got := benchRecv(b, host); len(got) != len(payload) {
				b.Fatalf("forwarded %d bytes, want %d", len(got), len(payload))
			}
		}
	}
}
