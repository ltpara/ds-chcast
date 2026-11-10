package rendezvous

import (
	"crypto/ed25519"
	"encoding/base64"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func transportIssuer(t *testing.T) *acl.Issuer {
	t.Helper()
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(i + 11)
	}
	issuer, err := acl.NewIssuer("chast.example", "kid-transport", seed)
	if err != nil {
		t.Fatalf("issuer: %v", err)
	}
	return issuer
}

func punchTransportVerifier(t *testing.T, issuer *acl.Issuer) *acl.TransportVerifier {
	t.Helper()
	verifier := acl.NewTransportVerifier(issuer.Name(), acl.TransportAudiencePunch)
	if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	return verifier
}

type transportViewer struct {
	priv ed25519.PrivateKey
	pub  [32]byte
}

func newTransportViewer(t *testing.T) transportViewer {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatalf("viewer key: %v", err)
	}
	viewer := transportViewer{priv: priv}
	copy(viewer.pub[:], pub)
	return viewer
}

func mintPunchTicket(t *testing.T, issuer *acl.Issuer, host string, audience string,
	viewer transportViewer, policyVersion uint32, now time.Time) string {
	t.Helper()
	signed, err := issuer.SignTransport(acl.TransportTicket{
		Audience:        audience,
		HostID:          host,
		Subject:         "alice",
		ClientPublicKey: viewer.pub,
		PolicyVersion:   policyVersion,
		IssuedAt:        now.Unix(),
		NotBefore:       now.Unix(),
		ExpiresAt:       now.Add(60 * time.Second).Unix(),
		ID:              "transport-" + host,
	}, now)
	if err != nil {
		t.Fatalf("sign transport ticket: %v", err)
	}
	return base64.RawURLEncoding.EncodeToString(signed)
}

func transportPunch(t *testing.T, conn *net.UDPConn, meetAddr *net.UDPAddr,
	viewer transportViewer, ticket, nonce string, stamp int64) wire.Message {
	t.Helper()
	msg := wire.Message{
		Type:           wire.TypePunchRequest,
		HostID:         "devbox",
		Nonce:          nonce,
		Timestamp:      stamp,
		Transport:      ticket,
		TransportProof: identity.Sign(viewer.priv, identity.TransportProofMessage(acl.TransportAudiencePunch, "devbox", nonce, stamp)),
	}
	conn.WriteToUDP(wire.Encode(msg), meetAddr)
	return recv(t, conn)
}

// TestManagedPunchRequiresTransportCredential proves that a host with an
// assigned device policy cannot be reached with the deployment-wide link or
// management key, only with a short-lived credential bound to the host and to
// the viewer's key.
func TestManagedPunchRequiresTransportCredential(t *testing.T) {
	const (
		linkKey = "viewer-link-secret"
		mgmtKey = "management-secret"
	)
	issuer := transportIssuer(t)
	_, reg, meetAddr, priv := startMeet(t,
		WithServerKey(mgmtKey),
		WithLinkKey(linkKey),
		WithTransportVerifier(punchTransportVerifier(t, issuer)),
	)
	reg.SetPolicy("devbox", true, 7)
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}

	if reply := viewerPunch(t, viewer, meetAddr, "managed-link-key1", linkKey); reply.OK {
		t.Fatalf("managed host accepted the link key: %+v", reply)
	}
	if reply := viewerPunch(t, viewer, meetAddr, "managed-mgmt-key1", mgmtKey); reply.OK {
		t.Fatalf("managed host accepted the management key: %+v", reply)
	}

	now := time.Now()
	client := newTransportViewer(t)
	ticket := mintPunchTicket(t, issuer, "devbox", acl.TransportAudiencePunch, client, 7, now)
	reply := transportPunch(t, viewer, meetAddr, client, ticket, "managed-transport1", now.Unix())
	if !reply.OK || reply.Candidate == "" {
		t.Fatalf("transport credential punch: %+v", reply)
	}
	if notify := recv(t, host); notify.Type != wire.TypePunchPeer {
		t.Fatalf("host notification: %+v", notify)
	}

	// The proof is what ties the credential to this exchange; a different
	// key cannot present a stolen ticket.
	other := newTransportViewer(t)
	reply = transportPunch(t, viewer, meetAddr, other, ticket, "managed-transport2", now.Unix())
	if reply.OK {
		t.Fatalf("ticket presented without its key was accepted: %+v", reply)
	}

	// A relay credential must not work on the punch path.
	relayTicket := mintPunchTicket(t, issuer, "devbox", acl.TransportAudienceRelay, client, 7, now)
	reply = transportPunch(t, viewer, meetAddr, client, relayTicket, "managed-transport3", now.Unix())
	if reply.OK {
		t.Fatalf("relay credential accepted for punch: %+v", reply)
	}

	// A credential minted for another host must not punch devbox.
	foreign := mintPunchTicket(t, issuer, "otherbox", acl.TransportAudiencePunch, client, 7, now)
	reply = transportPunch(t, viewer, meetAddr, client, foreign, "managed-transport4", now.Unix())
	if reply.OK {
		t.Fatalf("foreign-host credential accepted: %+v", reply)
	}

	// An unreadable ticket must not fall back to the shared keys.
	raw, err := base64.RawURLEncoding.DecodeString(ticket)
	if err != nil {
		t.Fatalf("decode minted ticket: %v", err)
	}
	raw[12] ^= 0x01
	broken := base64.RawURLEncoding.EncodeToString(raw)
	reply = transportPunch(t, viewer, meetAddr, client, broken, "managed-transport5", now.Unix())
	if reply.OK {
		t.Fatalf("broken ticket fell back to the link key: %+v", reply)
	}
}

