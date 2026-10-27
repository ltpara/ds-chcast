package webauthn_test

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"math/big"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/webauthn"
	"github.com/ltpara/ds-chcast/server/internal/webauthn/webauthntest"
)

const (
	testRPID   = "example.com"
	testOrigin = "https://example.com"
)

var testOrigins = []string{testOrigin, "https://rd.example.com:8443"}

func mustRegister(t *testing.T, alg int) (*webauthntest.Authenticator, webauthn.Credential) {
	t.Helper()
	auth, err := webauthntest.New(alg, testRPID, testOrigin)
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	challenge, err := webauthn.NewChallenge()
	if err != nil {
		t.Fatalf("challenge: %v", err)
	}
	clientData, attestation, err := auth.MakeCredential(challenge)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	credential, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, clientData, attestation)
	if err != nil {
		t.Fatalf("verify registration: %v", err)
	}
	if !bytes.Equal(credential.ID, auth.CredentialID) {
		t.Fatalf("credential id %x, want %x", credential.ID, auth.CredentialID)
	}
	if credential.Alg != alg {
		t.Fatalf("credential alg %d, want %d", credential.Alg, alg)
	}
	return auth, credential
}

func TestRegisterAndAssertAllAlgorithms(t *testing.T) {
	for _, alg := range []int{webauthn.AlgES256, webauthn.AlgRS256, webauthn.AlgEdDSA} {
		t.Run(algName(alg), func(t *testing.T) {
			auth, credential := mustRegister(t, alg)
			challenge, err := webauthn.NewChallenge()
			if err != nil {
				t.Fatalf("challenge: %v", err)
			}
			clientData, authData, signature, err := auth.Assert(challenge)
			if err != nil {
				t.Fatalf("assert: %v", err)
			}
			counter, err := webauthn.VerifyAssertion(testRPID, testOrigins, challenge, credential, clientData, authData, signature)
			if err != nil {
				t.Fatalf("verify assertion: %v", err)
			}
			if counter != auth.SignCount {
				t.Fatalf("counter %d, want %d", counter, auth.SignCount)
			}
			// A different origin from the allowlist is fine.
			other, err := webauthntest.New(alg, testRPID, "https://rd.example.com:8443")
			if err != nil {
				t.Fatalf("new authenticator: %v", err)
			}
			clientData, attestation, err := other.MakeCredential(challenge)
			if err != nil {
				t.Fatalf("make credential: %v", err)
			}
			if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, clientData, attestation); err != nil {
				t.Fatalf("subdomain origin rejected: %v", err)
			}
		})
	}
}

func TestPackedSelfAttestation(t *testing.T) {
	auth, err := webauthntest.New(webauthn.AlgES256, testRPID, testOrigin)
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	challenge, _ := webauthn.NewChallenge()
	clientData, attestation, err := auth.MakeCredentialWithAttestation(challenge, true)
	if err != nil {
		t.Fatalf("make packed credential: %v", err)
	}
	credential, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, clientData, attestation)
	if err != nil {
		t.Fatalf("verify packed registration: %v", err)
	}
	if credential.Alg != webauthn.AlgES256 {
		t.Fatalf("alg %d", credential.Alg)
	}
}

func TestPackedCertificateAttestation(t *testing.T) {
	auth, err := webauthntest.New(webauthn.AlgES256, testRPID, testOrigin)
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	certKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("cert key: %v", err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "chast test attestation"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &certKey.PublicKey, certKey)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	challenge, _ := webauthn.NewChallenge()
	clientData, attestation, err := auth.MakeCredentialWithCertificate(challenge, der, certKey)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, clientData, attestation); err != nil {
		t.Fatalf("verify certificate attestation: %v", err)
	}
}

func TestRegistrationRejections(t *testing.T) {
	auth, err := webauthntest.New(webauthn.AlgES256, testRPID, testOrigin)
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	challenge, _ := webauthn.NewChallenge()
	other, _ := webauthn.NewChallenge()
	clientData, attestation, err := auth.MakeCredential(challenge)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	cases := []struct {
		name         string
		challenge    []byte
		clientData   []byte
		attestation  []byte
		wantFragment string
	}{
		{"wrong challenge", other, clientData, attestation, "challenge mismatch"},
		{"malformed attestation", challenge, clientData, []byte{0xff}, "attestation object"},
	}
	for _, tc := range cases {
		if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, tc.challenge, tc.clientData, tc.attestation); err == nil {
			t.Fatalf("%s: registration accepted", tc.name)
		} else if !strings.Contains(err.Error(), tc.wantFragment) {
			t.Fatalf("%s: error %v, want fragment %q", tc.name, err, tc.wantFragment)
		}
	}

	cross, _ := webauthntest.ClientData("webauthn.create", challenge, testOrigin, true)
	if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, cross, attestation); err == nil ||
		!strings.Contains(err.Error(), "cross-origin") {
		t.Fatalf("cross-origin registration: %v", err)
	}

	foreign, _ := webauthntest.ClientData("webauthn.create", challenge, "https://evil.example", false)
	if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, foreign, attestation); err == nil ||
		!strings.Contains(err.Error(), "origin") {
		t.Fatalf("foreign origin registration: %v", err)
	}

	noPresenceClientData, noPresenceAttestation, err := auth.MakeCredentialWithFlags(challenge, 0x40, false)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, noPresenceClientData, noPresenceAttestation); err == nil ||
		!strings.Contains(err.Error(), "user presence") {
		t.Fatalf("registration without user presence: %v", err)
	}

	// A credential registered for another relying party must not verify.
	foreignRP, err := webauthntest.New(webauthn.AlgES256, "other.example", testOrigin)
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	foreignClientData, foreignAttestation, err := foreignRP.MakeCredential(challenge)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	if _, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, foreignClientData, foreignAttestation); err == nil ||
		!strings.Contains(err.Error(), "rp id hash") {
		t.Fatalf("foreign rp id registration: %v", err)
	}
}

