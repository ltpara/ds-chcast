package acl

import (
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"
)

func testSeed() []byte {
	seed := make([]byte, ed25519.SeedSize)
	for i := range seed {
		seed[i] = byte(i)
	}
	return seed
}

func testClientKey() [32]byte {
	var key [32]byte
	for i := range key {
		key[i] = byte(0x40 + i)
	}
	return key
}

func testTicket() Ticket {
	const issued = int64(1794225600) // 2026-11-09T12:00:00Z
	return Ticket{
		Audience:                 "device:devbox",
		KeyID:                    "kid-2026-11",
		HostID:                   "devbox",
		HostPublicKeyFingerprint: "SHA256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		Subject:                  "alice",
		ClientPublicKey:          testClientKey(),
		Permissions:              0x0000000f,
		PolicyVersion:            2,
		IssuedAt:                 issued,
		NotBefore:                issued,
		ExpiresAt:                issued + 60,
		ID:                       "jti-0001",
	}
}

func testIssuer(t *testing.T) *Issuer {
	t.Helper()
	issuer, err := NewIssuer("chast.example", "kid-2026-11", testSeed())
	if err != nil {
		t.Fatalf("issuer: %v", err)
	}
	return issuer
}

func testVerifier(t *testing.T) *Verifier {
	t.Helper()
	verifier := NewVerifier("chast.example", "device:devbox")
	if err := verifier.PinKey("kid-2026-11", testIssuer(t).PublicKeyText()); err != nil {
		t.Fatalf("pin key: %v", err)
	}
	return verifier
}

