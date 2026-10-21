package relay

import (
	"io"
	"log/slog"
	"net"
	"strings"
	"testing"
	"time"

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
	conn.SetReadDeadline(time.Now().Add(timeout))
	buf := make([]byte, 4096)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("receive: %v", err)
	}
	return append([]byte(nil), buf[:n]...)
}

func startRelay(t *testing.T, id, keyHash string) (*Manager, *registry.Registry, *net.UDPAddr) {
	t.Helper()
	reg := registry.New(time.Minute)
	if _, err := reg.Register(registry.RegisterRequest{ID: id, KeyHash: keyHash, TTL: time.Minute}); err != nil {
		t.Fatalf("register: %v", err)
	}
	m, err := NewManager("127.0.0.1", []int{0}, reg, testLog)
	if err != nil {
		t.Fatalf("manager: %v", err)
	}
	t.Cleanup(m.Close)
	port, err := m.Allocate(id)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	reg.SetRelayPort(id, port)
	return m, reg, &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}
}

func TestRelayForwardsBothWays(t *testing.T) {
	key := strings.Repeat("a", 64)
	m, _, relayAddr := startRelay(t, "devbox", key)
	host := listenUDP(t)
	viewer := listenUDP(t)

	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: "devbox", KeyHash: key}), relayAddr)
	ack, err := wire.Decode(recv(t, host, 2*time.Second))
	if err != nil || !ack.OK {
		t.Fatalf("relay hello: %v %+v", err, ack)
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

func TestRelayRefusesSecondViewerAndBadKey(t *testing.T) {
	key := strings.Repeat("b", 64)
	_, _, relayAddr := startRelay(t, "devbox", key)
	host := listenUDP(t)
	viewer := listenUDP(t)
	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: "devbox", KeyHash: key}), relayAddr)
	if ack, err := wire.Decode(recv(t, host, 2*time.Second)); err != nil || !ack.OK {
		t.Fatalf("relay hello: %v %+v", err, ack)
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
	badHost.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: "devbox", KeyHash: strings.Repeat("c", 64)}), relayAddr)
	ack, err := wire.Decode(recv(t, badHost, 2*time.Second))
	if err != nil || ack.OK {
		t.Fatalf("bad key was accepted: %v %+v", err, ack)
	}
}

func TestRelayRequiresHello(t *testing.T) {
	key := strings.Repeat("d", 64)
	m, _, relayAddr := startRelay(t, "devbox", key)
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
