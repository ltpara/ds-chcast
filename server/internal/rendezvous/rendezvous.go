// Package rendezvous is the UDP meet-up point for hosts and viewers.
//
// Hosts prove liveness with keepalives; the address the server sees is the only
// public candidate it hands out. A viewer's punch_request makes the server tell
// both sides about each other so they can send probes from the same sockets they
// will use for QUIC. The server never forwards video data on this port.
package rendezvous

import (
	"crypto/ed25519"
	"encoding/base64"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/lanmask"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	punchTTL        = 30 * time.Second
	punchAuthWindow = time.Minute
	// localWaitTTL covers the viewer's retries while its punch is outstanding.
	// The refreshed answer supplements the cached one, so an expired entry only
	// costs freshness.
	localWaitTTL   = 15 * time.Second
	DefaultRateMax = 30
	// DefaultKeepaliveRateMax admits roughly twelve keepalive messages per
	// second per source IP: far above the host's own interval while still
	// capping a flood.
	DefaultKeepaliveRateMax = 120
	// DefaultNatProbeRateMax caps unauthenticated NAT probes per source IP per
	// ten seconds. Two endpoints are probed per measurement, retried on failure.
	DefaultNatProbeRateMax = 60
	defaultRateSpan        = 10 * time.Second
	// punchReplayCapacity bounds the seen transport proofs. Like the relay
	// guard, the cache refuses new admissions when full instead of evicting a
	// live entry, which would forget a replay.
	punchReplayCapacity = 8192
	// punchWaiterCapacity bounds viewers remembered for a host's relay hint.
	// A dropped entry only loses the hint; the viewer's own timeout still
	// reaches the relay.
	punchWaiterCapacity = 1024
)

type options struct {
	readBuffer       int
	rateMax          int
	keepaliveRateMax int
	serverKey        string
	linkKey          string
	tracer           *tracing.Tracer
	// tcpPunchListenSet distinguishes "derive from the UDP bind" from an
	// explicit value; an explicitly empty value disables the listener.
	tcpPunchListenSet bool
	tcpPunchListen    string
	relayOnly         func() bool
	hostCarrierLinked func(string) bool
	transport         *acl.TransportVerifier
	// lanMask classifies observed peer addresses like hbbs's -m; lanLocalIP is
	// the local relay address handed to a LAN peer. Both are inert when the
	// mask is nil, which keeps deployments without a LAN unchanged.
	lanMask    *lanmask.Mask
	lanLocalIP string
}

// Option customizes the rendezvous socket without changing the call sites that
// only need the defaults.
type Option func(*options)

// WithReadBuffer sets the UDP receive buffer (SO_RCVBUF) requested at bind
// time. Zero keeps the system default.
func WithReadBuffer(size int) Option {
	return func(o *options) { o.readBuffer = size }
}

// WithPunchRateMax caps punch requests per client IP per ten seconds. A
// non-positive value disables the limiter.
func WithPunchRateMax(max int) Option {
	return func(o *options) { o.rateMax = max }
}

// WithKeepaliveRateMax caps keepalive control messages per client IP per ten
// seconds. A non-positive value disables the limiter.
func WithKeepaliveRateMax(max int) Option {
	return func(o *options) { o.keepaliveRateMax = max }
}

// WithServerKey requires viewers to prove knowledge of the server key before a
// punch is coordinated. An empty key keeps public rendezvous behavior unless a
// link key is configured.
func WithServerKey(key string) Option {
	return func(o *options) { o.serverKey = key }
}

// WithLinkKey lets ordinary viewers punch with the deployment's connection
// credential instead of the management key. The server key remains accepted.
func WithLinkKey(key string) Option {
	return func(o *options) { o.linkKey = key }
}

// WithTracer records one span per punch request. Spans join the trace derived
// from the host ID, which the relay also uses, so rendezvous and relay activity
// for one host is visible together.
func WithTracer(tracer *tracing.Tracer) Option {
	return func(o *options) { o.tracer = tracer }
}

// WithTCPPunchListen sets the address of the TCP punch coordination listener.
// An empty addr disables it; without this option the listener is derived from
// the bound UDP address and uses the same port.
func WithTCPPunchListen(addr string) Option {
	return func(o *options) {
		o.tcpPunchListen = addr
		o.tcpPunchListenSet = true
	}
}

// WithRelayOnly makes the TCP punch listener refuse coordination while
// relay-only mode is active, matching the API, which stops advertising the
// endpoint in the same state.
func WithRelayOnly(relayOnly func() bool) Option {
	return func(o *options) { o.relayOnly = relayOnly }
}

// WithHostCarrierLinked lets TCP punch coordination treat an authenticated
// shared-carrier link as proof that a host is online. This matches the lookup
// path: a host whose UDP mapping expired but whose carrier is connected is
// still online. Punch instructions use the independent rendezvous control
// link or UDP; the carrier only supplies a compatible liveness signal.
func WithHostCarrierLinked(linked func(string) bool) Option {
	return func(o *options) { o.hostCarrierLinked = linked }
}

