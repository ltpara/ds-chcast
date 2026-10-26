package relaynode

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
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
	port    int
	expires time.Time
}

type pendingChallenge struct {
	nonce   string
	purpose string
	expires time.Time
}

// Authenticator holds the leases provisioned by hbbs. A relay node never talks
// back to hbbs: hbbs hands it the host's public key and carrier token, so the
// node can verify relay and carrier challenges on its own.
type Authenticator struct {
	mu         sync.Mutex
	leases     map[string]*lease
	challenges map[string]pendingChallenge
	now        func() time.Time
}

func NewAuthenticator() *Authenticator {
	return &Authenticator{
		leases:     make(map[string]*lease),
		challenges: make(map[string]pendingChallenge),
		now:        time.Now,
	}
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
func (a *Authenticator) Put(id, pubText, token string, port int, ttl time.Duration) (LeaseInfo, error) {
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
		port:    port,
		expires: now.Add(ttl),
	}
	a.leases[id] = entry
	for key, pending := range a.challenges {
		if now.After(pending.expires) {
			delete(a.challenges, key)
		}
	}
	return LeaseInfo{ID: id, PublicKey: pubText, Port: port, ExpiresAt: entry.expires}, nil
}

func (a *Authenticator) Remove(id string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	_, ok := a.leases[id]
	delete(a.leases, id)
	delete(a.challenges, challengeKey(id, identity.DomainRelay))
	delete(a.challenges, challengeKey(id, identity.DomainCarrierHost))
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
			delete(a.challenges, challengeKey(id, identity.DomainRelay))
			delete(a.challenges, challengeKey(id, identity.DomainCarrierHost))
		}
	}
	for key, pending := range a.challenges {
		if now.After(pending.expires) {
			delete(a.challenges, key)
		}
	}
	return expired
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

func (a *Authenticator) VerifyCarrier(id, token string) bool {
	if token == "" {
		return false
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	entry := a.liveLocked(id, a.now())
	if entry == nil || entry.token == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(entry.token), []byte(token)) == 1
}

func (a *Authenticator) IssueChallenge(id, purpose string) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	if a.liveLocked(id, now) == nil {
		return "", ErrUnknownLease
	}
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	nonce := base64.RawURLEncoding.EncodeToString(raw)
	a.challenges[challengeKey(id, purpose)] = pendingChallenge{
		nonce:   nonce,
		purpose: purpose,
		expires: now.Add(challengeTTL),
	}
	return nonce, nil
}

func (a *Authenticator) CheckChallenge(id, purpose, nonce, message, signature string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now()
	key := challengeKey(id, purpose)
	pending, ok := a.challenges[key]
	if !ok || pending.purpose != purpose || pending.nonce != nonce || now.After(pending.expires) {
		return false
	}
	delete(a.challenges, key)
	entry := a.liveLocked(id, now)
	if entry == nil || !identity.Verify(entry.pub, []byte(message), signature) {
		return false
	}
	return true
}

func (a *Authenticator) liveLocked(id string, now time.Time) *lease {
	entry, ok := a.leases[id]
	if !ok || !now.Before(entry.expires) {
		return nil
	}
	return entry
}

func challengeKey(id, purpose string) string { return id + "\x00" + purpose }
