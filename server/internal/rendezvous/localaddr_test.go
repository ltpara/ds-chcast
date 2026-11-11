package rendezvous

import (
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// A viewer on the host's own network is told the LAN candidates from the last
// keepalive; an address that changed since then would send it to a stale
// endpoint. The punch itself must ask the host for the current list, and the
// signed answer must reach the same viewer as a refreshed reply.
func TestPunchRefreshesLANCandidates(t *testing.T) {
	_, _, meetAddr, priv := startMeet(t)
	host := listenUDP(t)
	viewer := listenUDP(t)

	keepalive(t, host, meetAddr, priv)

	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
	}), meetAddr)

	notice := recv(t, host)
	if notice.Type != wire.TypePunchPeer || !notice.WantLocalAddrs {
		t.Fatalf("punch notification = %+v, want a LAN candidate request", notice)
	}
	cached := recv(t, viewer)
	if cached.Type != wire.TypePunchPeer || !cached.OK || !cached.IsLocal {
		t.Fatalf("cached reply = %+v", cached)
	}
	if len(cached.LocalAddrs) != 1 || cached.LocalAddrs[0] != "192.168.1.9:14434" {
		t.Fatalf("cached local addrs = %+v", cached.LocalAddrs)
	}

	fresh := []string{"192.168.1.77:14434"}
	ts := time.Now().Unix()
	message := identity.LocalAddrMessage("devbox", nonce, fresh, ts)
	host.WriteToUDP(wire.Encode(wire.Message{
		Type:       wire.TypeLocalAddr,
		HostID:     "devbox",
		Nonce:      nonce,
		Timestamp:  ts,
		LocalAddrs: fresh,
		Signature:  identity.Sign(priv, message),
	}), meetAddr)

	refreshed := recv(t, viewer)
	if refreshed.Type != wire.TypePunchPeer || refreshed.Nonce != nonce || !refreshed.IsLocal {
		t.Fatalf("refreshed reply = %+v", refreshed)
	}
	if len(refreshed.LocalAddrs) != 1 || refreshed.LocalAddrs[0] != fresh[0] {
		t.Fatalf("refreshed local addrs = %+v, want %+v", refreshed.LocalAddrs, fresh)
	}
	if refreshed.Candidate != cached.Candidate || refreshed.PublicKey != cached.PublicKey {
		t.Fatalf("refresh changed the public answer: %+v vs %+v", refreshed, cached)
	}
}

// Only a signature over this exact nonce and list may refresh a punch; a
// tampered or replayed answer is dropped and the waiter stays available for
// the honest one.
func TestLocalAddrAnswerMustVerify(t *testing.T) {
	_, _, meetAddr, priv := startMeet(t)
	host := listenUDP(t)
	viewer := listenUDP(t)

	keepalive(t, host, meetAddr, priv)

	const nonce = "fedcba9876543210"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
	}), meetAddr)
	if notice := recv(t, host); !notice.WantLocalAddrs {
		t.Fatalf("punch notification = %+v", notice)
	}
	recv(t, viewer) // cached reply

	fresh := []string{"10.0.0.5:14434"}
	ts := time.Now().Unix()
	forged := identity.LocalAddrMessage("devbox", nonce, []string{"10.0.0.6:14434"}, ts)
	host.WriteToUDP(wire.Encode(wire.Message{
		Type:       wire.TypeLocalAddr,
		HostID:     "devbox",
		Nonce:      nonce,
		Timestamp:  ts,
		LocalAddrs: fresh,
		Signature:  identity.Sign(priv, forged),
	}), meetAddr)
	expectNoMessage(t, viewer, 200*time.Millisecond)

	host.WriteToUDP(wire.Encode(wire.Message{
		Type:       wire.TypeLocalAddr,
		HostID:     "devbox",
		Nonce:      nonce,
		Timestamp:  ts,
		LocalAddrs: fresh,
		Signature:  identity.Sign(priv, identity.LocalAddrMessage("devbox", nonce, fresh, ts)),
	}), meetAddr)
	refreshed := recv(t, viewer)
	if len(refreshed.LocalAddrs) != 1 || refreshed.LocalAddrs[0] != fresh[0] {
		t.Fatalf("refreshed local addrs = %+v", refreshed.LocalAddrs)
	}
}
