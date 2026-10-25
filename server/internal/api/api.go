// Package api exposes registration and lookup over HTTP. It is deliberately
// small: the rendezvous and relay data planes never block on HTTP, and a
// reverse proxy can add TLS without the Go process knowing about it.
//
// Registration mirrors RustDesk's RegisterPk: a host proves possession of the
// private key for the public key it wants bound to its ID, and the server
// answers with a server-signed binding that viewers can verify. The binding is
// permanent; a lease only controls whether the host is currently reachable.
package api

import (
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/ratelimit"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
)

const (
	maxBody = 4096
	maxSkew = 2 * time.Minute
	apiVer  = "chast-server/1"
)

type Config struct {
	Registry *registry.Registry
	Relay    *relay.Manager
	// Identity signs host bindings. Without it responses omit the binding,
	// which leaves viewers unable to verify which key belongs to an ID.
	Identity *identity.Server
	// Advertised public endpoint of the rendezvous UDP port, already joined.
	PunchAddr string
	// Hostname or IP advertised for relay ports.
	RelayHost string
	ServerKey string
	TTL       time.Duration
	// RelayOnly hides direct candidates from lookups, mirroring RustDesk's
	// ALWAYS_USE_RELAY: viewers can still reach the host through the relay or
	// the shared carrier, but never punch. The admin console flips it at
	// runtime, so lookups read the current value instead of a startup copy.
	RelayOnly *atomic.Bool
	// Blocker throttles registration attempts per IP. Nil disables it.
	Blocker *abuse.Blocker
	// PunchLog backs the admin punch-request listing; nil disables exposure.
	PunchLog *punchlog.Log
	// IPChanges audits short-lived public address changes. Nil disables it.
	IPChanges *ipchange.Tracker
	// RelayAddrs rotates the advertised relay hosts. Nil keeps RelayHost.
	RelayAddrs *relayaddr.List
	// RelayPool provisions leases on standalone relay nodes. Nil keeps every
	// host on the local relay manager.
	RelayPool *relaypool.Pool
	Logger    *slog.Logger
	// Limiter caps HTTP requests per client IP. Nil or disabled means no cap.
	Limiter *ratelimit.Limiter
	// LoginLimiter locks a username+IP out after repeated wrong passwords.
	// Nil or disabled leaves login attempts uncounted beyond Limiter.
	LoginLimiter *authlimit.Limiter
	// ProxyHeader names the header carrying the client address when the API
	// sits behind a trusted proxy (for example X-Forwarded-For). Empty means
	// the TCP peer address is used.
	ProxyHeader string
	// TLS reports whether clients reach this API over HTTPS.
	TLS bool
	// PunchReadBuffer is the kernel-reported SO_RCVBUF of the rendezvous
	// socket. Zero means the system default or an unreadable value.
	PunchReadBuffer int
	// SoftwareVersion and SoftwareURL advertise a client/host update the same
	// way hbbs answers SoftwareUpdate. Both empty disables the notice.
	SoftwareVersion string
	SoftwareURL     string
	// Accounts backs user login and the per-user address book. Nil means the
	// server runs without persistent state, so those routes report 503.
	Accounts *account.Store
}

type server struct {
	cfg     Config
	started time.Time

	mu            sync.Mutex
	requests      map[string]uint64
	durationSum   map[string]float64
	durationCount map[string]uint64
	rateLimited   uint64
	inFlight      atomic.Int64
}

