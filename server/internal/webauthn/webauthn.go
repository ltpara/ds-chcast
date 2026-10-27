// Package webauthn verifies FIDO2/WebAuthn registration and assertion
// responses without any third-party dependency. The server only needs the
// subset used by platform authenticators and security keys: ES256, RS256 and
// EdDSA credentials with none or packed attestation.
package webauthn

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"
)

const (
	// ChallengeTTL bounds how long a begin/finish pair may be apart. The
	// viewer passes TimeoutMS to the authenticator, so the two agree.
	ChallengeTTL = 5 * time.Minute
	// TimeoutMS is the WebAuthn client timeout hint in the options JSON.
	TimeoutMS = 300000

	maxCredentialIDLength = 1023
	// maxAuthDataLength keeps a hostile request from being copied around.
	maxAuthDataLength = 16 * 1024
)

const (
	flagUserPresent  = 0x01
	flagUserVerified = 0x04
	flagAttested     = 0x40
	flagExtension    = 0x80
)

// Credential is a parsed registration result. PublicKey is the COSE_Key bytes
// exactly as the authenticator produced them.
type Credential struct {
	ID        []byte
	PublicKey []byte
	Alg       int
	SignCount uint32
}

// NewChallenge returns 32 random bytes for one registration or login.
func NewChallenge() ([]byte, error) {
	challenge := make([]byte, 32)
	if _, err := rand.Read(challenge); err != nil {
		return nil, fmt.Errorf("webauthn challenge: %w", err)
	}
	return challenge, nil
}

// ValidateConfig checks the relying party id and the accepted origins. The
// rp-id must be a bare host name; every origin's host must equal it or be one
// of its subdomains. Empty origins disable the feature, which the caller is
// expected to reject when an rp-id is set.
func ValidateConfig(rpID string, origins []string) error {
	rpID = strings.TrimSpace(rpID)
	if rpID == "" {
		return errors.New("webauthn: rp id is empty")
	}
	if strings.ContainsAny(rpID, "/:?#@ \t") {
		return fmt.Errorf("webauthn: rp id %q must be a bare host name", rpID)
	}
	if len(origins) == 0 {
		return errors.New("webauthn: at least one origin is required")
	}
	rpLower := strings.ToLower(rpID)
	for _, origin := range origins {
		origin = strings.TrimSpace(origin)
		if _, err := normalizeOrigin(origin); err != nil {
			return err
		}
		parsed, err := url.Parse(origin)
		if err != nil {
			return fmt.Errorf("webauthn: origin %q: %w", origin, err)
		}
		host := strings.ToLower(parsed.Hostname())
		if host != rpLower && !strings.HasSuffix(host, "."+rpLower) {
			return fmt.Errorf("webauthn: origin %q is not under rp id %q", origin, rpID)
		}
	}
	return nil
}

func normalizeOrigin(origin string) (string, error) {
	parsed, err := url.Parse(strings.TrimSpace(origin))
	if err != nil {
		return "", fmt.Errorf("webauthn: origin %q: %w", origin, err)
	}
	scheme := strings.ToLower(parsed.Scheme)
	if scheme != "http" && scheme != "https" {
		return "", fmt.Errorf("webauthn: origin %q must use http or https", origin)
	}
	if parsed.Host == "" || parsed.User != nil || parsed.RawQuery != "" || parsed.Fragment != "" {
		return "", fmt.Errorf("webauthn: origin %q must be scheme://host[:port]", origin)
	}
	if path := parsed.EscapedPath(); path != "" && path != "/" {
		return "", fmt.Errorf("webauthn: origin %q must not carry a path", origin)
	}
	return scheme + "://" + strings.ToLower(parsed.Host), nil
}

func originAllowed(origins []string, origin string) bool {
	normalized, err := normalizeOrigin(origin)
	if err != nil {
		return false
	}
	for _, allowed := range origins {
		value, err := normalizeOrigin(allowed)
		if err == nil && value == normalized {
			return true
		}
	}
	return false
}

