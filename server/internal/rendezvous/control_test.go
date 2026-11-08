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

func openHostControl(t *testing.T, meet *Server, id string, priv ed25519.PrivateKey) (net.Conn, wire.Message) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", meet.TCPAddr(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	hello := wire.Message{Type: wire.TypeRendezvousControl, HostID: id, Nonce: "client-connection-nonce"}
	writeAnnounce(conn, hello)
	challenge, err := readAnnounce(conn)
	if err != nil {
		t.Fatal(err)
	}
	pub, err := identity.ParsePublicKey(meet.identity.PublicKeyText())
	if err != nil {
		t.Fatal(err)
	}
	if challenge.TID != hello.Nonce || !identity.Verify(pub,
		identity.RendezvousControlMessage("server", id, hello.Nonce, challenge.Nonce), challenge.Signature) {
		t.Fatal("server did not authenticate its control challenge")
	}
	writeAnnounce(conn, wire.Message{
		Type: wire.TypeRendezvousProof, HostID: id, TID: hello.Nonce, Nonce: challenge.Nonce,
		Signature: identity.Sign(priv, identity.RendezvousControlMessage("host", id, hello.Nonce, challenge.Nonce)),
	})
	ack, err := readAnnounce(conn)
	if err != nil {
		t.Fatal(err)
	}
	return conn, ack
}

