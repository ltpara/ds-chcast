package relay

import (
	"bufio"
	"crypto/ed25519"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

func startSharedRelay(t *testing.T, id, secret string) (*Manager, *registry.Registry, string, string, string) {
	t.Helper()
	priv := identity.PrivateKeyFromSecret(secret)
	reg := registry.New(time.Minute)
	host, err := reg.Register(registry.RegisterRequest{
		ID:        id,
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		TTL:       time.Minute,
	})
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	m, err := NewManager("127.0.0.1", []int{0}, reg, testLog)
	if err != nil {
		t.Fatalf("manager: %v", err)
	}
	t.Cleanup(m.Close)
	if err := m.EnableSharedCarrier("127.0.0.1:0"); err != nil {
		t.Fatalf("shared carrier: %v", err)
	}
	port, err := m.Allocate(id)
	if err != nil {
		t.Fatalf("allocate: %v", err)
	}
	reg.SetRelay(id, port, "")
	return m, reg, m.CarrierAddr(), host.CarrierToken, secret
}

// sharedLink is an authenticated host link on the shared carrier.
type sharedLink struct {
	conn   net.Conn
	br     *bufio.Reader
	frames *carrier.FrameReader
	send   *carrier.FrameWriter
}

func openSharedHost(t *testing.T, addr, id, secret string) *sharedLink {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial shared carrier: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	hello, _ := json.Marshal(carrier.HostHello{HostID: id})
	if _, err := io.WriteString(conn, carrier.MagicHost+string(hello)+"\n"); err != nil {
		t.Fatalf("write host hello: %v", err)
	}
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("read challenge: %v", err)
	}
	challengeText, ok := cutChallenge(line)
	if !ok {
		t.Fatalf("host link refused: %q", line)
	}
	var challenge carrier.HostChallenge
	if err := json.Unmarshal([]byte(challengeText), &challenge); err != nil {
		t.Fatalf("challenge: %v", err)
	}
	priv := identity.PrivateKeyFromSecret(secret)
	auth, _ := json.Marshal(carrier.HostAuth{
		HostID:    id,
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(priv, identity.CarrierHostMessage(id, "", challenge.Nonce)),
	})
	if _, err := io.WriteString(conn, carrier.MagicAuth+string(auth)+"\n"); err != nil {
		t.Fatalf("write host proof: %v", err)
	}
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("read host ack: %v", err)
	}
	if line+"\n" != carrier.MagicOK {
		t.Fatalf("host link refused: %q", line)
	}
	return &sharedLink{conn: conn, br: br, frames: carrier.NewFrameReader(br), send: carrier.NewFrameWriter(conn)}
}

func cutChallenge(line string) (string, bool) {
	if text, ok := cutPrefix(line, carrier.MagicChallenge); ok {
		return text, true
	}
	return "", false
}

func cutPrefix(s, prefix string) (string, bool) {
	if len(s) >= len(prefix) && s[:len(prefix)] == prefix {
		return s[len(prefix):], true
	}
	return "", false
}

func (h *sharedLink) read(t *testing.T) []byte {
	t.Helper()
	h.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := h.frames.ReadFrame()
	if err != nil {
		t.Fatalf("host read frame: %v", err)
	}
	return payload
}

func openSharedRawViewer(t *testing.T, addr, id, token string) (net.Conn, *carrier.FrameReader, *carrier.FrameWriter) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial shared carrier: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: id, Token: token})
	if _, err := io.WriteString(conn, carrier.MagicViewer+string(hello)+"\n"); err != nil {
		t.Fatalf("write viewer hello: %v", err)
	}
	return conn, carrier.NewFrameReader(conn), carrier.NewFrameWriter(conn)
}

