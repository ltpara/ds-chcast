// Package registry keeps host registrations (TTL lease, observed addresses,
// relay assignment) and the persistent ID -> public-key binding. The binding
// outlives leases and server restarts so an expired or restarted host cannot
// have its ID silently taken over, mirroring RustDesk's RegisterPk semantics.
package registry

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

var (
	ErrKeyMismatch = errors.New("host id is bound to another public key")
	ErrUnknownHost = errors.New("unknown host")
	ErrChallenge   = errors.New("challenge is missing, mismatched or expired")
)

const challengeTTL = 30 * time.Second

var idPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)

// ValidID keeps IDs readable (they end up in URLs and logs) and bounded.
func ValidID(id string) bool { return idPattern.MatchString(id) }

type Identity struct {
	ID          string    `json:"id"`
	PublicKey   string    `json:"public_key"`
	Fingerprint string    `json:"fingerprint,omitempty"`
	CreatedAt   time.Time `json:"created_at"`
}

type Host struct {
	ID          string
	PublicKey   string
	Fingerprint string
	RelayPort   int
	// RelayHost is the advertised relay endpoint assigned to this lease. It
	// stays empty when the server has only its startup --advertise address.
	RelayHost string
	// RelayNode is the standalone relay node URL holding this lease. Empty
	// means the local relay manager owns RelayPort.
	RelayNode string
	// Carrier is the shared carrier endpoint advertised by RelayNode.
	Carrier string
	// Candidate is the public UDP endpoint most recently observed by the
	// rendezvous service. It is not trusted for routing on the server; it is
	// only handed to peers that ask for this host.
	Candidate string
	// LocalAddrs and NATType are reported by the host agent to improve direct
	// connections on LANs and to describe the path to the viewer.
	LocalAddrs []string
	NATType    string
	// CarrierToken authorizes the host's slot on the shared TCP/WebSocket
	// carrier. It is regenerated per registration lease, so after a restart it
	// no longer matches on old viewers, similar to RustDesk's relay uuid.
	CarrierToken string
	LastSeen     time.Time
	Registered   time.Time
	ExpiresAt    time.Time
}

type RegisterRequest struct {
	ID          string
	PublicKey   string
	Fingerprint string
	TTL         time.Duration
}

type Registry struct {
	mu           sync.Mutex
	hosts        map[string]*Host
	identities   map[string]Identity
	offlineAfter time.Duration
	now          func() time.Time
	store        *Store
	challenges   map[string]challenge
}

type challenge struct {
	nonce   string
	purpose string
	expires time.Time
}

// New creates a registry without persistence. Tests and embedders that manage
// their own lifecycle use it; the server uses NewPersistent.
func New(offlineAfter time.Duration) *Registry {
	if offlineAfter <= 0 {
		offlineAfter = 30 * time.Second
	}
	return &Registry{
		hosts:        make(map[string]*Host),
		identities:   make(map[string]Identity),
		offlineAfter: offlineAfter,
		now:          time.Now,
		challenges:   make(map[string]challenge),
	}
}

// NewPersistent loads the identity database at path. A missing file starts
// empty; an unreadable or malformed file is an error instead of silently
// discarding identities.
func NewPersistent(offlineAfter time.Duration, path string) (*Registry, error) {
	r := New(offlineAfter)
	if path == "" {
		return r, nil
	}
	store, err := NewStore(path)
	if err != nil {
		return nil, err
	}
	ids, err := store.Load()
	if err != nil {
		return nil, err
	}
	for _, id := range ids {
		r.identities[id.ID] = id
	}
	r.store = store
	return r, nil
}

// SetClock is used by tests to control expiry without sleeping.
func (r *Registry) SetClock(now func() time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.now = now
}

