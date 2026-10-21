package rendezvous

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

func listenUDP(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func recv(t *testing.T, conn *net.UDPConn) wire.Message {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("receive: %v", err)
	}
	msg, err := wire.Decode(buf[:n])
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return msg
}

func startMeet(t *testing.T) (*Server, *registry.Registry, *net.UDPAddr) {
	t.Helper()
	reg := registry.New(5 * time.Second)
	reg.Register(registry.RegisterRequest{
		ID:      "devbox",
		KeyHash: strings.Repeat("a", 64),
		TTL:     time.Minute,
	})
	reg.SetRelayPort("devbox", 41000)
	meet, err := New("127.0.0.1:0", reg, func(port int) string { return "192.0.2.1:41000" }, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatalf("rendezvous: %v", err)
	}
	go meet.Serve()
	t.Cleanup(meet.Close)
	addr, err := net.ResolveUDPAddr("udp", meet.Addr())
	if err != nil {
		t.Fatal(err)
	}
	return meet, reg, addr
}

func TestKeepaliveAndPunchExchange(t *testing.T) {
	_, _, meetAddr := startMeet(t)
	host := listenUDP(t)
	viewer := listenUDP(t)

	host.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypeKeepalive, HostID: "devbox", KeyHash: strings.Repeat("a", 64),
	}), meetAddr)
	ack := recv(t, host)
	if !ack.OK || ack.Type != wire.TypeKeepaliveAck {
		t.Fatalf("keepalive: %+v", ack)
	}
	if ack.Candidate != host.LocalAddr().String() {
		t.Fatalf("observed candidate %q, want %q", ack.Candidate, host.LocalAddr().String())
	}
	if ack.Relay != "192.0.2.1:41000" {
		t.Fatalf("relay = %q", ack.Relay)
	}

	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.Type != wire.TypePunchPeer {
		t.Fatalf("punch reply: %+v", reply)
	}
	if reply.Candidate != host.LocalAddr().String() || reply.Nonce != nonce {
		t.Fatalf("punch reply: %+v", reply)
	}
	notify := recv(t, host)
	if notify.Type != wire.TypePunchPeer || notify.Candidate != viewer.LocalAddr().String() {
		t.Fatalf("host notification: %+v", notify)
	}
	if notify.Nonce != nonce || notify.ExpiresMS <= 0 {
		t.Fatalf("host notification: %+v", notify)
	}
}

func TestKeepaliveRejectsWrongKeyAndUnknownPunch(t *testing.T) {
	_, _, meetAddr := startMeet(t)
	stranger := listenUDP(t)
	stranger.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypeKeepalive, HostID: "devbox", KeyHash: strings.Repeat("b", 64),
	}), meetAddr)
	ack := recv(t, stranger)
	if ack.OK || ack.Error == "" {
		t.Fatalf("wrong key accepted: %+v", ack)
	}

	missing := listenUDP(t)
	missing.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "ghost", Nonce: "0123456789abcdef",
	}), meetAddr)
	reply := recv(t, missing)
	if reply.OK || reply.Error == "" {
		t.Fatalf("unknown host accepted: %+v", reply)
	}
}

func TestPunchRequiresFreshKeepalive(t *testing.T) {
	_, reg, meetAddr := startMeet(t)
	viewer := listenUDP(t)
	// This host is registered but has never sent a keepalive, so its observed
	// address is unknown and a punch cannot succeed.
	_ = reg
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: "0123456789abcdef",
	}), meetAddr)
	reply := recv(t, viewer)
	if reply.OK || reply.Error != "host is offline" {
		t.Fatalf("offline host punch: %+v", reply)
	}
}

func TestRateLimiter(t *testing.T) {
	l := newLimiter(2, 50*time.Millisecond)
	if !l.allow("198.51.100.1") || !l.allow("198.51.100.1") {
		t.Fatal("first two requests were refused")
	}
	if l.allow("198.51.100.1") {
		t.Fatal("third request in the window was allowed")
	}
	if !l.allow("198.51.100.2") {
		t.Fatal("another source was refused")
	}
	time.Sleep(60 * time.Millisecond)
	if !l.allow("198.51.100.1") {
		t.Fatal("window did not reset")
	}
}