// WithTransportVerifier makes punch coordination accept short-lived transport
// credentials. A managed host then refuses the legacy shared-key HMAC, so an
// account deployment never falls back to a deployment-wide punch secret.
func WithTransportVerifier(verifier *acl.TransportVerifier) Option {
	return func(o *options) { o.transport = verifier }
}

// WithLAN enables the hbbs LAN rule. Pairs with one peer inside the mask and
// the other outside are classified as symmetric so both sides start on the
// relay; LAN peers are handed localIP as the relay address when the lease is
// local. A nil or disabled mask keeps the previous behavior.
func WithLAN(mask *lanmask.Mask, localIP string) Option {
	return func(o *options) {
		o.lanMask = mask
		o.lanLocalIP = localIP
	}
}

type Server struct {
	conn      *net.UDPConn
	reg       *registry.Registry
	relayAddr func(port int) string
	identity  *identity.Server
	punchLog  *punchlog.Log
	ipChanges *ipchange.Tracker
	log       *slog.Logger
	limiter   *limiter
	// keepaliveLimiter throttles both keepalive legs per source IP. Without
	// it an unauthenticated flood could mint challenges as fast as the
	// registry can sign them.
	keepaliveLimiter *limiter
	// natProbeLimiter throttles stateless NAT observation replies so the punch
	// port cannot be used as a reflector.
	natProbeLimiter   *limiter
	serverKey         string
	linkKey           string
	tracer            *tracing.Tracer
	readBuffer        int
	relayOnly         func() bool
	hostCarrierLinked func(string) bool
	transport         *acl.TransportVerifier
	lanMask           *lanmask.Mask
	lanLocalIP        string
	transportReplayMu sync.Mutex
	transportReplay   map[string]punchReplay
	// punchWaiters remembers the socket that asked to punch so the host's
	// relay decision can be forwarded to it. It is keyed by host ID and nonce
	// and consumed once, like the response the viewer is waiting for.
	punchWaiterMu sync.Mutex
	punchWaiters  map[string]punchWaiter
	// localWaiters remembers the viewer that asked for a LAN punch while the
	// host re-reports its current candidates. Unlike a relay hint this answer
	// supplements the cached reply, so a lost fetch costs freshness only.
	localWaiterMu sync.Mutex
	localWaiters  map[string]localWaiter
	// tcpPunch is nil when the coordination listener is disabled or could not
	// bind; punching then stays on UDP.
	tcpPunch  *tcpPuncher
	done      chan struct{}
	closeOnce sync.Once
}

// New binds the meeting port. bind is an IP:port; port 0 lets the kernel pick.
func New(bind string, reg *registry.Registry, relayAddr func(port int) string, log *slog.Logger, opts ...Option) (*Server, error) {
	cfg := options{rateMax: DefaultRateMax, keepaliveRateMax: DefaultKeepaliveRateMax}
	for _, opt := range opts {
		opt(&cfg)
	}
	addr, err := net.ResolveUDPAddr("udp", bind)
	if err != nil {
		return nil, fmt.Errorf("punch listen %q: %w", bind, err)
	}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return nil, fmt.Errorf("punch listen %q: %w", bind, err)
	}
	if log == nil {
		log = slog.Default()
	}
	readBuffer, err := netutil.SetUDPReadBuffer(conn, cfg.readBuffer)
	if err != nil {
		log.Warn("punch UDP receive buffer was not applied",
			"requested", cfg.readBuffer, "error", err)
	}
	server := &Server{
		conn:              conn,
		reg:               reg,
		relayAddr:         relayAddr,
		log:               log,
		limiter:           newLimiter(cfg.rateMax, defaultRateSpan),
		keepaliveLimiter:  newLimiter(cfg.keepaliveRateMax, defaultRateSpan),
		natProbeLimiter:   newLimiter(DefaultNatProbeRateMax, defaultRateSpan),
		serverKey:         cfg.serverKey,
		linkKey:           cfg.linkKey,
		tracer:            cfg.tracer,
		readBuffer:        readBuffer,
		relayOnly:         cfg.relayOnly,
		hostCarrierLinked: cfg.hostCarrierLinked,
		transport:         cfg.transport,
		lanMask:           cfg.lanMask,
		lanLocalIP:        cfg.lanLocalIP,
		transportReplay:   make(map[string]punchReplay),
		punchWaiters:      make(map[string]punchWaiter),
		localWaiters:      make(map[string]localWaiter),
		done:              make(chan struct{}),
	}
	server.startTCPPunch(cfg)
	return server, nil
}

// startTCPPunch binds the TCP punch listener, deriving its address from the
// bound UDP address unless one was configured explicitly.
func (s *Server) startTCPPunch(cfg options) {
	bind := ""
	if cfg.tcpPunchListenSet {
		bind = cfg.tcpPunchListen
	} else if udp, ok := s.conn.LocalAddr().(*net.UDPAddr); ok {
		bind = deriveTCPPunchListen(udp)
	}
	if bind == "" {
		return
	}
	s.enableTCPPunch(bind)
}

// ReadBuffer is the SO_RCVBUF value reported by the kernel, or zero when the
// default was kept or the option could not be read back.
func (s *Server) ReadBuffer() int { return s.readBuffer }