func New(cfg Config) http.Handler {
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.TTL <= 0 {
		cfg.TTL = 60 * time.Second
	}
	s := &server{
		cfg:           cfg,
		started:       time.Now(),
		requests:      make(map[string]uint64),
		durationSum:   make(map[string]float64),
		durationCount: make(map[string]uint64),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/hosts", s.register)
	mux.HandleFunc("GET /v1/hosts", s.lookupBatch)
	mux.HandleFunc("GET /v1/hosts/{id}", s.lookup)
	mux.HandleFunc("DELETE /v1/hosts/{id}", s.unregister)
	mux.HandleFunc("DELETE /v1/identities/{id}", s.resetIdentity)
	mux.HandleFunc("PATCH /v1/identities/{id}", s.updateIdentity)
	mux.HandleFunc("POST /v1/account/login", s.login)
	mux.HandleFunc("POST /v1/account/logout", s.logout)
	mux.HandleFunc("GET /v1/account", s.accountInfo)
	mux.HandleFunc("POST /v1/account/password", s.changeAccountPassword)
	mux.HandleFunc("POST /v1/account/2fa/start", s.startAccountTwoFactor)
	mux.HandleFunc("POST /v1/account/2fa/enable", s.enableAccountTwoFactor)
	mux.HandleFunc("POST /v1/account/2fa/disable", s.disableAccountTwoFactor)
	mux.HandleFunc("POST /v1/account/2fa/recovery", s.generateAccountRecoveryCodes)
	mux.HandleFunc("GET /v1/account/sessions", s.accountSessions)
	mux.HandleFunc("POST /v1/account/sessions/revoke-others", s.revokeOtherAccountSessions)
	mux.HandleFunc("PATCH /v1/account/sessions/{id}", s.renameAccountSession)
	mux.HandleFunc("DELETE /v1/account/sessions/{id}", s.revokeAccountSession)
	mux.HandleFunc("GET /v1/addressbook", s.addressBook)
	mux.HandleFunc("GET /v1/addressbook/grants", s.addressBookGrants)
	mux.HandleFunc("POST /v1/addressbook/grants", s.shareAddressBook)
	mux.HandleFunc("DELETE /v1/addressbook/grants/{username}", s.unshareAddressBook)
	mux.HandleFunc("GET /v1/addressbook/shared", s.sharedAddressBooks)
	mux.HandleFunc("GET /v1/addressbook/shared/{owner}/grants", s.sharedAddressBookGrants)
	mux.HandleFunc("POST /v1/addressbook/shared/{owner}/grants", s.shareSharedAddressBook)
	mux.HandleFunc("DELETE /v1/addressbook/shared/{owner}/grants/{username}", s.unshareSharedAddressBook)
	mux.HandleFunc("PATCH /v1/addressbook/shared/{owner}/peers/{id}", s.updateSharedAddressBookPeer)
	mux.HandleFunc("DELETE /v1/addressbook/shared/{owner}/peers/{id}", s.deleteSharedAddressBookPeer)
	mux.HandleFunc("POST /v1/addressbook/shared/{owner}/peers", s.addSharedAddressBookPeer)
	mux.HandleFunc("PATCH /v1/addressbook/peers/{id}", s.updateAddressBookPeer)
	mux.HandleFunc("DELETE /v1/addressbook/peers/{id}", s.deleteAddressBookPeer)
	mux.HandleFunc("PATCH /v1/addressbook/groups/{id}", s.updateAddressBookGroup)
	mux.HandleFunc("DELETE /v1/addressbook/groups/{id}", s.deleteAddressBookGroup)
	mux.HandleFunc("GET /v1/server", s.serverInfo)
	mux.HandleFunc("GET /healthz", s.health)
	mux.HandleFunc("GET /v1/stats", s.stats)
	mux.HandleFunc("GET /v1/audit", s.auditLog)
	mux.HandleFunc("GET /v1/audit/connections", s.auditConnections)
	mux.HandleFunc("POST /v1/account/connections", s.recordAccountConnection)
	mux.HandleFunc("GET /v1/account/connections", s.accountConnections)
	mux.HandleFunc("GET /v1/audit/file-transfers", s.auditFileTransfers)
	mux.HandleFunc("POST /v1/account/file-transfers", s.recordAccountFileTransfer)
	mux.HandleFunc("GET /v1/account/file-transfers", s.accountFileTransfers)
	mux.HandleFunc("GET /metrics", s.metrics)
	return s.instrument(mux)
}

type registerRequest struct {
	ID          string `json:"id"`
	PublicKey   string `json:"public_key"`
	Fingerprint string `json:"fingerprint"`
	Hostname    string `json:"hostname"`
	Platform    string `json:"platform"`
	Version     string `json:"version"`
	Timestamp   int64  `json:"timestamp"`
	Signature   string `json:"signature"`
}

type registerResponse struct {
	ID              string        `json:"id"`
	Relay           string        `json:"relay"`
	Relays          []string      `json:"relays,omitempty"`
	Punch           string        `json:"punch"`
	ExpiresInSec    int64         `json:"expires_in_sec"`
	PublicKey       string        `json:"public_key,omitempty"`
	ServerPublicKey string        `json:"server_public_key,omitempty"`
	Binding         string        `json:"binding,omitempty"`
	Carrier         string        `json:"carrier,omitempty"`
	CarrierToken    string        `json:"carrier_token,omitempty"`
	Software        *softwareInfo `json:"software,omitempty"`
}

// softwareInfo mirrors RustDesk's SoftwareUpdate reply: the newest version and
// where to download it. Clients compare it with their own build.
type softwareInfo struct {
	Version string `json:"version"`
	URL     string `json:"url"`
}

type lookupResponse struct {
	ID              string   `json:"id"`
	Online          bool     `json:"online"`
	Candidate       string   `json:"candidate,omitempty"`
	Candidates      []string `json:"candidates,omitempty"`
	Relay           string   `json:"relay,omitempty"`
	Punch           string   `json:"punch"`
	Fingerprint     string   `json:"fingerprint,omitempty"`
	PublicKey       string   `json:"public_key,omitempty"`
	ServerPublicKey string   `json:"server_public_key,omitempty"`
	Binding         string   `json:"binding,omitempty"`
	LocalAddrs      []string `json:"local_addrs,omitempty"`
	NATType         string   `json:"nat_type,omitempty"`
	Hostname        string   `json:"hostname,omitempty"`
	Platform        string   `json:"platform,omitempty"`
	Version         string   `json:"version,omitempty"`
	Alias           string   `json:"alias,omitempty"`
	Tags            []string `json:"tags,omitempty"`
	LastSeenMS      int64    `json:"last_seen_ms,omitempty"`
	ExpiresInMS     int64    `json:"expires_in_ms"`
	Carrier         string   `json:"carrier,omitempty"`
	CarrierToken    string   `json:"carrier_token,omitempty"`
	RelayOnly       bool     `json:"relay_only,omitempty"`
	// Transport is the path the server believes is usable: direct when a
	// candidate was reported, carrier when only the shared carrier link proves
	// the host alive, relay otherwise. It is a hint; viewers may still try
	// every candidate.
	Transport string        `json:"transport,omitempty"`
	Software  *softwareInfo `json:"software,omitempty"`
}

type batchLookupResponse struct {
	Hosts    []lookupResponse `json:"hosts"`
	Missing  []string         `json:"missing,omitempty"`
	Software *softwareInfo    `json:"software,omitempty"`
}

func (s *server) register(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	var req registerRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !registry.ValidID(req.ID) {
		writeError(w, http.StatusBadRequest, "id must be 1..64 characters of [A-Za-z0-9_.-]")
		return
	}
	clientAddr := s.clientIP(r)
	if s.cfg.Blocker != nil {
		if ok, reason := s.cfg.Blocker.Allow(clientAddr, req.ID); !ok {
			s.cfg.Logger.Warn("registration blocked", "id", req.ID, "ip", clientAddr, "reason", reason)
			w.Header().Set("Retry-After", "1")
			writeError(w, http.StatusTooManyRequests, "too many registration attempts; retry later")
			return
		}
	}
	pub, err := identity.ParsePublicKey(req.PublicKey)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	now := time.Now()
	signedAt := time.Unix(req.Timestamp, 0)
	if req.Timestamp == 0 || now.Sub(signedAt) > maxSkew || signedAt.Sub(now) > maxSkew {
		writeError(w, http.StatusUnauthorized, "registration timestamp is outside the allowed window")
		return
	}
	msg := identity.RegisterMessage(req.ID, req.Fingerprint, req.Timestamp)
	if !identity.Verify(pub, msg, req.Signature) {
		writeError(w, http.StatusUnauthorized, "registration signature was not accepted")
		return
	}

	host, err := s.cfg.Registry.Register(registry.RegisterRequest{
		ID:          req.ID,
		PublicKey:   req.PublicKey,
		Fingerprint: req.Fingerprint,
		Hostname:    req.Hostname,
		Platform:    req.Platform,
		Version:     req.Version,
		TTL:         s.cfg.TTL,
	})
	if err != nil {
		if errors.Is(err, registry.ErrKeyMismatch) {
			writeError(w, http.StatusForbidden, "host id is already registered with another public key")
			return
		}
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if s.cfg.IPChanges != nil {
		s.cfg.IPChanges.Observe(req.ID, clientAddr)
	}
	relayEndpoint, relays, carrier, err := s.assignRelay(req.ID, req.PublicKey, host)
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, "no relay port is available")
		return
	}
	resp := registerResponse{
		ID:              req.ID,
		Relay:           relayEndpoint,
		Relays:          relays,
		Punch:           s.cfg.PunchAddr,
		ExpiresInSec:    int64(s.cfg.TTL / time.Second),
		PublicKey:       host.PublicKey,
		ServerPublicKey: s.cfg.Identity.PublicKeyText(),
		Carrier:         carrier,
		CarrierToken:    host.CarrierToken,
		Software:        s.software(),
	}
	if binding, err := s.binding(host, now); err == nil {
		resp.Binding = binding
	} else {
		s.cfg.Logger.Error("sign host binding", "id", host.ID, "error", err)
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) lookup(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	id := r.PathValue("id")
	if !registry.ValidID(id) {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	resp, ok := s.hostLookup(id, time.Now(), s.relayOnly())
	if !ok {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	resp.Software = s.software()
	writeJSON(w, http.StatusOK, resp)
}

// lookupBatch answers address-book refreshes with one round trip. Unknown IDs
// are reported separately instead of failing the request, so a viewer can
// render the peers it does know about while flagging stale entries.
func (s *server) lookupBatch(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	ids, err := parseIDs(r.URL.Query().Get("ids"))
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	now := time.Now()
	relayOnly := s.relayOnly()
	resp := batchLookupResponse{
		Hosts:    make([]lookupResponse, 0, len(ids)),
		Software: s.software(),
	}
	for _, id := range ids {
		host, ok := s.hostLookup(id, now, relayOnly)
		if !ok {
			resp.Missing = append(resp.Missing, id)
			continue
		}
		resp.Hosts = append(resp.Hosts, host)
	}
	writeJSON(w, http.StatusOK, resp)
}

const maxBatchIDs = 200

func parseIDs(raw string) ([]string, error) {
	parts := strings.Split(raw, ",")
	ids := make([]string, 0, len(parts))
	seen := make(map[string]struct{}, len(parts))
	for _, part := range parts {
		id := strings.TrimSpace(part)
		if id == "" {
			return nil, errors.New("ids must be a comma-separated list of host ids")
		}
		if !registry.ValidID(id) {
			return nil, fmt.Errorf("id %q must be 1..64 characters of [A-Za-z0-9_.-]", id)
		}
		if _, dup := seen[id]; dup {
			continue
		}
		seen[id] = struct{}{}
		ids = append(ids, id)
		if len(ids) > maxBatchIDs {
			return nil, fmt.Errorf("at most %d ids per request", maxBatchIDs)
		}
	}
	return ids, nil
}

// hostLookup builds the response shared by the single and batch endpoints.
func (s *server) hostLookup(id string, now time.Time, relayOnly bool) (lookupResponse, bool) {
	host, live := s.cfg.Registry.Lookup(id)
	if !live {
		record, ok := s.cfg.Registry.Identity(id)
		if !ok {
			return lookupResponse{}, false
		}
		// An expired lease keeps the ID -> key binding. Peers can still learn
		// the identity (and verify it later) but cannot connect until the host
		// comes back, matching RustDesk's persistent peer records.
		host = registry.Host{ID: record.ID, PublicKey: record.PublicKey, Fingerprint: record.Fingerprint}
	}
	carrierLinked := false
	switch {
	case host.RelayNode != "":
		carrierLinked = s.cfg.RelayPool != nil && s.cfg.RelayPool.HostLinked(host.RelayNode, id)
	case s.cfg.Relay != nil:
		carrierLinked = s.cfg.Relay.HostLinked(id)
	}
	udpLive := live && s.cfg.Registry.Online(host)
	online := udpLive
	if live && carrierLinked {
		// The shared carrier proves the host is up even when UDP keepalives
		// cannot leave its network, matching hbbs's TCP/WS rendezvous fallback.
		online = true
	}
	resp := lookupResponse{
		ID:              host.ID,
		Online:          online,
		Punch:           s.cfg.PunchAddr,
		Fingerprint:     host.Fingerprint,
		PublicKey:       host.PublicKey,
		ServerPublicKey: s.cfg.Identity.PublicKeyText(),
		RelayOnly:       relayOnly,
	}
	record, hasRecord := s.cfg.Registry.Identity(host.ID)
	if hasRecord {
		resp.Hostname = record.Hostname
		resp.Platform = record.Platform
		resp.Version = record.Version
		resp.Alias = record.Alias
		resp.Tags = record.Tags
	}
	if !relayOnly {
		resp.LocalAddrs = host.LocalAddrs
		resp.NATType = host.NATType
	}
	if binding, err := s.binding(host, now); err == nil {
		resp.Binding = binding
	}
	if resp.Online {
		switch {
		case relayOnly:
			// RustDesk forces clients to relay by reporting a symmetric NAT
			// alongside an empty candidate list.
			resp.NATType = "symmetric"
		case !udpLive:
			// Only the carrier link proves liveness, so the last UDP mapping
			// is too old to punch at; hide it and let transport name the
			// carrier. Local addresses go with the mapping.
			resp.LocalAddrs = nil
		default:
			resp.Candidate = host.Candidate
			resp.Candidates = append(resp.Candidates, host.Candidate)
			resp.Candidates = append(resp.Candidates, host.LocalAddrs...)
			resp.Candidates = dedupe(resp.Candidates)
		}
		resp.CarrierToken = host.CarrierToken
		resp.Carrier = s.carrierEndpoint(host)
		resp.Transport = s.transport(host, carrierLinked, len(resp.Candidates) > 0)
	}
	if host.RelayPort != 0 {
		resp.Relay = net.JoinHostPort(s.relayHostFor(host), strconv.Itoa(host.RelayPort))
	}
	if !host.LastSeen.IsZero() {
		resp.LastSeenMS = now.Sub(host.LastSeen).Milliseconds()
	} else if hasRecord && !record.LastSeen.IsZero() {
		// An offline peer still reports when it was last registered, matching
		// hbbs's last_online for peers without a live lease.
		resp.LastSeenMS = now.Sub(record.LastSeen).Milliseconds()
	}
	if live {
		resp.ExpiresInMS = host.ExpiresAt.Sub(now).Milliseconds()
	}
	return resp, true
}

func (s *server) unregister(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	id := r.PathValue("id")
	host, ok := s.cfg.Registry.Remove(id)
	if !ok {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	if host.RelayNode != "" {
		if s.cfg.RelayPool != nil {
			s.cfg.RelayPool.ReleaseAsync(host.RelayNode, id)
		}
	} else {
		s.cfg.Relay.Release(id)
	}
	w.WriteHeader(http.StatusNoContent)
}

// resetIdentity is the administrative escape hatch for a lost host key. It
// removes the permanent binding as well as any live lease.
func (s *server) resetIdentity(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	id := r.PathValue("id")
	host, _ := s.cfg.Registry.Lookup(id)
	removed, err := s.cfg.Registry.RemoveIdentity(id)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "could not persist the identity removal")
		return
	}
	if !removed {
		writeError(w, http.StatusNotFound, "host identity is unknown")
		return
	}
	if host.RelayNode != "" {
		if s.cfg.RelayPool != nil {
			s.cfg.RelayPool.ReleaseAsync(host.RelayNode, id)
		}
	} else {
		s.cfg.Relay.Release(id)
	}
	w.WriteHeader(http.StatusNoContent)
}

type identityLabelsRequest struct {
	Alias *string   `json:"alias"`
	Tags  *[]string `json:"tags"`
}

type identityLabelsResponse struct {
	ID    string   `json:"id"`
	Alias string   `json:"alias,omitempty"`
	Tags  []string `json:"tags,omitempty"`
}

// updateIdentity sets operator-managed display labels. Unlike host-reported
// metadata, these survive host renewals and are never accepted from the host
// registration body.
func (s *server) updateIdentity(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	id := r.PathValue("id")
	if !registry.ValidID(id) {
		writeError(w, http.StatusNotFound, "host identity is unknown")
		return
	}
	var req identityLabelsRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Alias == nil && req.Tags == nil {
		writeError(w, http.StatusBadRequest, "alias or tags is required")
		return
	}
	record, found, err := s.cfg.Registry.SetLabels(id, req.Alias, req.Tags)
	if err != nil {
		writeError(w, http.StatusInternalServerError, "could not persist identity labels")
		return
	}
	if !found {
		writeError(w, http.StatusNotFound, "host identity is unknown")
		return
	}
	writeJSON(w, http.StatusOK, identityLabelsResponse{ID: record.ID, Alias: record.Alias, Tags: record.Tags})
}

func (s *server) serverInfo(w http.ResponseWriter, _ *http.Request) {
	carrier := ""
	if port := s.cfg.Relay.CarrierPort(); port != 0 {
		carrier = net.JoinHostPort(s.cfg.RelayHost, strconv.Itoa(port))
	}
	info := map[string]any{
		"version":               apiVer,
		"public_key":            s.cfg.Identity.PublicKeyText(),
		"punch":                 s.cfg.PunchAddr,
		"carrier":               carrier,
		"tls":                   s.cfg.TLS,
		"host_ttl_sec":          int64(s.cfg.TTL / time.Second),
		"relay_configured":      s.cfg.RelayHost != "",
		"relay_servers":         s.relayServers(),
		"relay_only":            s.relayOnly(),
		"punch_udp_read_buffer": s.cfg.PunchReadBuffer,
		"relay_udp_read_buffer": s.cfg.Relay.ReadBuffer(),
		"accounts":              s.cfg.Accounts != nil,
	}
	if s.cfg.RelayPool != nil {
		info["relay_nodes"] = s.cfg.RelayPool.Status()
	}
	if software := s.software(); software != nil {
		info["software"] = software
	}
	writeJSON(w, http.StatusOK, info)
}

// software returns the advertised update, or nil when the operator did not
// configure one. RustDesk sends SoftwareUpdate only when its URL-derived
// version is non-empty, so an unset server stays silent.
func (s *server) software() *softwareInfo {
	if s.cfg.SoftwareVersion == "" || s.cfg.SoftwareURL == "" {
		return nil
	}
	return &softwareInfo{Version: s.cfg.SoftwareVersion, URL: s.cfg.SoftwareURL}
}

func (s *server) health(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	io.WriteString(w, "ok\n")
}

func (s *server) stats(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	out := map[string]any{
		"hosts":         s.cfg.Registry.Count(),
		"identities":    s.cfg.Registry.IdentityCount(),
		"relay_only":    s.relayOnly(),
		"relay":         s.cfg.Relay.Stats(),
		"relay_servers": s.relayServers(),
	}
	if s.cfg.Blocker != nil {
		out["registration_blocked"] = s.cfg.Blocker.Stats()
	}
	if s.cfg.PunchLog != nil {
		out["punch_requests"] = s.cfg.PunchLog.Stats()
	}
	if s.cfg.IPChanges != nil {
		out["ip_changes"] = s.cfg.IPChanges.Stats()
	}
	if s.cfg.RelayPool != nil {
		out["relay_nodes"] = s.cfg.RelayPool.Status()
	}
	writeJSON(w, http.StatusOK, out)
}

// instrument applies the per-IP limiter and records request counters. The
// endpoint label comes from the mux pattern, so cardinality stays bounded.
func (s *server) instrument(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		s.inFlight.Add(1)
		defer s.inFlight.Add(-1)
		if !s.cfg.Limiter.Allow(s.clientIP(r)) {
			s.mu.Lock()
			s.rateLimited++
			s.mu.Unlock()
			w.Header().Set("Retry-After", "1")
			writeError(w, http.StatusTooManyRequests, "too many requests; retry later")
			s.record(r, http.StatusTooManyRequests, time.Since(start))
			return
		}
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		s.record(r, rec.status, time.Since(start))
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status      int
	wroteHeader bool
}

func (r *statusRecorder) WriteHeader(status int) {
	if r.wroteHeader {
		return
	}
	r.wroteHeader = true
	r.status = status
	r.ResponseWriter.WriteHeader(status)
}

func (r *statusRecorder) Write(p []byte) (int, error) {
	if !r.wroteHeader {
		r.wroteHeader = true
	}
	return r.ResponseWriter.Write(p)
}

func (s *server) record(r *http.Request, status int, took time.Duration) {
	endpoint := r.Pattern
	if endpoint == "" {
		endpoint = "other"
	}
	key := endpoint + "\x00" + strconv.Itoa(status)
	s.mu.Lock()
	s.requests[key]++
	s.durationSum[endpoint] += took.Seconds()
	s.durationCount[endpoint]++
	s.mu.Unlock()
}

func (s *server) relayHostFor(host registry.Host) string {
	if host.RelayHost != "" {
		return host.RelayHost
	}
	return s.cfg.RelayHost
}

func (s *server) relayServers() []string {
	var addrs []string
	if s.cfg.RelayAddrs != nil {
		addrs = s.cfg.RelayAddrs.Addrs()
	} else if s.cfg.RelayHost != "" {
		addrs = []string{s.cfg.RelayHost}
	}
	if addrs == nil {
		addrs = []string{}
	}
	return addrs
}

func (s *server) clientIP(r *http.Request) string {
	if s.cfg.ProxyHeader != "" {
		if raw := r.Header.Get(s.cfg.ProxyHeader); raw != "" {
			parts := strings.Split(raw, ",")
			for i := len(parts) - 1; i >= 0; i-- {
				if value := strings.TrimSpace(parts[i]); value != "" {
					return value
				}
			}
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (s *server) metrics(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	stats := s.cfg.Relay.Stats()
	var (
		assigned, online, busyPorts int
		toHost, toViewer, dropped   uint64
		udpBusy                     uint64
		tcpToHost, tcpToViewer      uint64
		tcpViewers, tcpBusy         uint64
	)
	for _, port := range stats.Ports {
		if port.HostID != "" {
			assigned++
		}
		if port.HostOnline {
			online++
		}
		if port.Viewer != "" {
			busyPorts++
		}
		toHost += port.ToHost
		toViewer += port.ToViewer
		dropped += port.Dropped
		udpBusy += port.Busy
		tcpToHost += port.TCPToHost
		tcpToViewer += port.TCPToViewer
		tcpViewers += port.TCPViewers
		tcpBusy += port.TCPBusy
	}

	var b strings.Builder
	gauge := func(name, help string, value any) {
		fmt.Fprintf(&b, "# HELP %s %s\n# TYPE %s gauge\n%s %v\n", name, help, name, name, value)
	}
	counter := func(name, help, labels string, value any) {
		fmt.Fprintf(&b, "# HELP %s %s\n# TYPE %s counter\n%s%s %v\n", name, help, name, name, labels, value)
	}
	counterPair := func(name, help string, aLabel string, a any, bLabel string, bValue any) {
		fmt.Fprintf(&b, "# HELP %s %s\n# TYPE %s counter\n%s{%s} %v\n%s{%s} %v\n",
			name, help, name, name, aLabel, a, name, bLabel, bValue)
	}

	fmt.Fprintf(&b, "# HELP chast_server_info Build information.\n# TYPE chast_server_info gauge\nchast_server_info{version=%q,tls=%q} 1\n",
		apiVer, strconv.FormatBool(s.cfg.TLS))
	gauge("chast_uptime_seconds", "Seconds since the API process started.", fmt.Sprintf("%.3f", time.Since(s.started).Seconds()))
	gauge("chast_hosts", "Live host leases.", s.cfg.Registry.Count())
	gauge("chast_identities", "Permanent ID to public-key bindings.", s.cfg.Registry.IdentityCount())
	gauge("chast_server_relay_only", "Whether the server hides direct candidates.", boolFloat(s.relayOnly()))
	gauge("chast_relay_ports_assigned", "Relay ports bound to a host.", assigned)
	gauge("chast_relay_ports_host_online", "Relay ports with a host link or recent traffic.", online)
	gauge("chast_relay_ports_busy", "Relay ports with a live viewer.", busyPorts)
	gauge("chast_punch_udp_read_buffer_bytes", "Kernel-reported SO_RCVBUF of the rendezvous socket; 0 means the default.", s.cfg.PunchReadBuffer)
	gauge("chast_relay_udp_read_buffer_bytes", "Smallest kernel-reported SO_RCVBUF across relay sockets; 0 means the default.", stats.ReadBuffer)
	counterPair("chast_relay_bytes_total", "UDP relay bytes.", `direction="to_host"`, toHost, `direction="to_viewer"`, toViewer)
	counter("chast_relay_datagrams_dropped_total", "Datagrams dropped by the UDP relay.", "", dropped)
	counter("chast_relay_busy_total", "UDP relay viewers refused because the port was busy.", "", udpBusy)
	counterPair("chast_relay_tcp_bytes_total", "Per-port carrier bytes.", `direction="to_host"`, tcpToHost, `direction="to_viewer"`, tcpToViewer)
	counter("chast_relay_tcp_viewers_total", "Per-port carrier viewers attached.", "", tcpViewers)
	counter("chast_relay_tcp_busy_total", "Per-port carrier viewers refused because the port was busy.", "", tcpBusy)

	policy := stats.Policy
	gauge("chast_relay_single_bandwidth_bps", "Per-session relay bandwidth limit in bits per second.", policy.SingleBandwidthBps)
	gauge("chast_relay_total_bandwidth_bps", "Process-wide relay bandwidth limit in bits per second.", policy.TotalBandwidthBps)
	gauge("chast_relay_blacklist_speed_bps", "Relay bandwidth for blacklisted addresses in bits per second.", policy.BlacklistSpeedBps)
	gauge("chast_relay_policy_blacklist", "Addresses limited to the blacklist speed.", policy.Blacklist)
	gauge("chast_relay_policy_blocklist", "Addresses whose viewers are disconnected.", policy.Blocklist)
	gauge("chast_relay_policy_sessions", "Relay sessions under policy accounting.", policy.Sessions)
	counter("chast_relay_limited_bytes_total", "Relay bytes delayed or dropped by a bandwidth limit.", "", policy.LimitedBytes)
	counterPair("chast_relay_rate_dropped_total", "UDP datagrams dropped by the bandwidth limit.",
		`unit="datagrams"`, policy.RateDroppedDatagrams, `unit="bytes"`, policy.RateDroppedBytes)
	counter("chast_relay_blocked_sessions_total", "Relay sessions refused because the address was blocklisted.", "", policy.BlockedSessions)
	counter("chast_relay_downgraded_sessions_total", "Relay sessions downgraded to the blacklist speed.", "", policy.DowngradedSessions)

	if s.cfg.PunchLog != nil {
		punch := s.cfg.PunchLog.Stats()
		counter("chast_punch_requests_total", "Coordinated punch requests offered to the audit log.", "", punch.Total)
		counter("chast_punch_requests_deduped_total", "Punch requests suppressed by the same-peer dedupe window.", "", punch.Deduped)
		gauge("chast_punch_log_entries", "Punch requests currently retained.", punch.Stored)
	}
	if s.cfg.Blocker != nil {
		blocked := s.cfg.Blocker.Stats()
		counterPair("chast_register_blocked_total", "Registration attempts rejected per client IP.",
			`reason="rate"`, blocked.RateBlocked, `reason="ids"`, blocked.IDBlocked)
		gauge("chast_registration_tracked_ips", "Client addresses tracked by the registration blocker.", blocked.TrackedIPs)
	}
	if s.cfg.IPChanges != nil {
		changes := s.cfg.IPChanges.Stats()
		counter("chast_ip_change_events_total", "Authenticated public address changes observed.", "", changes.Changes)
		gauge("chast_ip_change_tracked", "Hosts with more than one address in the current IP-change window.", changes.Tracked)
	}
	gauge("chast_relay_servers", "Relay endpoints advertised to hosts.", len(s.relayServers()))
	relayNodes, relayNodesHealthy := 0, 0
	if s.cfg.RelayPool != nil {
		nodes := s.cfg.RelayPool.Status()
		relayNodes, relayNodesHealthy = len(nodes.Nodes), nodes.Healthy
	}
	gauge("chast_relay_nodes", "Standalone relay nodes configured.", relayNodes)
	gauge("chast_relay_nodes_healthy", "Standalone relay nodes currently healthy.", relayNodesHealthy)
	if shared := stats.SharedCarrier; shared != nil {
		gauge("chast_carrier_hosts", "Shared carrier host links.", shared.Hosts)
		counter("chast_carrier_host_links_total", "Shared carrier host links established.", "", shared.HostLinks)
		gauge("chast_carrier_viewers", "Shared carrier viewers attached.", shared.Viewers)
		counter("chast_carrier_busy_total", "Shared carrier viewers refused because the host was busy.", "", shared.Busy)
		counter("chast_carrier_rejected_total", "Shared carrier handshakes rejected.", "", shared.Rejected)
		counter("chast_carrier_blocked_total", "Shared carrier viewers refused because the address was blocklisted.", "", shared.Blocked)
		counterPair("chast_carrier_bytes_total", "Shared carrier bytes.", `direction="to_host"`, shared.ToHostBytes, `direction="to_viewer"`, shared.ToViewerBytes)
	}

	s.mu.Lock()
	rateLimited := s.rateLimited
	requests := make(map[string]uint64, len(s.requests))
	for key, value := range s.requests {
		requests[key] = value
	}
	sums := make(map[string]float64, len(s.durationSum))
	counts := make(map[string]uint64, len(s.durationCount))
	for key, value := range s.durationSum {
		sums[key] = value
	}
	for key, value := range s.durationCount {
		counts[key] = value
	}
	s.mu.Unlock()

	inFlight := s.inFlight.Load()
	fmt.Fprintf(&b, "# HELP chast_http_requests_in_flight API requests currently being served.\n# TYPE chast_http_requests_in_flight gauge\nchast_http_requests_in_flight %d\n", inFlight)
	counter("chast_http_rate_limited_total", "API requests rejected by the per-IP limiter.", "", rateLimited)

	keys := make([]string, 0, len(requests))
	for key := range requests {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	fmt.Fprintf(&b, "# HELP chast_http_requests_total API requests by endpoint and status.\n# TYPE chast_http_requests_total counter\n")
	for _, key := range keys {
		endpoint, code, _ := strings.Cut(key, "\x00")
		fmt.Fprintf(&b, "chast_http_requests_total{endpoint=%q,code=%q} %d\n", endpoint, code, requests[key])
	}

	endpoints := make([]string, 0, len(sums))
	for endpoint := range sums {
		endpoints = append(endpoints, endpoint)
	}
	sort.Strings(endpoints)
	fmt.Fprintf(&b, "# HELP chast_http_request_duration_seconds API request duration.\n# TYPE chast_http_request_duration_seconds summary\n")
	for _, endpoint := range endpoints {
		fmt.Fprintf(&b, "chast_http_request_duration_seconds_sum{endpoint=%q} %.6f\n", endpoint, sums[endpoint])
		fmt.Fprintf(&b, "chast_http_request_duration_seconds_count{endpoint=%q} %d\n", endpoint, counts[endpoint])
	}

	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = io.WriteString(w, b.String())
}

func (s *server) binding(host registry.Host, now time.Time) (string, error) {
	if s.cfg.Identity == nil || host.PublicKey == "" {
		return "", nil
	}
	pub, err := identity.ParsePublicKey(host.PublicKey)
	if err != nil {
		return "", err
	}
	return s.cfg.Identity.SignHostBinding(host.ID, pub, host.Fingerprint, now)
}

func (s *server) relayOnly() bool {
	return s.cfg.RelayOnly != nil && s.cfg.RelayOnly.Load()
}

func (s *server) transport(host registry.Host, carrierLinked, direct bool) string {
	if direct {
		return "direct"
	}
	if carrierLinked && host.CarrierToken != "" && s.carrierEndpoint(host) != "" {
		return "carrier"
	}
	if host.RelayPort != 0 {
		return "relay"
	}
	return ""
}

// carrierEndpoint names the shared carrier a viewer should dial. A remote
// lease carries its own endpoint; a local lease uses the relay host assigned
// to this host.
func (s *server) carrierEndpoint(host registry.Host) string {
	if host.RelayNode != "" {
		return host.Carrier
	}
	if s.cfg.Relay == nil || host.RelayPort == 0 {
		return ""
	}
	port := s.cfg.Relay.CarrierPort()
	if port == 0 {
		return ""
	}
	return net.JoinHostPort(s.relayHostFor(host), strconv.Itoa(port))
}

// assignRelay gives a host a relay lease, preferring standalone relay nodes.
// When every node is unavailable it falls back to the in-process relay so a
// single-node outage does not stop new sessions, matching hbbs's relay list
// with chast's local fallback.
func (s *server) assignRelay(id, publicKey string, host registry.Host) (string, []string, string, error) {
	if s.cfg.RelayPool != nil {
		// The node lease outlives the host lease so a brief hbbs outage does
		// not tear down a working relay path.
		lease, err := s.cfg.RelayPool.Provision(id, publicKey, host.CarrierToken, 2*s.cfg.TTL, host.RelayNode)
		if err == nil {
			nodeHost, _, splitErr := splitRelayEndpoint(lease.Relay)
			if splitErr == nil {
				s.moveRelayLease(id, host, lease.Node)
				s.cfg.Registry.SetRemoteRelay(id, lease.Node, lease.Port, nodeHost, lease.Carrier)
				return lease.Relay, []string{lease.Relay}, lease.Carrier, nil
			}
			err = splitErr
			s.cfg.RelayPool.ReleaseAsync(lease.Node, id)
		}
		s.cfg.Logger.Warn("relay node unavailable; falling back to the local relay",
			"id", id, "preferred", host.RelayNode, "error", err)
		if host.RelayNode != "" {
			s.cfg.RelayPool.ReleaseAsync(host.RelayNode, id)
		}
	}
	endpoint, relays, err := s.assignLocalRelay(id, host)
	if err != nil {
		return "", nil, "", err
	}
	relayHost, _, _ := splitRelayEndpoint(endpoint)
	return endpoint, relays, s.localCarrier(relayHost), nil
}

// moveRelayLease frees whatever the previous registration owned. A lease on
// the same node is left alone so a renewal keeps its port and viewer.
func (s *server) moveRelayLease(id string, previous registry.Host, node string) {
	switch {
	case previous.RelayNode == "":
		if previous.RelayPort != 0 && s.cfg.Relay != nil {
			s.cfg.Relay.Release(id)
		}
	case previous.RelayNode != node:
		s.cfg.RelayPool.ReleaseAsync(previous.RelayNode, id)
	}
}

func (s *server) assignLocalRelay(id string, host registry.Host) (string, []string, error) {
	if s.cfg.Relay == nil {
		return "", nil, errors.New("no relay manager is configured")
	}
	port, err := s.cfg.Relay.Allocate(id)
	if err != nil {
		return "", nil, err
	}
	previous := ""
	if host.RelayNode == "" {
		// Keep a local lease on its advertised address across renewals.
		previous = host.RelayHost
	}
	assigned := ""
	var hosts []string
	if s.cfg.RelayAddrs != nil {
		assigned, hosts = s.cfg.RelayAddrs.AssignFrom(previous)
	} else {
		assigned = s.cfg.RelayHost
		if assigned != "" {
			hosts = []string{assigned}
		}
	}
	if assigned == "" {
		s.cfg.Relay.Release(id)
		return "", nil, errors.New("no relay host is configured")
	}
	s.cfg.Registry.SetRelay(id, port, assigned)
	portText := strconv.Itoa(port)
	relays := make([]string, 0, len(hosts))
	for _, relayHost := range hosts {
		relays = append(relays, net.JoinHostPort(relayHost, portText))
	}
	return net.JoinHostPort(assigned, portText), relays, nil
}

func (s *server) localCarrier(relayHost string) string {
	if s.cfg.Relay == nil || relayHost == "" {
		return ""
	}
	port := s.cfg.Relay.CarrierPort()
	if port == 0 {
		return ""
	}
	return net.JoinHostPort(relayHost, strconv.Itoa(port))
}

func splitRelayEndpoint(endpoint string) (string, int, error) {
	host, portText, err := net.SplitHostPort(endpoint)
	if err != nil {
		return "", 0, fmt.Errorf("relay endpoint %q: %w", endpoint, err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port <= 0 || port > 65535 {
		return "", 0, fmt.Errorf("relay endpoint %q has an invalid port", endpoint)
	}
	return host, port, nil
}

func boolFloat(v bool) float64 {
	if v {
		return 1
	}
	return 0
}

func (s *server) authorized(r *http.Request) bool {
	if s.cfg.ServerKey == "" {
		return true
	}
	header := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if !strings.HasPrefix(header, prefix) {
		return false
	}
	got := strings.TrimSpace(header[len(prefix):])
	return subtle.ConstantTimeCompare([]byte(got), []byte(s.cfg.ServerKey)) == 1
}

func decodeBody(w http.ResponseWriter, r *http.Request, dst any) error {
	defer r.Body.Close()
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxBody))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}
	if dec.More() {
		return errors.New("invalid JSON: trailing data")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeError(w http.ResponseWriter, status int, text string) {
	writeJSON(w, status, map[string]string{"error": text})
}

func dedupe(values []string) []string {
	seen := make(map[string]struct{}, len(values))
	out := values[:0]
	for _, value := range values {
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	return out
}
