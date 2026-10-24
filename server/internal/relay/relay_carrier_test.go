package relay

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func dialTCP(t *testing.T, relayAddr *net.UDPAddr) net.Conn {
	t.Helper()
	conn, err := net.DialTimeout("tcp",
		net.JoinHostPort(relayAddr.IP.String(), strconv.Itoa(relayAddr.Port)), 2*time.Second)
	if err != nil {
		t.Fatalf("dial relay tcp: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

type hostLinkConn struct {
	conn   net.Conn
	br     *bufio.Reader
	frames *carrier.FrameReader
	send   *carrier.FrameWriter
}

func openHostLink(t *testing.T, relayAddr *net.UDPAddr, id, secret string) *hostLinkConn {
	t.Helper()
	conn := dialTCP(t, relayAddr)
	hello, err := json.Marshal(carrier.HostHello{HostID: id})
	if err != nil {
		t.Fatalf("marshal hello: %v", err)
	}
	if _, err := io.WriteString(conn, carrier.MagicHost+string(hello)+"\n"); err != nil {
		t.Fatalf("write host hello: %v", err)
	}
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("read host challenge: %v", err)
	}
	challengeText, ok := strings.CutPrefix(line, carrier.MagicChallenge)
	if !ok {
		t.Fatalf("host link refused: %q", line)
	}
	var challenge carrier.HostChallenge
	if err := json.Unmarshal([]byte(challengeText), &challenge); err != nil {
		t.Fatalf("challenge: %v", err)
	}
	priv := identity.PrivateKeyFromSecret(secret)
	auth, err := json.Marshal(carrier.HostAuth{
		HostID:    id,
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(priv, identity.CarrierHostMessage(id, "", challenge.Nonce)),
	})
	if err != nil {
		t.Fatalf("marshal proof: %v", err)
	}
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
	return &hostLinkConn{
		conn:   conn,
		br:     br,
		frames: carrier.NewFrameReader(br),
		send:   carrier.NewFrameWriter(conn),
	}
}

func (h *hostLinkConn) read(t *testing.T) []byte {
	t.Helper()
	h.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := h.frames.ReadFrame()
	if err != nil {
		t.Fatalf("host read frame: %v", err)
	}
	return payload
}

func openRawViewer(t *testing.T, relayAddr *net.UDPAddr) (net.Conn, *carrier.FrameReader, *carrier.FrameWriter) {
	t.Helper()
	conn := dialTCP(t, relayAddr)
	if _, err := io.WriteString(conn, carrier.MagicRaw); err != nil {
		t.Fatalf("write viewer magic: %v", err)
	}
	return conn, carrier.NewFrameReader(conn), carrier.NewFrameWriter(conn)
}

func expectClosed(t *testing.T, conn net.Conn) {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.Copy(io.Discard, conn); err != nil {
		if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
			t.Fatal("the connection stayed open")
		}
	}
}

func TestCarrierHostLinkRefusesBadCredentials(t *testing.T) {
	_, _, relayAddr, _ := startRelay(t, "devbox", "host-secret")

	conn := dialTCP(t, relayAddr)
	hello, _ := json.Marshal(carrier.HostHello{HostID: "devbox"})
	io.WriteString(conn, carrier.MagicHost+string(hello)+"\n")
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("read challenge: %v", err)
	}
	challengeText, ok := strings.CutPrefix(line, carrier.MagicChallenge)
	if !ok {
		t.Fatalf("expected a challenge, got %q", line)
	}
	var challenge carrier.HostChallenge
	if err := json.Unmarshal([]byte(challengeText), &challenge); err != nil {
		t.Fatalf("challenge: %v", err)
	}
	wrong := identity.PrivateKeyFromSecret("another-secret")
	auth, _ := json.Marshal(carrier.HostAuth{
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(wrong, identity.CarrierHostMessage("devbox", "", challenge.Nonce)),
	})
	io.WriteString(conn, carrier.MagicAuth+string(auth)+"\n")
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("read refusal: %v", err)
	}
	if !strings.HasPrefix(line, carrier.MagicErrorPrefix) {
		t.Fatalf("bad proof was accepted: %q", line)
	}

	conn = dialTCP(t, relayAddr)
	hello, _ = json.Marshal(carrier.HostHello{HostID: "other"})
	io.WriteString(conn, carrier.MagicHost+string(hello)+"\n")
	line, err = carrier.ReadLine(bufio.NewReader(conn), carrier.MaxHandshake)
	if err != nil || !strings.HasPrefix(line, carrier.MagicErrorPrefix) {
		t.Fatalf("unknown host reply = %q, %v", line, err)
	}
}