func waitForSharedStats(t *testing.T, m *Manager, check func(SharedCarrierStats) bool) SharedCarrierStats {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		stats, ok := m.SharedCarrierStats()
		if ok && check(stats) {
			return stats
		}
		if time.Now().After(deadline) {
			t.Fatalf("shared carrier stats did not settle: %+v", stats)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestSharedCarrierRawViewerRoundTrip(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-shared")
	host := openSharedHost(t, addr, "devbox", secret)

	viewer, frames, send := openSharedRawViewer(t, addr, "devbox", token)
	uplink := []byte("quic-initial-through-shared")
	if err := send.WriteFrame(uplink); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	if got := host.read(t); string(got) != string(uplink) {
		t.Fatalf("host received %q", got)
	}
	downlink := []byte("quic-server-flight-shared")
	if err := host.send.WriteFrame(downlink); err != nil {
		t.Fatalf("host send: %v", err)
	}
	viewer.SetReadDeadline(time.Now().Add(2 * time.Second))
	got, err := frames.ReadFrame()
	if err != nil || string(got) != string(downlink) {
		t.Fatalf("viewer received %q, %v", got, err)
	}

	stats := waitForSharedStats(t, m, func(s SharedCarrierStats) bool {
		return s.Hosts == 1 && s.Viewers == 1 &&
			s.ToHostBytes == uint64(len(uplink)) && s.ToViewerBytes == uint64(len(downlink))
	})
	if stats.Addr != addr {
		t.Fatalf("stats addr = %q, want %q", stats.Addr, addr)
	}
}

func TestSharedCarrierWebSocketViewer(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-ws")
	host := openSharedHost(t, addr, "devbox", secret)

	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial shared carrier: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	request := "GET /chast HTTP/1.1\r\n" +
		"Host: carrier\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
		"Sec-WebSocket-Version: 13\r\n" +
		"Sec-WebSocket-Protocol: chast-carrier.v1\r\n\r\n"
	if _, err := io.WriteString(conn, request); err != nil {
		t.Fatalf("write upgrade: %v", err)
	}
	hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: token})
	if _, err := conn.Write(clientBinaryFrame(hello)); err != nil {
		t.Fatalf("write hello: %v", err)
	}
	if _, err := conn.Write(clientBinaryFrame([]byte("ws-shared-uplink"))); err != nil {
		t.Fatalf("write ws frame: %v", err)
	}
	if got := host.read(t); string(got) != "ws-shared-uplink" {
		t.Fatalf("host received %q", got)
	}
	if err := host.send.WriteFrame([]byte("ws-shared-downlink")); err != nil {
		t.Fatalf("host send: %v", err)
	}
	br := bufio.NewReader(conn)
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		t.Fatalf("read upgrade: %v", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("upgrade status = %s", resp.Status)
	}
	if got := readWSBinary(t, br); string(got) != "ws-shared-downlink" {
		t.Fatalf("viewer received %q", got)
	}
	waitForSharedStats(t, m, func(s SharedCarrierStats) bool {
		return s.ToHostBytes == uint64(len("ws-shared-uplink")) &&
			s.ToViewerBytes == uint64(len("ws-shared-downlink"))
	})
}

func TestSharedCarrierRejectsWrongToken(t *testing.T) {
	m, _, addr, _, secret := startSharedRelay(t, "devbox", "host-secret-token")
	openSharedHost(t, addr, "devbox", secret)

	viewer, _, _ := openSharedRawViewer(t, addr, "devbox", "not-the-token")
	expectClosed(t, viewer)
	waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Rejected == 1 })
}

func TestSharedCarrierRejectsViewerWithoutHostLink(t *testing.T) {
	m, _, addr, token, _ := startSharedRelay(t, "devbox", "host-secret-nolink")
	viewer, _, _ := openSharedRawViewer(t, addr, "devbox", token)
	expectClosed(t, viewer)
	stats, ok := m.SharedCarrierStats()
	if !ok || stats.Hosts != 0 {
		t.Fatalf("stats = %+v", stats)
	}
}

func TestSharedCarrierBusyRejectsSecondViewer(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-busy")
	host := openSharedHost(t, addr, "devbox", secret)
	first, _, send := openSharedRawViewer(t, addr, "devbox", token)
	if err := send.WriteFrame([]byte("first")); err != nil {
		t.Fatalf("first viewer: %v", err)
	}
	host.read(t)

	second, _, _ := openSharedRawViewer(t, addr, "devbox", token)
	expectClosed(t, second)
	_ = first

	stats := waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Busy == 1 })
	if stats.Viewers != 1 {
		t.Fatalf("viewers = %d", stats.Viewers)
	}
}

func TestSharedCarrierHostLinkDropClosesViewer(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-drop")
	host := openSharedHost(t, addr, "devbox", secret)
	viewer, _, send := openSharedRawViewer(t, addr, "devbox", token)
	if err := send.WriteFrame([]byte("hello")); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	host.read(t)

	host.conn.Close()
	expectClosed(t, viewer)
	waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Hosts == 0 && s.Viewers == 0 })
}

