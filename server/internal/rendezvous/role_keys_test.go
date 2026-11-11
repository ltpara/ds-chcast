package rendezvous

import (
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func viewerPunch(t *testing.T, viewer *net.UDPConn, meetAddr *net.UDPAddr, nonce, key string) wire.Message {
	t.Helper()
	stamp := time.Now().Unix()
	msg := wire.Message{
		Type:      wire.TypePunchRequest,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: stamp,
	}
	if key != "" {
		msg.Auth = identity.PunchAuth(key, "devbox", nonce, stamp)
	}
	viewer.WriteToUDP(wire.Encode(msg), meetAddr)
	return recv(t, viewer)
}

// TestPunchAcceptsLinkKeyAndManagementKey pins the connection-credential
// contract for UDP punching: the link key authenticates a viewer, a wrong key
// does not, and the management key keeps working as the root credential.
func TestPunchAcceptsLinkKeyAndManagementKey(t *testing.T) {
	const (
		linkKey = "viewer-link-secret"
		mgmtKey = "management-secret"
	)
	_, _, meetAddr, priv := startMeet(t, WithServerKey(mgmtKey), WithLinkKey(linkKey))
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}

	if reply := viewerPunch(t, viewer, meetAddr, "link-key-wrong-1", "not-the-link-key"); reply.OK {
		t.Fatalf("wrong punch key accepted: %+v", reply)
	}
	reply := viewerPunch(t, viewer, meetAddr, "link-key-viewer1", linkKey)
	if !reply.OK || reply.Candidate == "" {
		t.Fatalf("link key punch: %+v", reply)
	}
	if notify := recv(t, host); notify.Type != wire.TypePunchPeer {
		t.Fatalf("host notification: %+v", notify)
	}

	reply = viewerPunch(t, viewer, meetAddr, "link-key-server1", mgmtKey)
	if !reply.OK || reply.Candidate == "" {
		t.Fatalf("management key punch: %+v", reply)
	}
	if notify := recv(t, host); notify.Type != wire.TypePunchPeer {
		t.Fatalf("host notification: %+v", notify)
	}
}

// TestTCPPunchAcceptsLinkKey does the same over the TCP coordination path,
// including a full pairing so the accepted credential is the only difference
// from the legacy single-key flow.
func TestTCPPunchAcceptsLinkKey(t *testing.T) {
	const (
		linkKey = "viewer-link-secret"
		mgmtKey = "management-secret"
	)
	meet, _, meetAddr, priv := startMeet(t, WithServerKey(mgmtKey), WithLinkKey(linkKey))
	keepalive(t, listenUDP(t), meetAddr, priv)
	addr := tcpPunchAddr(t, meet)

	pair := func(nonce, viewerKey string) (tcpAnnounceResult, tcpAnnounceResult) {
		now := time.Now().Unix()
		hostCh := make(chan tcpAnnounceResult, 1)
		go func() { hostCh <- tcpAnnounce(addr, hostAnnounce(priv, nonce, now)) }()
		time.Sleep(100 * time.Millisecond)
		viewer := tcpAnnounce(addr, wire.Message{
			Type:      wire.TypeTCPPunch,
			Role:      wire.RoleViewer,
			HostID:    "devbox",
			Nonce:     nonce,
			Timestamp: now,
			Auth:      identity.PunchAuth(viewerKey, "devbox", nonce, now),
		})
		return viewer, <-hostCh
	}

	viewer, host := pair("tcp-punch-link-key1", linkKey)
	if viewer.err != nil || host.err != nil {
		t.Fatalf("announce: viewer=%v host=%v", viewer.err, host.err)
	}
	if !viewer.reply.OK || !host.reply.OK {
		t.Fatalf("link key announces not paired: viewer=%+v host=%+v", viewer.reply, host.reply)
	}

	viewer, host = pair("tcp-punch-mgmt-key1", mgmtKey)
	if viewer.err != nil || host.err != nil || !viewer.reply.OK || !host.reply.OK {
		t.Fatalf("management key announces not paired: viewer=%+v host=%+v errors=%v/%v",
			viewer.reply, host.reply, viewer.err, host.err)
	}

	// A wrong viewer credential is refused before the host state is revealed.
	p := &tcpPuncher{srv: meet}
	nonce := "tcp-punch-wrong-key"
	stamp := time.Now().Unix()
	if err := p.checkAnnounce(wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleViewer,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: stamp,
		Auth:      identity.PunchAuth("not-the-link-key", "devbox", nonce, stamp),
	}, "127.0.0.1:40000"); err == nil || err.Error() != "punch authentication failed" {
		t.Fatalf("wrong viewer key accepted: %v", err)
	}
}