// Register creates or renews a host. The first successful registration binds
// the ID to the public key permanently; later registrations must present the
// same key. The caller verifies the registration signature before calling.
func (r *Registry) Register(req RegisterRequest) (Host, error) {
	if !ValidID(req.ID) {
		return Host{}, fmt.Errorf("id %q must be 1..64 characters of [A-Za-z0-9_.-]", req.ID)
	}
	if _, err := identity.ParsePublicKey(req.PublicKey); err != nil {
		return Host{}, err
	}
	if err := validFingerprint(req.Fingerprint); err != nil {
		return Host{}, err
	}
	if req.TTL < 5*time.Second {
		req.TTL = 5 * time.Second
	}
	now := r.now()
	r.mu.Lock()
	defer r.mu.Unlock()

	record, exists := r.identities[req.ID]
	if exists {
		if record.PublicKey != req.PublicKey {
			return Host{}, ErrKeyMismatch
		}
		if req.Fingerprint != "" && req.Fingerprint != record.Fingerprint {
			updated := record
			updated.Fingerprint = req.Fingerprint
			if err := r.saveIdentityLocked(updated); err != nil {
				return Host{}, err
			}
			r.identities[req.ID] = updated
			record = updated
		}
	} else {
		record = Identity{
			ID:          req.ID,
			PublicKey:   req.PublicKey,
			Fingerprint: req.Fingerprint,
			CreatedAt:   now,
		}
		if err := r.saveIdentityLocked(record); err != nil {
			return Host{}, err
		}
		r.identities[req.ID] = record
	}

	host, ok := r.hosts[req.ID]
	if !ok {
		host = &Host{ID: req.ID, Registered: now}
		r.hosts[req.ID] = host
	}
	if host.CarrierToken == "" || !now.Before(host.ExpiresAt) {
		// A renewal keeps the token so a viewer that is already paired survives
		// it; a lease that lapsed must hand out a fresh one.
		token, err := newCarrierToken()
		if err != nil {
			return Host{}, err
		}
		host.CarrierToken = token
	}
	host.PublicKey = record.PublicKey
	host.Fingerprint = record.Fingerprint
	host.ExpiresAt = now.Add(req.TTL)
	return *host, nil
}

func newCarrierToken() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("generate carrier token: %w", err)
	}
	return hex.EncodeToString(raw[:]), nil
}

func (r *Registry) saveIdentityLocked(record Identity) error {
	if r.store == nil {
		return nil
	}
	ids := make([]Identity, 0, len(r.identities)+1)
	for id, existing := range r.identities {
		if id == record.ID {
			continue
		}
		ids = append(ids, existing)
	}
	ids = append(ids, record)
	return r.store.Save(ids)
}

func (r *Registry) SetRelay(id string, port int, host string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if live, ok := r.hosts[id]; ok {
		live.RelayPort = port
		live.RelayHost = host
		live.RelayNode = ""
		live.Carrier = ""
	}
}

// SetRemoteRelay records a lease on a standalone relay node. The registry does
// not verify the endpoint; the API only calls it after the node answered.
func (r *Registry) SetRemoteRelay(id, node string, port int, host, carrier string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if live, ok := r.hosts[id]; ok {
		live.RelayPort = port
		live.RelayHost = host
		live.RelayNode = node
		live.Carrier = carrier
	}
}

func (r *Registry) Lookup(id string) (Host, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || r.now().After(host.ExpiresAt) {
		return Host{}, false
	}
	return *host, true
}

// Online reports whether a host is registered and has sent a keepalive within
// the offline window. Without a fresh keepalive its observed address is stale,
// so a punch request cannot succeed.
func (r *Registry) Online(host Host) bool {
	now := r.now()
	return now.Before(host.ExpiresAt) && !host.LastSeen.IsZero() &&
		now.Sub(host.LastSeen) <= r.offlineAfter && host.Candidate != ""
}

// Touch records an authenticated keepalive from the address the rendezvous
// service observed.
func (r *Registry) Touch(id, candidate string, localAddrs []string, natType string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.touchLocked(id, candidate, localAddrs, natType)
}

func (r *Registry) touchLocked(id, candidate string, localAddrs []string, natType string) error {
	host, ok := r.hosts[id]
	if !ok {
		return ErrUnknownHost
	}
	if r.now().After(host.ExpiresAt) {
		return ErrUnknownHost
	}
	host.LastSeen = r.now()
	if candidate != "" {
		host.Candidate = candidate
	}
	if localAddrs != nil {
		host.LocalAddrs = normalizeAddrs(localAddrs)
	}
	if natType != "" {
		host.NATType = natType
	}
	return nil
}

