package rendezvous

import (
	"crypto/ed25519"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

type tcpAnnounceResult struct {
	reply wire.Message
	local string
	err   error
}

func tcpAnnounce(addr string, msg wire.Message) tcpAnnounceResult {
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		return tcpAnnounceResult{err: err}
	}
	defer conn.Close()
	local := conn.LocalAddr().String()
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	if err := carrier.NewFrameWriter(conn).WriteFrame(wire.Encode(msg)); err != nil {
		return tcpAnnounceResult{err: err}
	}
	payload, err := carrier.NewFrameReader(conn).ReadFrame()
	if err != nil {
		return tcpAnnounceResult{err: err}
	}
	reply, err := wire.Decode(payload)
	return tcpAnnounceResult{reply: reply, local: local, err: err}
}

func tcpPunchAddr(t *testing.T, meet *Server) string {
	t.Helper()
	addr := meet.TCPAddr()
	if addr == "" {
		t.Fatal("TCP punch listener is not enabled")
	}
	return addr
}

func hostAnnounce(priv ed25519.PrivateKey, nonce string, unix int64) wire.Message {
	return wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleHost,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: unix,
		Signature: identity.Sign(priv, identity.TCPPunchMessage("devbox", nonce, wire.RoleHost, unix)),
	}
}

func viewerAnnounce(nonce string, unix int64) wire.Message {
	return wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleViewer,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: unix,
		Auth:      identity.PunchAuth("test-key", "devbox", nonce, unix),
	}
}

// TestTCPPunchPairsBothHalves checks the coordination contract: each side
// learns the endpoint the server observed for the other socket, which is the
// only address the punch is proven to be able to reach.
func TestTCPPunchPairsBothHalves(t *testing.T) {
	meet, _, meetAddr, priv := startMeet(t, WithServerKey("test-key"))
	keepalive(t, listenUDP(t), meetAddr, priv)
	addr := tcpPunchAddr(t, meet)

	nonce := "tcp-punch-0123456789"
	now := time.Now().Unix()
	hostCh := make(chan tcpAnnounceResult, 1)
	go func() { hostCh <- tcpAnnounce(addr, hostAnnounce(priv, nonce, now)) }()
	time.Sleep(100 * time.Millisecond)

	viewer := tcpAnnounce(addr, viewerAnnounce(nonce, now))
	if viewer.err != nil {
		t.Fatalf("viewer announce: %v", viewer.err)
	}
	host := <-hostCh
	if host.err != nil {
		t.Fatalf("host announce: %v", host.err)
	}
	if !host.reply.OK || !viewer.reply.OK {
		t.Fatalf("announces not paired: host=%+v viewer=%+v", host.reply, viewer.reply)
	}
	if viewer.reply.Candidate != host.local {
		t.Fatalf("viewer got %q, want host socket %q", viewer.reply.Candidate, host.local)
	}
	if host.reply.Candidate != viewer.local {
		t.Fatalf("host got %q, want viewer socket %q", host.reply.Candidate, viewer.local)
	}
}

func TestTCPPunchRejectsBadHostSignature(t *testing.T) {
	meet, _, meetAddr, _ := startMeet(t, WithServerKey("test-key"))
	keepalive(t, listenUDP(t), meetAddr, identity.PrivateKeyFromSecret("host-secret"))

	nonce := "tcp-punch-bad-signature"
	now := time.Now().Unix()
	reply := tcpAnnounce(tcpPunchAddr(t, meet), wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleHost,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: now,
		Signature: identity.Sign(identity.PrivateKeyFromSecret("other-key"),
			identity.TCPPunchMessage("devbox", nonce, wire.RoleHost, now)),
	})
	if reply.err != nil {
		t.Fatalf("announce: %v", reply.err)
	}
	if reply.reply.OK || reply.reply.Error != "punch authentication failed" {
		t.Fatalf("bad host signature accepted: %+v", reply.reply)
	}
}

func TestTCPPunchRejectsViewerWithoutAuth(t *testing.T) {
	meet, _, meetAddr, priv := startMeet(t, WithServerKey("test-key"))
	keepalive(t, listenUDP(t), meetAddr, priv)

	nonce := "tcp-punch-no-auth"
	now := time.Now().Unix()
	reply := tcpAnnounce(tcpPunchAddr(t, meet), wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleViewer,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: now,
	})
	if reply.err != nil {
		t.Fatalf("announce: %v", reply.err)
	}
	if reply.reply.OK || reply.reply.Error != "punch authentication failed" {
		t.Fatalf("unauthenticated viewer accepted: %+v", reply.reply)
	}
}

func TestTCPPunchAcceptsHostOnlineThroughCarrier(t *testing.T) {
	meet, _, _, priv := startMeet(t,
		WithServerKey("test-key"),
		WithHostCarrierLinked(func(id string) bool { return id == "devbox" }),
	)
	p := &tcpPuncher{srv: meet}
	nonce := "tcp-punch-carrier-only"
	if err := p.checkAnnounce(viewerAnnounce(nonce, time.Now().Unix()), "127.0.0.1:40000"); err != nil {
		t.Fatalf("carrier-backed host was rejected: %v", err)
	}
	if err := p.checkAnnounce(hostAnnounce(priv, nonce, time.Now().Unix()), "127.0.0.1:40001"); err != nil {
		t.Fatalf("authenticated carrier-backed host was rejected: %v", err)
	}
}

func TestTCPPunchUnauthenticatedRolesDoNotRevealHostState(t *testing.T) {
	for _, online := range []bool{false, true} {
		name := "offline"
		if online {
			name = "online"
		}
		t.Run(name, func(t *testing.T) {
			meet, reg, _, _ := startMeet(t, WithServerKey("test-key"))
			if online {
				if err := reg.Touch("devbox", "127.0.0.1:40001", nil, ""); err != nil {
					t.Fatal(err)
				}
			}
			for _, role := range []string{wire.RoleViewer, wire.RoleHost} {
				for _, id := range []string{"devbox", "unknown-host"} {
					msg := wire.Message{
						Type: wire.TypeTCPPunch, Role: role, HostID: id,
						Nonce: "tcp-punch-auth-state", Timestamp: time.Now().Unix(),
					}
					reply := tcpAnnounce(tcpPunchAddr(t, meet), msg)
					if reply.err != nil {
						t.Fatal(reply.err)
					}
					if reply.reply.OK || reply.reply.Error != "punch authentication failed" {
						t.Fatalf("role %s, id %s disclosed host state: %+v", role, id, reply.reply)
					}
				}
			}
		})
	}
}

func TestTCPPunchAuthenticatesViewerBeforeRevealingHostState(t *testing.T) {
	meet, _, _, _ := startMeet(t, WithServerKey("test-key"))
	p := &tcpPuncher{srv: meet}
	msg := viewerAnnounce("tcp-punch-auth-order", time.Now().Unix())
	msg.Auth = ""
	err := p.checkAnnounce(msg, "127.0.0.1:40000")
	if err == nil || err.Error() != "punch authentication failed" {
		t.Fatalf("unauthenticated announce error = %v, want authentication failure", err)
	}
}
