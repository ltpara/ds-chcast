package relay

import (
	"crypto/ed25519"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

var testLog = slog.New(slog.NewTextHandler(io.Discard, nil))

func listenUDP(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func recv(t *testing.T, conn *net.UDPConn, timeout time.Duration) []byte {
	t.Helper()
	data, _ := recvFrom(t, conn, timeout)
	return data
}

func recvFrom(t *testing.T, conn *net.UDPConn, timeout time.Duration) ([]byte, *net.UDPAddr) {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(timeout))
	buf := make([]byte, 4096)
	n, src, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("receive: %v", err)
	}
	return append([]byte(nil), buf[:n]...), src
}

func startRelay(t *testing.T, id, secret string) (*Manager, *registry.Registry, *net.UDPAddr, ed25519.PrivateKey) {
	t.Helper()
	return startRelayWith(t, id, secret)
}

func startRelayWith(t *testing.T, id, secret string, opts ...Option) (*Manager, *registry.Registry, *net.UDPAddr, ed25519.PrivateKey) {
	t.Helper()
	priv := identity.PrivateKeyFromSecret(secret)
	reg := registry.New(time.Minute)
	if _, err := reg.Register(registry.RegisterRequest{
		ID:        id,
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		TTL:       time.Minute,
	}); err != nil {
		t.Fatalf("register: %v", err)
	}
	m, err := NewManager("127.0.0.1", []int{0}, reg, testLog, opts...)
	if err != nil {
		t.Fatalf("manager: %v", err)
	}
	t.Cleanup(m.Close)
	port, err := m.Allocate(id)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	reg.SetRelay(id, port, "")
	return m, reg, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}, priv
}

// relayHandshake performs hello -> challenge -> signed proof and returns the
// final acknowledgement.
func relayHandshake(t *testing.T, conn *net.UDPConn, relayAddr *net.UDPAddr, id string, priv ed25519.PrivateKey) wire.Message {
	t.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: id}), relayAddr)
	challenge, err := wire.Decode(recv(t, conn, 2*time.Second))
	if err != nil || challenge.Type != wire.TypeRelayChallenge || challenge.Nonce == "" {
		t.Fatalf("relay challenge: %v %+v", err, challenge)
	}
	message := identity.RelayMessage(id, relayAddr.Port, challenge.Candidate, challenge.Nonce)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeRelayAuth,
		HostID:    id,
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, message),
	}), relayAddr)
	ack, err := wire.Decode(recv(t, conn, 2*time.Second))
	if err != nil {
		t.Fatalf("relay ack: %v", err)
	}
	return ack
}

func TestRelayForwardsBothWays(t *testing.T) {
	m, _, relayAddr, priv := startRelay(t, "devbox", "host-secret")
	host := listenUDP(t)
	viewer := listenUDP(t)

	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}

	viewer.WriteToUDP([]byte("quic-from-viewer"), relayAddr)
	if got := string(recv(t, host, 2*time.Second)); got != "quic-from-viewer" {
		t.Fatalf("host received %q", got)
	}
	host.WriteToUDP([]byte("quic-from-host"), relayAddr)
	if got := string(recv(t, viewer, 2*time.Second)); got != "quic-from-host" {
		t.Fatalf("viewer received %q", got)
	}

	stats := m.Stats().Ports[0]
	if !stats.HostOnline || stats.HostID != "devbox" {
		t.Fatalf("stats: %+v", stats)
	}
	if stats.Viewer != viewer.LocalAddr().String() {
		t.Fatalf("viewer in stats = %q, want %q", stats.Viewer, viewer.LocalAddr().String())
	}
	if stats.ToHost != uint64(len("quic-from-viewer")) || stats.ToViewer != uint64(len("quic-from-host")) {
		t.Fatalf("byte counters: %+v", stats)
	}
}

// TestBindHostKeepsRelaySourceAddress checks that a port moved to another
// local address replies from that address. Without the rebind the viewer sees
// the configured bind address and discards the relayed datagrams.
func TestBindHostKeepsRelaySourceAddress(t *testing.T) {
	m, _, relayAddr, priv := startRelay(t, "devbox", "host-secret")
	assigned := "127.0.0.2"
	if err := m.BindHost(relayAddr.Port, assigned); err != nil {
		t.Fatalf("bind host: %v", err)
	}
	target := &net.UDPAddr{IP: net.ParseIP(assigned), Port: relayAddr.Port}
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, target, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}

	viewer.WriteToUDP([]byte("quic-from-viewer"), target)
	data, src := recvFrom(t, host, 2*time.Second)
	if string(data) != "quic-from-viewer" {
		t.Fatalf("host received %q", data)
	}
	if !src.IP.Equal(net.ParseIP(assigned)) || src.Port != target.Port {
		t.Fatalf("host saw relay source %s, want %s", src, target)
	}
	host.WriteToUDP([]byte("quic-from-host"), src)
	reply, from := recvFrom(t, viewer, 2*time.Second)
	if string(reply) != "quic-from-host" {
		t.Fatalf("viewer received %q", reply)
	}
	if !from.IP.Equal(net.ParseIP(assigned)) || from.Port != target.Port {
		t.Fatalf("viewer saw relay source %s, want %s", from, target)
	}
}