func TestAssertionRejections(t *testing.T) {
	auth, credential := mustRegister(t, webauthn.AlgES256)
	challenge, _ := webauthn.NewChallenge()
	clientData, authData, signature, err := auth.Assert(challenge)
	if err != nil {
		t.Fatalf("assert: %v", err)
	}
	verify := func(cd, data, sig []byte) error {
		_, err := webauthn.VerifyAssertion(testRPID, testOrigins, challenge, credential, cd, data, sig)
		return err
	}
	if err := verify(clientData, authData, webauthntest.TamperSignature(signature)); err == nil ||
		!strings.Contains(err.Error(), "signature") {
		t.Fatalf("tampered signature: %v", err)
	}
	if err := verify(clientData, authData, signature); err != nil {
		t.Fatalf("fresh assertion replay check setup: %v", err)
	}

	secondChallenge, _ := webauthn.NewChallenge()
	cd, data, sig, err := auth.Assert(secondChallenge)
	if err != nil {
		t.Fatalf("assert: %v", err)
	}
	if _, err := webauthn.VerifyAssertion(testRPID, testOrigins, challenge, credential, cd, data, sig); err == nil ||
		!strings.Contains(err.Error(), "challenge") {
		t.Fatalf("assertion for another challenge: %v", err)
	}

	cd, data, sig, err = auth.Assert(secondChallenge)
	if err != nil {
		t.Fatalf("assert: %v", err)
	}
	data[32] = 0x00 // clear user presence
	if _, err := webauthn.VerifyAssertion(testRPID, testOrigins, secondChallenge, credential, cd, data, sig); err == nil ||
		!strings.Contains(err.Error(), "user presence") {
		t.Fatalf("assertion without user presence: %v", err)
	}

	cd, data, sig, err = auth.Assert(secondChallenge)
	if err != nil {
		t.Fatalf("assert: %v", err)
	}
	data[32] = 0x41 // attested-credential bit on an assertion
	if _, err := webauthn.VerifyAssertion(testRPID, testOrigins, secondChallenge, credential, cd, data, sig); err == nil ||
		!strings.Contains(err.Error(), "attested") {
		t.Fatalf("assertion with attested data: %v", err)
	}

	trailing := append(append([]byte(nil), authData...), 0x00)
	if _, err := webauthn.VerifyAssertion(testRPID, testOrigins, challenge, credential, clientData, trailing, signature); err == nil {
		t.Fatalf("assertion with trailing bytes accepted")
	}
}

func TestAssertionCounterRollback(t *testing.T) {
	auth, err := webauthntest.New(webauthn.AlgES256, testRPID, testOrigin)
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	auth.SignCount = 5
	challenge, _ := webauthn.NewChallenge()
	clientData, attestation, err := auth.MakeCredential(challenge)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	credential, err := webauthn.VerifyRegistration(testRPID, testOrigins, challenge, clientData, attestation)
	if err != nil {
		t.Fatalf("verify registration: %v", err)
	}
	auth.SignCount = 2
	cd, data, sig, err := auth.Assert(challenge)
	if err != nil {
		t.Fatalf("assert: %v", err)
	}
	if _, err := webauthn.VerifyAssertion(testRPID, testOrigins, challenge, credential, cd, data, sig); err == nil ||
		!strings.Contains(err.Error(), "counter") {
		t.Fatalf("counter rollback: %v", err)
	}
}

func TestValidateConfig(t *testing.T) {
	cases := []struct {
		name    string
		rpID    string
		origins []string
		ok      bool
	}{
		{"bare host", "example.com", []string{"https://example.com"}, true},
		{"subdomain origin", "example.com", []string{"https://rd.example.com:8443"}, true},
		{"loopback", "127.0.0.1", []string{"http://127.0.0.1:21114"}, true},
		{"empty rp id", "", []string{"https://example.com"}, false},
		{"scheme in rp id", "https://example.com", []string{"https://example.com"}, false},
		{"origin outside rp id", "example.com", []string{"https://example.net"}, false},
		{"origin without scheme", "example.com", []string{"example.com"}, false},
		{"origin with path", "example.com", []string{"https://example.com/app"}, false},
		{"no origins", "example.com", nil, false},
	}
	for _, tc := range cases {
		err := webauthn.ValidateConfig(tc.rpID, tc.origins)
		if tc.ok && err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		if !tc.ok && err == nil {
			t.Fatalf("%s: accepted", tc.name)
		}
	}
}

func TestNewChallengeIsRandom(t *testing.T) {
	first, err := webauthn.NewChallenge()
	if err != nil {
		t.Fatalf("challenge: %v", err)
	}
	second, _ := webauthn.NewChallenge()
	if len(first) != 32 || bytes.Equal(first, second) {
		t.Fatalf("challenges are not 32 random bytes: %x %x", first, second)
	}
}

func algName(alg int) string {
	switch alg {
	case webauthn.AlgES256:
		return "es256"
	case webauthn.AlgRS256:
		return "rs256"
	case webauthn.AlgEdDSA:
		return "eddsa"
	}
	return "unknown"
}