// Expire removes hosts whose HTTP lease is over and returns them so the caller
// can release their relay ports. Identity bindings are intentionally kept.
func (r *Registry) Expire() []Host {
	now := r.now()
	r.mu.Lock()
	defer r.mu.Unlock()
	var expired []Host
	for id, host := range r.hosts {
		if now.After(host.ExpiresAt) {
			expired = append(expired, *host)
			delete(r.hosts, id)
		}
	}
	return expired
}

func (r *Registry) Remove(id string) (Host, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok {
		return Host{}, false
	}
	delete(r.hosts, id)
	return *host, true
}

// RemoveIdentity forgets the permanent ID -> public-key binding. This is the
// administrative escape hatch for a lost host key; runtime removal does not
// touch the binding.
func (r *Registry) RemoveIdentity(id string) (bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	record, ok := r.identities[id]
	if !ok {
		return false, nil
	}
	delete(r.identities, id)
	delete(r.hosts, id)
	if r.store != nil {
		ids := make([]Identity, 0, len(r.identities))
		for _, existing := range r.identities {
			ids = append(ids, existing)
		}
		if err := r.store.Save(ids); err != nil {
			r.identities[record.ID] = record
			return false, err
		}
	}
	return true, nil
}

func (r *Registry) Count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.hosts)
}

// Hosts returns a stable, credential-free snapshot of live leases for the
// admin console. Carrier tokens stay inside the registry.
func (r *Registry) Hosts() []Host {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := r.now()
	out := make([]Host, 0, len(r.hosts))
	for _, host := range r.hosts {
		if now.After(host.ExpiresAt) {
			continue
		}
		host.CarrierToken = ""
		out = append(out, *host)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

func (r *Registry) IdentityCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.identities)
}

// Identity returns the permanent public-key binding for id.
func (r *Registry) Identity(id string) (Identity, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	record, ok := r.identities[id]
	return record, ok
}

// PublicKey returns the verified Ed25519 key bound to id.
func (r *Registry) PublicKey(id string) (ed25519.PublicKey, bool) {
	record, ok := r.Identity(id)
	if !ok {
		return nil, false
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil {
		return nil, false
	}
	return pub, true
}

// VerifyHost checks that a relay port is still reserved for a live host.
func (r *Registry) VerifyHost(id string, port int) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || host.RelayPort != port {
		return false
	}
	return !r.now().After(host.ExpiresAt)
}

// HostOnline reports whether a host holds an unexpired registration lease.
func (r *Registry) HostOnline(id string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	return ok && !r.now().After(host.ExpiresAt)
}

// VerifyCarrier checks the shared-carrier token of a live host. The token is
// what lets many hosts share one TCP port while viewers still reach exactly
// the host they looked up.
func (r *Registry) VerifyCarrier(id, token string) bool {
	if token == "" {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || host.CarrierToken == "" || r.now().After(host.ExpiresAt) {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(host.CarrierToken), []byte(token)) == 1
}

// IssueChallenge creates a one-time nonce for a host and purpose. The caller
// sends it to the host and includes it in the signed reply.
func (r *Registry) IssueChallenge(id, purpose string) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || r.now().After(host.ExpiresAt) {
		return "", ErrUnknownHost
	}
	nonce, err := newNonce()
	if err != nil {
		return "", err
	}
	now := r.now()
	key := challengeKey(id, purpose)
	for k, c := range r.challenges {
		if now.After(c.expires) {
			delete(r.challenges, k)
		}
	}
	r.challenges[key] = challenge{nonce: nonce, purpose: purpose, expires: now.Add(challengeTTL)}
	return nonce, nil
}

