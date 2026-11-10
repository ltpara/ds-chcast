package relay

import (
	"crypto/ed25519"
	"encoding/base64"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func relayTransportIssuer(t *testing.T) *acl.Issuer {
	t.Helper()
	seed := make([]byte, 32)
	for i := range seed {
		seed[i] = byte(i + 23)
	}
	issuer, err := acl.NewIssuer("chast.example", "kid-relay", seed)
	if err != nil {
		t.Fatalf("issuer: %v", err)
	}
	return issuer
}

func relayPinnedVerifier(t *testing.T, issuer *acl.Issuer) *acl.TransportVerifier {
	t.Helper()
	verifier := acl.NewTransportVerifier(issuer.Name(), "")
	if err := verifier.PinKey(issuer.KeyID(), issuer.PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	return verifier
}

// relayTransportTicket mints one transport credential for a test viewer key.
func relayTransportTicket(t *testing.T, issuer *acl.Issuer, audience, hostID, id string,
	viewerKey ed25519.PrivateKey, now time.Time) string {
	t.Helper()
	ticket := acl.TransportTicket{
		Audience:      audience,
		HostID:        hostID,
		Subject:       "shared",
		PolicyVersion: 2,
		IssuedAt:      now.Unix(),
		NotBefore:     now.Unix(),
		ExpiresAt:     now.Add(60 * time.Second).Unix(),
		ID:            id,
	}
	copy(ticket.ClientPublicKey[:], viewerKey.Public().(ed25519.PublicKey))
	signed, err := issuer.SignTransport(ticket, now)
	if err != nil {
		t.Fatalf("sign transport ticket: %v", err)
	}
	return base64.RawURLEncoding.EncodeToString(signed)
}

func transportViewerMessage(t *testing.T, audience, hostID, ticket string, viewerKey ed25519.PrivateKey,
	nonce string, timestamp int64) wire.Message {
	t.Helper()
	return wire.Message{
		Type:           wire.TypeRelayViewer,
		HostID:         hostID,
		Nonce:          nonce,
		Timestamp:      timestamp,
		Transport:      ticket,
		TransportProof: identity.Sign(viewerKey, identity.TransportProofMessage(audience, hostID, nonce, timestamp)),
	}
}

func TestRelayViewerAuthAcceptsTransportCredential(t *testing.T) {
	issuer := relayTransportIssuer(t)
	_, _, relayAddr, priv := startRelayWith(t, "devbox", "host-secret",
		WithTransportVerifier(relayPinnedVerifier(t, issuer)))
	host := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}

	_, viewerKey, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	const nonce = "relay-nonce-1234"
	ticket := relayTransportTicket(t, issuer, acl.TransportAudienceRelay, "devbox", "relay-ticket-1", viewerKey, now)
	viewer := listenUDP(t)
	viewer.WriteToUDP(wire.Encode(transportViewerMessage(t, acl.TransportAudienceRelay,
		"devbox", ticket, viewerKey, nonce, now.Unix())), relayAddr)
	ack, err := wire.Decode(recv(t, viewer, 2*time.Second))
	if err != nil || !ack.OK {
		t.Fatalf("transport viewer auth: %v %+v", err, ack)
	}

	// The same proof replayed from another source must not authorize that
	// source; only the original socket may retransmit it.
	replay := listenUDP(t)
	replay.WriteToUDP(wire.Encode(transportViewerMessage(t, acl.TransportAudienceRelay,
		"devbox", ticket, viewerKey, nonce, now.Unix())), relayAddr)
	if ack, err := wire.Decode(recv(t, replay, 2*time.Second)); err != nil || ack.OK {
		t.Fatalf("replayed credential: %v %+v", err, ack)
	}
}

func TestRelayViewerAuthRefusesWrongAudienceAndBadProof(t *testing.T) {
	issuer := relayTransportIssuer(t)
	_, reg, relayAddr, priv := startRelayWith(t, "devbox", "host-secret",
		WithTransportVerifier(relayPinnedVerifier(t, issuer)))
	host := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}
	record, _ := reg.Lookup("devbox")
	_, viewerKey, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	const nonce = "relay-nonce-5678"

	// A ticket minted for the carrier must never admit a relay viewer, even
	// with a valid proof and a valid legacy token attached.
	punchTicket := relayTransportTicket(t, issuer, acl.TransportAudiencePunch,
		"devbox", "relay-ticket-2", viewerKey, now)
	msg := transportViewerMessage(t, acl.TransportAudiencePunch, "devbox", punchTicket, viewerKey, nonce, now.Unix())
	msg.Token = record.CarrierToken
	viewer := listenUDP(t)
	viewer.WriteToUDP(wire.Encode(msg), relayAddr)
	if ack, err := wire.Decode(recv(t, viewer, 2*time.Second)); err != nil || ack.OK {
		t.Fatalf("wrong audience: %v %+v", err, ack)
	}

	// A stale proof outside the freshness window is refused.
	stale := transportViewerMessage(t, acl.TransportAudienceRelay,
		"devbox", relayTransportTicket(t, issuer, acl.TransportAudienceRelay, "devbox", "relay-ticket-3", viewerKey, now),
		viewerKey, nonce, now.Add(-transportProofWindow-time.Second).Unix())
	viewer.WriteToUDP(wire.Encode(stale), relayAddr)
	if ack, err := wire.Decode(recv(t, viewer, 2*time.Second)); err != nil || ack.OK {
		t.Fatalf("stale proof: %v %+v", err, ack)
	}
}

func TestRelayCarrierHelloAcceptsCarrierTransport(t *testing.T) {
	issuer := relayTransportIssuer(t)
	reg := registry.New(time.Minute)
	priv := identity.PrivateKeyFromSecret("host-secret")
	if _, err := reg.Register(registry.RegisterRequest{
		ID:        "devbox",
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		TTL:       time.Minute,
	}); err != nil {
		t.Fatalf("register: %v", err)
	}
	record, _ := reg.Lookup("devbox")
	sc := &SharedCarrier{auth: reg, transport: newTransportGuard(relayPinnedVerifier(t, issuer))}

	_, viewerKey, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	const nonce = "carrier-nonce-1"
	ticket := relayTransportTicket(t, issuer, acl.TransportAudienceCarrier,
		"devbox", "carrier-ticket-1", viewerKey, now)
	hello := carrier.SharedViewerHello{
		HostID:         "devbox",
		Nonce:          nonce,
		Timestamp:      now.Unix(),
		Transport:      ticket,
		TransportProof: identity.Sign(viewerKey, identity.TransportProofMessage(acl.TransportAudienceCarrier, "devbox", nonce, now.Unix())),
	}
	if !sc.viewerAdmitted(hello, "10.0.0.1:40000") {
		t.Fatal("carrier transport credential was refused")
	}
	if sc.viewerAdmitted(hello, "10.0.0.2:40000") {
		t.Fatal("carrier proof replayed from another peer was accepted")
	}

	// A legacy token is still accepted for an unmanaged host but refused once
	// the device policy makes the host managed.
	legacy := carrier.SharedViewerHello{HostID: "devbox", Token: record.CarrierToken}
	if !sc.viewerAdmitted(legacy, "10.0.0.3:40000") {
		t.Fatal("unmanaged host refused its carrier token")
	}
	reg.SetPolicy("devbox", true, 3)
	if sc.viewerAdmitted(legacy, "10.0.0.4:40000") {
		t.Fatal("managed host accepted the legacy carrier token")
	}
}