func TestTicketRoundTrip(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.Sign(testTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	ticket, err := testVerifier(t).Verify(signed, now)
	if err != nil {
		t.Fatalf("verify: %v", err)
	}
	if ticket.Issuer != "chast.example" || ticket.KeyID != "kid-2026-11" ||
		ticket.HostID != "devbox" || ticket.Subject != "alice" ||
		ticket.Permissions != 0x0000000f || ticket.PolicyVersion != 2 ||
		ticket.ID != "jti-0001" || ticket.ClientPublicKey != testClientKey() {
		t.Fatalf("round trip = %+v", ticket)
	}
}

func TestTicketVerifyRejectsContext(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.Sign(testTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	otherIssuer := NewVerifier("other.example", "device:devbox")
	_ = otherIssuer.PinKey("kid-2026-11", issuer.PublicKeyText())
	if _, err := otherIssuer.Verify(signed, now); !errors.Is(err, ErrIssuer) {
		t.Fatalf("wrong issuer = %v", err)
	}
	otherAudience := NewVerifier("chast.example", "device:other")
	_ = otherAudience.PinKey("kid-2026-11", issuer.PublicKeyText())
	if _, err := otherAudience.Verify(signed, now); !errors.Is(err, ErrAudience) {
		t.Fatalf("wrong audience = %v", err)
	}
	unpinned := NewVerifier("chast.example", "device:devbox")
	if _, err := unpinned.Verify(signed, now); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("unknown key = %v", err)
	}
	tampered := append([]byte(nil), signed...)
	tampered[10] ^= 0x01
	if _, err := testVerifier(t).Verify(tampered, now); !errors.Is(err, ErrSignature) && !errors.Is(err, ErrField) {
		t.Fatalf("tampered = %v", err)
	}
	short := signed[:len(signed)-1]
	if _, err := testVerifier(t).Verify(short, now); !errors.Is(err, ErrSignature) && !errors.Is(err, ErrMalformed) {
		t.Fatalf("truncated = %v", err)
	}
}

func TestTicketTimeWindow(t *testing.T) {
	issuer := testIssuer(t)
	ticket := testTicket()
	now := time.Unix(ticket.IssuedAt, 0)
	signed, err := issuer.Sign(ticket, now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	verifier := testVerifier(t)
	if _, err := verifier.Verify(signed, now.Add(-6*time.Second)); !errors.Is(err, ErrNotYetValid) {
		t.Fatalf("early = %v", err)
	}
	if _, err := verifier.Verify(signed, now.Add(-4*time.Second)); err != nil {
		t.Fatalf("inside skew = %v", err)
	}
	if _, err := verifier.Verify(signed, now.Add(59*time.Second)); err != nil {
		t.Fatalf("near expiry = %v", err)
	}
	if _, err := verifier.Verify(signed, now.Add(66*time.Second)); !errors.Is(err, ErrExpired) {
		t.Fatalf("expired = %v", err)
	}

	tooLong := ticket
	tooLong.ExpiresAt = tooLong.IssuedAt + 61
	if _, err := issuer.Sign(tooLong, now); !errors.Is(err, ErrLifetime) {
		t.Fatalf("long lifetime = %v", err)
	}
	future := ticket
	future.NotBefore = now.Unix() + 10
	future.ExpiresAt = now.Unix() + 50
	if _, err := issuer.Sign(future, now); !errors.Is(err, ErrNotYetValid) {
		t.Fatalf("future issuance = %v", err)
	}
}

func TestTicketFieldValidation(t *testing.T) {
	base := testTicket()
	now := time.Unix(base.IssuedAt, 0)
	issuer := testIssuer(t)
	cases := []struct {
		name   string
		mutate func(*Ticket)
		want   error
	}{
		{"empty subject", func(tk *Ticket) { tk.Subject = "" }, ErrField},
		{"empty fingerprint", func(tk *Ticket) { tk.HostPublicKeyFingerprint = "" }, ErrField},
		{"empty jti", func(tk *Ticket) { tk.ID = "" }, ErrField},
		{"zero client key", func(tk *Ticket) { tk.ClientPublicKey = [32]byte{} }, ErrField},
		{"long host id", func(tk *Ticket) { tk.HostID = strings.Repeat("h", 65) }, ErrField},
		{"long jti", func(tk *Ticket) { tk.ID = strings.Repeat("j", 65) }, ErrField},
		{"not before after expiry", func(tk *Ticket) { tk.NotBefore = tk.ExpiresAt }, ErrField},
		{"negative time", func(tk *Ticket) { tk.IssuedAt = -1 }, ErrField},
	}
	for _, tc := range cases {
		ticket := base
		tc.mutate(&ticket)
		if _, err := issuer.Sign(ticket, now); !errors.Is(err, tc.want) {
			t.Fatalf("%s = %v, want %v", tc.name, err, tc.want)
		}
	}
}

func TestTicketDecodeRejectsMalformed(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.Sign(testTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	payload := signed[:len(signed)-ed25519.SignatureSize]
	if _, err := Decode(payload); err != nil {
		t.Fatalf("decode canonical: %v", err)
	}
	for _, tc := range []struct {
		name string
		data []byte
	}{
		{"unknown version", append([]byte{FormatVersion + 1}, payload[1:]...)},
		{"unknown algorithm", append([]byte{payload[0], AlgorithmEd25519 + 1}, payload[2:]...)},
		{"reserved flags", append([]byte{payload[0], payload[1], 0, 1}, payload[4:]...)},
		{"trailing byte", append(append([]byte(nil), payload...), 0)},
		{"empty fields", append([]byte{1, 1, 0, 0}, make([]byte, minPayloadSize)...)},
	} {
		if _, err := Decode(tc.data); err == nil {
			t.Fatalf("%s was accepted", tc.name)
		}
	}
	if _, err := Decode(nil); !errors.Is(err, ErrMalformed) {
		t.Fatalf("nil = %v", err)
	}
}

func TestReplayGuard(t *testing.T) {
	now := time.Unix(1794225600, 0)
	guard := NewReplayGuard(2)
	if !guard.Consume("jti-a", now.Unix()+60, now) {
		t.Fatalf("first consume rejected")
	}
	if guard.Consume("jti-a", now.Unix()+60, now) {
		t.Fatalf("replay accepted")
	}
	if !guard.Consume("jti-b", now.Unix()+60, now) {
		t.Fatalf("second consume rejected")
	}
	if guard.Consume("jti-c", now.Unix()+60, now) {
		t.Fatalf("full cache accepted a third ticket")
	}
	later := now.Add(70 * time.Second)
	if !guard.Consume("jti-c", later.Unix()+60, later) {
		t.Fatalf("expired entries were not reclaimed")
	}
	if guard.Len() != 1 {
		t.Fatalf("live entries = %d, want 1", guard.Len())
	}
}

// TestGoldenTicket pins the byte encoding and signature so a C++ verifier can
// reproduce them exactly. Regenerating these values without a format version
// change would break existing hosts by design.
func TestGoldenTicket(t *testing.T) {
	issuer := testIssuer(t)
	now := time.Unix(1794225600, 0)
	signed, err := issuer.Sign(testTicket(), now)
	if err != nil {
		t.Fatalf("sign: %v", err)
	}
	payload := signed[:len(signed)-ed25519.SignatureSize]
	signature := signed[len(signed)-ed25519.SignatureSize:]
	t.Logf("payload=%s", hex.EncodeToString(payload))
	t.Logf("signature=%s", hex.EncodeToString(signature))

	const wantPayloadHex = "01010000000d63686173742e6578616d706c65000d6465766963653a646576626f78000b6b69642d323032362d31310006646576626f7800475348413235363a303132333435363738396162636465663031323334353637383961626364656630313233343536373839616263646566303132333435363738396162636465660005616c696365404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f0000000f00000002000000006af1b5c0000000006af1b5c0000000006af1b5fc00086a74692d30303031"
	const wantSignatureHex = "e907b6f1daaf566b88b482132e197190803bdb5e1d87b5c3100eb3bda6a993799033f6b3b09ab233dc61d3a82ccc8d70f78f2b2f6df5634794cdec5b31bad504"
	if got := hex.EncodeToString(payload); got != wantPayloadHex {
		t.Fatalf("payload hex = %s", got)
	}
	if got := hex.EncodeToString(signature); got != wantSignatureHex {
		t.Fatalf("signature hex = %s", got)
	}
	ticket, err := testVerifier(t).Verify(signed, now)
	if err != nil {
		t.Fatalf("verify golden: %v", err)
	}
	if ticket.ID != "jti-0001" {
		t.Fatalf("golden ticket = %+v", ticket)
	}
}