func (s *Server) Addr() string { return s.conn.LocalAddr().String() }

// SetIdentity enables server-signed host bindings in punch replies.
func (s *Server) SetIdentity(server *identity.Server) { s.identity = server }

// SetPunchLog records successful punch coordination for the admin console.
func (s *Server) SetPunchLog(log *punchlog.Log) { s.punchLog = log }

// SetIPChanges audits public address changes seen in authenticated keepalives.
func (s *Server) SetIPChanges(tracker *ipchange.Tracker) { s.ipChanges = tracker }

func (s *Server) Close() {
	s.closeOnce.Do(func() {
		close(s.done)
		s.conn.Close()
		if s.tcpPunch != nil {
			s.tcpPunch.close()
		}
	})
}

func (s *Server) Serve() {
	buf := make([]byte, wire.MaxControlSize+1)
	for {
		n, src, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			select {
			case <-s.done:
				return
			default:
			}
			if errors.Is(err, net.ErrClosed) {
				return
			}
			s.log.Warn("rendezvous read", "error", err)
			continue
		}
		msg, err := wire.Decode(buf[:n])
		if err != nil {
			// The port only speaks control packets; ignore anything else
			// without replying so it cannot be used as a reflector.
			continue
		}
		s.handle(msg, src)
	}
}

func (s *Server) handle(msg wire.Message, src *net.UDPAddr) {
	switch msg.Type {
	case wire.TypeKeepalive:
		s.handleKeepalive(msg, src)
	case wire.TypeKeepaliveAuth:
		s.handleKeepaliveAuth(msg, src)
	case wire.TypePunchRequest:
		s.handlePunchRequest(msg, src)
	case wire.TypeRelayHint:
		s.handleRelayHint(msg, src)
	case wire.TypeLocalAddr:
		s.handleLocalAddr(msg, src)
	case wire.TypeNatProbe:
		s.handleNatProbe(msg, src)
	}
}

// normalizeNATType keeps only the classifications the agents exchange. An
// arbitrary string from a viewer must not reach a host's logging or decisions.
func normalizeNATType(nat string) string {
	switch nat {
	case "unknown", "asymmetric", "symmetric":
		return nat
	default:
		return ""
	}
}

// handleNatProbe echoes the source address a probe arrived from. Two server
// ports observe the same client socket: a cone NAT keeps the public port while
// a symmetric NAT allocates one per destination, which is how the agent tells
// them apart. It is stateless and rate-limited because the reply can be sent
// to a spoofed source.
func (s *Server) handleNatProbe(msg wire.Message, src *net.UDPAddr) {
	if len(msg.Nonce) < 8 || len(msg.Nonce) > 64 {
		return
	}
	if !s.natProbeLimiter.allow(src.IP.String()) {
		return
	}
	candidate, candidateV6 := observedCandidate(src)
	if candidate == "" {
		candidate = candidateV6
	}
	s.reply(src, wire.Message{
		Type:      wire.TypeNatProbeAck,
		Nonce:     msg.Nonce,
		Candidate: candidate,
	})
}

// handleKeepalive only starts the exchange. The host must prove its key over a
// one-time nonce before the observed address is allowed to move, so a spoofed
// source cannot redirect a host's mapping.
func (s *Server) handleKeepalive(msg wire.Message, src *net.UDPAddr) {
	if !s.keepaliveLimiter.allow(src.IP.String()) {
		// Silent drop: any reply would make the port a reflector.
		return
	}
	candidate, candidateV6 := observedCandidate(src)
	scope := "v4"
	if candidateV6 != "" {
		scope = "v6"
	}
	nonce, err := s.reg.IssueChallengeScoped(msg.HostID, identity.DomainKeepalive, scope)
	if err != nil {
		s.reply(src, wire.Message{Type: wire.TypeKeepaliveAck, Error: "host has no live registration"})
		return
	}
	s.reply(src, wire.Message{
		Type:        wire.TypeKeepaliveChallenge,
		HostID:      msg.HostID,
		Nonce:       nonce,
		Candidate:   candidate,
		CandidateV6: candidateV6,
	})
}

