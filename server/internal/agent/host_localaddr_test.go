package agent

import (
	"context"
	"crypto/ed25519"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// A punch from a viewer on the host's network asks for the LAN candidates the
// socket holds now. The answer must echo the punch nonce and be signed over
// the exact list, so the rendezvous service can hand it to that viewer only.
func TestHostAnswersLANCandidateRequest(t *testing.T) {
	h, priv := testHostAgent(t)
	h.ctx = context.Background()
	server, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	h.punch = server.LocalAddr().(*net.UDPAddr)
	peer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer peer.Close()

	const nonce = "0123456789abcdef"
	h.handleControl(wire.Encode(wire.Message{
		Type:           wire.TypePunchPeer,
		Candidate:      peer.LocalAddr().String(),
		Nonce:          nonce,
		WantLocalAddrs: true,
	}), h.punch)

	_ = server.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, wire.MaxControlSize)
	n, _, err := server.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("no LAN candidate answer: %v", err)
	}
	msg, err := wire.Decode(buf[:n])
	if err != nil {
		t.Fatal(err)
	}
	if msg.Type != wire.TypeLocalAddr || msg.HostID != "devbox" || msg.Nonce != nonce {
		t.Fatalf("answer = %+v", msg)
	}
	if age := time.Now().Unix() - msg.Timestamp; age < -5 || age > 5 {
		t.Fatalf("answer timestamp is not fresh: %d", msg.Timestamp)
	}
	want4, want6 := splitByFamily(localCandidates(h.conn.LocalAddr().(*net.UDPAddr)))
	if len(msg.LocalAddrs) != len(want4) || len(msg.LocalAddrsV6) != len(want6) {
		t.Fatalf("answer addrs = %+v/%+v, want %+v/%+v", msg.LocalAddrs, msg.LocalAddrsV6, want4, want6)
	}
	addrs := append(append([]string{}, msg.LocalAddrs...), msg.LocalAddrsV6...)
	if !identity.Verify(priv.Public().(ed25519.PublicKey),
		identity.LocalAddrMessage(msg.HostID, nonce, addrs, msg.Timestamp), msg.Signature) {
		t.Fatal("LAN candidate answer did not verify")
	}

	// The request is only honoured from the rendezvous address and only when
	// the punch actually asked for it.
	stranger := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 9), Port: 9}
	h.handleControl(wire.Encode(wire.Message{
		Type: wire.TypePunchPeer, Candidate: peer.LocalAddr().String(),
		Nonce: nonce + "x", WantLocalAddrs: true,
	}), stranger)
	h.handleControl(wire.Encode(wire.Message{
		Type: wire.TypePunchPeer, Candidate: peer.LocalAddr().String(),
		Nonce: nonce + "y",
	}), h.punch)
	_ = server.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if n, _, err := server.ReadFromUDP(buf); err == nil {
		t.Fatalf("unexpected answer %q for an unasked or unauthenticated punch", buf[:n])
	}
}
