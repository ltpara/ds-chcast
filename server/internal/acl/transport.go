package acl

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"time"
)

// Transport audiences separate the admission credential a viewer presents to a
// transport node from the application ticket it presents to the host. A
// credential minted for one transport never authorizes another.
const (
	TransportAudiencePunch   = "transport:punch"
	TransportAudienceRelay   = "transport:relay"
	TransportAudienceCarrier = "transport:carrier"
)

const (
	transportFormatVersion    = 1
	transportAlgorithmEd25519 = 1
	maxTransportSignedBytes   = 768 + ed25519.SignatureSize
	minTransportPayloadSize   = headerSize + 5*3 + clientKeySize + 4 + 3*8 + 3
)

// TransportDomainPrefix keeps a transport signature from validating as an
// access ticket or any other server-signed message even when the deployment
// uses one issuer key for both.
var TransportDomainPrefix = []byte("chast-transport-ticket-v1\x00")

// ErrTransportAudience reports a credential minted for a different transport.
var ErrTransportAudience = errors.New("transport ticket audience does not match")

// TransportTicket binds one viewer connection to one transport purpose. It
// carries no application permissions: the host still enforces the access
// ticket, and this credential only decides whether a transport node forwards
// datagrams for the named host.
type TransportTicket struct {
	Issuer          string
	Audience        string
	KeyID           string
	HostID          string
	Subject         string
	ClientPublicKey [clientKeySize]byte
	PolicyVersion   uint32
	IssuedAt        int64
	NotBefore       int64
	ExpiresAt       int64
	ID              string
}

// Encode returns the canonical envelope without the signature.
func (t TransportTicket) Encode() ([]byte, error) {
	if err := t.validateFields(); err != nil {
		return nil, err
	}
	out := make([]byte, 0, 512)
	out = append(out, transportFormatVersion, transportAlgorithmEd25519, 0, 0)
	out = appendField(out, t.Issuer)
	out = appendField(out, t.Audience)
	out = appendField(out, t.KeyID)
	out = appendField(out, t.HostID)
	out = appendField(out, t.Subject)
	out = append(out, t.ClientPublicKey[:]...)
	out = binary.BigEndian.AppendUint32(out, t.PolicyVersion)
	out = binary.BigEndian.AppendUint64(out, uint64(t.IssuedAt))
	out = binary.BigEndian.AppendUint64(out, uint64(t.NotBefore))
	out = binary.BigEndian.AppendUint64(out, uint64(t.ExpiresAt))
	out = appendField(out, t.ID)
	if len(out) > maxTransportSignedBytes-ed25519.SignatureSize {
		return nil, ErrTooLarge
	}
	return out, nil
}

// DecodeTransport parses one canonical transport envelope. Unknown versions,
// reserved flags, over-long fields and trailing bytes are refused.
func DecodeTransport(payload []byte) (TransportTicket, error) {
	if len(payload) < minTransportPayloadSize || len(payload) > maxTransportSignedBytes-ed25519.SignatureSize {
		return TransportTicket{}, ErrMalformed
	}
	if payload[0] != transportFormatVersion || payload[1] != transportAlgorithmEd25519 {
		return TransportTicket{}, ErrUnsupported
	}
	if payload[2] != 0 || payload[3] != 0 {
		return TransportTicket{}, ErrUnsupported
	}
	r := reader{data: payload, off: headerSize}
	var (
		t   TransportTicket
		err error
	)
	if t.Issuer, err = r.field(maxIssuer); err != nil {
		return TransportTicket{}, err
	}
	if t.Audience, err = r.field(maxAudience); err != nil {
		return TransportTicket{}, err
	}
	if t.KeyID, err = r.field(maxKeyID); err != nil {
		return TransportTicket{}, err
	}
	if t.HostID, err = r.field(maxHostID); err != nil {
		return TransportTicket{}, err
	}
	if t.Subject, err = r.field(maxSubject); err != nil {
		return TransportTicket{}, err
	}
	if t.ClientPublicKey, err = r.clientKey(); err != nil {
		return TransportTicket{}, err
	}
	if t.PolicyVersion, err = r.u32(); err != nil {
		return TransportTicket{}, err
	}
	if t.IssuedAt, err = r.i64(); err != nil {
		return TransportTicket{}, err
	}
	if t.NotBefore, err = r.i64(); err != nil {
		return TransportTicket{}, err
	}
	if t.ExpiresAt, err = r.i64(); err != nil {
		return TransportTicket{}, err
	}
	if t.ID, err = r.field(maxTicketID); err != nil {
		return TransportTicket{}, err
	}
	if r.off != len(payload) {
		return TransportTicket{}, ErrMalformed
	}
	if err := t.validateFields(); err != nil {
		return TransportTicket{}, err
	}
	return t, nil
}