// VerifyChallenge consumes the nonce and verifies the host signature over the
// caller-built canonical message. On success it returns the live host.
func (r *Registry) VerifyChallenge(id, purpose, nonce, message, signature string) (Host, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	key := challengeKey(id, purpose)
	pending, ok := r.challenges[key]
	if !ok || pending.purpose != purpose || pending.nonce != nonce || r.now().After(pending.expires) {
		return Host{}, ErrChallenge
	}
	delete(r.challenges, key)
	record, ok := r.identities[id]
	if !ok {
		return Host{}, ErrUnknownHost
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil || !identity.Verify(pub, []byte(message), signature) {
		return Host{}, identity.ErrBadSignature
	}
	host, ok := r.hosts[id]
	if !ok || r.now().After(host.ExpiresAt) {
		return Host{}, ErrUnknownHost
	}
	return *host, nil
}

// CheckChallenge is the boolean form used by the relay and carrier
// authenticators, which only need to know whether the proof was accepted.
func (r *Registry) CheckChallenge(id, purpose, nonce, message, signature string) bool {
	_, err := r.VerifyChallenge(id, purpose, nonce, message, signature)
	return err == nil
}

// TouchChallenge verifies and records a keepalive in one step. The observed
// candidate is part of the signed message, so a replay from another address
// cannot move the host's mapping.
func (r *Registry) TouchChallenge(id, nonce, candidate, signature, message string, localAddrs []string, natType string) error {
	_, err := r.VerifyChallenge(id, identity.DomainKeepalive, nonce, message, signature)
	if err != nil {
		return err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.touchLocked(id, candidate, localAddrs, natType)
}

func challengeKey(id, purpose string) string { return id + "\x00" + purpose }

func newNonce() (string, error) {
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(raw), nil
}

func validFingerprint(fp string) error {
	if len(fp) > 128 {
		return fmt.Errorf("fingerprint is longer than 128 characters")
	}
	for _, r := range fp {
		if r < 0x20 || r == 0x7f {
			return fmt.Errorf("fingerprint contains a control character")
		}
	}
	return nil
}

func normalizeAddrs(addrs []string) []string {
	seen := make(map[string]struct{}, len(addrs))
	out := make([]string, 0, len(addrs))
	for _, addr := range addrs {
		addr = strings.TrimSpace(addr)
		if addr == "" || len(addr) > 128 {
			continue
		}
		if _, ok := seen[addr]; ok {
			continue
		}
		seen[addr] = struct{}{}
		out = append(out, addr)
		if len(out) == 16 {
			break
		}
	}
	return out
}

// Store is the on-disk identity database. It only stores permanent bindings,
// not leases or relay assignments; those are rebuilt by host re-registration.
type Store struct {
	path string
	mu   sync.Mutex
}

func NewStore(path string) (*Store, error) {
	if path == "" {
		return nil, errors.New("identity store path is empty")
	}
	return &Store{path: path}, nil
}

type stateFile struct {
	Version    int        `json:"version"`
	Identities []Identity `json:"identities"`
}

func (s *Store) Load() ([]Identity, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	data, err := os.ReadFile(s.path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("read state %s: %w", s.path, err)
	}
	var state stateFile
	if err := json.Unmarshal(data, &state); err != nil {
		return nil, fmt.Errorf("parse state %s: %w", s.path, err)
	}
	if state.Version != 1 {
		return nil, fmt.Errorf("state %s has unsupported version %d", s.path, state.Version)
	}
	seen := make(map[string]struct{}, len(state.Identities))
	for i := range state.Identities {
		id := state.Identities[i]
		if !ValidID(id.ID) {
			return nil, fmt.Errorf("state %s contains invalid id %q", s.path, id.ID)
		}
		if _, err := identity.ParsePublicKey(id.PublicKey); err != nil {
			return nil, fmt.Errorf("state %s: host %s: %w", s.path, id.ID, err)
		}
		if _, ok := seen[id.ID]; ok {
			return nil, fmt.Errorf("state %s contains duplicate id %q", s.path, id.ID)
		}
		seen[id.ID] = struct{}{}
	}
	return state.Identities, nil
}

func (s *Store) Save(ids []Identity) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	sorted := append([]Identity(nil), ids...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].ID < sorted[j].ID })
	data, err := json.MarshalIndent(stateFile{Version: 1, Identities: sorted}, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	dir := filepath.Dir(s.path)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return fmt.Errorf("state directory %s: %w", dir, err)
	}
	tmp, err := os.CreateTemp(dir, ".chast-state-*")
	if err != nil {
		return fmt.Errorf("state %s: %w", s.path, err)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0o600); err != nil {
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
	if err := os.Rename(tmpName, s.path); err != nil {
		return fmt.Errorf("state %s: %w", s.path, err)
	}
	return nil
}
