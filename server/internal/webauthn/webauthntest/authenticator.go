// Package webauthntest provides a software WebAuthn authenticator for tests
// and loopback verification. It only implements the shapes the server accepts
// (none/packed attestation, ES256/RS256/EdDSA), so it must not be used as a
// production client.
package webauthntest

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math/big"

	"github.com/ltpara/ds-chcast/server/internal/webauthn"
)

// Authenticator is one software security key bound to an rp-id and origin.
type Authenticator struct {
	Alg          int
	RPID         string
	Origin       string
	CredentialID []byte
	SignCount    uint32

	ecdsaKey *ecdsa.PrivateKey
	rsaKey   *rsa.PrivateKey
	edKey    ed25519.PrivateKey
}

// New creates a fresh key pair for alg (-7, -257 or -8).
func New(alg int, rpID, origin string) (*Authenticator, error) {
	auth := &Authenticator{Alg: alg, RPID: rpID, Origin: origin, SignCount: 1}
	auth.CredentialID = make([]byte, 32)
	if _, err := rand.Read(auth.CredentialID); err != nil {
		return nil, err
	}
	switch alg {
	case webauthn.AlgES256:
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			return nil, err
		}
		auth.ecdsaKey = key
	case webauthn.AlgRS256:
		key, err := rsa.GenerateKey(rand.Reader, 2048)
		if err != nil {
			return nil, err
		}
		auth.rsaKey = key
	case webauthn.AlgEdDSA:
		_, key, err := ed25519.GenerateKey(rand.Reader)
		if err != nil {
			return nil, err
		}
		auth.edKey = key
	default:
		return nil, fmt.Errorf("webauthntest: unsupported alg %d", alg)
	}
	return auth, nil
}

// Credential returns what the server should have stored after registration.
func (a *Authenticator) Credential() webauthn.Credential {
	return webauthn.Credential{
		ID:        append([]byte(nil), a.CredentialID...),
		PublicKey: a.coseKey(),
		Alg:       a.Alg,
		SignCount: a.SignCount,
	}
}

func (a *Authenticator) coseKey() []byte {
	switch a.Alg {
	case webauthn.AlgES256:
		size := (elliptic.P256().Params().BitSize + 7) / 8
		x := a.ecdsaKey.X.FillBytes(make([]byte, size))
		y := a.ecdsaKey.Y.FillBytes(make([]byte, size))
		return cborMap(
			cborEntry{cborUint(1), cborUint(2)},
			cborEntry{cborUint(3), cborNeg(-7)},
			cborEntry{cborNeg(-1), cborUint(1)},
			cborEntry{cborNeg(-2), cborBytes(x)},
			cborEntry{cborNeg(-3), cborBytes(y)},
		)
	case webauthn.AlgRS256:
		exponent := big.NewInt(int64(a.rsaKey.E)).Bytes()
		return cborMap(
			cborEntry{cborUint(1), cborUint(3)},
			cborEntry{cborUint(3), cborNeg(-257)},
			cborEntry{cborNeg(-1), cborBytes(a.rsaKey.N.Bytes())},
			cborEntry{cborNeg(-2), cborBytes(exponent)},
		)
	case webauthn.AlgEdDSA:
		public := a.edKey.Public().(ed25519.PublicKey)
		return cborMap(
			cborEntry{cborUint(1), cborUint(1)},
			cborEntry{cborUint(3), cborNeg(-8)},
			cborEntry{cborNeg(-1), cborUint(6)},
			cborEntry{cborNeg(-2), cborBytes(public)},
		)
	}
	return nil
}

// MakeCredential builds a registration response with none attestation.
func (a *Authenticator) MakeCredential(challenge []byte) (clientDataJSON, attestationObject []byte, err error) {
	return a.MakeCredentialWithAttestation(challenge, false)
}

// MakeCredentialWithAttestation optionally uses packed self attestation.
func (a *Authenticator) MakeCredentialWithAttestation(challenge []byte, packed bool) (clientDataJSON, attestationObject []byte, err error) {
	return a.MakeCredentialWithFlags(challenge, 0x41, packed)
}