func (s *Server) handleKeepaliveAuth(msg wire.Message, src *net.UDPAddr) {
	if !s.keepaliveLimiter.allow(src.IP.String()) {
		return
	}
	ack := wire.Message{Type: wire.TypeKeepaliveAck}
	candidate, candidateV6 := observedCandidate(src)
	if (msg.Candidate != "" && msg.Candidate != candidate) ||
		(msg.CandidateV6 != "" && msg.CandidateV6 != candidateV6) ||
		(msg.Candidate == "" && msg.CandidateV6 == "") {
		ack.Error = "keepalive arrived from a different address than the challenge"
		s.reply(src, ack)
		return
	}
	observed := candidate
	if observed == "" {
		observed = candidateV6
	}
	if !validUpnpPort(msg.UpnpPort) {
		ack.Error = "upnp_port must be 0..65535"
		s.reply(src, ack)
		return
	}
	message := identity.KeepaliveMessage(msg.HostID, observed, msg.Nonce)
	localAddrs := append(append([]string{}, msg.LocalAddrs...), msg.LocalAddrsV6...)
	if err := s.reg.TouchChallenge(msg.HostID, msg.Nonce, observed, msg.Signature,
		string(message), localAddrs, msg.NATType, msg.UpnpPort, validSelfV6(msg.SelfV6)); err != nil {
		ack.Error = err.Error()
		s.reply(src, ack)
		return
	}
	if s.ipChanges != nil {
		s.ipChanges.Observe(msg.HostID, src.IP.String())
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok {
		ack.Error = "host is no longer registered"
		s.reply(src, ack)
		return
	}
	ack.OK = true
	ack.Candidate = candidate
	ack.CandidateV6 = candidateV6
	ack.LocalAddrs = host.LocalAddrs
	ack.LocalAddrsV6 = host.LocalAddrsV6
	ack.NATType = host.NATType
	ack.Fingerprint = host.Fingerprint
	if host.RelayPort != 0 {
		ack.Relay = s.relayFor(host, s.isLAN(src.IP))
	}
	s.reply(src, ack)
}

// relayFor returns the relay endpoint for one receiving side. A side on the
// server's LAN dials the configured local address when the lease is local, so
// it never depends on NAT hairpin; a relay node lease keeps its own host.
func (s *Server) relayFor(host registry.Host, lanPeer bool) string {
	if host.RelayPort == 0 {
		return ""
	}
	if lanPeer && host.RelayNode == "" && s.lanLocalIP != "" {
		return net.JoinHostPort(s.lanLocalIP, strconv.Itoa(host.RelayPort))
	}
	if host.RelayHost != "" {
		return net.JoinHostPort(host.RelayHost, strconv.Itoa(host.RelayPort))
	}
	return s.relayAddr(host.RelayPort)
}

// isLAN classifies one observed address.
func (s *Server) isLAN(ip net.IP) bool { return s.lanMask.Contains(ip) }

// hostOnLAN reports whether any stored public candidate of the host falls
// inside the LAN mask. A dual-stack host is treated as one peer, matching
// hbbs, which classifies the single address it last observed.
func (s *Server) hostOnLAN(host registry.Host) bool {
	return s.lanMask.ContainsAddr(host.Candidate) || s.lanMask.ContainsAddr(host.CandidateV6)
}

// punchNAT is the NAT classification the service exchanges for one punch.
// hbbs reports a LAN/WAN pair as symmetric in both directions, which makes
// both agents start on the relay; otherwise the peer's own report is used.
func (s *Server) punchNAT(host registry.Host, peerNAT string, peerIP net.IP) string {
	if s.lanXOR(host, peerIP) {
		return "symmetric"
	}
	return normalizeNATType(peerNAT)
}

// lanXOR is hbbs's peer_is_lan ^ is_lan: a LAN peer and a WAN peer cannot
// open a hole to each other, so the pair is forced onto the relay.
func (s *Server) lanXOR(host registry.Host, peerIP net.IP) bool {
	if !s.lanMask.Enabled() {
		return false
	}
	return s.hostOnLAN(host) != s.isLAN(peerIP)
}

// lanXORAddr is lanXOR for endpoints that arrive as text.
func (s *Server) lanXORAddr(host registry.Host, peer string) bool {
	return s.lanXOR(host, lanmask.AddrIP(peer))
}

// sameIntranet reports whether a peer can reach any of the host's observed
// addresses on the same network.
func (s *Server) sameIntranet(host registry.Host, src *net.UDPAddr) bool {
	if src == nil {
		return false
	}
	for _, text := range []string{host.Candidate, host.CandidateV6} {
		if s.lanMask.SameIntranet(lanmask.AddrIP(text), src.IP) {
			return true
		}
	}
	return false
}

// checkViewerPunch verifies the viewer half of a punch. A transport
// credential, when presented, is authoritative: an invalid one never falls
// back to the legacy keys. A managed host always requires a credential, so
// device policy can never be bypassed with the deployment-wide punch secret;
// unmanaged devices keep the historical shared-key behavior.
func (s *Server) checkViewerPunch(msg wire.Message, now time.Time, peer string) bool {
	if s.verifyTransportPunch(msg, now, peer) {
		return true
	}
	if msg.Transport != "" {
		return false
	}
	if host, ok := s.reg.Lookup(msg.HostID); ok && host.Managed {
		return false
	}
	if s.linkKey != "" &&
		identity.CheckPunchAuth(s.linkKey, msg.HostID, msg.Nonce, msg.Auth, msg.Timestamp, now, punchAuthWindow) {
		return true
	}
	if s.serverKey != "" &&
		identity.CheckPunchAuth(s.serverKey, msg.HostID, msg.Nonce, msg.Auth, msg.Timestamp, now, punchAuthWindow) {
		return true
	}
	return s.linkKey == "" && s.serverKey == ""
}

// verifyTransportPunch validates the signed credential and the viewer's proof
// of possession. The proof covers the fresh nonce and timestamp, so a captured
// credential cannot be presented by another party.
func (s *Server) verifyTransportPunch(msg wire.Message, now time.Time, peer string) bool {
	if s.transport == nil || msg.Transport == "" || msg.TransportProof == "" || msg.Timestamp <= 0 {
		return false
	}
	stale := now.Unix() - msg.Timestamp
	if stale < 0 {
		stale = -stale
	}
	if stale > int64(punchAuthWindow/time.Second) {
		return false
	}
	signed, err := base64.RawURLEncoding.DecodeString(msg.Transport)
	if err != nil {
		return false
	}
	ticket, err := s.transport.Verify(signed, now)
	if err != nil || ticket.HostID != msg.HostID ||
		ticket.Audience != acl.TransportAudiencePunch {
		return false
	}
	message := identity.TransportProofMessage(ticket.Audience, ticket.HostID, msg.Nonce, msg.Timestamp)
	if !identity.Verify(ed25519.PublicKey(ticket.ClientPublicKey[:]), message, msg.TransportProof) {
		return false
	}
	return s.admitTransportPunch(ticket.ID, msg.Nonce, msg.Timestamp, ticket.ExpiresAt, peer, now)
}

type punchReplay struct {
	peer    string
	expires time.Time
}

// admitTransportPunch records one verified proof. An identical proof may repeat
// from the same peer, which keeps UDP retransmits and TCP punch retries working,
// but the same datagram replayed from another source is refused. The cache never
// evicts a live entry to make room: when full, new proofs fail closed.
func (s *Server) admitTransportPunch(ticketID, nonce string, timestamp, expiresAt int64,
	peer string, now time.Time) bool {
	key := ticketID + "\x00" + nonce + "\x00" + strconv.FormatInt(timestamp, 10)
	s.transportReplayMu.Lock()
	defer s.transportReplayMu.Unlock()
	for seenKey, entry := range s.transportReplay {
		if !now.Before(entry.expires) {
			delete(s.transportReplay, seenKey)
		}
	}
	if entry, ok := s.transportReplay[key]; ok {
		return entry.peer == peer
	}
	if len(s.transportReplay) >= punchReplayCapacity {
		return false
	}
	expiry := time.Unix(expiresAt, 0).Add(punchAuthWindow)
	if !expiry.After(now) {
		expiry = now.Add(punchAuthWindow)
	}
	s.transportReplay[key] = punchReplay{peer: peer, expires: expiry}
	return true
}

func (s *Server) handlePunchRequest(msg wire.Message, src *net.UDPAddr) {
	reply := wire.Message{Type: wire.TypePunchPeer, Nonce: msg.Nonce}
	span := s.tracer.StartDerived(tracing.DeriveTraceID("host", msg.HostID), "punch",
		tracing.String("chast.host_id", msg.HostID),
		tracing.String("chast.nonce", msg.Nonce),
		tracing.String("client.address", src.IP.String()),
	)
	result := "error"
	relayOffered := false
	defer func() {
		span.Add(tracing.String("chast.result", result))
		if relayOffered {
			span.Add(tracing.Int("relay.offered", 1))
		}
		if result != "ok" {
			span.SetStatus(2)
		}
		s.tracer.EndSpan(span)
	}()
	if !s.limiter.allow(src.IP.String()) {
		result = "rate_limited"
		reply.Error = "too many punch requests; retry later"
		s.reply(src, reply)
		return
	}
	if len(msg.Nonce) < 8 || len(msg.Nonce) > 64 {
		result = "invalid_nonce"
		reply.Error = "nonce must be 8..64 characters"
		s.reply(src, reply)
		return
	}
	if !validUpnpPort(msg.UpnpPort) {
		result = "invalid_upnp_port"
		reply.Error = "upnp_port must be 0..65535"
		s.reply(src, reply)
		return
	}
	if !s.checkViewerPunch(msg, time.Now(), src.String()) {
		result = "auth_failed"
		reply.Error = "punch authentication failed"
		s.reply(src, reply)
		return
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok || !s.reg.Online(host) {
		// One message for unknown and offline hosts: distinguishing them
		// would turn the punch port into a registration oracle.
		result = "host_offline"
		reply.Error = "host is offline"
		s.reply(src, reply)
		return
	}
	// Relay-only mode hides direct candidates in lookups and refuses TCP punch
	// coordination; the UDP port must refuse the same way. A viewer that only
	// speaks punch still learns where the relay is, so it is not stranded.
	if s.relayOnly != nil && s.relayOnly() {
		result = "relay_only"
		if host.RelayPort != 0 {
			reply.Relay = s.relayFor(host, s.isLAN(src.IP))
			relayOffered = true
		}
		reply.Error = "server is in relay-only mode"
		s.reply(src, reply)
		return
	}
	var hostAddrs []*net.UDPAddr
	for _, text := range []string{host.Candidate, host.CandidateV6} {
		if text == "" {
			continue
		}
		addr, err := net.ResolveUDPAddr("udp", text)
		if err == nil && addr != nil {
			hostAddrs = append(hostAddrs, addr)
		}
	}
	if len(hostAddrs) == 0 {
		result = "no_host_address"
		reply.Error = "host has no usable address"
		s.reply(src, reply)
		return
	}
	result = "ok"
	viewer4, viewer6 := observedCandidate(src)
	if viewer6 == "" {
		// This port only sees the family the request arrived on. A viewer that
		// reached it over IPv4 reports its own global IPv6 endpoint, which is
		// what RustDesk carries as socket_addr_v6; without it a host could
		// never punch back over IPv6.
		viewer6 = validSelfV6(msg.SelfV6)
	}
	// Record only requests that can actually be coordinated, matching hbbs's
	// record point after the target's online check. A dual-stack host gets one
	// entry per family.
	if s.punchLog != nil {
		for _, addr := range hostAddrs {
			s.punchLog.Record(src.IP.String(), addr.IP.String(), host.ID)
		}
	}
	// The host may answer this request with a relay decision instead of a
	// punch. Remember where to deliver it; the entry is consumed by the first
	// valid hint and otherwise expires with the punch window.
	s.rememberPunchWaiter(host.ID, msg.Nonce, src)
	// Tell the host to punch towards the viewer. Best effort: the viewer is
	// also told the host candidate and will probe first. The viewer's NAT type
	// lets the host skip a v4 punch that a symmetric mapping cannot complete;
	// a v6 punch is unaffected because it does not traverse the NAT.
	viewerNAT := s.punchNAT(host, msg.NATType, src.IP)
	viewerLocal4, viewerLocal6 := s.viewerLocalAddrs(host, src, msg.LocalAddrs, msg.LocalAddrsV6)
	// A viewer that shares the host's network is the only one that can use the
	// host's private candidates. Ask for a fresh report in the same message:
	// the last keepalive may predate an address change, and a second datagram
	// would only add a round trip to the punch.
	isLocal := s.sameIntranet(host, src)
	if isLocal {
		s.rememberLocalWaiter(host.ID, msg.Nonce, src)
	}
	for _, hostAddr := range hostAddrs {
		s.reply(hostAddr, wire.Message{
			Type:           wire.TypePunchPeer,
			Nonce:          msg.Nonce,
			Candidate:      viewer4,
			CandidateV6:    viewer6,
			LocalAddrs:     viewerLocal4,
			LocalAddrsV6:   viewerLocal6,
			UpnpPort:       msg.UpnpPort,
			NATType:        viewerNAT,
			WantLocalAddrs: isLocal,
			ExpiresMS:      punchTTL.Milliseconds(),
		})
	}
	reply = s.punchReply(host, src, msg.Nonce)
	if reply.Relay != "" {
		relayOffered = true
	}
	s.reply(src, reply)
}

// punchReply builds the answer a viewer receives for one host. The cached
// answer and the LAN refresh share it, so the two cannot drift apart.
func (s *Server) punchReply(host registry.Host, src *net.UDPAddr, nonce string) wire.Message {
	reply := wire.Message{Type: wire.TypePunchPeer, Nonce: nonce, OK: true}
	reply.Candidate = host.Candidate
	reply.CandidateV6 = host.CandidateV6
	reply.UpnpPort = host.UpnpPort
	reply.LocalAddrs = host.LocalAddrs
	reply.LocalAddrsV6 = host.LocalAddrsV6
	reply.NATType = s.punchNAT(host, host.NATType, src.IP)
	reply.IsLocal = s.sameIntranet(host, src)
	reply.PublicKey = host.PublicKey
	reply.Fingerprint = host.Fingerprint
	if s.identity != nil && host.PublicKey != "" {
		if pub, err := identity.ParsePublicKey(host.PublicKey); err == nil {
			if binding, err := s.identity.SignHostBinding(host.ID, pub, host.Fingerprint, time.Now()); err == nil {
				reply.Binding = binding
			}
		}
	}
	if host.RelayPort != 0 {
		reply.Relay = s.relayFor(host, s.isLAN(src.IP))
	}
	reply.ExpiresMS = punchTTL.Milliseconds()
	return reply
}

func (s *Server) reply(to *net.UDPAddr, m wire.Message) {
	if _, err := s.conn.WriteToUDP(wire.Encode(m), to); err != nil {
		s.log.Debug("rendezvous reply", "to", to, "error", err)
	}
}

type punchWaiter struct {
	src     *net.UDPAddr
	expires time.Time
}

func punchWaiterKey(hostID, nonce string) string { return hostID + "\x00" + nonce }

func cloneSource(src *net.UDPAddr) *net.UDPAddr {
	if src == nil {
		return nil
	}
	copied := *src
	if src.IP != nil {
		copied.IP = append(net.IP(nil), src.IP...)
	}
	return &copied
}

func (s *Server) rememberPunchWaiter(hostID, nonce string, src *net.UDPAddr) {
	if src == nil {
		return
	}
	now := time.Now()
	key := punchWaiterKey(hostID, nonce)
	s.punchWaiterMu.Lock()
	defer s.punchWaiterMu.Unlock()
	if len(s.punchWaiters) >= punchWaiterCapacity {
		for k, w := range s.punchWaiters {
			if now.After(w.expires) {
				delete(s.punchWaiters, k)
			}
		}
		if len(s.punchWaiters) >= punchWaiterCapacity {
			return
		}
	}
	s.punchWaiters[key] = punchWaiter{src: cloneSource(src), expires: now.Add(punchTTL)}
}

// takePunchWaiter removes the viewer waiting on this punch and returns where
// its reply must go. A hint is single-shot: the viewer acts on the first one
// and any repeat would only be duplicate control traffic.
func (s *Server) takePunchWaiter(hostID, nonce string) *net.UDPAddr {
	key := punchWaiterKey(hostID, nonce)
	now := time.Now()
	s.punchWaiterMu.Lock()
	defer s.punchWaiterMu.Unlock()
	waiter, ok := s.punchWaiters[key]
	if !ok {
		return nil
	}
	delete(s.punchWaiters, key)
	if now.After(waiter.expires) {
		return nil
	}
	return waiter.src
}

// localWaiter is the viewer waiting for a host's current LAN candidates. The
// key is the punch nonce, so an answer only reaches the request that asked.
type localWaiter struct {
	hostID  string
	src     *net.UDPAddr
	expires time.Time
}

func localWaiterKey(hostID, nonce string) string { return hostID + "\x00" + nonce }

func (s *Server) rememberLocalWaiter(hostID, nonce string, src *net.UDPAddr) {
	if src == nil {
		return
	}
	now := time.Now()
	key := localWaiterKey(hostID, nonce)
	s.localWaiterMu.Lock()
	defer s.localWaiterMu.Unlock()
	if len(s.localWaiters) >= punchWaiterCapacity {
		for k, w := range s.localWaiters {
			if now.After(w.expires) {
				delete(s.localWaiters, k)
			}
		}
		if len(s.localWaiters) >= punchWaiterCapacity {
			return
		}
	}
	s.localWaiters[key] = localWaiter{hostID: hostID, src: cloneSource(src), expires: now.Add(localWaitTTL)}
}

// takeLocalWaiter removes the viewer waiting for a LAN refresh. The refreshed
// reply is a supplement, so a repeat would only be duplicate traffic.
func (s *Server) takeLocalWaiter(hostID, nonce string) *localWaiter {
	key := localWaiterKey(hostID, nonce)
	now := time.Now()
	s.localWaiterMu.Lock()
	defer s.localWaiterMu.Unlock()
	waiter, ok := s.localWaiters[key]
	if !ok {
		return nil
	}
	delete(s.localWaiters, key)
	if now.After(waiter.expires) || waiter.hostID != hostID {
		return nil
	}
	return &waiter
}

// handleLocalAddr forwards a host's current LAN candidates to the viewer that
// asked for them. Only a host that currently owns the id and answers from its
// registered address, with a signature over the exact list and nonce, can
// refresh a punch in progress.
func (s *Server) handleLocalAddr(msg wire.Message, src *net.UDPAddr) {
	if len(msg.Nonce) < 8 || len(msg.Nonce) > 64 || msg.Timestamp == 0 {
		return
	}
	age := time.Now().Unix() - msg.Timestamp
	if age < -60 || age > 60 {
		return
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok || !hostSourceMatches(host, src) {
		return
	}
	pub, err := identity.ParsePublicKey(host.PublicKey)
	if err != nil {
		return
	}
	addrs := make([]string, 0, len(msg.LocalAddrs)+len(msg.LocalAddrsV6))
	addrs = append(append(addrs, msg.LocalAddrs...), msg.LocalAddrsV6...)
	if !identity.Verify(pub, identity.LocalAddrMessage(host.ID, msg.Nonce, addrs, msg.Timestamp), msg.Signature) {
		return
	}
	waiter := s.takeLocalWaiter(host.ID, msg.Nonce)
	if waiter == nil {
		return
	}
	s.log.Debug("host reported current LAN candidates", "host", host.ID,
		"viewer", waiter.src.String(), "addrs", addrs)
	reply := s.punchReply(host, waiter.src, msg.Nonce)
	reply.LocalAddrs = msg.LocalAddrs
	reply.LocalAddrsV6 = msg.LocalAddrsV6
	reply.IsLocal = true
	s.reply(waiter.src, reply)
}

// hostSourceMatches accepts a hint only from the address the registry currently
// holds for this host. A viewer cannot fabricate the decision, and a spoofed
// source cannot redirect it.
func hostSourceMatches(host registry.Host, src *net.UDPAddr) bool {
	if src == nil {
		return false
	}
	for _, text := range []string{host.Candidate, host.CandidateV6} {
		if text == "" {
			continue
		}
		addr, err := net.ResolveUDPAddr("udp", text)
		if err == nil && addr != nil && addr.Port == src.Port && addr.IP.Equal(src.IP) {
			return true
		}
	}
	return false
}

// handleRelayHint forwards the host's relay decision to the viewer that asked
// to punch. RustDesk carries the same decision as RelayResponse, which lets a
// symmetric-NAT pair start the relay connection at once instead of waiting out
// a punch window that cannot succeed. The relay address itself already
// travelled in the punch reply, so the hint only needs to name the session.
func (s *Server) handleRelayHint(msg wire.Message, src *net.UDPAddr) {
	if len(msg.Nonce) < 8 || len(msg.Nonce) > 64 {
		return
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok || host.RelayPort == 0 || !hostSourceMatches(host, src) {
		return
	}
	waiter := s.takePunchWaiter(host.ID, msg.Nonce)
	if waiter == nil {
		return
	}
	s.log.Debug("host chose the relay; forwarding to the viewer",
		"host", host.ID, "viewer", waiter.String())
	s.reply(waiter, wire.Message{
		Type:   wire.TypeRelayHint,
		HostID: host.ID,
		Nonce:  msg.Nonce,
	})
}

// validUpnpPort accepts 0 for "no mapping reported" and otherwise a port the
// peer could plausibly be reached on.
func validUpnpPort(port int) bool {
	return port >= 0 && port <= 65535
}

type limitWindow struct {
	start time.Time
	count int
}

type limiter struct {
	mu        sync.Mutex
	max       int
	span      time.Duration
	windows   map[string]*limitWindow
	lastSwept time.Time
}

func newLimiter(max int, span time.Duration) *limiter {
	return &limiter{max: max, span: span, windows: make(map[string]*limitWindow)}
}

func (l *limiter) allow(ip string) bool {
	if l.max <= 0 {
		return true
	}
	now := time.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	if now.Sub(l.lastSwept) > l.span*6 {
		for key, w := range l.windows {
			if now.Sub(w.start) > l.span {
				delete(l.windows, key)
			}
		}
		l.lastSwept = now
	}
	w, ok := l.windows[ip]
	if !ok || now.Sub(w.start) > l.span {
		l.windows[ip] = &limitWindow{start: now, count: 1}
		return true
	}
	if w.count >= l.max {
		return false
	}
	w.count++
	return true
}

// NormalizeRelay accepts either a hostname or an IP for the advertised relay
// address and joins it with a port. IPv6 literals get brackets.
func NormalizeRelay(host string, port int) string {
	return net.JoinHostPort(strings.Trim(host, "[]"), fmt.Sprint(port))
}

// observedCandidate splits a source address into the IPv4 and IPv6 candidate
// fields. UDPAddr.String already renders IPv4-mapped addresses (what a
// dual-stack socket reports for an IPv4 peer) as dotted quads, so only the
// family needs classifying here.
func observedCandidate(src *net.UDPAddr) (v4, v6 string) {
	if src == nil {
		return "", ""
	}
	if src.IP.To4() != nil {
		return src.String(), ""
	}
	return "", src.String()
}

// validSelfV6 accepts a self-reported IPv6 endpoint only in 2000::/3, the
// range RustDesk uses for its v6 punch socket. Loopback, link-local, ULA and
// multicast addresses cannot be reached by a peer, so one agent must not be
// able to point another at them.
func validSelfV6(text string) string {
	host, portText, err := net.SplitHostPort(text)
	if err != nil {
		return ""
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return ""
	}
	ip := net.ParseIP(host)
	if ip == nil || ip.To4() != nil {
		return ""
	}
	v6 := ip.To16()
	if v6 == nil || v6[0]&0xe0 != 0x20 {
		return ""
	}
	return net.JoinHostPort(ip.String(), portText)
}

// maxViewerLocalAddrs caps the LAN endpoints forwarded from a viewer so a
// single punch request cannot inflate the control datagram handed to the host.
const maxViewerLocalAddrs = 8

// viewerLocalAddrs hands a viewer's LAN candidates to the host only when the
// two peers share a network: hbbs's same_intranet, which accepts both observed
// addresses inside the operator's mask or one identical address. On any other
// path those addresses describe a different network: punching them would probe
// a stranger's LAN, and only on the same network can a host reach them at all.
// RustDesk draws the same line with is_local and its FetchLocalAddr exchange.
func (s *Server) viewerLocalAddrs(host registry.Host, src *net.UDPAddr, local4, local6 []string) ([]string, []string) {
	if !s.sameIntranet(host, src) {
		return nil, nil
	}
	return validLocalAddrs(local4, false), validLocalAddrs(local6, true)
}

// validLocalAddrs keeps unicast host:port endpoints of the requested family.
// The values are only routing hints from an authenticated viewer, so they are
// normalized before they reach a host as punch targets.
func validLocalAddrs(texts []string, wantV6 bool) []string {
	var out []string
	for _, text := range texts {
		if len(out) >= maxViewerLocalAddrs {
			break
		}
		host, portText, err := net.SplitHostPort(text)
		if err != nil {
			continue
		}
		port, err := strconv.Atoi(portText)
		if err != nil || port < 1 || port > 65535 {
			continue
		}
		ip := net.ParseIP(host)
		if ip == nil || ip.IsUnspecified() || ip.IsMulticast() ||
			ip.IsLinkLocalUnicast() || ip.IsLoopback() {
			continue
		}
		if wantV6 != (ip.To4() == nil) {
			continue
		}
		out = append(out, net.JoinHostPort(ip.String(), strconv.Itoa(port)))
	}
	return out
}
