package webauthn

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"errors"
	"fmt"
	"math/big"
)

// Supported COSE algorithm identifiers (RFC 9053).
const (
	AlgES256 = -7
	AlgEdDSA = -8
	AlgRS256 = -257
)

var (
	// ErrVerification is the sentinel every failed check wraps. The API turns
	// it into a 401 without exposing the detail.
	ErrVerification = errors.New("webauthn verification failed")
	// ErrUnsupportedAlgorithm rejects a credential whose COSE key is not one
	// of the algorithms this server implements.
	ErrUnsupportedAlgorithm = fmt.Errorf("%w: unsupported key algorithm", ErrVerification)
)

// publicKey is a parsed COSE_Key. verify checks a signature over message with
// the algorithm the key was registered with.
type publicKey struct {
	alg    int
	verify func(message, signature []byte) error
}

func parsePublicKey(data []byte) (publicKey, error) {
	value, err := decodeCBOR(data)
	if err != nil {
		return publicKey{}, fmt.Errorf("%w: cose key: %v", ErrVerification, err)
	}
	if value.kind != 5 {
		return publicKey{}, fmt.Errorf("%w: cose key is not a map", ErrVerification)
	}
	kty, ok := intField(value, 1)
	if !ok {
		return publicKey{}, fmt.Errorf("%w: cose key has no kty", ErrVerification)
	}
	alg, ok := intField(value, 3)
	if !ok {
		return publicKey{}, fmt.Errorf("%w: cose key has no alg", ErrVerification)
	}
	switch kty {
	case 2: // EC2
		if alg != AlgES256 {
			return publicKey{}, fmt.Errorf("%w: ES256 shows kty 2 with alg %d", ErrUnsupportedAlgorithm, alg)
		}
		curve, ok := intField(value, -1)
		if !ok || curve != 1 {
			return publicKey{}, fmt.Errorf("%w: only the P-256 curve is supported", ErrUnsupportedAlgorithm)
		}
		x, okX := bytesField(value, -2)
		y, okY := bytesField(value, -3)
		if !okX || !okY || len(x) != 32 || len(y) != 32 {
			return publicKey{}, fmt.Errorf("%w: malformed P-256 coordinates", ErrVerification)
		}
		key := &ecdsa.PublicKey{
			Curve: elliptic.P256(),
			X:     new(big.Int).SetBytes(x),
			Y:     new(big.Int).SetBytes(y),
		}
		if !key.Curve.IsOnCurve(key.X, key.Y) {
			return publicKey{}, fmt.Errorf("%w: cose key is not on the curve", ErrVerification)
		}
		return publicKey{alg: int(alg), verify: func(message, signature []byte) error {
			digest := sha256.Sum256(message)
			if !ecdsa.VerifyASN1(key, digest[:], signature) {
				return fmt.Errorf("%w: bad ES256 signature", ErrVerification)
			}
			return nil
		}}, nil
	case 3: // RSA
		if alg != AlgRS256 {
			return publicKey{}, fmt.Errorf("%w: RSA shows alg %d", ErrUnsupportedAlgorithm, alg)
		}
		modulus, okN := bytesField(value, -1)
		exponent, okE := bytesField(value, -2)
		if !okN || !okE || len(modulus) < 256 || len(exponent) == 0 || len(exponent) > 8 {
			return publicKey{}, fmt.Errorf("%w: malformed RSA key", ErrVerification)
		}
		e := 0
		for _, b := range exponent {
			e = e<<8 | int(b)
		}
		if e < 3 || e%2 == 0 {
			return publicKey{}, fmt.Errorf("%w: bad RSA exponent", ErrVerification)
		}
		key := &rsa.PublicKey{N: new(big.Int).SetBytes(modulus), E: e}
		if bits := key.N.BitLen(); bits < 2048 || bits > 8192 {
			return publicKey{}, fmt.Errorf("%w: unsupported RSA modulus size", ErrVerification)
		}
		return publicKey{alg: int(alg), verify: func(message, signature []byte) error {
			digest := sha256.Sum256(message)
			if err := rsa.VerifyPKCS1v15(key, crypto.SHA256, digest[:], signature); err != nil {
				return fmt.Errorf("%w: bad RS256 signature", ErrVerification)
			}
			return nil
		}}, nil
	case 1: // OKP
		if alg != AlgEdDSA {
			return publicKey{}, fmt.Errorf("%w: OKP shows alg %d", ErrUnsupportedAlgorithm, alg)
		}
		curve, ok := intField(value, -1)
		if !ok || curve != 6 {
			return publicKey{}, fmt.Errorf("%w: only Ed25519 is supported", ErrUnsupportedAlgorithm)
		}
		x, ok := bytesField(value, -2)
		if !ok || len(x) != ed25519.PublicKeySize {
			return publicKey{}, fmt.Errorf("%w: malformed Ed25519 key", ErrVerification)
		}
		key := ed25519.PublicKey(append([]byte(nil), x...))
		return publicKey{alg: int(alg), verify: func(message, signature []byte) error {
			if len(signature) != ed25519.SignatureSize || !ed25519.Verify(key, message, signature) {
				return fmt.Errorf("%w: bad EdDSA signature", ErrVerification)
			}
			return nil
		}}, nil
	}
	return publicKey{}, fmt.Errorf("%w: kty %d", ErrUnsupportedAlgorithm, kty)
}