type clientData struct {
	Type        string `json:"type"`
	Challenge   string `json:"challenge"`
	Origin      string `json:"origin"`
	CrossOrigin *bool  `json:"crossOrigin"`
}

func parseClientData(raw []byte, wantType string, expectedChallenge []byte, origins []string) error {
	var data clientData
	if err := json.Unmarshal(raw, &data); err != nil {
		return fmt.Errorf("%w: client data json: %v", ErrVerification, err)
	}
	if data.Type != wantType {
		return fmt.Errorf("%w: client data type %q, want %q", ErrVerification, data.Type, wantType)
	}
	wantChallenge := base64.RawURLEncoding.EncodeToString(expectedChallenge)
	if subtle.ConstantTimeCompare([]byte(data.Challenge), []byte(wantChallenge)) != 1 {
		return fmt.Errorf("%w: challenge mismatch", ErrVerification)
	}
	if !originAllowed(origins, data.Origin) {
		return fmt.Errorf("%w: origin %q is not allowed", ErrVerification, data.Origin)
	}
	if data.CrossOrigin != nil && *data.CrossOrigin {
		return fmt.Errorf("%w: cross-origin credentials are not accepted", ErrVerification)
	}
	return nil
}

type authData struct {
	rpIDHash            []byte
	flags               byte
	signCount           uint32
	aaguid              []byte
	credentialID        []byte
	credentialPublicKey []byte
}

func parseAuthData(raw []byte, rpID string, requireAttested bool) (authData, error) {
	var data authData
	if len(raw) < 37 {
		return data, fmt.Errorf("%w: authenticator data is too short", ErrVerification)
	}
	if len(raw) > maxAuthDataLength {
		return data, fmt.Errorf("%w: authenticator data is too large", ErrVerification)
	}
	rpIDHash := sha256.Sum256([]byte(rpID))
	if subtle.ConstantTimeCompare(raw[:32], rpIDHash[:]) != 1 {
		return data, fmt.Errorf("%w: rp id hash does not match", ErrVerification)
	}
	data.rpIDHash = raw[:32]
	data.flags = raw[32]
	data.signCount = binary.BigEndian.Uint32(raw[33:37])
	if data.flags&flagUserPresent == 0 {
		return data, fmt.Errorf("%w: user presence was not asserted", ErrVerification)
	}
	attested := data.flags&flagAttested != 0
	if requireAttested && !attested {
		return data, fmt.Errorf("%w: attested credential data is missing", ErrVerification)
	}
	if !requireAttested && attested {
		return data, fmt.Errorf("%w: assertion carries attested credential data", ErrVerification)
	}
	rest := raw[37:]
	if attested {
		if len(rest) < 18 {
			return data, fmt.Errorf("%w: authenticator data is truncated", ErrVerification)
		}
		data.aaguid = append([]byte(nil), rest[:16]...)
		idLength := int(binary.BigEndian.Uint16(rest[16:18]))
		if idLength == 0 || idLength > maxCredentialIDLength {
			return data, fmt.Errorf("%w: credential id length %d is invalid", ErrVerification, idLength)
		}
		rest = rest[18:]
		if len(rest) < idLength {
			return data, fmt.Errorf("%w: credential id is truncated", ErrVerification)
		}
		data.credentialID = append([]byte(nil), rest[:idLength]...)
		rest = rest[idLength:]
		keyValue, after, err := decodeCBORAt(rest)
		if err != nil {
			return data, fmt.Errorf("%w: credential key: %v", ErrVerification, err)
		}
		keyLength := len(rest) - len(after)
		if keyValue.kind != 5 {
			return data, fmt.Errorf("%w: credential key is not a COSE map", ErrVerification)
		}
		data.credentialPublicKey = append([]byte(nil), rest[:keyLength]...)
		rest = after
	}
	if data.flags&flagExtension != 0 {
		if len(rest) == 0 {
			return data, fmt.Errorf("%w: extension data is missing", ErrVerification)
		}
		if _, err := decodeCBOR(rest); err != nil {
			return data, fmt.Errorf("%w: extension data: %v", ErrVerification, err)
		}
	} else if len(rest) != 0 {
		return data, fmt.Errorf("%w: authenticator data has trailing bytes", ErrVerification)
	}
	return data, nil
}

