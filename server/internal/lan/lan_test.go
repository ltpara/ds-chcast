package lan

import (
	"context"
	"log/slog"
	"net"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
)

func startResponder(t *testing.T, info Info) *Responder {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	r, err := Start(ctx, "127.0.0.1:0", info, nil)
	if err != nil {
		t.Fatalf("start responder: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func portOf(t *testing.T, addr net.Addr) int {
	t.Helper()
	udp, ok := addr.(*net.UDPAddr)
	if !ok {
		t.Fatalf("unexpected responder address %T", addr)
	}
	return udp.Port
}

func TestResponderAnswersPing(t *testing.T) {
	r := startResponder(t, Info{
		ID:       "devbox",
		Hostname: "devbox-host",
		Username: "alice",
		Platform: "windows/amd64",
		Version:  "1.2.3",
		MAC:      "aa:bb:cc:dd:ee:ff",
	})
	port := portOf(t, r.Addr())
	target := net.JoinHostPort("127.0.0.1", strconv.Itoa(port))

	peers, err := Discover(context.Background(), DiscoverOptions{
		Port:    port,
		Timeout: time.Second,
		Targets: []string{target},
	})
	if err != nil {
		t.Fatalf("discover: %v", err)
	}
	if len(peers) != 1 {
		t.Fatalf("want one peer, got %+v", peers)
	}
	got := peers[0]
	if got.ID != "devbox" || got.Hostname != "devbox-host" || got.Username != "alice" ||
		got.Platform != "windows/amd64" || got.Version != "1.2.3" ||
		got.MAC != "aa:bb:cc:dd:ee:ff" {
		t.Fatalf("peer metadata: %+v", got)
	}
	if got.Address != target {
		t.Fatalf("peer address %q, want %q", got.Address, target)
	}
}

func TestDiscoverFiltersSelf(t *testing.T) {
	r := startResponder(t, Info{ID: "devbox", Hostname: "devbox-host"})
	port := portOf(t, r.Addr())

	peers, err := Discover(context.Background(), DiscoverOptions{
		Port:    port,
		Timeout: 500 * time.Millisecond,
		SelfID:  "devbox",
		Targets: []string{net.JoinHostPort("127.0.0.1", strconv.Itoa(port))},
	})
	if err != nil {
		t.Fatalf("discover: %v", err)
	}
	if len(peers) != 0 {
		t.Fatalf("self must be filtered out: %+v", peers)
	}
}

func TestResponderIgnoresOwnIDAndBadRequests(t *testing.T) {
	const id = "devbox"
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelDebug}))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r, err := Start(ctx, "127.0.0.1:0", Info{ID: id}, logger)
	if err != nil {
		t.Fatalf("start responder: %v", err)
	}
	defer r.Close()
	addr := r.Addr().(*net.UDPAddr)

	conn, err := net.DialUDP("udp4", nil, addr)
	if err != nil {
		t.Fatalf("client: %v", err)
	}
	defer conn.Close()
	buf := make([]byte, MaxDatagram)

	expectSilence := func(name string, payload []byte) {
		t.Helper()
		if _, err := conn.Write(payload); err != nil {
			t.Fatalf("%s: write: %v", name, err)
		}
		_ = conn.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
		if n, _, err := conn.ReadFromUDP(buf); err == nil {
			t.Fatalf("%s: unexpected reply (%d bytes)", name, n)
		}
	}
	expectSilence("own id", encode(Message{Type: TypePing, ID: id}))
	expectSilence("invalid id", encode(Message{Type: TypePing, ID: "bad id"}))
	expectSilence("garbage", []byte("hello"))
	expectSilence("wrong type", encode(Message{Type: "pong"}))
	expectSilence("oversize", append([]byte(Magic), make([]byte, MaxDatagram)...))

	// A valid ping still gets an answer after the rejected traffic.
	if _, err := conn.Write(encode(Message{Type: TypePing})); err != nil {
		t.Fatalf("valid ping: %v", err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(time.Second))
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("valid ping: %v", err)
	}
	msg, err := decode(buf[:n])
	if err != nil || msg.Type != TypePong || msg.ID != id {
		t.Fatalf("reply: %+v, %v", msg, err)
	}
}

