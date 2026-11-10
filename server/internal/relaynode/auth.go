package relaynode

import (
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

const challengeTTL = 30 * time.Second

var (
	ErrUnknownLease = errors.New("host has no live relay lease")
	ErrKeyMismatch  = errors.New("host id is already leased with another public key")
)

type lease struct {
	id      string
	pubText string
	pub     ed25519.PublicKey
	token   string
	managed bool
	port    int
	expires time.Time
}

// Authenticator holds the leases provisioned by hbbs. A relay node never talks
// back to hbbs: hbbs hands it the host's public key and carrier token, so the
// node can verify relay and carrier challenges on its own.
type Authenticator struct {
	mu     sync.Mutex
	leases map[string]*lease
	// usedChallenges remembers nonces that completed a handshake so a replay
	// is refused until the minted expiry passes.
	usedChallenges map[string]time.Time
	secret         [32]byte
	now            func() time.Time
}

func NewAuthenticator() *Authenticator {
	a := &Authenticator{
		leases:         make(map[string]*lease),
		usedChallenges: make(map[string]time.Time),
		now:            time.Now,
	}
	if _, err := rand.Read(a.secret[:]); err != nil {
		panic("relaynode: cannot generate the challenge secret: " + err.Error())
	}
	return a
}

func (a *Authenticator) SetClock(now func() time.Time) {
	if now == nil {
		return
	}
	a.mu.Lock()
	a.now = now
	a.mu.Unlock()
}

type LeaseInfo struct {
	ID        string
	PublicKey string
	Port      int
	ExpiresAt time.Time
}

func (a *Authenticator) Lookup(id string) (LeaseInfo, bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	entry := a.liveLocked(id, a.now())
	if entry == nil {
		return LeaseInfo{}, false
	}
	return LeaseInfo{ID: id, PublicKey: entry.pubText, Port: entry.port, ExpiresAt: entry.expires}, true
}

// Put creates or renews a lease for an already allocated port. Reusing a live
// id with a different key is refused; an expired id may be bound to a new key.
// The managed flag is recomputed by hbbs on every renewal, so a device policy
// assignment reaches the node within one host registration TTL.
func (a *Authenticator) Put(id, pubText, token string, managed bool, port int, ttl time.Duration) (LeaseInfo, error) {
	pub, err := identity.ParsePublicKey(pubText)
	if err != nil {
		return LeaseInfo{}, err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	if existing := a.liveLocked(id, now); existing != nil && existing.pubText != pubText {
		return LeaseInfo{}, ErrKeyMismatch
	}
	entry := &lease{
		id:      id,
		pubText: pubText,
		pub:     pub,
		token:   token,
		managed: managed,
		port:    port,
		expires: now.Add(ttl),
	}
	a.leases[id] = entry
	a.sweepLocked(now)
	return LeaseInfo{ID: id, PublicKey: pubText, Port: port, ExpiresAt: entry.expires}, nil
}

func (a *Authenticator) Remove(id string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	_, ok := a.leases[id]
	delete(a.leases, id)
	return ok
}

// Prune forgets expired leases and returns their ids so the caller can release
// the corresponding relay ports and links.
func (a *Authenticator) Prune(now time.Time) []string {
	a.mu.Lock()
	defer a.mu.Unlock()
	var expired []string
	for id, entry := range a.leases {
		if !now.Before(entry.expires) {
			delete(a.leases, id)
			expired = append(expired, id)
		}
	}
	a.sweepLocked(now)
	return expired
}

// sweepLocked drops used-nonce records whose minted lifetime has passed.
func (a *Authenticator) sweepLocked(now time.Time) {
	for nonce, expires := range a.usedChallenges {
		if now.After(expires) {
			delete(a.usedChallenges, nonce)
		}
	}
}

func (a *Authenticator) Count() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	count := 0
	for id := range a.leases {
		if a.liveLocked(id, now) != nil {
			count++
		}
	}
	return count
}

func (a *Authenticator) IDs() []string {
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	ids := make([]string, 0, len(a.leases))
	for id := range a.leases {
		if a.liveLocked(id, now) != nil {
			ids = append(ids, id)
		}
	}
	return ids
}

func (a *Authenticator) VerifyHost(id string, port int) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	entry := a.liveLocked(id, a.now())
	return entry != nil && entry.port == port
}