func TestSharedCarrierHostLinkReplacement(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-replace")
	first := openSharedHost(t, addr, "devbox", secret)
	viewer, _, send := openSharedRawViewer(t, addr, "devbox", token)
	if err := send.WriteFrame([]byte("before-replace")); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	first.read(t)

	second := openSharedHost(t, addr, "devbox", secret)
	// The new link replaces the old one and drops the stale viewer.
	expectClosed(t, viewer)
	first.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := first.frames.ReadFrame(); err == nil {
		t.Fatal("the replaced host link stayed open")
	}

	fresh, _, freshSend := openSharedRawViewer(t, addr, "devbox", token)
	if err := freshSend.WriteFrame([]byte("after-replace")); err != nil {
		t.Fatalf("fresh viewer send: %v", err)
	}
	if got := second.read(t); string(got) != "after-replace" {
		t.Fatalf("second host link received %q", got)
	}
	_ = fresh
	waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Hosts == 1 && s.Viewers == 1 })
}

func TestSharedCarrierKeepalive(t *testing.T) {
	_, _, addr, _, secret := startSharedRelay(t, "devbox", "host-secret-keepalive")
	host := openSharedHost(t, addr, "devbox", secret)
	if err := host.send.WriteFrame(nil); err != nil {
		t.Fatalf("keepalive: %v", err)
	}
	host.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := host.frames.ReadFrame()
	if err != nil || len(payload) != 0 {
		t.Fatalf("keepalive reply = %v, %v", payload, err)
	}
}

func TestSharedCarrierViewerKeepaliveReply(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-viewer-keepalive")
	_ = openSharedHost(t, addr, "devbox", secret)
	viewer, frames, send := openSharedRawViewer(t, addr, "devbox", token)
	waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Viewers == 1 })
	if err := send.WriteFrame(nil); err != nil {
		t.Fatalf("viewer keepalive: %v", err)
	}
	viewer.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := frames.ReadFrame()
	if err != nil || len(payload) != 0 {
		t.Fatalf("viewer keepalive reply = %v, %v", payload, err)
	}
}

