// Package acl defines the access ticket issued by the rendezvous server and
// verified by managed hosts. Tickets are signed with a dedicated Ed25519
// issuer key that hosts pin out of band; the deployment host, link and
// management keys never sign tickets.
package acl

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"time"
)

const (
	// FormatVersion is the ticket envelope version. Unknown versions are
	// rejected rather than parsed leniently.
	FormatVersion = 1
	// AlgorithmEd25519 is the only signature algorithm in this version.
	AlgorithmEd25519 = 1
	// MaxLifetime bounds every ticket. Renewal is a new ticket, never a longer
	// validity for the old one.
	MaxLifetime = 60 * time.Second
	// ClockSkew is the most a host or server may disagree on the current time.
	ClockSkew = 5 * time.Second
	// MaxSignedBytes bounds a complete ticket, including the signature.
	MaxSignedBytes = 1024 + ed25519.SignatureSize
)

// DomainPrefix is prepended to the encoded envelope before signing. It keeps a
// ticket signature from validating as any other server-signed message.
var DomainPrefix = []byte("chast-access-ticket-v1\x00")

const (
	maxIssuer      = 128
	maxAudience    = 128
	maxKeyID       = 64
	maxHostID      = 64
	maxFingerprint = 128
	maxSubject     = 128
	maxTicketID    = 64
	maxPayload     = 1024
	headerSize     = 4
	clientKeySize  = ed25519.PublicKeySize
	minPayloadSize = headerSize + 7*3 + clientKeySize + 2*4 + 3*8
)

var (
	ErrMalformed   = errors.New("access ticket is malformed")
	ErrTooLarge    = errors.New("access ticket is too large")
	ErrUnsupported = errors.New("access ticket version or algorithm is unsupported")
	ErrField       = errors.New("access ticket field is invalid")
	ErrUnknownKey  = errors.New("access ticket key is not trusted")
	ErrSignature   = errors.New("access ticket signature was not accepted")
	ErrIssuer      = errors.New("access ticket issuer does not match")
	ErrAudience    = errors.New("access ticket audience does not match")
	ErrNotYetValid = errors.New("access ticket is not yet valid")
	ErrExpired     = errors.New("access ticket has expired")
	ErrLifetime    = errors.New("access ticket lifetime exceeds the limit")
)

// Ticket is the authorization payload. Times are Unix seconds so the encoding
// is exact and a C++ verifier can reproduce it byte for byte.
type Ticket struct {
	Issuer                   string
	Audience                 string
	KeyID                    string
	HostID                   string
	HostPublicKeyFingerprint string
	Subject                  string
	ClientPublicKey          [clientKeySize]byte
	Permissions              uint32
	PolicyVersion            uint32
	IssuedAt                 int64
	NotBefore                int64
	ExpiresAt                int64
	ID                       string
}

// Encode returns the canonical envelope. It rejects structurally invalid
// fields but does not judge the current time; issuance and verification do
// that separately.
func (t Ticket) Encode() ([]byte, error) {
	if err := t.validateFields(); err != nil {
		return nil, err
	}
	out := make([]byte, 0, maxPayload)
	out = append(out, FormatVersion, AlgorithmEd25519, 0, 0)
	out = appendField(out, t.Issuer)
	out = appendField(out, t.Audience)
	out = appendField(out, t.KeyID)
	out = appendField(out, t.HostID)
	out = appendField(out, t.HostPublicKeyFingerprint)
	out = appendField(out, t.Subject)
	out = append(out, t.ClientPublicKey[:]...)
	out = binary.BigEndian.AppendUint32(out, t.Permissions)
	out = binary.BigEndian.AppendUint32(out, t.PolicyVersion)
	out = binary.BigEndian.AppendUint64(out, uint64(t.IssuedAt))
	out = binary.BigEndian.AppendUint64(out, uint64(t.NotBefore))
	out = binary.BigEndian.AppendUint64(out, uint64(t.ExpiresAt))
	out = appendField(out, t.ID)
	if len(out) > maxPayload {
		return nil, ErrTooLarge
	}
	return out, nil
}

