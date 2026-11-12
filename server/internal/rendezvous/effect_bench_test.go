package rendezvous

import (
	"crypto/ed25519"
	"io"
	"log/slog"
	"net"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// The benchmarks below drive the same exchanges an agent and a viewer use.
// Rate limits are disabled so the numbers describe the handler, not the
// deliberate per-IP caps; the caps themselves are asserted by the unit tests.

func benchListenUDP(b *testing.B) *net.UDPConn {
	b.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		b.Fatal(err)
	}
	b.Cleanup(func() { conn.Close() })
	return conn
}

func benchRecv(b *testing.B, conn *net.UDPConn) wire.Message {
	b.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		b.Fatal(err)
	}
	msg, err := wire.Decode(buf[:n])
	if err != nil {
		b.Fatal(err)
	}
	return msg
}

// startBenchMeet builds the rendezvous server around one registered host. A
// non-empty candidate marks the host online and is what a viewer is told to
// punch; the benchmarks pass the local drain socket so the instruction stays on
// loopback instead of leaving for a documentation range.
func startBenchMeet(b *testing.B, candidate string) (*Server, *registry.Registry, *net.UDPAddr, ed25519.PrivateKey) {
	b.Helper()
	priv := identity.PrivateKeyFromSecret("bench host secret")
	pub := identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
	reg := registry.New(time.Minute)
	if _, err := reg.Register(registry.RegisterRequest{
		ID:        "bench-host",
		PublicKey: pub,
		TTL:       time.Minute,
	}); err != nil {
		b.Fatal(err)
	}
	reg.SetRelay("bench-host", 41000, "")
	if candidate != "" {
		if err := reg.Touch("bench-host", candidate, []string{"192.168.1.9:5000"}, "port-restricted"); err != nil {
			b.Fatal(err)
		}
	}
	meet, err := New("127.0.0.1:0", reg, func(port int) string {
		return "198.51.100.1:" + strconv.Itoa(port)
	}, slog.New(slog.NewTextHandler(io.Discard, nil)),
		WithKeepaliveRateMax(0), WithPunchRateMax(0))
	if err != nil {
		b.Fatal(err)
	}
	serverIdentity, err := identity.GenerateServer()
	if err != nil {
		b.Fatal(err)
	}
	meet.SetIdentity(serverIdentity)
	go meet.Serve()
	b.Cleanup(meet.Close)
	addr, err := net.ResolveUDPAddr("udp", meet.Addr())
	if err != nil {
		b.Fatal(err)
	}
	return meet, reg, addr, priv
}

// BenchmarkKeepalive measures one full challenge-response: the server issues a
// nonce, verifies the host signature and refreshes the mapping.
func BenchmarkKeepalive(b *testing.B) {
	_, _, meetAddr, priv := startBenchMeet(b, "")
	conn := benchListenUDP(b)

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "bench-host"}), meetAddr)
		challenge := benchRecv(b, conn)
		if challenge.Type != wire.TypeKeepaliveChallenge || challenge.Nonce == "" {
			b.Fatalf("challenge: %+v", challenge)
		}
		message := identity.KeepaliveMessage("bench-host", challenge.Candidate, challenge.Nonce)
		conn.WriteToUDP(wire.Encode(wire.Message{
			Type:       wire.TypeKeepaliveAuth,
			HostID:     "bench-host",
			Nonce:      challenge.Nonce,
			Candidate:  challenge.Candidate,
			Signature:  identity.Sign(priv, message),
			LocalAddrs: []string{"192.168.1.9:14434"},
			NATType:    "port-restricted",
		}), meetAddr)
		ack := benchRecv(b, conn)
		if !ack.OK {
			b.Fatalf("keepalive ack: %+v", ack)
		}
	}
}

// BenchmarkPunch measures one viewer punch: lookup, candidate exchange, the
// instruction to the host and a server-signed reply to the viewer. The host
// socket is drained because the server forwards the viewer candidate to it.
func BenchmarkPunch(b *testing.B) {
	viewer := benchListenUDP(b)
	host := benchListenUDP(b)
	_, _, meetAddr, _ := startBenchMeet(b, host.LocalAddr().String())
	drained := make(chan struct{})
	go func() {
		defer close(drained)
		buf := make([]byte, 2048)
		for {
			if _, _, err := host.ReadFromUDP(buf); err != nil {
				return
			}
		}
	}()
	b.Cleanup(func() {
		host.Close()
		<-drained
	})

	const nonce = "bench-punch-nonce"
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		viewer.WriteToUDP(wire.Encode(wire.Message{
			Type: wire.TypePunchRequest, HostID: "bench-host", Nonce: nonce,
		}), meetAddr)
		reply := benchRecv(b, viewer)
		if !reply.OK || reply.Type != wire.TypePunchPeer || reply.Binding == "" {
			b.Fatalf("punch reply: %+v", reply)
		}
	}
}