// SignTransport fills the issuer identity and returns envelope plus signature.
func (i *Issuer) SignTransport(t TransportTicket, now time.Time) ([]byte, error) {
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
	msg := make([]byte, 0, len(TransportDomainPrefix)+len(payload))
	msg = append(msg, TransportDomainPrefix...)
	msg = append(msg, payload...)
	return append(payload, ed25519.Sign(i.priv, msg)...), nil
}

// TransportVerifier pins the issuer keys a transport node trusts. It is built
// from deployment configuration, never from data supplied by the caller.
type TransportVerifier struct {
	issuer   string
	keys     map[string]ed25519.PublicKey
	audience string
}

// NewTransportVerifier returns a verifier for one transport purpose. An empty
// audience accepts any of the registered transport audiences so a node that
// serves several paths can use one verifier.
func NewTransportVerifier(issuer, audience string) *TransportVerifier {
	return &TransportVerifier{
		issuer:   trim(issuer),
		audience: trim(audience),
		keys:     make(map[string]ed25519.PublicKey),
	}
}

// PinKey installs a trusted issuer public key for one kid.
func (v *TransportVerifier) PinKey(kid, publicKeyText string) error {
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

// Verify checks the raw envelope and signature before any claim is trusted.
func (v *TransportVerifier) Verify(signed []byte, now time.Time) (TransportTicket, error) {
	if len(signed) < minTransportPayloadSize+ed25519.SignatureSize || len(signed) > maxTransportSignedBytes {
		return TransportTicket{}, ErrMalformed
	}
	payload := signed[:len(signed)-ed25519.SignatureSize]
	sig := signed[len(signed)-ed25519.SignatureSize:]
	t, err := DecodeTransport(payload)
	if err != nil {
		return TransportTicket{}, err
	}
	pub, ok := v.keys[t.KeyID]
	if !ok {
		return TransportTicket{}, ErrUnknownKey
	}
	msg := make([]byte, 0, len(TransportDomainPrefix)+len(payload))
	msg = append(msg, TransportDomainPrefix...)
	msg = append(msg, payload...)
	if !ed25519.Verify(pub, msg, sig) {
		return TransportTicket{}, ErrSignature
	}
	if v.issuer != "" && t.Issuer != v.issuer {
		return TransportTicket{}, ErrIssuer
	}
	if v.audience != "" && t.Audience != v.audience {
		return TransportTicket{}, ErrTransportAudience
	}
	if err := t.validateWindow(now.Unix()); err != nil {
		return TransportTicket{}, err
	}
	return t, nil
}

func (t TransportTicket) validateFields() error {
	if t.Audience != TransportAudiencePunch &&
		t.Audience != TransportAudienceRelay &&
		t.Audience != TransportAudienceCarrier {
		return fmt.Errorf("%w: transport audience", ErrField)
	}
	fields := []struct {
		name  string
		value string
		max   int
	}{
		{"issuer", t.Issuer, maxIssuer},
		{"audience", t.Audience, maxAudience},
		{"key id", t.KeyID, maxKeyID},
		{"host id", t.HostID, maxHostID},
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
	if t.ExpiresAt-t.IssuedAt > int64(MaxLifetime/time.Second) {
		return ErrLifetime
	}
	return nil
}

func (t TransportTicket) validateWindow(now int64) error {
	skew := int64(ClockSkew / time.Second)
	if now < t.NotBefore-skew {
		return ErrNotYetValid
	}
	if now > t.ExpiresAt+skew {
		return ErrExpired
	}
	return nil
}