// Decode parses one canonical envelope. It rejects unknown versions or
// algorithms, reserved flags, over-long fields and trailing bytes.
func Decode(payload []byte) (Ticket, error) {
	if len(payload) < minPayloadSize || len(payload) > maxPayload {
		return Ticket{}, ErrMalformed
	}
	if payload[0] != FormatVersion || payload[1] != AlgorithmEd25519 {
		return Ticket{}, ErrUnsupported
	}
	if payload[2] != 0 || payload[3] != 0 {
		return Ticket{}, ErrUnsupported
	}
	r := reader{data: payload, off: headerSize}
	var (
		t   Ticket
		err error
	)
	if t.Issuer, err = r.field(maxIssuer); err != nil {
		return Ticket{}, err
	}
	if t.Audience, err = r.field(maxAudience); err != nil {
		return Ticket{}, err
	}
	if t.KeyID, err = r.field(maxKeyID); err != nil {
		return Ticket{}, err
	}
	if t.HostID, err = r.field(maxHostID); err != nil {
		return Ticket{}, err
	}
	if t.HostPublicKeyFingerprint, err = r.field(maxFingerprint); err != nil {
		return Ticket{}, err
	}
	if t.Subject, err = r.field(maxSubject); err != nil {
		return Ticket{}, err
	}
	if t.ClientPublicKey, err = r.clientKey(); err != nil {
		return Ticket{}, err
	}
	if t.Permissions, err = r.u32(); err != nil {
		return Ticket{}, err
	}
	if t.PolicyVersion, err = r.u32(); err != nil {
		return Ticket{}, err
	}
	if t.IssuedAt, err = r.i64(); err != nil {
		return Ticket{}, err
	}
	if t.NotBefore, err = r.i64(); err != nil {
		return Ticket{}, err
	}
	if t.ExpiresAt, err = r.i64(); err != nil {
		return Ticket{}, err
	}
	if t.ID, err = r.field(maxTicketID); err != nil {
		return Ticket{}, err
	}
	if r.off != len(payload) {
		return Ticket{}, ErrMalformed
	}
	if err := t.validateFields(); err != nil {
		return Ticket{}, err
	}
	return t, nil
}

// Issuer signs tickets for one deployment. It is separate from the server
// identity used for host bindings and software notices.
type Issuer struct {
	name string
	kid  string
	priv ed25519.PrivateKey
	pub  ed25519.PublicKey
}

// NewIssuer builds an issuer from a 32-byte Ed25519 seed. An empty name or kid
// is rejected because both are part of the signed payload.
func NewIssuer(name, kid string, seed []byte) (*Issuer, error) {
	name = trim(name)
	kid = trim(kid)
	if name == "" || len(name) > maxIssuer {
		return nil, fmt.Errorf("%w: issuer name", ErrField)
	}
	if kid == "" || len(kid) > maxKeyID {
		return nil, fmt.Errorf("%w: key id", ErrField)
	}
	if len(seed) != ed25519.SeedSize {
		return nil, fmt.Errorf("%w: seed must be %d bytes", ErrField, ed25519.SeedSize)
	}
	priv := ed25519.NewKeyFromSeed(seed)
	return &Issuer{name: name, kid: kid, priv: priv, pub: priv.Public().(ed25519.PublicKey)}, nil
}

// NewIssuerFromSeed decodes a base64url seed, matching how the server stores
// other keys in configuration.
func NewIssuerFromSeed(name, kid, seedText string) (*Issuer, error) {
	seed, err := base64.RawURLEncoding.DecodeString(trim(seedText))
	if err != nil {
		return nil, fmt.Errorf("%w: seed is not base64url", ErrField)
	}
	return NewIssuer(name, kid, seed)
}

// PublicKeyText is the pin value an administrator installs on managed hosts.
func (i *Issuer) PublicKeyText() string {
	return base64.RawURLEncoding.EncodeToString(i.pub)
}

func (i *Issuer) Name() string  { return i.name }
func (i *Issuer) KeyID() string { return i.kid }

// Sign fills the issuer, key and lifetime checks, then returns envelope plus
// signature. The caller sets audience, host binding, subject, permissions and
// the time window.
func (i *Issuer) Sign(t Ticket, now time.Time) ([]byte, error) {
	t.Issuer = i.name
	t.KeyID = i.kid
	if err := t.validateFields(); err != nil {
		return nil, err
	}
	if t.IssuedAt > now.Unix()+int64(ClockSkew/time.Second) ||
		t.NotBefore > now.Unix()+int64(ClockSkew/time.Second) {
		return nil, ErrNotYetValid
	}
	if t.ExpiresAt < now.Unix() {
		return nil, ErrExpired
	}
	payload, err := t.Encode()
	if err != nil {
		return nil, err
	}
	msg := make([]byte, 0, len(DomainPrefix)+len(payload))
	msg = append(msg, DomainPrefix...)
	msg = append(msg, payload...)
	return append(payload, ed25519.Sign(i.priv, msg)...), nil
}

// Verifier holds the pinned issuer public keys for one deployment. A host
// constructs it from administrator-installed pins, never from lookup data.
type Verifier struct {
	issuer   string
	audience string
	keys     map[string]ed25519.PublicKey
}

func NewVerifier(issuer, audience string) *Verifier {
	return &Verifier{
		issuer:   trim(issuer),
		audience: trim(audience),
		keys:     make(map[string]ed25519.PublicKey),
	}
}

// PinKey installs a trusted public key for a key id. Rotating keys stay
// available until the administrator removes the old pin.
func (v *Verifier) PinKey(kid, publicKeyText string) error {
	kid = trim(kid)
	if kid == "" || len(kid) > maxKeyID {
		return fmt.Errorf("%w: key id", ErrField)
	}
	raw, err := base64.RawURLEncoding.DecodeString(trim(publicKeyText))
	if err != nil || len(raw) != ed25519.PublicKeySize {
		return fmt.Errorf("%w: public key must be %d base64url bytes", ErrField, ed25519.PublicKeySize)
	}
	v.keys[kid] = ed25519.PublicKey(raw)
	return nil
}