func TestHostControlAuthenticatesBeforeBecomingOnline(t *testing.T) {
	meet, _, _, priv := startMeet(t, WithServerKey("test-key"))
	wrong := identity.PrivateKeyFromSecret("unregistered-key")
	for _, id := range []string{"devbox", "unknown-host"} {
		conn, reply := openHostControl(t, meet, id, wrong)
		conn.Close()
		if reply.OK || reply.Error != "punch authentication failed" || meet.HostControlLinked(id) {
			t.Fatalf("unauthenticated control for %s: %+v", id, reply)
		}
	}
	conn, reply := openHostControl(t, meet, "devbox", priv)
	if !reply.OK || !meet.HostControlLinked("devbox") {
		t.Fatalf("valid control refused: %+v", reply)
	}
	conn.Close()
	deadline := time.Now().Add(time.Second)
	for meet.HostControlLinked("devbox") {
		if time.Now().After(deadline) {
			t.Fatal("closed control still reports host online")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestHostControlTriggerAndHostReportOrdering(t *testing.T) {
	meet, _, _, priv := startMeet(t, WithServerKey("test-key"))
	control, ack := openHostControl(t, meet, "devbox", priv)
	if !ack.OK {
		t.Fatal(ack.Error)
	}
	viewer, err := net.DialTimeout("tcp", meet.TCPAddr(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer viewer.Close()
	nonce := "server-directed-tcp-punch"
	writeAnnounce(viewer, viewerAnnounce(nonce, time.Now().Unix()))
	trigger, err := readAnnounce(control)
	if err != nil {
		t.Fatal(err)
	}
	if trigger.Type != wire.TypeTCPPunch || trigger.Nonce != nonce || trigger.Candidate != viewer.LocalAddr().String() {
		t.Fatalf("incorrect server-directed trigger: %+v", trigger)
	}
	pub, _ := identity.ParsePublicKey(meet.identity.PublicKeyText())
	if !identity.Verify(pub, identity.RendezvousTriggerMessage("devbox", "client-connection-nonce",
		nonce, trigger.Candidate, trigger.NATType, trigger.IsLocal, trigger.Timestamp), trigger.Signature) {
		t.Fatal("trigger not bound to authenticated control connection")
	}
	responses := make(chan tcpAnnounceResult, 1)
	go func() {
		msg, err := readAnnounce(viewer)
		responses <- tcpAnnounceResult{reply: msg, err: err}
	}()
	select {
	case reply := <-responses:
		t.Fatalf("viewer released before the host's pre-punch report: %+v", reply)
	case <-time.After(75 * time.Millisecond):
	}
	host := tcpAnnounce(meet.TCPAddr(), hostAnnounce(priv, nonce, time.Now().Unix()))
	if host.err != nil || !host.reply.OK {
		t.Fatalf("host report: %+v", host)
	}
	select {
	case reply := <-responses:
		if reply.err != nil || !reply.reply.OK || reply.reply.Candidate != host.local {
			t.Fatalf("viewer response: %+v", reply)
		}
	case <-time.After(time.Second):
		t.Fatal("host report did not release viewer")
	}
	if err := carrier.NewFrameWriter(control).WriteFrame(nil); err != nil {
		t.Fatal(err)
	}
	if data, err := carrier.NewFrameReader(control).ReadFrame(); err != nil || len(data) != 0 {
		t.Fatalf("control keepalive failed: %q, %v", data, err)
	}
}

func TestHostControlReplacementAndLeaseRemoval(t *testing.T) {
	meet, _, _, priv := startMeet(t, WithServerKey("test-key"))
	first, ack := openHostControl(t, meet, "devbox", priv)
	if !ack.OK {
		t.Fatal(ack.Error)
	}
	second, ack := openHostControl(t, meet, "devbox", priv)
	if !ack.OK {
		t.Fatal(ack.Error)
	}
	_ = first.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := readAnnounce(first); err == nil {
		t.Fatal("replaced control still open")
	}
	if !meet.HostControlLinked("devbox") {
		t.Fatal("old connection cleanup deleted replacement")
	}
	meet.reg.Remove("devbox")
	if meet.HostControlLinked("devbox") {
		t.Fatal("removed lease still online")
	}
	if meet.tcpPunch.triggerControl(wire.Message{HostID: "devbox", Nonce: "must-not-deliver"}) {
		t.Fatal("removed lease received a punch trigger")
	}
	_ = carrier.NewFrameWriter(second).WriteFrame(nil)
	if _, err := carrier.NewFrameReader(second).ReadFrame(); err == nil {
		t.Fatal("removed lease kept control alive")
	}
}

func TestTCPRetryReusesSessionAndDisconnectCleansUp(t *testing.T) {
	meet, _, _, priv := startMeet(t, WithServerKey("test-key"))
	control, ack := openHostControl(t, meet, "devbox", priv)
	if !ack.OK {
		t.Fatal(ack.Error)
	}
	viewer, err := net.DialTimeout("tcp", meet.TCPAddr(), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer viewer.Close()
	msg := viewerAnnounce("retry-same-session", time.Now().Unix())
	for i := 0; i < 2; i++ {
		writeAnnounce(viewer, msg)
		trigger, err := readAnnounce(control)
		if err != nil || trigger.Nonce != msg.Nonce || trigger.Candidate != viewer.LocalAddr().String() {
			t.Fatalf("retry trigger: %+v %v", trigger, err)
		}
		meet.tcpPunch.mu.Lock()
		count := len(meet.tcpPunch.sessions)
		meet.tcpPunch.mu.Unlock()
		if count != 1 {
			t.Fatalf("retry created %d pending sessions", count)
		}
	}
	viewer.Close()
	deadline := time.Now().Add(time.Second)
	for {
		meet.tcpPunch.mu.Lock()
		count := len(meet.tcpPunch.sessions)
		meet.tcpPunch.mu.Unlock()
		if count == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("disconnected viewer retained pending session")
		}
		time.Sleep(time.Millisecond)
	}
}

func TestTCPLocalAddressOnlyReturnedForSameObservedIP(t *testing.T) {
	peer := &tcpWait{observed: "198.51.100.1:45000", local: "192.168.1.2:35000"}
	for _, tc := range []struct {
		observed, want string
		local          bool
	}{
		{"198.51.100.1:50000", "192.168.1.2:35000", true},
		{"198.51.100.2:50000", "198.51.100.1:45000", false},
	} {
		got := tcpPeerResult(peer, &tcpWait{observed: tc.observed})
		if got.peer != tc.want || got.local != tc.local {
			t.Fatalf("peer result = %+v", got)
		}
	}
}