func TestDiscoverKeepsOnePeerPerAddress(t *testing.T) {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("fake responder: %v", err)
	}
	defer conn.Close()
	go func() {
		buf := make([]byte, MaxDatagram)
		if _, src, err := conn.ReadFromUDP(buf); err == nil {
			valid := encode(Message{Type: TypePong, ID: "peer", Hostname: "host-1"})
			_, _ = conn.WriteToUDP(valid, src)
			_, _ = conn.WriteToUDP(valid, src)
			_, _ = conn.WriteToUDP(encode(Message{Type: TypePong, ID: "bad id"}), src)
			_, _ = conn.WriteToUDP(encode(Message{Type: TypePing, ID: "peer"}), src)
			_, _ = conn.WriteToUDP([]byte("garbage"), src)
		}
	}()
	port := conn.LocalAddr().(*net.UDPAddr).Port

	peers, err := Discover(context.Background(), DiscoverOptions{
		Port:    port,
		Timeout: time.Second,
		Targets: []string{net.JoinHostPort("127.0.0.1", strconv.Itoa(port))},
	})
	if err != nil {
		t.Fatalf("discover: %v", err)
	}
	if len(peers) != 1 || peers[0].ID != "peer" || peers[0].Hostname != "host-1" {
		t.Fatalf("peers: %+v", peers)
	}
}

func TestDiscoveryTargetsIncludeBroadcastAndLoopback(t *testing.T) {
	targets := discoveryTargets(DefaultPort)
	joined := strings.Join(targets, ",")
	if !strings.Contains(joined, "255.255.255.255:21129") {
		t.Fatalf("missing global broadcast: %v", targets)
	}
	if !strings.Contains(joined, "127.0.0.1:21129") {
		t.Fatalf("missing loopback: %v", targets)
	}
}

func TestMessageRoundTripAndDecodeLimits(t *testing.T) {
	msg := Message{Type: TypePong, ID: "devbox", Hostname: "box", Username: "u",
		Platform: "windows/amd64", Version: "1.0", MAC: "aa:bb"}
	got, err := decode(encode(msg))
	if err != nil || got != msg {
		t.Fatalf("round trip: %+v, %v", got, err)
	}
	for name, payload := range map[string][]byte{
		"empty":    nil,
		"no magic": []byte(`{"type":"ping"}`),
		"bad json": []byte(Magic + "{"),
		"unknown":  []byte(Magic + `{"type":"hello"}`),
		"oversize": append([]byte(Magic), make([]byte, MaxDatagram)...),
	} {
		if _, err := decode(payload); err == nil {
			t.Fatalf("%s: expected an error", name)
		}
	}
}

func TestCleanBoundsAndStripsControls(t *testing.T) {
	if got := clean("ab\x1b[31mcd\x00", 32); got != "ab[31mcd" {
		t.Fatalf("control characters: %q", got)
	}
	if got := clean("123456", 4); got != "1234" {
		t.Fatalf("bound: %q", got)
	}
	if got := clean("ab\xffcd", 32); got != "abcd" {
		t.Fatalf("invalid utf8: %q", got)
	}
}

func TestMergePeerCombinesAddresses(t *testing.T) {
	var peers []Peer
	index := make(map[string]int)
	msg := Message{Type: TypePong, ID: "devbox", Hostname: "box"}
	mergePeer(&peers, index, msg, "127.0.0.1:21129")
	mergePeer(&peers, index, msg, "192.0.2.5:21129")
	mergePeer(&peers, index, msg, "192.0.2.5:21129")
	mergePeer(&peers, index, Message{Type: TypePong, ID: "other"}, "192.0.2.6:21129")
	if len(peers) != 2 {
		t.Fatalf("peers: %+v", peers)
	}
	preferAddresses(&peers[0])
	if peers[0].Address != "192.0.2.5:21129" {
		t.Fatalf("LAN address must come first: %+v", peers[0])
	}
	if len(peers[0].Addresses) != 2 ||
		peers[0].Addresses[1] != "127.0.0.1:21129" {
		t.Fatalf("addresses: %+v", peers[0].Addresses)
	}
	if peers[0].Hostname != "box" {
		t.Fatalf("metadata lost: %+v", peers[0])
	}
}

func TestSourceLimiterBurst(t *testing.T) {
	l := newSourceLimiter()
	ip := net.IPv4(192, 0, 2, 1)
	allowed := 0
	for i := 0; i < 10; i++ {
		if l.allow(ip) {
			allowed++
		}
	}
	if allowed != 5 {
		t.Fatalf("burst allowed %d, want 5", allowed)
	}
	if !l.allow(net.IPv4(192, 0, 2, 2)) {
		t.Fatal("a different source must have its own bucket")
	}
}