func waitForStats(t *testing.T, m *Manager, check func(PortStats) bool) PortStats {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		stats := m.Stats().Ports[0]
		if check(stats) {
			return stats
		}
		if time.Now().After(deadline) {
			t.Fatalf("stats did not settle: %+v", stats)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestCarrierRawViewerPairing(t *testing.T) {
	const secret = "host-secret-1"
	m, _, relayAddr, _ := startRelay(t, "devbox", secret)
	host := openHostLink(t, relayAddr, "devbox", secret)
	viewer, frames, send := openRawViewer(t, relayAddr)

	uplink := []byte("quic-initial-from-viewer")
	if err := send.WriteFrame(uplink); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	if got := host.read(t); string(got) != string(uplink) {
		t.Fatalf("host received %q", got)
	}
	downlink := []byte("quic-server-flight")
	if err := host.send.WriteFrame(downlink); err != nil {
		t.Fatalf("host send: %v", err)
	}
	viewer.SetReadDeadline(time.Now().Add(2 * time.Second))
	got, err := frames.ReadFrame()
	if err != nil || string(got) != string(downlink) {
		t.Fatalf("viewer received %q, %v", got, err)
	}

	stats := waitForStats(t, m, func(s PortStats) bool {
		return s.TCPViewers == 1 && s.TCPToHost == uint64(len(uplink)) &&
			s.TCPToViewer == uint64(len(downlink))
	})
	if !stats.HostOnline || stats.Viewer == "" {
		t.Fatalf("stats without a visible viewer: %+v", stats)
	}
}

const wsRequest = "GET /chast HTTP/1.1\r\n" +
	"Host: relay\r\n" +
	"Upgrade: websocket\r\n" +
	"Connection: Upgrade\r\n" +
	"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
	"Sec-WebSocket-Version: 13\r\n" +
	"Sec-WebSocket-Protocol: chast-carrier.v1\r\n\r\n"

func clientBinaryFrame(payload []byte) []byte {
	frame := []byte{0x82}
	if len(payload) <= 125 {
		frame = append(frame, 0x80|byte(len(payload)))
	} else {
		frame = append(frame, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	}
	mask := [4]byte{9, 8, 7, 6}
	frame = append(frame, mask[:]...)
	for i, b := range payload {
		frame = append(frame, b^mask[i%4])
	}
	return frame
}

func readWSBinary(t *testing.T, br *bufio.Reader) []byte {
	t.Helper()
	var head [2]byte
	if _, err := io.ReadFull(br, head[:]); err != nil {
		t.Fatalf("read ws header: %v", err)
	}
	if head[0]&0x0F != 0x2 || head[1]&0x80 != 0 {
		t.Fatalf("unexpected ws frame %x %x", head[0], head[1])
	}
	length := uint64(head[1] & 0x7F)
	if length == 126 {
		var extended [2]byte
		if _, err := io.ReadFull(br, extended[:]); err != nil {
			t.Fatalf("read ws length: %v", err)
		}
		length = uint64(binary.BigEndian.Uint16(extended[:]))
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(br, payload); err != nil {
		t.Fatalf("read ws payload: %v", err)
	}
	return payload
}

func TestCarrierWebSocketViewerPairing(t *testing.T) {
	const secret = "host-secret-2"
	m, _, relayAddr, _ := startRelay(t, "devbox", secret)
	host := openHostLink(t, relayAddr, "devbox", secret)
	conn := dialTCP(t, relayAddr)
	if _, err := io.WriteString(conn, wsRequest); err != nil {
		t.Fatalf("write upgrade: %v", err)
	}
	if _, err := conn.Write(clientBinaryFrame([]byte("ws-uplink"))); err != nil {
		t.Fatalf("write ws frame: %v", err)
	}
	if got := host.read(t); string(got) != "ws-uplink" {
		t.Fatalf("host received %q", got)
	}
	if err := host.send.WriteFrame([]byte("ws-downlink")); err != nil {
		t.Fatalf("host send: %v", err)
	}
	br := bufio.NewReader(conn)
	if err := conn.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("deadline: %v", err)
	}
	// Skip the 101 response before the data frame.
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			t.Fatalf("read upgrade: %v", err)
		}
		if line == "\r\n" {
			break
		}
	}
	if got := readWSBinary(t, br); string(got) != "ws-downlink" {
		t.Fatalf("viewer received %q", got)
	}
	waitForStats(t, m, func(s PortStats) bool {
		return s.TCPToHost == uint64(len("ws-uplink")) &&
			s.TCPToViewer == uint64(len("ws-downlink"))
	})
}

func TestCarrierBusyRejectsSecondViewer(t *testing.T) {
	const secret = "host-secret-3"
	m, _, relayAddr, _ := startRelay(t, "devbox", secret)
	host := openHostLink(t, relayAddr, "devbox", secret)
	first, _, send := openRawViewer(t, relayAddr)
	if err := send.WriteFrame([]byte("first")); err != nil {
		t.Fatalf("first viewer: %v", err)
	}
	host.read(t)

	second, _, secondSend := openRawViewer(t, relayAddr)
	_ = secondSend
	expectClosed(t, second)
	_ = first

	stats := m.Stats().Ports[0]
	if stats.TCPBusy != 1 {
		t.Fatalf("tcp_busy = %d", stats.TCPBusy)
	}
	if stats.TCPViewers != 1 {
		t.Fatalf("tcp_viewers = %d", stats.TCPViewers)
	}
}

func TestCarrierHostLinkDropClosesViewer(t *testing.T) {
	const secret = "host-secret-4"
	_, _, relayAddr, _ := startRelay(t, "devbox", secret)
	host := openHostLink(t, relayAddr, "devbox", secret)
	viewer, _, send := openRawViewer(t, relayAddr)
	if err := send.WriteFrame([]byte("hello")); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	host.read(t)

	host.conn.Close()
	expectClosed(t, viewer)
}

func TestCarrierViewerWithoutHostIsClosed(t *testing.T) {
	_, _, relayAddr, _ := startRelay(t, "devbox", "host-secret-5")
	viewer, _, _ := openRawViewer(t, relayAddr)
	expectClosed(t, viewer)
}

func TestCarrierHostLinkKeepalive(t *testing.T) {
	const secret = "host-secret-6"
	_, _, relayAddr, _ := startRelay(t, "devbox", secret)
	host := openHostLink(t, relayAddr, "devbox", secret)
	if err := host.send.WriteFrame(nil); err != nil {
		t.Fatalf("keepalive: %v", err)
	}
	host.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := host.frames.ReadFrame()
	if err != nil || len(payload) != 0 {
		t.Fatalf("keepalive reply = %v, %v", payload, err)
	}
}
