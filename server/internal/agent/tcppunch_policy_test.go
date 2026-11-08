package agent

import (
	"bufio"
	"context"
	"crypto/ed25519"
	"net"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func tcpTestListener(t *testing.T) *net.TCPListener {
	t.Helper()
	ln, err := net.ListenTCP("tcp", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	_ = ln.SetDeadline(time.Now().Add(2 * time.Second))
	return ln
}

func TestHostTCPPolicyDeclinesWithoutPeerDial(t *testing.T) {
	for _, mode := range []string{"own-symmetric", "peer-symmetric", "proxy"} {
		t.Run(mode, func(t *testing.T) {
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			coordinator, peer := tcpTestListener(t), tcpTestListener(t)
			h := &HostAgent{ctx: ctx, cfg: HostConfig{ID: "host"}, priv: identity.PrivateKeyFromSecret("key"),
				tcpPunch: coordinator.Addr().(*net.TCPAddr), log: quietLogger()}
			peerNAT := ""
			if mode == "own-symmetric" {
				h.natType = NATSymmetric
			}
			if mode == "peer-symmetric" {
				peerNAT = NATSymmetric
			}
			if mode == "proxy" {
				h.cfg.Proxy, _ = netproxy.Parse("http://" + coordinator.Addr().String())
				h.tcpPunch = &net.TCPAddr{IP: net.ParseIP("198.51.100.9"), Port: 30000}
			}
			h.startTCPPunch("policy-punch-nonce", peer.Addr().String(), peerNAT, false)
			conn, err := coordinator.AcceptTCP()
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			_ = conn.SetDeadline(time.Now().Add(time.Second))
			br := bufio.NewReader(conn)
			if mode == "proxy" {
				req, err := http.ReadRequest(br)
				if err != nil || req.Method != http.MethodConnect || req.Host != h.tcpPunch.String() {
					t.Fatalf("not proxied: %v %v", req, err)
				}
				_, _ = conn.Write([]byte("HTTP/1.1 200 Connection Established\r\n\r\n"))
			}
			msg, err := readHandshake(br)
			if err != nil || msg.Error == "" || !identity.Verify(h.priv.Public().(ed25519.PublicKey),
				identity.TCPPunchDeclineMessage("host", msg.Nonce, msg.Error, msg.Timestamp), msg.Signature) {
				t.Fatalf("missing authenticated decline: %+v %v", msg, err)
			}
			_ = peer.SetDeadline(time.Now().Add(50 * time.Millisecond))
			if c, err := peer.Accept(); err == nil {
				c.Close()
				t.Fatal("policy bypass dialled peer")
			}
			// Replaying an already processed instruction must not start a new dial.
			h.startTCPPunch("policy-punch-nonce", peer.Addr().String(), peerNAT, false)
			_ = coordinator.SetDeadline(time.Now().Add(50 * time.Millisecond))
			if c, err := coordinator.Accept(); err == nil {
				c.Close()
				t.Fatal("replayed trigger restarted punch")
			}
		})
	}
}

func TestHostPrePunchUsesReportPortAndThenOnlyListens(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	coordinator, peer := tcpTestListener(t), tcpTestListener(t)
	h := &HostAgent{ctx: ctx, cfg: HostConfig{ID: "host"}, priv: identity.PrivateKeyFromSecret("key"), log: quietLogger()}
	go h.runTCPPunch("pre-punch-nonce", coordinator.Addr().(*net.TCPAddr), peer.Addr().(*net.TCPAddr), false)
	announce, err := coordinator.AcceptTCP()
	if err != nil {
		t.Fatal(err)
	}
	defer announce.Close()
	_ = announce.SetDeadline(time.Now().Add(time.Second))
	pre, err := peer.AcceptTCP()
	if err != nil {
		t.Fatal("host never pre-punched before waiting for a coordinator response:", err)
	}
	defer pre.Close()
	if pre.RemoteAddr().String() != announce.RemoteAddr().String() {
		t.Fatal("pre-punch changed local port")
	}
	report, err := readHandshake(bufio.NewReader(announce))
	if err != nil || report.Nonce != "pre-punch-nonce" || report.Candidate != announce.RemoteAddr().String() {
		t.Fatalf("host report: %+v %v", report, err)
	}
	// The coordinator sends no pairing response: the host must switch to listen.
	var conn net.Conn
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		conn, err = net.DialTimeout("tcp", report.Candidate, 100*time.Millisecond)
		if err == nil {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if err != nil {
		t.Fatal("host did not listen after report:", err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(time.Second))
	hello, err := readHandshake(bufio.NewReader(conn))
	if err != nil || hello.Type != wire.TypeTCPDirect {
		t.Fatalf("listener handshake: %+v %v", hello, err)
	}
	_ = peer.SetDeadline(time.Now().Add(100 * time.Millisecond))
	if c, err := peer.Accept(); err == nil {
		c.Close()
		t.Fatal("host kept reverse-dialling after its report")
	}
}