// TestUnmanagedPunchKeepsLegacyLinkKey keeps mixed deployments working: a host
// without an assigned policy still accepts the link key even when the
// deployment can mint transport credentials.
func TestUnmanagedPunchKeepsLegacyLinkKey(t *testing.T) {
	const linkKey = "viewer-link-secret"
	issuer := transportIssuer(t)
	_, _, meetAddr, priv := startMeet(t,
		WithServerKey("management-secret"),
		WithLinkKey(linkKey),
		WithTransportVerifier(punchTransportVerifier(t, issuer)),
	)
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}
	reply := viewerPunch(t, viewer, meetAddr, "unmanaged-link-key1", linkKey)
	if !reply.OK || reply.Candidate == "" {
		t.Fatalf("unmanaged link key punch: %+v", reply)
	}
	if notify := recv(t, host); notify.Type != wire.TypePunchPeer {
		t.Fatalf("host notification: %+v", notify)
	}
}

// TestTCPPunchManagedViewerNeedsTransportCredential repeats the check on the
// TCP coordination path, which shares the viewer authentication.
func TestTCPPunchManagedViewerNeedsTransportCredential(t *testing.T) {
	const linkKey = "viewer-link-secret"
	issuer := transportIssuer(t)
	meet, reg, meetAddr, priv := startMeet(t,
		WithServerKey("management-secret"),
		WithLinkKey(linkKey),
		WithTransportVerifier(punchTransportVerifier(t, issuer)),
	)
	reg.SetPolicy("devbox", true, 7)
	keepalive(t, listenUDP(t), meetAddr, priv)
	addr := tcpPunchAddr(t, meet)

	now := time.Now()
	client := newTransportViewer(t)
	ticket := mintPunchTicket(t, issuer, "devbox", acl.TransportAudiencePunch, client, 7, now)
	nonce := "tcp-managed-transport1"

	hostCh := make(chan tcpAnnounceResult, 1)
	go func() { hostCh <- tcpAnnounce(addr, hostAnnounce(priv, nonce, now.Unix())) }()
	time.Sleep(100 * time.Millisecond)
	viewer := tcpAnnounce(addr, wire.Message{
		Type:           wire.TypeTCPPunch,
		Role:           wire.RoleViewer,
		HostID:         "devbox",
		Nonce:          nonce,
		Timestamp:      now.Unix(),
		Transport:      ticket,
		TransportProof: identity.Sign(client.priv, identity.TransportProofMessage(acl.TransportAudiencePunch, "devbox", nonce, now.Unix())),
	})
	host := <-hostCh
	if viewer.err != nil || host.err != nil || !viewer.reply.OK || !host.reply.OK {
		t.Fatalf("transport announces not paired: viewer=%+v host=%+v errors=%v/%v",
			viewer.reply, host.reply, viewer.err, host.err)
	}

	p := &tcpPuncher{srv: meet}
	stamp := time.Now().Unix()
	if err := p.checkAnnounce(wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleViewer,
		HostID:    "devbox",
		Nonce:     "tcp-managed-linkkey",
		Timestamp: stamp,
		Auth:      identity.PunchAuth(linkKey, "devbox", "tcp-managed-linkkey", stamp),
	}); err == nil || err.Error() != "punch authentication failed" {
		t.Fatalf("managed host accepted the link key over TCP punch: %v", err)
	}
}