func (a *Authenticator) HostOnline(id string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.liveLocked(id, a.now()) != nil
}

// VerifyCarrier checks the legacy carrier token. A managed host never accepts
// it: only a short-lived transport credential can admit a viewer to a managed
// device, exactly as the in-process registry enforces.
func (a *Authenticator) VerifyCarrier(id, token string) bool {
	if token == "" {
		return false
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	entry := a.liveLocked(id, a.now())
	if entry == nil || entry.managed || entry.token == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(entry.token), []byte(token)) == 1
}

// IssueChallenge returns a self-describing nonce bound to the host and purpose.
// It carries its own expiry and HMAC, so repeated unauthenticated hello
// messages cannot replace a host's outstanding challenge.
func (a *Authenticator) IssueChallenge(id, purpose string) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	if a.liveLocked(id, now) == nil {
		return "", ErrUnknownLease
	}
	a.sweepLocked(now)
	return a.mintChallenge(id, purpose, now.Add(challengeTTL)), nil
}

func (a *Authenticator) mintChallenge(id, purpose string, expires time.Time) string {
	stamp := strconv.FormatInt(expires.UnixMilli(), 10)
	raw := make([]byte, 8)
	if _, err := rand.Read(raw); err != nil {
		panic("relaynode: cannot generate a challenge salt: " + err.Error())
	}
	salt := base64.RawURLEncoding.EncodeToString(raw)
	mac := hmac.New(sha256.New, a.secret[:])
	mac.Write([]byte(id))
	mac.Write([]byte{0})
	mac.Write([]byte(purpose))
	mac.Write([]byte{0})
	mac.Write([]byte(stamp))
	mac.Write([]byte{0})
	mac.Write([]byte(salt))
	return stamp + "." + salt + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func (a *Authenticator) challengeExpiry(id, purpose, nonce string) (time.Time, bool) {
	parts := strings.Split(nonce, ".")
	if len(parts) != 3 {
		return time.Time{}, false
	}
	stamp, salt, encoded := parts[0], parts[1], parts[2]
	if len(salt) > 32 || len(encoded) > 64 {
		return time.Time{}, false
	}
	millis, err := strconv.ParseInt(stamp, 10, 64)
	if err != nil || millis <= 0 {
		return time.Time{}, false
	}
	if _, err := base64.RawURLEncoding.DecodeString(salt); err != nil {
		return time.Time{}, false
	}
	got, err := base64.RawURLEncoding.DecodeString(encoded)
	if err != nil {
		return time.Time{}, false
	}
	mac := hmac.New(sha256.New, a.secret[:])
	mac.Write([]byte(id))
	mac.Write([]byte{0})
	mac.Write([]byte(purpose))
	mac.Write([]byte{0})
	mac.Write([]byte(stamp))
	mac.Write([]byte{0})
	mac.Write([]byte(salt))
	if !hmac.Equal(got, mac.Sum(nil)) {
		return time.Time{}, false
	}
	return time.UnixMilli(millis), true
}

func (a *Authenticator) CheckChallenge(id, purpose, nonce, message, signature string) bool {
	expires, ok := a.challengeExpiry(id, purpose, nonce)
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	if !ok || now.After(expires) {
		return false
	}
	if _, used := a.usedChallenges[nonce]; used {
		return false
	}
	entry := a.liveLocked(id, now)
	if entry == nil || !identity.Verify(entry.pub, []byte(message), signature) {
		return false
	}
	// Record only after the proof verifies so a bad signature cannot burn the
	// host's challenge.
	a.usedChallenges[nonce] = expires
	return true
}

func (a *Authenticator) liveLocked(id string, now time.Time) *lease {
	entry, ok := a.leases[id]
	if !ok || !now.Before(entry.expires) {
		return nil
	}
	return entry
}
