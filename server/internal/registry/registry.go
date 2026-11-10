// Package registry keeps host registrations (TTL lease, observed addresses,
// relay assignment) and the persistent ID -> public-key binding. The binding
// outlives leases and server restarts so an expired or restarted host cannot
// have its ID silently taken over, mirroring RustDesk's RegisterPk semantics.
package registry

import (
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

var (
	ErrKeyMismatch = errors.New("host id is bound to another public key")
	ErrUnknownHost = errors.New("unknown host")
	ErrChallenge   = errors.New("challenge is missing, mismatched or expired")
	// ErrMigrationDenied means old_id exists but belongs to another key. The
	// caller has already proven the new id with the presented key, so this is
	// an attempt to move an identity it does not own.
	ErrMigrationDenied = errors.New("old id belongs to another public key")
)

const challengeTTL = 30 * time.Second

var idPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`)

// ValidID keeps IDs readable (they end up in URLs and logs) and bounded.
func ValidID(id string) bool { return idPattern.MatchString(id) }

// Identity is the durable peer record: the permanent key binding plus the
// last metadata the host reported. Alias and Tags are operator-managed labels;
// they are never overwritten by a host registration. RustDesk keeps the same
// information in its SQLite peer table so an offline peer still has a name and
// a version.
type Identity struct {
	ID          string `json:"id"`
	PublicKey   string `json:"public_key"`
	Fingerprint string `json:"fingerprint,omitempty"`
	Hostname    string `json:"hostname,omitempty"`
	Platform    string `json:"platform,omitempty"`
	Version     string `json:"version,omitempty"`
	// MetadataSignature is the host key's signature over ID, Hostname,
	// Platform and Version. It is only kept while those values match the
	// signed report, so a viewer can verify the lookup metadata directly.
	MetadataSignature string    `json:"metadata_signature,omitempty"`
	Alias             string    `json:"alias,omitempty"`
	Tags              []string  `json:"tags,omitempty"`
	CreatedAt         time.Time `json:"created_at"`
	LastSeen          time.Time `json:"last_seen,omitempty"`
	// WakeTargets are the directed broadcast endpoints on the host's LAN as
	// last reported by the agent. They are internal routing hints for wake
	// relays, not public metadata: never marshal them into lookup responses.
	WakeTargets []string `json:"-"`
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
	// CarrierV6 is the IPv6 endpoint of the same shared carrier when it has a
	// second listener reachable over IPv6.
	CarrierV6 string
	// Candidate is the public UDP endpoint most recently observed by the
	// rendezvous service. It is not trusted for routing on the server; it is
	// only handed to peers that ask for this host. CandidateV6 holds the
	// IPv6 mapping when the host keeps a second, separate mapping.
	Candidate   string
	CandidateV6 string
	// LocalAddrs/LocalAddrsV6 and NATType are reported by the host agent to
	// improve direct connections on LANs and to describe the path to the
	// viewer. The agent splits its candidates by address family.
	LocalAddrs   []string
	LocalAddrsV6 []string
	NATType      string
	// UpnpPort is the external UDP port the host asked its gateway to map to
	// this agent's socket. It is renewed with each keepalive and cleared when
	// the host re-registers without a mapping.
	UpnpPort int
	// CarrierToken authorizes the host's slot on the shared TCP/WebSocket
	// carrier. It is regenerated per registration lease, so after a restart it
	// no longer matches on old viewers, similar to RustDesk's relay uuid.
	CarrierToken string
	// Managed and PolicyVersion mirror the device policy delivered at
	// registration. A managed host refuses viewer traffic without a transport
	// credential, so relay and punch checks read this instead of trusting the
	// caller to say whether the device is managed.
	Managed       bool
	PolicyVersion uint32
	LastSeen      time.Time
	Registered    time.Time
	ExpiresAt     time.Time
}

type RegisterRequest struct {
	ID          string
	PublicKey   string
	Fingerprint string
	// Hostname, Platform and Version are display metadata. Empty values keep
	// whatever a previous registration reported. MetadataSignature is the
	// host's signature over all three; it is dropped when normalization
	// changes the reported strings.
	Hostname          string
	Platform          string
	Version           string
	MetadataSignature string
	TTL               time.Duration
	// OldID is the id this device used before. The caller must have proven
	// ownership of it; Register moves its durable record to ID, preserving the
	// creation time and operator labels, and releases its lease. An unknown
	// old id is accepted so a retry after a successful migration is
	// idempotent.
	OldID string
	// WakeTargets is the set of directed broadcast endpoints the agent can
	// relay wake packets to. A nil slice keeps the previous report; a non-nil
	// slice replaces it, so an empty slice clears stale links when the agent
	// loses its network.
	WakeTargets []string
}

type Registry struct {
	mu           sync.Mutex
	hosts        map[string]*Host
	identities   map[string]Identity
	offlineAfter time.Duration
	now          func() time.Time
	store        *Store
	// challengeSecret signs stateless challenge nonces. Issuing a challenge
	// does not store anything, so an unauthenticated flood cannot evict an
	// outstanding nonce the way a single overwritten slot could.
	challengeSecret [32]byte
	// usedChallenges remembers successful proofs until they expire, keeping
	// nonces one-time without tracking every issued challenge.
	usedChallenges map[string]time.Time
}

// New creates a registry without persistence. Tests and embedders that manage
// their own lifecycle use it; the server uses NewPersistent.
func New(offlineAfter time.Duration) *Registry {
	if offlineAfter <= 0 {
		offlineAfter = 30 * time.Second
	}
	r := &Registry{
		hosts:          make(map[string]*Host),
		identities:     make(map[string]Identity),
		offlineAfter:   offlineAfter,
		now:            time.Now,
		usedChallenges: make(map[string]time.Time),
	}
	if _, err := rand.Read(r.challengeSecret[:]); err != nil {
		panic("registry: cannot generate the challenge secret: " + err.Error())
	}
	return r
}

// NewPersistent loads the peer database at path. A missing file starts empty;
// a pre-SQLite JSON state file at that path (or next to state.db) is imported
// once instead of being silently discarded. An unreadable or malformed file is
// an error.
func NewPersistent(offlineAfter time.Duration, path string) (*Registry, error) {
	r := New(offlineAfter)
	if path == "" {
		return r, nil
	}
	if err := prepareStore(path); err != nil {
		return nil, err
	}
	store, err := NewStore(path)
	if err != nil {
		return nil, err
	}
	return loadPersistent(r, store)
}

// NewPersistentPostgres is NewPersistent backed by PostgreSQL instead of a
// SQLite file. Legacy JSON state is not imported in this mode.
func NewPersistentPostgres(offlineAfter time.Duration, dsn string) (*Registry, error) {
	r := New(offlineAfter)
	store, err := NewStorePostgres(dsn)
	if err != nil {
		return nil, err
	}
	return loadPersistent(r, store)
}

func loadPersistent(r *Registry, store *Store) (*Registry, error) {
	ids, err := store.Load()
	if err != nil {
		store.Close()
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

	if req.OldID != "" {
		if !ValidID(req.OldID) {
			return Host{}, fmt.Errorf("old id %q must be 1..64 characters of [A-Za-z0-9_.-]", req.OldID)
		}
		if req.OldID == req.ID {
			return Host{}, errors.New("old id must differ from the new id")
		}
		old, oldExists := r.identities[req.OldID]
		if oldExists {
			if old.PublicKey != req.PublicKey {
				return Host{}, ErrMigrationDenied
			}
			if existing, exists := r.identities[req.ID]; exists {
				// A retry after the server already moved the record. The new
				// record is authoritative; only the leftover old binding goes.
				if existing.PublicKey != req.PublicKey {
					return Host{}, ErrKeyMismatch
				}
			} else {
				migrated := applyRegistrationMetadata(old, req)
				migrated.ID = req.ID
				migrated.LastSeen = now
				if err := r.storeIdentityLocked(migrated); err != nil {
					return Host{}, err
				}
				if r.store != nil {
					if err := r.store.Delete(req.OldID); err != nil {
						return Host{}, err
					}
				}
			}
			delete(r.identities, req.OldID)
			// The old lease carries addresses and relay ownership. The API
			// releases the relay itself; the lease must never answer lookups.
			delete(r.hosts, req.OldID)
		}
	}

	record, exists := r.identities[req.ID]
	if exists {
		if record.PublicKey != req.PublicKey {
			return Host{}, ErrKeyMismatch
		}
		updated := applyRegistrationMetadata(record, req)
		updated.LastSeen = now
		if identityChanged(record, updated) {
			if err := r.storeIdentityLocked(updated); err != nil {
				return Host{}, err
			}
			record = updated
		}
	} else {
		record = Identity{
			ID:                req.ID,
			PublicKey:         req.PublicKey,
			Fingerprint:       req.Fingerprint,
			Hostname:          normalizeMetadata(req.Hostname, maxHostname),
			Platform:          normalizeMetadata(req.Platform, maxPlatform),
			Version:           normalizeMetadata(req.Version, maxVersion),
			MetadataSignature: signedMetadata(req),
			CreatedAt:         now,
			LastSeen:          now,
			WakeTargets:       slices.Clone(req.WakeTargets),
		}
		if err := r.storeIdentityLocked(record); err != nil {
			return Host{}, err
		}
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

// applyRegistrationMetadata overlays the fields a host may update. Alias,
// tags and CreatedAt are operator history and stay untouched.
func applyRegistrationMetadata(record Identity, req RegisterRequest) Identity {
	if req.Fingerprint != "" {
		record.Fingerprint = req.Fingerprint
	}
	if req.Hostname != "" {
		record.Hostname = normalizeMetadata(req.Hostname, maxHostname)
	}
	if req.Platform != "" {
		record.Platform = normalizeMetadata(req.Platform, maxPlatform)
	}
	if req.Version != "" {
		record.Version = normalizeMetadata(req.Version, maxVersion)
	}
	if req.Hostname != "" || req.Platform != "" || req.Version != "" {
		// A later unsigned registration invalidates an older signature rather
		// than leaving one attached to metadata it did not cover.
		record.MetadataSignature = signedMetadata(req)
	}
	if req.WakeTargets != nil {
		record.WakeTargets = slices.Clone(req.WakeTargets)
	}
	return record
}

// signedMetadata keeps a metadata signature only when the reported values are
// exactly the normalized strings that will be stored and served. If
// normalization changed anything the signature would not verify against the
// lookup response, so it is dropped and the metadata is treated as unsigned.
func signedMetadata(req RegisterRequest) string {
	if req.MetadataSignature == "" || req.Hostname == "" || req.Platform == "" || req.Version == "" {
		return ""
	}
	if normalizeMetadata(req.Hostname, maxHostname) != req.Hostname ||
		normalizeMetadata(req.Platform, maxPlatform) != req.Platform ||
		normalizeMetadata(req.Version, maxVersion) != req.Version {
		return ""
	}
	return req.MetadataSignature
}

func newCarrierToken() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("generate carrier token: %w", err)
	}
	return hex.EncodeToString(raw[:]), nil
}

func (r *Registry) storeIdentityLocked(record Identity) error {
	if r.store != nil {
		if err := r.store.Save(record); err != nil {
			return err
		}
	}
	r.identities[record.ID] = record
	return nil
}

func (r *Registry) SetRelay(id string, port int, host string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if live, ok := r.hosts[id]; ok {
		live.RelayPort = port
		live.RelayHost = host
		live.RelayNode = ""
		live.Carrier = ""
		live.CarrierV6 = ""
	}
}

// SetRemoteRelay records a lease on a standalone relay node. The registry does
// not verify the endpoint; the API only calls it after the node answered.
func (r *Registry) SetRemoteRelay(id, node string, port int, host, carrier, carrierV6 string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if live, ok := r.hosts[id]; ok {
		live.RelayPort = port
		live.RelayHost = host
		live.RelayNode = node
		live.Carrier = carrier
		live.CarrierV6 = carrierV6
	}
}

// SetPolicy records the managed marker and policy version delivered on the
// current registration. Relay and punch admission use it to decide whether a
// legacy shared credential is still acceptable for this host.
func (r *Registry) SetPolicy(id string, managed bool, version uint32) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if live, ok := r.hosts[id]; ok {
		live.Managed = managed
		live.PolicyVersion = version
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
		now.Sub(host.LastSeen) <= r.offlineAfter && (host.Candidate != "" || host.CandidateV6 != "")
}

// Touch records an authenticated keepalive from the address the rendezvous
// service observed.
func (r *Registry) Touch(id, candidate string, localAddrs []string, natType string) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.touchLocked(id, candidate, localAddrs, natType, 0, "")
}

func (r *Registry) touchLocked(id, candidate string, localAddrs []string, natType string, upnpPort int, selfV6 string) error {
	host, ok := r.hosts[id]
	if !ok {
		return ErrUnknownHost
	}
	if r.now().After(host.ExpiresAt) {
		return ErrUnknownHost
	}
	host.LastSeen = r.now()
	if record, ok := r.identities[id]; ok {
		record.LastSeen = host.LastSeen
		r.identities[id] = record
	}
	if candidate != "" {
		if isIPv6Text(candidate) {
			host.CandidateV6 = candidate
		} else {
			host.Candidate = candidate
			// The rendezvous port was reached over IPv4, so it can never see the
			// host's IPv6 mapping. Trust the authenticated host's reported
			// endpoint instead, which is what RustDesk's local_addr_v6 does.
			if selfV6 != "" && isIPv6Text(selfV6) {
				host.CandidateV6 = selfV6
			}
		}
	}
	if localAddrs != nil {
		host.LocalAddrs, host.LocalAddrsV6 = splitAddrsByFamily(localAddrs)
	}
	if natType != "" {
		host.NATType = natType
	}
	if upnpPort > 0 && upnpPort <= 65535 {
		host.UpnpPort = upnpPort
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
	if _, ok := r.identities[id]; !ok {
		return false, nil
	}
	if r.store != nil {
		if err := r.store.Delete(id); err != nil {
			return false, err
		}
	}
	delete(r.identities, id)
	delete(r.hosts, id)
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

// SetLabels updates operator-managed display labels without touching the
// public-key binding or host-reported metadata. A nil field is left unchanged;
// an empty alias or tag list clears it.
func (r *Registry) SetLabels(id string, alias *string, tags *[]string) (Identity, bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	record, ok := r.identities[id]
	if !ok {
		return Identity{}, false, nil
	}
	if alias != nil {
		record.Alias = normalizeMetadata(*alias, maxAlias)
	}
	if tags != nil {
		record.Tags = normalizeTags(*tags)
	}
	if err := r.storeIdentityLocked(record); err != nil {
		return Identity{}, true, err
	}
	return record, true, nil
}

// Identities returns every durable peer record sorted by ID, including peers
// that are currently offline. The admin console uses it as the peer list.
func (r *Registry) Identities() []Identity {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]Identity, 0, len(r.identities))
	for _, record := range r.identities {
		out = append(out, record)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Close releases the persistent store.
func (r *Registry) Close() error {
	if r.store == nil {
		return nil
	}
	return r.store.Close()
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
//
// A managed host refuses the token outright: its transport admission must be a
// short-lived transport credential, so assigning a device policy cannot be
// bypassed with the deployment-wide token.
func (r *Registry) VerifyCarrier(id, token string) bool {
	if token == "" {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || host.Managed || host.CarrierToken == "" || r.now().After(host.ExpiresAt) {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(host.CarrierToken), []byte(token)) == 1
}

// IssueChallenge creates a one-time nonce for a host and purpose. The caller
// sends it to the host and includes it in the signed reply.
func (r *Registry) IssueChallenge(id, purpose string) (string, error) {
	return r.issueChallenge(id, purpose, "")
}

// IssueChallengeScoped is IssueChallenge with an extra key component. A
// dual-stack host keeps its IPv4 and IPv6 mappings alive from two sources, and
// a single pending nonce would be replaced by whichever exchange arrived last.
func (r *Registry) IssueChallengeScoped(id, purpose, scope string) (string, error) {
	return r.issueChallenge(id, purpose, scope)
}

func (r *Registry) issueChallenge(id, purpose, scope string) (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	host, ok := r.hosts[id]
	if !ok || r.now().After(host.ExpiresAt) {
		return "", ErrUnknownHost
	}
	now := r.now()
	for nonce, expires := range r.usedChallenges {
		if now.After(expires) {
			delete(r.usedChallenges, nonce)
		}
	}
	return r.mintChallenge(id, purpose, scope, now.Add(challengeTTL))
}

// mintChallenge returns a self-describing nonce bound to the host, purpose and
// scope. It carries its own expiry, a random salt and an HMAC, so verification
// needs no per-host pending slot that an unauthenticated request could replace
// and two issues never collide.
func (r *Registry) mintChallenge(id, purpose, scope string, expires time.Time) (string, error) {
	stamp := strconv.FormatInt(expires.UnixMilli(), 10)
	raw := make([]byte, 8)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	salt := base64.RawURLEncoding.EncodeToString(raw)
	mac := hmac.New(sha256.New, r.challengeSecret[:])
	mac.Write([]byte(id))
	mac.Write([]byte{0})
	mac.Write([]byte(purpose))
	mac.Write([]byte{0})
	mac.Write([]byte(scope))
	mac.Write([]byte{0})
	mac.Write([]byte(stamp))
	mac.Write([]byte{0})
	mac.Write([]byte(salt))
	return stamp + "." + salt + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil)), nil
}

// challengeExpiry validates a nonce minted by mintChallenge and returns its
// expiry. A nonce for another host, purpose or scope fails the HMAC check.
func (r *Registry) challengeExpiry(id, purpose, scope, nonce string) (time.Time, bool) {
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
	mac := hmac.New(sha256.New, r.challengeSecret[:])
	mac.Write([]byte(id))
	mac.Write([]byte{0})
	mac.Write([]byte(purpose))
	mac.Write([]byte{0})
	mac.Write([]byte(scope))
	mac.Write([]byte{0})
	mac.Write([]byte(stamp))
	mac.Write([]byte{0})
	mac.Write([]byte(salt))
	if !hmac.Equal(got, mac.Sum(nil)) {
		return time.Time{}, false
	}
	return time.UnixMilli(millis), true
}

// VerifyChallenge consumes the nonce and verifies the host signature over the
// caller-built canonical message. On success it returns the live host.
func (r *Registry) VerifyChallenge(id, purpose, nonce, message, signature string) (Host, error) {
	return r.verifyChallenge(id, purpose, "", nonce, message, signature)
}

func (r *Registry) verifyChallenge(id, purpose, scope, nonce, message, signature string) (Host, error) {
	expires, ok := r.challengeExpiry(id, purpose, scope, nonce)
	now := r.now()
	if !ok || now.After(expires) {
		return Host{}, ErrChallenge
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, used := r.usedChallenges[nonce]; used {
		return Host{}, ErrChallenge
	}
	record, ok := r.identities[id]
	if !ok {
		return Host{}, ErrUnknownHost
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil || !identity.Verify(pub, []byte(message), signature) {
		return Host{}, identity.ErrBadSignature
	}
	host, ok := r.hosts[id]
	if !ok || now.After(host.ExpiresAt) {
		return Host{}, ErrUnknownHost
	}
	// Recording the nonce only after the proof verifies means a bad signature
	// cannot burn a host's outstanding challenge.
	r.usedChallenges[nonce] = expires
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
// cannot move the host's mapping. selfV6 is the host's own IPv6 endpoint: it
// is only used when the keepalive itself arrived over IPv4, because then the
// rendezvous port can never observe the IPv6 mapping.
func (r *Registry) TouchChallenge(id, nonce, candidate, signature, message string, localAddrs []string, natType string, upnpPort int, selfV6 string) error {
	_, err := r.verifyChallenge(id, identity.DomainKeepalive, challengeScope(candidate),
		nonce, message, signature)
	if err != nil {
		return err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.touchLocked(id, candidate, localAddrs, natType, upnpPort, selfV6)
}

// challengeScope matches the family used by IssueChallengeScoped for a
// keepalive: the observed candidate is the signed address, so its family is
// the same one the challenge was issued for.
func challengeScope(candidate string) string {
	if isIPv6Text(candidate) {
		return "v6"
	}
	return "v4"
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

// isIPv6Text reports whether a host:port candidate is IPv6. Text that cannot
// be parsed stays on the IPv4 side so older agents keep working unchanged.
func isIPv6Text(text string) bool {
	host, _, err := net.SplitHostPort(text)
	if err != nil {
		return false
	}
	ip := net.ParseIP(strings.Trim(host, "[]"))
	return ip != nil && ip.To4() == nil
}

// splitAddrsByFamily keeps IPv4 and IPv6 local candidates apart so a peer can
// be told which family each address belongs to without probing both.
func splitAddrsByFamily(addrs []string) (v4, v6 []string) {
	for _, addr := range normalizeAddrs(addrs) {
		if isIPv6Text(addr) {
			v6 = append(v6, addr)
		} else {
			v4 = append(v4, addr)
		}
	}
	return v4, v6
}

const (
	maxHostname = 128
	maxPlatform = 64
	maxVersion  = 64
	maxAlias    = 128
	maxTag      = 32
	maxTags     = 16
)

func identityChanged(a, b Identity) bool {
	return a.ID != b.ID ||
		a.PublicKey != b.PublicKey ||
		a.Fingerprint != b.Fingerprint ||
		a.Hostname != b.Hostname ||
		a.Platform != b.Platform ||
		a.Version != b.Version ||
		a.MetadataSignature != b.MetadataSignature ||
		a.Alias != b.Alias ||
		!slices.Equal(a.Tags, b.Tags) ||
		!slices.Equal(a.WakeTargets, b.WakeTargets) ||
		!a.CreatedAt.Equal(b.CreatedAt) ||
		!a.LastSeen.Equal(b.LastSeen)
}

// normalizeTags keeps operator labels bounded and duplicate-free. Empty tags
// are dropped rather than rejected so a comma-heavy edit does not fail.
func normalizeTags(tags []string) []string {
	seen := make(map[string]struct{}, len(tags))
	out := make([]string, 0, len(tags))
	for _, tag := range tags {
		tag = normalizeMetadata(tag, maxTag)
		if tag == "" {
			continue
		}
		if _, ok := seen[tag]; ok {
			continue
		}
		seen[tag] = struct{}{}
		out = append(out, tag)
		if len(out) == maxTags {
			break
		}
	}
	return out
}

// normalizeMetadata keeps peer metadata display-safe. A malformed hostname
// must not fail registration, so control characters are dropped and the value
// is truncated instead of rejected.
func normalizeMetadata(value string, limit int) string {
	value = strings.TrimSpace(value)
	value = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, value)
	if runes := []rune(value); len(runes) > limit {
		value = string(runes[:limit])
	}
	return value
}
