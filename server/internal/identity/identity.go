// Package identity implements the long-term host and server identities used by
// registration and the authenticated control plane. It mirrors RustDesk's
// RegisterPk/signed-id model with Ed25519 and unambiguous canonical messages.
package identity

import (
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const (
	// DomainRegister covers the HTTP registration proof.
	DomainRegister = "chast-register-v1"
	// DomainMigrate covers moving a durable peer identity from one id to
	// another. The signature is made with the key already bound to the old id,
	// which is what proves the caller owns it.
	DomainMigrate = "chast-id-migrate-v1"
	// DomainKeepalive covers the rendezvous keepalive challenge.
	DomainKeepalive = "chast-keepalive-v1"
	// DomainRelay covers the UDP relay host hello challenge.
	DomainRelay = "chast-relay-v1"
	// DomainCarrierHost covers the TCP/WebSocket relay host link challenge.
	DomainCarrierHost = "chast-carrier-host-v1"
	// DomainHostBinding covers the server-signed id/public-key/fingerprint blob.
	DomainHostBinding = "chast-host-binding-v1"
	// DomainPunch covers the viewer's server-key proof in a punch request.
	DomainPunch = "chast-punch-v1"
	// DomainHostAudit covers a host reporting an inbound session or finished
	// file. The host key that owns the id signs the canonical fields, so a
	// leaked server key alone cannot fabricate host-side records.
	DomainHostAudit = "chast-host-audit-v1"
)

var (
	ErrBadPublicKey = errors.New("public key must be 32 base64-encoded bytes")
	ErrBadSignature = errors.New("signature does not match the message")
	ErrBadBinding   = errors.New("host binding is malformed")
)

// PrivateKeyFromSecret derives a stable host key pair from the host secret.
// The secret never leaves the host; the server only learns the public key.
func PrivateKeyFromSecret(secret string) ed25519.PrivateKey {
	seed := sha256.Sum256([]byte("chast-host-identity-v1\n" + secret))
	return ed25519.NewKeyFromSeed(seed[:])
}

func PublicKeyText(pub ed25519.PublicKey) string {
	return base64.RawURLEncoding.EncodeToString(pub)
}

func ParsePublicKey(text string) (ed25519.PublicKey, error) {
	raw, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(text))
	if err != nil || len(raw) != ed25519.PublicKeySize {
		return nil, ErrBadPublicKey
	}
	return ed25519.PublicKey(raw), nil
}

func Sign(priv ed25519.PrivateKey, msg []byte) string {
	return base64.RawURLEncoding.EncodeToString(ed25519.Sign(priv, msg))
}

func Verify(pub ed25519.PublicKey, msg []byte, sigText string) bool {
	sig, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(sigText))
	if err != nil || len(sig) != ed25519.SignatureSize || len(pub) != ed25519.PublicKeySize {
		return false
	}
	return ed25519.Verify(pub, msg, sig)
}

// fieldsMessage length-prefixes every field so a separator inside an ID or
// fingerprint cannot be used to reuse a signature for a different message.
func fieldsMessage(domain string, fields ...string) []byte {
	var b strings.Builder
	b.WriteString(domain)
	for _, field := range fields {
		b.WriteByte('\n')
		b.WriteString(strconv.Itoa(len(field)))
		b.WriteByte(':')
		b.WriteString(field)
	}
	return []byte(b.String())
}

func RegisterMessage(id, fingerprint string, unix int64) []byte {
	return fieldsMessage(DomainRegister, id, fingerprint, strconv.FormatInt(unix, 10))
}

// MigrateMessage binds the old and new ids to the same timestamp as the
// registration proof. RustDesk compares a device uuid for the same purpose;
// an Ed25519 signature made by the key already bound to oldID is stronger,
// because only the holder of the host key can produce it.
func MigrateMessage(oldID, newID string, unix int64) []byte {
	return fieldsMessage(DomainMigrate, oldID, newID, strconv.FormatInt(unix, 10))
}

func boolText(value bool) string {
	if value {
		return "1"
	}
	return "0"
}

// HostAuditSessionMessage binds one inbound session report. kind separates it
// from file reports so a signature cannot be replayed as the other kind.
func HostAuditSessionMessage(hostID, auditRef string, startedAt, endedAt int64,
	success bool, detail string) []byte {
	return fieldsMessage(DomainHostAudit, "session", hostID, auditRef,
		strconv.FormatInt(startedAt, 10), strconv.FormatInt(endedAt, 10),
		boolText(success), detail)
}

func HostAuditFileMessage(hostID, auditRef, direction, name string, size, occurredAt int64,
	success bool, detail string) []byte {
	return fieldsMessage(DomainHostAudit, "file", hostID, auditRef, direction, name,
		strconv.FormatInt(size, 10), strconv.FormatInt(occurredAt, 10),
		boolText(success), detail)
}

func KeepaliveMessage(id, candidate, nonce string) []byte {
	return fieldsMessage(DomainKeepalive, id, candidate, nonce)
}

func RelayMessage(id string, port int, candidate, nonce string) []byte {
	return fieldsMessage(DomainRelay, id, strconv.Itoa(port), candidate, nonce)
}

func CarrierHostMessage(id, candidate, nonce string) []byte {
	return fieldsMessage(DomainCarrierHost, id, candidate, nonce)
}

func PunchMessage(id, nonce string, unix int64) []byte {
	return fieldsMessage(DomainPunch, id, nonce, strconv.FormatInt(unix, 10))
}