func TestRelayRequiresViewerToken(t *testing.T) {
	_, reg, relayAddr, priv := startRelayWith(t, "devbox", "host-secret", WithViewerTokenRequired(true))
	record, ok := reg.Lookup("devbox")
	if !ok || record.CarrierToken == "" {
		t.Fatal("registry did not issue a carrier token")
	}
	token := record.CarrierToken
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}

	viewer.WriteToUDP([]byte("before-auth"), relayAddr)
	host.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
	buf := make([]byte, 64)
	if n, _, err := host.ReadFromUDP(buf); err == nil {
		t.Fatalf("viewer data was forwarded before auth: %q", buf[:n])
	}

	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type:   wire.TypeRelayViewer,
		HostID: "devbox",
		Token:  "wrong-token",
	}), relayAddr)
	if ack, err := wire.Decode(recv(t, viewer, 2*time.Second)); err != nil || ack.OK || ack.Error == "" {
		t.Fatalf("wrong viewer token: %v %+v", err, ack)
	}
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type:   wire.TypeRelayViewer,
		HostID: "devbox",
		Token:  token,
	}), relayAddr)
	if ack, err := wire.Decode(recv(t, viewer, 2*time.Second)); err != nil || !ack.OK {
		t.Fatalf("viewer token: %v %+v", err, ack)
	}

	viewer.WriteToUDP([]byte("after-auth"), relayAddr)
	if got := string(recv(t, host, 2*time.Second)); got != "after-auth" {
		t.Fatalf("host received %q", got)
	}
}

func TestRelayRefusesSecondViewerAndBadProof(t *testing.T) {
	_, _, relayAddr, priv := startRelay(t, "devbox", "host-secret")
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}
	viewer.WriteToUDP([]byte("first"), relayAddr)
	recv(t, host, 2*time.Second)

	second := listenUDP(t)
	second.WriteToUDP([]byte("second"), relayAddr)
	busy, err := wire.Decode(recv(t, second, 2*time.Second))
	if err != nil || busy.Type != wire.TypeBusy {
		t.Fatalf("second viewer got %v %+v", err, busy)
	}

	badHost := listenUDP(t)
	ack := relayHandshake(t, badHost, relayAddr, "devbox", identity.PrivateKeyFromSecret("another-secret"))
	if ack.OK || ack.Error == "" {
		t.Fatalf("bad proof was accepted: %+v", ack)
	}
}

func TestRelayRejectsReplayedProof(t *testing.T) {
	_, _, relayAddr, priv := startRelay(t, "devbox", "host-secret")
	host := listenUDP(t)
	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: "devbox"}), relayAddr)
	challenge, err := wire.Decode(recv(t, host, 2*time.Second))
	if err != nil || challenge.Type != wire.TypeRelayChallenge {
		t.Fatalf("challenge: %v %+v", err, challenge)
	}
	auth := wire.Message{
		Type:      wire.TypeRelayAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, identity.RelayMessage("devbox", relayAddr.Port, challenge.Candidate, challenge.Nonce)),
	}
	host.WriteToUDP(wire.Encode(auth), relayAddr)
	if ack, _ := wire.Decode(recv(t, host, 2*time.Second)); !ack.OK {
		t.Fatalf("first proof refused: %+v", ack)
	}
	// A new hello issues a fresh nonce; replaying the old proof must fail.
	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: "devbox"}), relayAddr)
	recv(t, host, 2*time.Second)
	host.WriteToUDP(wire.Encode(auth), relayAddr)
	ack, err := wire.Decode(recv(t, host, 2*time.Second))
	if err != nil || ack.OK {
		t.Fatalf("replayed proof accepted: %v %+v", err, ack)
	}
}

func TestRelayRequiresHello(t *testing.T) {
	m, _, relayAddr, _ := startRelay(t, "devbox", "host-secret")
	viewer := listenUDP(t)
	viewer.WriteToUDP([]byte("nobody-home"), relayAddr)
	time.Sleep(50 * time.Millisecond)
	// The viewer's datagram must not even be acknowledged, and the port must
	// not report an online host without an authenticated hello.
	for _, p := range m.Stats().Ports {
		if p.HostOnline {
			t.Fatalf("port %d reported online without hello", p.Port)
		}
	}
}