// MakeCredentialWithFlags lets negative tests drop the user-presence bit or
// the attested-credential bit.
func (a *Authenticator) MakeCredentialWithFlags(challenge []byte, flags byte, packed bool) (clientDataJSON, attestationObject []byte, err error) {
	clientDataJSON, err = clientData("webauthn.create", challenge, a.Origin)
	if err != nil {
		return nil, nil, err
	}
	authData := a.registrationAuthData(flags)
	if !packed {
		attestationObject = cborMap(
			cborEntry{cborText("fmt"), cborText("none")},
			cborEntry{cborText("attStmt"), cborMap()},
			cborEntry{cborText("authData"), cborBytes(authData)},
		)
		return clientDataJSON, attestationObject, nil
	}
	clientHash := sha256.Sum256(clientDataJSON)
	signature, err := a.sign(append(authData, clientHash[:]...))
	if err != nil {
		return nil, nil, err
	}
	attestationObject = cborMap(
		cborEntry{cborText("fmt"), cborText("packed")},
		cborEntry{cborText("attStmt"), cborMap(
			cborEntry{cborText("alg"), cborNeg(int64(a.Alg))},
			cborEntry{cborText("sig"), cborBytes(signature)},
		)},
		cborEntry{cborText("authData"), cborBytes(authData)},
	)
	return clientDataJSON, attestationObject, nil
}

// MakeCredentialWithCertificate builds a packed attestation signed by an
// external ECDSA key, with certificateDER carried in x5c. The server does not
// validate the chain, only the signature.
func (a *Authenticator) MakeCredentialWithCertificate(challenge []byte, certificateDER []byte, key *ecdsa.PrivateKey) (clientDataJSON, attestationObject []byte, err error) {
	clientDataJSON, err = clientData("webauthn.create", challenge, a.Origin)
	if err != nil {
		return nil, nil, err
	}
	authData := a.registrationAuthData(0x41)
	clientHash := sha256.Sum256(clientDataJSON)
	message := append(append([]byte{}, authData...), clientHash[:]...)
	digest := sha256.Sum256(message)
	signature, err := ecdsa.SignASN1(rand.Reader, key, digest[:])
	if err != nil {
		return nil, nil, err
	}
	attestationObject = cborMap(
		cborEntry{cborText("fmt"), cborText("packed")},
		cborEntry{cborText("attStmt"), cborMap(
			cborEntry{cborText("alg"), cborNeg(-7)},
			cborEntry{cborText("sig"), cborBytes(signature)},
			cborEntry{cborText("x5c"), cborArray(cborBytes(certificateDER))},
		)},
		cborEntry{cborText("authData"), cborBytes(authData)},
	)
	return clientDataJSON, attestationObject, nil
}

func (a *Authenticator) registrationAuthData(flags byte) []byte {
	hash := sha256.Sum256([]byte(a.RPID))
	authData := make([]byte, 0, 32+1+4+16+2+len(a.CredentialID)+128)
	authData = append(authData, hash[:]...)
	authData = append(authData, flags)
	count := make([]byte, 4)
	binary.BigEndian.PutUint32(count, a.SignCount)
	authData = append(authData, count...)
	authData = append(authData, make([]byte, 16)...) // AAGUID
	idLength := make([]byte, 2)
	binary.BigEndian.PutUint16(idLength, uint16(len(a.CredentialID)))
	authData = append(authData, idLength...)
	authData = append(authData, a.CredentialID...)
	authData = append(authData, a.coseKey()...)
	return authData
}

// Assert builds an assertion response for challenge.
func (a *Authenticator) Assert(challenge []byte) (clientDataJSON, authenticatorData, signature []byte, err error) {
	return a.AssertWithFlags(challenge, 0x01)
}

