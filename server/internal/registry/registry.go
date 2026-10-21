// Package registry keeps the set of registered hosts: their key digest, relay
// port, last observed public address and expiry. Lookups are solved with a TTL
// rather than a session, so a crashed agent never pins a relay port forever.
package registry

import (
	"errors"
	"fmt"
	"regexp"
	"sync"
	"time"
)

var (
	ErrKeyMismatch = errors.New("host key does not match the registered one")
	ErrUnknownHost = errors.New("unknown host")
)

var idPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)

// ValidID keeps IDs readable (they end up in URLs and logs) and bounded.
func ValidID(id string) bool { return idPattern.MatchString(id) }

type Host struct {
	ID          string
	KeyHash     string
	Fingerprint string
	RelayPort   int
	// Candidate is the public UDP endpoint most recently observed by the
	// rendezvous service. It is not trusted for routing on the server; it is
	// only handed to peers that ask for this host.
	Candidate  string
	LastSeen   time.Time
	Registered time.Time
	ExpiresAt  time.Time
}

type RegisterRequest struct {
	ID          string
	KeyHash     string
	Fingerprint string
	TTL         time.Duration
}

type Registry struct {
	mu           sync.Mutex
	hosts        map[string]*Host
	offlineAfter time.Duration
	now          func() time.Time
}

func New(offlineAfter time.Duration) *Registry {
	if offlineAfter <= 0 {
		offlineAfter = 30 * time.Second
	}
	return &Registry{
		hosts:        make(map[string]*Host),
		offlineAfter: offlineAfter,
		now:          time.Now,
	}
}

// SetClock is used by tests to control expiry without sleeping.
func (r *Registry) SetClock(now func() time.Time) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.now = now
}

// Register creates or renews a host. Renewing with a different key digest is
// refused so a stale or hostile agent cannot take over an ID.
func (r *Registry) Register(req RegisterRequest) (Host, error) {
	if !ValidID(req.ID) {
		return Host{}, fmt.Errorf("id %q must be 1..64 characters of [A-Za-z0-9_.-]", req.ID)
	}
	if len(req.KeyHash) != 64 {
		return Host{}, fmt.Errorf("key hash must be 64 hex characters")
	}
	if req.TTL < 5*time.Second {
		req.TTL = 5 * time.Second
	}
	now := r.now()
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[req.ID]
	if ok {
		if host.KeyHash != req.KeyHash {
			return Host{}, ErrKeyMismatch
		}
		if req.Fingerprint != "" {
			host.Fingerprint = req.Fingerprint
		}
		host.Registered = now
		host.ExpiresAt = now.Add(req.TTL)
		return *host, nil
	}
	host = &Host{
		ID:          req.ID,
		KeyHash:     req.KeyHash,
		Fingerprint: req.Fingerprint,
		Registered:  now,
		ExpiresAt:   now.Add(req.TTL),
	}
	r.hosts[req.ID] = host
	return *host, nil
}

func (r *Registry) SetRelayPort(id string, port int) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if host, ok := r.hosts[id]; ok {
		host.RelayPort = port
	}
}

func (r *Registry) Lookup(id string) (Host, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok {
		return Host{}, false
	}
	if r.now().After(host.ExpiresAt) {
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

// Touch records a keepalive from the address the rendezvous service observed.
func (r *Registry) Touch(id, keyHash, candidate string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok {
		return ErrUnknownHost
	}
	if host.KeyHash != keyHash {
		return ErrKeyMismatch
	}
	if r.now().After(host.ExpiresAt) {
		return ErrUnknownHost
	}
	host.LastSeen = r.now()
	host.Candidate = candidate
	return nil
}

// Expire removes hosts whose HTTP lease is over and returns them so the caller
// can release their relay ports.
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

func (r *Registry) Count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.hosts)
}

// VerifyHost is the relay authenticator: it accepts only a live registration
// with the matching key digest and relay port.
func (r *Registry) VerifyHost(id, keyHash string, port int) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || host.KeyHash != keyHash || host.RelayPort != port {
		return false
	}
	return !r.now().After(host.ExpiresAt)
}
