package rendezvous

import (
	"bufio"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// startSignal exposes the signalling WebSocket on an HTTP listener, as the
// binary does at the API address.
func startSignal(t *testing.T, meet *Server) string {
	t.Helper()
	srv := httptest.NewServer(meet.SignalHandler())
	t.Cleanup(srv.Close)
	return srv.URL
}

func openSignal(t *testing.T, url string) net.Conn {
	t.Helper()
	addr := strings.TrimPrefix(url, "http://")
	conn, err := net.DialTimeout("tcp", addr, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	ws, err := carrier.NewWSClient(conn, bufio.NewReader(conn), addr, wire.SignalWSPath, wire.SignalWSSubprotocol)
	if err != nil {
		t.Fatalf("signalling upgrade: %v", err)
	}
	return carrier.NewStreamConn(ws, conn)
}

// TestSignalWebSocketPairsBothHalves checks that a WebSocket viewer and a raw
// host announce pair through the same coordinator. The viewer only reports its
// punch socket's port, because the WebSocket's observed address belongs to the
// HTTP connection; the server must combine it with the observed IP.
func TestSignalWebSocketPairsBothHalves(t *testing.T) {
	meet, _, meetAddr, priv := startMeet(t, WithServerKey("test-key"))
	keepalive(t, listenUDP(t), meetAddr, priv)
	viewer := openSignal(t, startSignal(t, meet))

	nonce := "ws-punch-0123456789"
	sent := viewerAnnounce(nonce, time.Now().Unix())
	sent.Candidate = "0.0.0.0:40000"
	if err := carrier.NewFrameWriter(viewer).WriteFrame(wire.Encode(sent)); err != nil {
		t.Fatalf("viewer announce: %v", err)
	}
	time.Sleep(50 * time.Millisecond)

	host := tcpAnnounce(tcpPunchAddr(t, meet), hostAnnounce(priv, nonce, time.Now().Unix()))
	if host.err != nil {
		t.Fatalf("host announce: %v", host.err)
	}
	if !host.reply.OK {
		t.Fatalf("host announce refused: %+v", host.reply)
	}
	if host.reply.Candidate != "127.0.0.1:40000" {
		t.Fatalf("host got %q, want the viewer's observed IP with its punch port", host.reply.Candidate)
	}
	payload, err := carrier.NewFrameReader(viewer).ReadFrame()
	if err != nil {
		t.Fatalf("viewer reply: %v", err)
	}
	reply, err := wire.Decode(payload)
	if err != nil {
		t.Fatal(err)
	}
	if !reply.OK || reply.Nonce != nonce || reply.Candidate != host.local {
		t.Fatalf("viewer received %+v, want the host socket %q", reply, host.local)
	}
}

// TestSignalWebSocketHostControl verifies the control link and the punch
// coordination share the WebSocket endpoint, so a host behind a blocked raw TCP
// port still receives signed punch triggers.
func TestSignalWebSocketHostControl(t *testing.T) {
	meet, _, _, priv := startMeet(t, WithServerKey("test-key"))
	control := openSignal(t, startSignal(t, meet))
	ack := hostControlHandshake(t, meet, control, "devbox", priv)
	if !ack.OK || !meet.HostControlLinked("devbox") {
		t.Fatalf("control refused: %+v", ack)
	}

	viewer, err := net.DialTimeout("tcp", meet.TCPAddr(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer viewer.Close()
	nonce := "ws-control-tcp-punch"
	writeAnnounce(viewer, viewerAnnounce(nonce, time.Now().Unix()))
	_ = control.SetReadDeadline(time.Now().Add(3 * time.Second))
	trigger, err := readAnnounce(control)
	if err != nil {
		t.Fatalf("trigger over signalling WebSocket: %v", err)
	}
	pub, _ := identity.ParsePublicKey(meet.identity.PublicKeyText())
	if trigger.Type != wire.TypeTCPPunch || trigger.Nonce != nonce ||
		trigger.Candidate != viewer.LocalAddr().String() ||
		!identity.Verify(pub, identity.RendezvousTriggerMessage("devbox", "client-connection-nonce",
			nonce, trigger.Candidate, trigger.NATType, trigger.IsLocal, trigger.Timestamp), trigger.Signature) {
		t.Fatalf("incorrect trigger over WebSocket: %+v", trigger)
	}
	if err := carrier.NewFrameWriter(control).WriteFrame(nil); err != nil {
		t.Fatal(err)
	}
	_ = control.SetReadDeadline(time.Now().Add(3 * time.Second))
	if data, err := carrier.NewFrameReader(control).ReadFrame(); err != nil || len(data) != 0 {
		t.Fatalf("control keepalive over WebSocket failed: %q, %v", data, err)
	}
}

func TestSignalWebSocketRejectsNonSignalRequests(t *testing.T) {
	meet, _, _, _ := startMeet(t, WithServerKey("test-key"))
	url := startSignal(t, meet)
	addr := strings.TrimPrefix(url, "http://")
	for _, tc := range []struct {
		name, path, protocol string
	}{
		{"wrong path", "/chast", wire.SignalWSSubprotocol},
		{"carrier subprotocol", wire.SignalWSPath, carrier.WSSubprotocol},
	} {
		t.Run(tc.name, func(t *testing.T) {
			conn, err := net.DialTimeout("tcp", addr, time.Second)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
			if _, err := carrier.NewWSClient(conn, bufio.NewReader(conn), addr, tc.path, tc.protocol); err == nil {
				t.Fatal("the signalling endpoint accepted a foreign WebSocket handshake")
			}
		})
	}
	reply, err := http.Get(url + wire.SignalWSPath)
	if err != nil {
		t.Fatal(err)
	}
	defer reply.Body.Close()
	if reply.StatusCode != http.StatusBadRequest {
		t.Fatalf("plain HTTP status = %d, want 400", reply.StatusCode)
	}
}

func TestPunchEndpointOverWebSocketUsesReportedPort(t *testing.T) {
	for _, tc := range []struct {
		name     string
		observed string
		local    string
		viaWS    bool
		want     string
	}{
		{"raw announce keeps its socket", "198.51.100.7:45000", "0.0.0.0:40000", false, "198.51.100.7:45000"},
		{"websocket combines observed IP and punch port", "198.51.100.7:45000", "0.0.0.0:40000", true, "198.51.100.7:40000"},
		{"websocket IPv6", "[2001:db8::7]:45000", "[::]:40001", true, "[2001:db8::7]:40001"},
		{"websocket without a usable port", "198.51.100.7:45000", "0.0.0.0:0", true, "198.51.100.7:45000"},
		{"websocket without a candidate", "198.51.100.7:45000", "", true, "198.51.100.7:45000"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := punchAddress(tc.observed, tc.local, tc.viaWS); got != tc.want {
				t.Fatalf("punch address = %q, want %q", got, tc.want)
			}
		})
	}
	peer := &tcpWait{observed: "198.51.100.7:45000", local: "0.0.0.0:40000", viaWS: true}
	p := &tcpPuncher{srv: &Server{}}
	if got := p.peerResult(peer, &tcpWait{observed: "203.0.113.9:50000"}); got.peer != "198.51.100.7:40000" || got.local {
		t.Fatalf("websocket peer result = %+v", got)
	}
}