// PunchAuth proves knowledge of the server key without putting the key itself
// on the wire. Punch datagrams are UDP and may be observed, so a MAC over the
// target and timestamp is used instead of a bearer value.
func PunchAuth(serverKey, id, nonce string, unix int64) string {
	mac := hmac.New(sha256.New, []byte(serverKey))
	mac.Write(PunchMessage(id, nonce, unix))
	return hex.EncodeToString(mac.Sum(nil))
}

// CheckPunchAuth verifies a punch proof and bounds its age. An empty server
// key keeps deployments that do not use authentication working unchanged.
func CheckPunchAuth(serverKey, id, nonce, auth string, unix int64, now time.Time, window time.Duration) bool {
	if serverKey == "" {
		return true
	}
	delta := now.Unix() - unix
	if delta < 0 {
		delta = -delta
	}
	if delta > int64(window/time.Second) {
		return false
	}
	expected := PunchAuth(serverKey, id, nonce, unix)
	return subtle.ConstantTimeCompare([]byte(expected), []byte(auth)) == 1
}

// HostBinding is the server-signed proof that a host public key and optional
// certificate fingerprint belong to an ID. It corresponds to RustDesk's
// signed IdPk returned in punch and relay responses.
type HostBinding struct {
	Version     int    `json:"v"`
	ID          string `json:"id"`
	PublicKey   string `json:"pk"`
	Fingerprint string `json:"fp,omitempty"`
	IssuedAt    int64  `json:"iat"`
}

// Server is the rendezvous server identity. Its public key is what clients pin
// to verify host bindings; it must survive restarts to remain meaningful.
type Server struct {
	priv ed25519.PrivateKey
	pub  ed25519.PublicKey
}

func NewServer(priv ed25519.PrivateKey) *Server {
	pub := priv.Public().(ed25519.PublicKey)
	return &Server{priv: priv, pub: pub}
}

func NewServerFromSeed(seed []byte) (*Server, error) {
	if len(seed) != ed25519.SeedSize {
		return nil, fmt.Errorf("server identity seed must be %d bytes", ed25519.SeedSize)
	}
	return NewServer(ed25519.NewKeyFromSeed(seed)), nil
}

func GenerateServer() (*Server, error) {
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	return NewServer(priv), nil
}

// LoadOrCreateServer reads a base64 Ed25519 seed, generating and persisting one
// on first use. The file is created with owner-only permissions.
func LoadOrCreateServer(path string) (*Server, error) {
	if path == "" {
		return GenerateServer()
	}
	data, err := os.ReadFile(path)
	if err == nil {
		seed, decErr := base64.RawURLEncoding.DecodeString(strings.TrimSpace(string(data)))
		if decErr != nil {
			return nil, fmt.Errorf("server identity %s: %w", path, decErr)
		}
		return NewServerFromSeed(seed)
	}
	if !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("server identity %s: %w", path, err)
	}
	seed := make([]byte, ed25519.SeedSize)
	if _, err := rand.Read(seed); err != nil {
		return nil, fmt.Errorf("generate server identity: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil && filepath.Dir(path) != "." {
		return nil, fmt.Errorf("server identity directory: %w", err)
	}
	if err := writeAtomic(path, []byte(base64.RawURLEncoding.EncodeToString(seed)+"\n"), 0o600); err != nil {
		return nil, fmt.Errorf("server identity %s: %w", path, err)
	}
	return NewServerFromSeed(seed)
}

func (s *Server) PublicKeyText() string {
	if s == nil {
		return ""
	}
	return PublicKeyText(s.pub)
}

func (s *Server) SignHostBinding(id string, hostPub ed25519.PublicKey, fingerprint string, now time.Time) (string, error) {
	if !validID(id) {
		return "", fmt.Errorf("invalid host id for binding")
	}
	if len(hostPub) != ed25519.PublicKeySize {
		return "", ErrBadPublicKey
	}
	payload, err := json.Marshal(HostBinding{
		Version:     1,
		ID:          id,
		PublicKey:   PublicKeyText(hostPub),
		Fingerprint: fingerprint,
		IssuedAt:    now.Unix(),
	})
	if err != nil {
		return "", err
	}
	sig := ed25519.Sign(s.priv, fieldsMessage(DomainHostBinding, string(payload)))
	return base64.RawURLEncoding.EncodeToString(payload) + "." +
		base64.RawURLEncoding.EncodeToString(sig), nil
}

func VerifyHostBinding(text, serverPubText string) (HostBinding, error) {
	serverPub, err := ParsePublicKey(serverPubText)
	if err != nil {
		return HostBinding{}, err
	}
	parts := strings.Split(strings.TrimSpace(text), ".")
	if len(parts) != 2 {
		return HostBinding{}, ErrBadBinding
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return HostBinding{}, ErrBadBinding
	}
	sig, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil || !ed25519.Verify(serverPub, fieldsMessage(DomainHostBinding, string(payload)), sig) {
		return HostBinding{}, ErrBadSignature
	}
	var binding HostBinding
	if err := json.Unmarshal(payload, &binding); err != nil {
		return HostBinding{}, ErrBadBinding
	}
	if binding.Version != 1 || binding.ID == "" {
		return HostBinding{}, ErrBadBinding
	}
	if _, err := ParsePublicKey(binding.PublicKey); err != nil {
		return HostBinding{}, ErrBadBinding
	}
	return binding, nil
}

func validID(id string) bool {
	if len(id) == 0 || len(id) > 64 {
		return false
	}
	for _, r := range id {
		switch {
		case r >= 'A' && r <= 'Z':
		case r >= 'a' && r <= 'z':
		case r >= '0' && r <= '9':
		case r == '_' || r == '.' || r == '-':
		default:
			return false
		}
	}
	return true
}

func writeAtomic(path string, data []byte, mode os.FileMode) error {
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, ".chast-identity-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(mode); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}