// Verify checks the signature over the raw envelope, then the deployment
// identity and time window. It never re-encodes the payload for verification.
func (v *Verifier) Verify(signed []byte, now time.Time) (Ticket, error) {
	if len(signed) < minPayloadSize+ed25519.SignatureSize || len(signed) > MaxSignedBytes {
		return Ticket{}, ErrMalformed
	}
	payload := signed[:len(signed)-ed25519.SignatureSize]
	sig := signed[len(signed)-ed25519.SignatureSize:]
	t, err := Decode(payload)
	if err != nil {
		return Ticket{}, err
	}
	pub, ok := v.keys[t.KeyID]
	if !ok {
		return Ticket{}, ErrUnknownKey
	}
	msg := make([]byte, 0, len(DomainPrefix)+len(payload))
	msg = append(msg, DomainPrefix...)
	msg = append(msg, payload...)
	if !ed25519.Verify(pub, msg, sig) {
		return Ticket{}, ErrSignature
	}
	if v.issuer != "" && t.Issuer != v.issuer {
		return Ticket{}, ErrIssuer
	}
	if v.audience != "" && t.Audience != v.audience {
		return Ticket{}, ErrAudience
	}
	if err := t.validateWindow(now.Unix()); err != nil {
		return Ticket{}, err
	}
	return t, nil
}

func (t Ticket) validateFields() error {
	fields := []struct {
		name  string
		value string
		max   int
	}{
		{"issuer", t.Issuer, maxIssuer},
		{"audience", t.Audience, maxAudience},
		{"key id", t.KeyID, maxKeyID},
		{"host id", t.HostID, maxHostID},
		{"host fingerprint", t.HostPublicKeyFingerprint, maxFingerprint},
		{"subject", t.Subject, maxSubject},
		{"ticket id", t.ID, maxTicketID},
	}
	for _, field := range fields {
		if field.value == "" {
			return fmt.Errorf("%w: %s is required", ErrField, field.name)
		}
		if len(field.value) > field.max {
			return fmt.Errorf("%w: %s is longer than %d bytes", ErrField, field.name, field.max)
		}
	}
	zero := true
	for _, b := range t.ClientPublicKey {
		if b != 0 {
			zero = false
			break
		}
	}
	if zero {
		return fmt.Errorf("%w: client public key is empty", ErrField)
	}
	if t.IssuedAt <= 0 || t.NotBefore <= 0 || t.ExpiresAt <= 0 {
		return fmt.Errorf("%w: ticket times are required", ErrField)
	}
	if t.NotBefore < t.IssuedAt {
		return fmt.Errorf("%w: not-before precedes issued-at", ErrField)
	}
	if t.ExpiresAt <= t.NotBefore {
		return fmt.Errorf("%w: expiry must follow not-before", ErrField)
	}
	lifetime := t.ExpiresAt - t.IssuedAt
	if lifetime > int64(MaxLifetime/time.Second) {
		return ErrLifetime
	}
	return nil
}

func (t Ticket) validateWindow(now int64) error {
	skew := int64(ClockSkew / time.Second)
	if now < t.NotBefore-skew {
		return ErrNotYetValid
	}
	if now > t.ExpiresAt+skew {
		return ErrExpired
	}
	return nil
}

func appendField(dst []byte, value string) []byte {
	dst = binary.BigEndian.AppendUint16(dst, uint16(len(value)))
	return append(dst, value...)
}

type reader struct {
	data []byte
	off  int
}

func (r *reader) take(n int) ([]byte, error) {
	if n < 0 || r.off+n > len(r.data) {
		return nil, ErrMalformed
	}
	out := r.data[r.off : r.off+n]
	r.off += n
	return out, nil
}

func (r *reader) field(max int) (string, error) {
	raw, err := r.take(2)
	if err != nil {
		return "", err
	}
	n := int(binary.BigEndian.Uint16(raw))
	if n == 0 || n > max {
		return "", fmt.Errorf("%w: field length %d", ErrField, n)
	}
	value, err := r.take(n)
	if err != nil {
		return "", err
	}
	return string(value), nil
}

func (r *reader) clientKey() ([clientKeySize]byte, error) {
	var key [clientKeySize]byte
	raw, err := r.take(clientKeySize)
	if err != nil {
		return key, err
	}
	copy(key[:], raw)
	return key, nil
}

func (r *reader) u32() (uint32, error) {
	raw, err := r.take(4)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(raw), nil
}

func (r *reader) i64() (int64, error) {
	raw, err := r.take(8)
	if err != nil {
		return 0, err
	}
	value := int64(binary.BigEndian.Uint64(raw))
	if value < 0 {
		return 0, fmt.Errorf("%w: negative time", ErrField)
	}
	return value, nil
}

func trim(value string) string {
	return strings.TrimSpace(value)
}