// AssertWithFlags lets negative tests clear user presence or set the
// attested-credential bit on an assertion.
func (a *Authenticator) AssertWithFlags(challenge []byte, flags byte) (clientDataJSON, authenticatorData, signature []byte, err error) {
	clientDataJSON, err = clientData("webauthn.get", challenge, a.Origin)
	if err != nil {
		return nil, nil, nil, err
	}
	hash := sha256.Sum256([]byte(a.RPID))
	a.SignCount++
	authenticatorData = make([]byte, 0, 37)
	authenticatorData = append(authenticatorData, hash[:]...)
	authenticatorData = append(authenticatorData, flags)
	count := make([]byte, 4)
	binary.BigEndian.PutUint32(count, a.SignCount)
	authenticatorData = append(authenticatorData, count...)
	clientHash := sha256.Sum256(clientDataJSON)
	signature, err = a.sign(append(authenticatorData, clientHash[:]...))
	if err != nil {
		return nil, nil, nil, err
	}
	return clientDataJSON, authenticatorData, signature, nil
}

func (a *Authenticator) sign(message []byte) ([]byte, error) {
	digest := sha256.Sum256(message)
	switch a.Alg {
	case webauthn.AlgES256:
		return ecdsa.SignASN1(rand.Reader, a.ecdsaKey, digest[:])
	case webauthn.AlgRS256:
		return rsa.SignPKCS1v15(rand.Reader, a.rsaKey, crypto.SHA256, digest[:])
	case webauthn.AlgEdDSA:
		return ed25519.Sign(a.edKey, message), nil
	}
	return nil, fmt.Errorf("webauthntest: unsupported alg %d", a.Alg)
}

// Sign tampering helpers keep negative tests close to the authenticator.

// TamperSignature flips a bit so verification must fail.
func TamperSignature(signature []byte) []byte {
	if len(signature) == 0 {
		return []byte{1}
	}
	out := append([]byte(nil), signature...)
	out[len(out)-1] ^= 0x01
	return out
}

// ClientData builds a clientDataJSON for negative tests, including the
// crossOrigin flag.
func ClientData(kind string, challenge []byte, origin string, crossOrigin bool) ([]byte, error) {
	return json.Marshal(struct {
		Type        string `json:"type"`
		Challenge   string `json:"challenge"`
		Origin      string `json:"origin"`
		CrossOrigin bool   `json:"crossOrigin"`
	}{
		Type:        kind,
		Challenge:   base64.RawURLEncoding.EncodeToString(challenge),
		Origin:      origin,
		CrossOrigin: crossOrigin,
	})
}

func clientData(kind string, challenge []byte, origin string) ([]byte, error) {
	return json.Marshal(struct {
		Type        string `json:"type"`
		Challenge   string `json:"challenge"`
		Origin      string `json:"origin"`
		CrossOrigin bool   `json:"crossOrigin"`
	}{
		Type:      kind,
		Challenge: base64.RawURLEncoding.EncodeToString(challenge),
		Origin:    origin,
	})
}

// Minimal CBOR encoder for the shapes above.

type cborEntry struct{ key, value []byte }

func cborHead(major byte, value uint64) []byte {
	switch {
	case value < 24:
		return []byte{major<<5 | byte(value)}
	case value <= 0xff:
		return []byte{major<<5 | 24, byte(value)}
	case value <= 0xffff:
		return []byte{major<<5 | 25, byte(value >> 8), byte(value)}
	case value <= 0xffffffff:
		return []byte{major<<5 | 26, byte(value >> 24), byte(value >> 16), byte(value >> 8), byte(value)}
	}
	out := []byte{major<<5 | 27}
	for shift := 56; shift >= 0; shift -= 8 {
		out = append(out, byte(value>>shift))
	}
	return out
}

func cborUint(value uint64) []byte { return cborHead(0, value) }
func cborNeg(value int64) []byte   { return cborHead(1, uint64(-1-value)) }
func cborBytes(data []byte) []byte { return append(cborHead(2, uint64(len(data))), data...) }
func cborText(text string) []byte  { return append(cborHead(3, uint64(len(text))), text...) }
func cborArray(items ...[]byte) []byte {
	out := cborHead(4, uint64(len(items)))
	for _, item := range items {
		out = append(out, item...)
	}
	return out
}
func cborMap(entries ...cborEntry) []byte {
	out := cborHead(5, uint64(len(entries)))
	for _, entry := range entries {
		out = append(out, entry.key...)
		out = append(out, entry.value...)
	}
	return out
}