func TestSharedCarrierHostLinked(t *testing.T) {
	m, _, addr, _, secret := startSharedRelay(t, "devbox", "host-secret-linked")
	if m.HostLinked("devbox") {
		t.Fatal("host counted as linked before its handshake")
	}
	host := openSharedHost(t, addr, "devbox", secret)
	deadline := time.Now().Add(2 * time.Second)
	for !m.HostLinked("devbox") {
		if time.Now().After(deadline) {
			t.Fatal("HostLinked stayed false after the handshake")
		}
		time.Sleep(5 * time.Millisecond)
	}
	if m.HostLinked("other") {
		t.Fatal("HostLinked reported an unrelated host")
	}
	host.conn.Close()
	deadline = time.Now().Add(2 * time.Second)
	for m.HostLinked("devbox") {
		if time.Now().After(deadline) {
			t.Fatal("HostLinked stayed true after the host link closed")
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestSharedCarrierRefusesBadHostProof(t *testing.T) {
	_, _, addr, _, _ := startSharedRelay(t, "devbox", "host-secret-badproof")
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	hello, _ := json.Marshal(carrier.HostHello{HostID: "devbox"})
	io.WriteString(conn, carrier.MagicHost+string(hello)+"\n")
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("challenge: %v", err)
	}
	challengeText, ok := cutChallenge(line)
	if !ok {
		t.Fatalf("expected a challenge, got %q", line)
	}
	var challenge carrier.HostChallenge
	json.Unmarshal([]byte(challengeText), &challenge)
	wrong := identity.PrivateKeyFromSecret("another-secret")
	auth, _ := json.Marshal(carrier.HostAuth{
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(wrong, identity.CarrierHostMessage("devbox", "", challenge.Nonce)),
	})
	io.WriteString(conn, carrier.MagicAuth+string(auth)+"\n")
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil || len(line) < len(carrier.MagicErrorPrefix) || line[:len(carrier.MagicErrorPrefix)] != carrier.MagicErrorPrefix {
		t.Fatalf("bad proof accepted: %q, %v", line, err)
	}
}

func TestSharedCarrierUnknownHostRefused(t *testing.T) {
	_, _, addr, _, _ := startSharedRelay(t, "devbox", "host-secret-unknown")
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	hello, _ := json.Marshal(carrier.HostHello{HostID: "nosuchhost"})
	io.WriteString(conn, carrier.MagicHost+string(hello)+"\n")
	line, err := carrier.ReadLine(bufio.NewReader(conn), carrier.MaxHandshake)
	if err != nil || len(line) < len(carrier.MagicErrorPrefix) || line[:len(carrier.MagicErrorPrefix)] != carrier.MagicErrorPrefix {
		t.Fatalf("unknown host reply = %q, %v", line, err)
	}
}

func TestSharedCarrierHostOfflineRefused(t *testing.T) {
	_, reg, addr, _, secret := startSharedRelay(t, "devbox", "host-secret-offline")
	reg.Remove("devbox")

	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	hello, _ := json.Marshal(carrier.HostHello{HostID: "devbox"})
	io.WriteString(conn, carrier.MagicHost+string(hello)+"\n")
	line, err := carrier.ReadLine(bufio.NewReader(conn), carrier.MaxHandshake)
	if err != nil || len(line) < len(carrier.MagicErrorPrefix) || line[:len(carrier.MagicErrorPrefix)] != carrier.MagicErrorPrefix {
		t.Fatalf("offline host reply = %q, %v", line, err)
	}
	_ = secret
}

func TestSharedCarrierDisabledByDefault(t *testing.T) {
	reg := registry.New(time.Minute)
	m, err := NewManager("127.0.0.1", []int{0}, reg, testLog)
	if err != nil {
		t.Fatalf("manager: %v", err)
	}
	defer m.Close()
	if m.CarrierAddr() != "" || m.CarrierPort() != 0 {
		t.Fatalf("carrier addr = %q port = %d", m.CarrierAddr(), m.CarrierPort())
	}
	if _, ok := m.SharedCarrierStats(); ok {
		t.Fatal("shared carrier stats without a listener")
	}
	if stats := m.Stats(); stats.SharedCarrier != nil {
		t.Fatalf("shared carrier in stats: %+v", stats.SharedCarrier)
	}
}

func TestSharedCarrierPortIsReported(t *testing.T) {
	m, _, addr, _, _ := startSharedRelay(t, "devbox", "host-secret-port")
	_, portText, err := net.SplitHostPort(addr)
	if err != nil {
		t.Fatalf("split: %v", err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		t.Fatalf("port: %v", err)
	}
	if m.CarrierPort() != port {
		t.Fatalf("CarrierPort() = %d, want %d", m.CarrierPort(), port)
	}
	if stats, ok := m.SharedCarrierStats(); !ok || stats.Addr != addr {
		t.Fatalf("stats = %+v, %v", stats, ok)
	}
}

func TestSharedCarrierIPv6Listener(t *testing.T) {
	const secret = "host-secret-carrier-v6"
	priv := identity.PrivateKeyFromSecret(secret)
	reg := registry.New(time.Minute)
	host, err := reg.Register(registry.RegisterRequest{
		ID:        "devbox",
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		TTL:       time.Minute,
	})
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	m, err := NewManager("127.0.0.1", []int{0}, reg, testLog, WithCarrierBind6("::1"))
	if err != nil {
		t.Fatalf("manager: %v", err)
	}
	t.Cleanup(m.Close)
	if err := m.EnableSharedCarrier("127.0.0.1:0"); err != nil {
		t.Fatalf("shared carrier: %v", err)
	}
	addrs := m.CarrierAddrs()
	if len(addrs) != 2 {
		t.Fatalf("carrier addrs = %v", addrs)
	}
	v6 := m.CarrierV6Addr()
	if !strings.HasPrefix(v6, "[::1]:") {
		t.Fatalf("carrier IPv6 addr = %q", v6)
	}
	_, v4Port, err := net.SplitHostPort(addrs[0])
	if err != nil {
		t.Fatalf("split v4: %v", err)
	}
	_, v6Port, err := net.SplitHostPort(v6)
	if err != nil {
		t.Fatalf("split v6: %v", err)
	}
	if v4Port != v6Port || m.CarrierPort() != mustPort(t, v4Port) {
		t.Fatalf("carrier ports differ: %v %v", addrs, v6)
	}
	link := openSharedHost(t, v6, host.ID, secret)
	_ = link
	waitForSharedStats(t, m, func(stats SharedCarrierStats) bool { return stats.Hosts == 1 })
	if !m.HostLinked(host.ID) {
		t.Fatal("IPv6 carrier link did not authenticate")
	}
	stats, ok := m.SharedCarrierStats()
	if !ok || len(stats.Addrs) != 2 || stats.Addr != addrs[0] {
		t.Fatalf("stats = %+v, %v", stats, ok)
	}
}

func mustPort(t *testing.T, text string) int {
	t.Helper()
	port, err := strconv.Atoi(text)
	if err != nil {
		t.Fatalf("port %q: %v", text, err)
	}
	return port
}
