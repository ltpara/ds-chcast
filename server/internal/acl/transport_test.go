package acl

import (
	"encoding/base64"
	"errors"
	"testing"
	"time"
)

func testTransportTicket() TransportTicket {
	const issued = int64(1794225600) // 2026-11-09T12:00:00Z
	return TransportTicket{
		Audience:        TransportAudiencePunch,
		HostID:          "devbox",
		Subject:         "alice",
		ClientPublicKey: testClientKey(),
		PolicyVersion:   2,
		IssuedAt:        issued,
		NotBefore:       issued,
		ExpiresAt:       issued + 60,
		ID:              "transport-0001",
	}
}

func testTransportVerifier(t *testing.T, audience string) *TransportVerifier {
	t.Helper()
	verifier := NewTransportVerifier("chast.example", audience)
	if err := verifier.PinKey("kid-2026-11", testIssuer(t).PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	return verifier
}

func TestTransportTicketRoundTrip(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.SignTransport(testTransportTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	ticket, err := testTransportVerifier(t, TransportAudiencePunch).Verify(signed, now)
	if err != nil {
		t.Fatalf("verify: %v", err)
	}
	if ticket.Issuer != "chast.example" || ticket.KeyID != "kid-2026-11" ||
		ticket.Audience != TransportAudiencePunch || ticket.HostID != "devbox" ||
		ticket.Subject != "alice" || ticket.PolicyVersion != 2 ||
		ticket.ID != "transport-0001" || ticket.ClientPublicKey != testClientKey() {
		t.Fatalf("round trip = %+v", ticket)
	}
}

func TestTransportVerifyRejectsContext(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.SignTransport(testTransportTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	if _, err := testTransportVerifier(t, TransportAudienceRelay).Verify(signed, now); !errors.Is(err, ErrTransportAudience) {
		t.Fatalf("wrong audience = %v", err)
	}
	otherIssuer := NewTransportVerifier("other.example", TransportAudiencePunch)
	_ = otherIssuer.PinKey("kid-2026-11", issuer.PublicKeyText())
	if _, err := otherIssuer.Verify(signed, now); !errors.Is(err, ErrIssuer) {
		t.Fatalf("wrong issuer = %v", err)
	}
	unpinned := NewTransportVerifier("chast.example", TransportAudiencePunch)
	if _, err := unpinned.Verify(signed, now); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("unknown key = %v", err)
	}
	tampered := append([]byte(nil), signed...)
	tampered[10] ^= 0x01
	if _, err := testTransportVerifier(t, TransportAudiencePunch).Verify(tampered, now); !errors.Is(err, ErrSignature) && !errors.Is(err, ErrField) {
		t.Fatalf("tampered = %v", err)
	}
	if _, err := testTransportVerifier(t, TransportAudiencePunch).Verify(signed[:len(signed)-1], now); !errors.Is(err, ErrSignature) && !errors.Is(err, ErrMalformed) {
		t.Fatalf("truncated = %v", err)
	}
}

func TestTransportTicketTimeWindow(t *testing.T) {
	issuer := testIssuer(t)
	ticket := testTransportTicket()
	now := time.Unix(ticket.IssuedAt, 0)
	signed, err := issuer.SignTransport(ticket, now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	verifier := testTransportVerifier(t, TransportAudiencePunch)
	if _, err := verifier.Verify(signed, now.Add(-6*time.Second)); !errors.Is(err, ErrNotYetValid) {
		t.Fatalf("too early = %v", err)
	}
	if _, err := verifier.Verify(signed, now.Add(66*time.Second)); !errors.Is(err, ErrExpired) {
		t.Fatalf("expired = %v", err)
	}
	if _, err := verifier.Verify(signed, now.Add(64*time.Second)); err != nil {
		t.Fatalf("inside skew = %v", err)
	}
}

func TestTransportTicketRejectsBadFields(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	ticket := testTransportTicket()
	ticket.Audience = "device:devbox"
	if _, err := issuer.SignTransport(ticket, now); !errors.Is(err, ErrField) {
		t.Fatalf("application audience = %v", err)
	}
	ticket = testTransportTicket()
	ticket.ClientPublicKey = [32]byte{}
	if _, err := issuer.SignTransport(ticket, now); !errors.Is(err, ErrField) {
		t.Fatalf("empty key = %v", err)
	}
	ticket = testTransportTicket()
	ticket.ExpiresAt = ticket.IssuedAt + 61
	if _, err := issuer.SignTransport(ticket, now); !errors.Is(err, ErrLifetime) {
		t.Fatalf("long lifetime = %v", err)
	}
}

// TestTransportGoldenVector freezes the v1 bytes both the server and any
// independent verifier must reproduce.
func TestTransportGoldenVector(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.SignTransport(testTransportTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	got := base64.RawURLEncoding.EncodeToString(signed)
	const want = "AQEAAAANY2hhc3QuZXhhbXBsZQAPdHJhbnNwb3J0OnB1bmNoAAtraWQtMjAyNi0xMQAGZGV2Ym94AAVhbGljZUBBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5fAAAAAgAAAABq8bXAAAAAAGrxtcAAAAAAavG1_AAOdHJhbnNwb3J0LTAwMDHRBTxaU4gc94dG7n0EtaJskXSxG_HBu9BtyQQS-IozwHLq1LAMWJt-F7b8FuhJDA_mQnSbou_jTeNDXlrdjOsJ"
	if got != want {
		t.Fatalf("golden vector changed:\n got %s\nwant %s", got, want)
	}
	if _, err := base64.RawURLEncoding.DecodeString(want); err != nil {
		t.Fatalf("golden vector is not base64url: %v", err)
	}
}