// VerifyRegistration checks a navigator.credentials.create() response and
// returns the credential to store. Only none and packed attestation are
// accepted; the attestation chain is not validated by design.
func VerifyRegistration(rpID string, origins []string, expectedChallenge []byte, clientDataJSON, attestationObject []byte) (Credential, error) {
	if err := parseClientData(clientDataJSON, "webauthn.create", expectedChallenge, origins); err != nil {
		return Credential{}, err
	}
	attestation, err := decodeCBOR(attestationObject)
	if err != nil {
		return Credential{}, fmt.Errorf("%w: attestation object: %v", ErrVerification, err)
	}
	if attestation.kind != 5 {
		return Credential{}, fmt.Errorf("%w: attestation object is not a map", ErrVerification)
	}
	formatValue, ok := attestation.findText("fmt")
	if !ok || formatValue.kind != 3 {
		return Credential{}, fmt.Errorf("%w: attestation has no fmt", ErrVerification)
	}
	statement, ok := attestation.findText("attStmt")
	if !ok || statement.kind != 5 {
		return Credential{}, fmt.Errorf("%w: attestation has no attStmt", ErrVerification)
	}
	rawAuthData, ok := attestation.findText("authData")
	if !ok || rawAuthData.kind != 2 {
		return Credential{}, fmt.Errorf("%w: attestation has no authData", ErrVerification)
	}
	data, err := parseAuthData(rawAuthData.b, rpID, true)
	if err != nil {
		return Credential{}, err
	}
	key, err := parsePublicKey(data.credentialPublicKey)
	if err != nil {
		return Credential{}, err
	}
	switch string(formatValue.b) {
	case "none":
		// Nothing to verify: the authenticator makes no attestation claim.
	case "packed":
		digest := sha256.Sum256(clientDataJSON)
		message := make([]byte, 0, len(rawAuthData.b)+len(digest))
		message = append(message, rawAuthData.b...)
		message = append(message, digest[:]...)
		if _, err := verifyPacked(statement, message, data.credentialPublicKey); err != nil {
			return Credential{}, err
		}
	default:
		return Credential{}, fmt.Errorf("%w: attestation format %q is not supported", ErrVerification, string(formatValue.b))
	}
	return Credential{
		ID:        data.credentialID,
		PublicKey: data.credentialPublicKey,
		Alg:       key.alg,
		SignCount: data.signCount,
	}, nil
}

// VerifyAssertion checks a navigator.credentials.get() response against the
// stored credential and returns the signature counter to persist.
func VerifyAssertion(rpID string, origins []string, expectedChallenge []byte, credential Credential, clientDataJSON, authenticatorData, signature []byte) (uint32, error) {
	if err := parseClientData(clientDataJSON, "webauthn.get", expectedChallenge, origins); err != nil {
		return 0, err
	}
	data, err := parseAuthData(authenticatorData, rpID, false)
	if err != nil {
		return 0, err
	}
	key, err := parsePublicKey(credential.PublicKey)
	if err != nil {
		return 0, err
	}
	digest := sha256.Sum256(clientDataJSON)
	message := make([]byte, 0, len(authenticatorData)+len(digest))
	message = append(message, authenticatorData...)
	message = append(message, digest[:]...)
	if err := key.verify(message, signature); err != nil {
		return 0, err
	}
	// Signature counters are optional: many platform authenticators always
	// report zero. Only a strictly decreasing nonzero counter is a clone
	// signal worth rejecting.
	if credential.SignCount > 0 && data.signCount > 0 && data.signCount <= credential.SignCount {
		return 0, fmt.Errorf("%w: signature counter did not advance", ErrVerification)
	}
	return data.signCount, nil
}