// verifyPacked checks a packed attestation statement. A self attestation
// (no x5c) signs with the credential key itself; with x5c the leaf certificate
// signs, and the chain is deliberately not validated (registration is gated by
// the account password; vendor trust is out of scope).
func verifyPacked(attStmt cborValue, message, credentialKey []byte) (int, error) {
	algValue, ok := attStmt.findText("alg")
	if !ok {
		return 0, fmt.Errorf("%w: packed statement has no alg", ErrVerification)
	}
	alg, ok := algValue.negativeInt()
	if !ok {
		return 0, fmt.Errorf("%w: packed statement has a malformed alg", ErrVerification)
	}
	signature, ok := attStmt.findText("sig")
	if !ok || signature.kind != 2 || len(signature.b) == 0 {
		return 0, fmt.Errorf("%w: packed statement has no signature", ErrVerification)
	}
	var key publicKey
	if x5c, ok := attStmt.findText("x5c"); ok {
		if x5c.kind != 4 || len(x5c.a) == 0 || x5c.a[0].kind != 2 {
			return 0, fmt.Errorf("%w: packed statement has a malformed x5c", ErrVerification)
		}
		cert, err := x509.ParseCertificate(x5c.a[0].b)
		if err != nil {
			return 0, fmt.Errorf("%w: packed certificate: %v", ErrVerification, err)
		}
		parsed, err := keyFromCertificate(cert)
		if err != nil {
			return 0, err
		}
		key = parsed
	} else {
		parsed, err := parsePublicKey(credentialKey)
		if err != nil {
			return 0, err
		}
		key = parsed
		if int64(key.alg) != alg {
			return 0, fmt.Errorf("%w: packed alg does not match the credential key", ErrVerification)
		}
	}
	if err := key.verify(message, signature.b); err != nil {
		return 0, err
	}
	return key.alg, nil
}

func keyFromCertificate(cert *x509.Certificate) (publicKey, error) {
	switch pub := cert.PublicKey.(type) {
	case *ecdsa.PublicKey:
		if pub.Curve != elliptic.P256() {
			return publicKey{}, fmt.Errorf("%w: packed certificate uses an unsupported curve", ErrUnsupportedAlgorithm)
		}
		return publicKey{alg: AlgES256, verify: func(message, signature []byte) error {
			digest := sha256.Sum256(message)
			if !ecdsa.VerifyASN1(pub, digest[:], signature) {
				return fmt.Errorf("%w: bad packed ES256 signature", ErrVerification)
			}
			return nil
		}}, nil
	case *rsa.PublicKey:
		return publicKey{alg: AlgRS256, verify: func(message, signature []byte) error {
			digest := sha256.Sum256(message)
			if err := rsa.VerifyPKCS1v15(pub, crypto.SHA256, digest[:], signature); err != nil {
				return fmt.Errorf("%w: bad packed RS256 signature", ErrVerification)
			}
			return nil
		}}, nil
	case ed25519.PublicKey:
		return publicKey{alg: AlgEdDSA, verify: func(message, signature []byte) error {
			if len(signature) != ed25519.SignatureSize || !ed25519.Verify(pub, message, signature) {
				return fmt.Errorf("%w: bad packed EdDSA signature", ErrVerification)
			}
			return nil
		}}, nil
	}
	return publicKey{}, fmt.Errorf("%w: unsupported packed certificate key", ErrUnsupportedAlgorithm)
}

func intField(value cborValue, name int64) (int64, bool) {
	item, ok := value.findInt(name)
	if !ok {
		return 0, false
	}
	return item.negativeInt()
}

func bytesField(value cborValue, name int64) ([]byte, bool) {
	item, ok := value.findInt(name)
	if !ok || item.kind != 2 {
		return nil, false
	}
	return item.b, true
}
