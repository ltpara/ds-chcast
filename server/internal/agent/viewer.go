package agent

import (
	"context"
	"crypto/ed25519"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/upnp"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	ModeAuto    = "auto"
	ModeDirect  = "direct"
	ModeRelay   = "relay"
	ModeCarrier = "carrier"
	// ModeTCP forces the punched TCP path. It is the transport a network that
	// drops UDP needs, and it is also what makes the fallback testable without
	// an environment that blocks UDP.
	ModeTCP = "tcp"
)

type ViewerConfig struct {
	ServerURL string
	// Servers seeds the failover list before the first lookup.
	Servers   []string
	ServerKey string
	// LinkKey is the lookup and punch credential ordinary viewers hold. Empty
	// reuses ServerKey.
	LinkKey string
	// TLS configures HTTPS to the rendezvous API: a private CA bundle, a
	// client certificate for mTLS, or both. Nil uses the system roots.
	TLS *tls.Config
	// Proxy routes the lookup and the shared-carrier link through an outbound
	// proxy. Direct and relay paths carry UDP, which an HTTP(S) proxy or this
	// SOCKS5 client cannot forward, so a proxy forces carrier mode and fails
	// instead of falling back to an unproxied path.
	Proxy *netproxy.Proxy
	// ServerPublicKey pins the rendezvous server identity. When set, lookups
	// must carry a valid server-signed binding for the host.
	ServerPublicKey string
	HostID          string
	ListenAddr      string
	Mode            string
	// DirectTimeout > 0 explicitly caps the whole auto attempt, including coordination.
	// Zero uses RustDesk's separate coordination and adaptive connection budgets.
	DirectTimeout    time.Duration
	DirectHistoryDir string
	Logger           *slog.Logger
	// UPnP maps the viewer's UDP socket on the local gateway so the host can
	// punch towards a stable external port. It is opt-in.
	UPnP bool
	// OnPath receives every direct/relay change, starting with the initial
	// path once StartViewer has resolved the host. It is called from the read
	// or timer goroutine, so it must not block.
	OnPath func(PathInfo)
}

// PathInfo is the viewer agent's current forwarding path. Path is "direct",
// "relay", "carrier" or "connecting" while the first direct probe is still
// unanswered. NAT is the host's advertised NAT classification, OwnNAT the
// viewer's own; Candidates is the number of direct addresses the viewer is
// probing. Carrier and CarrierToken are the shared-carrier endpoint and the
// per-lease credential for it, empty when the server has no shared carrier.
type PathInfo struct {
	Path         string `json:"path"`
	Remote       string `json:"remote,omitempty"`
	Direct       string `json:"direct,omitempty"`
	Relay        string `json:"relay,omitempty"`
	NAT          string `json:"nat,omitempty"`
	OwnNAT       string `json:"own_nat,omitempty"`
	Candidates   int    `json:"candidates,omitempty"`
	Carrier      string `json:"carrier,omitempty"`
	CarrierToken string `json:"carrier_token,omitempty"`
	// Transport is "tcp" while the punched TCP link carries the session, and
	// "udp" otherwise. A relayed viewer stays on UDP; the carrier has its own
	// Path value.
	Transport string `json:"transport,omitempty"`
}

type ViewerAgent struct {
	cfg ViewerConfig
	// conn is the loopback listener the local QUIC client talks to. out is the
	// routable socket for rendezvous, punch, relay and direct traffic: a socket
	// bound to 127.0.0.1 cannot reach public addresses on Windows
	// (WSAENETUNREACH), and the host must punch towards the port out exposes.
	conn    *net.UDPConn
	out     *net.UDPConn
	api     *apiClient
	servers *serverPool
	log     *slog.Logger

	ctx           context.Context
	cancel        context.CancelFunc
	directCtx     context.Context
	directCancel  context.CancelFunc
	pendingDirect int
	directFailed  bool
	historyPath   string
	historyMu     sync.Mutex

	punch  *net.UDPAddr
	punch6 *net.UDPAddr
	direct *net.UDPAddr
	relay  *net.UDPAddr

	// carrier and carrierV6 come from the authenticated lookup. The token
	// authorizes this viewer's slot while the host's lease is live.
	carrier      string
	carrierV6    string
	carrierToken string
	// relayOnly means the server hides direct candidates; transport is its
	// hint (direct, relay or carrier). carrierMode forwards local datagrams
	// through the shared TCP carrier instead of a UDP address. carrierMode is
	// guarded by mu because auto mode can switch to it after a direct attempt.
	relayOnly   bool
	transport   string
	carrierMode bool
	carrierConn net.Conn
	carrierMu   sync.Mutex
	carrierSend *carrier.FrameWriter
	// carrierUsed records the endpoint the live link reached, which can be
	// either address family. It is empty while no link is up.
	carrierUsed atomic.Value

	mu                 sync.Mutex
	remote             *net.UDPAddr
	local              *net.UDPAddr
	latched            bool
	timer              *time.Timer
	directOK           atomic.Bool
	directLost         bool // A failed established path requires a new viewer/lookup.
	lastDirectReceive  time.Time
	directKeepaliveTID string
	punched            atomic.Bool
	// punchActive is true while this viewer probes for the host; only then may
	// a peer probe be acknowledged, which is the same window RustDesk keeps its
	// punch loop open.
	punchActive atomic.Bool
	relayAuthOK atomic.Bool
	relayAuth   sync.Once
	fellBack    bool
	nonce       string
	// tcpPunch is the rendezvous service's TCP punch coordination endpoint and
	// hostPub verifies the host hello on the punched socket. Both are empty
	// when the server did not advertise a TCP punch.
	tcpPunch *net.TCPAddr
	hostPub  ed25519.PublicKey
	// tcpConn/tcpSend/tcpStop hold the live punched TCP link. tcpRemote is the
	// host endpoint it reached and is guarded by mu because pathLocked reads it.
	tcpMu     sync.Mutex
	tcpConn   net.Conn
	tcpSend   *carrier.FrameWriter
	tcpStop   chan struct{}
	tcpRemote string
	tcpActive atomic.Bool
	// punchTID is the transaction id of the current probe round. An
	// acknowledgement must echo it to confirm the path.
	punchTID    string
	lastPathKey string
	candidates  []*net.UDPAddr
	udpLocal    bool
	// natType is the host's advertised NAT classification; ownNAT is the
	// viewer's own, measured against the rendezvous and relay ports.
	natType  string
	ownNAT   string
	upnpPort int
	// selfV6 is this viewer's own IPv6 UDP endpoint, announced in the punch
	// request so the host can punch back over IPv6. It is empty when this
	// machine has no global IPv6 route, which also disables v6 candidates.
	selfV6 string
	// localAddrs are this viewer's LAN endpoints, announced in the punch
	// request. The rendezvous service forwards them to the host only when it
	// observed both peers behind the same public address, where the host can
	// reach them; that is the same-network case a NAT without hairpinning
	// cannot carry. RustDesk requests the host's LAN address the same way.
	localAddrs   []string
	localAddrsV6 []string

	natProbeMu    sync.Mutex
	natProbeNonce string
	natProbeSeen  map[string]*net.UDPAddr
}

// proxyViewerModeError refuses a proxy setup that would need an unproxied UDP
// path. Carrier is the only transport an HTTP(S) or SOCKS5 CONNECT proxy can
// carry here; failing beats silently connecting around the proxy.
func proxyViewerModeError(hostID, mode string, carrierReady bool) error {
	if mode == ModeDirect || mode == ModeTCP {
		return fmt.Errorf("--mode %s dials the host directly and cannot use an outbound proxy", mode)
	}
	if !carrierReady {
		return fmt.Errorf("outbound proxy needs the server's TCP carrier, but host %s has none; direct and relay UDP cannot traverse the proxy", hostID)
	}
	return nil
}

// listenUpstream opens the routable socket that carries rendezvous, punch,
// relay and direct traffic. A wildcard IPv6 socket is dual-stack where the OS
// allows it, so one socket serves both families; hosts without IPv6 fall back
// to IPv4.
func listenUpstream() (*net.UDPConn, error) {
	if conn, err := net.ListenUDP("udp", &net.UDPAddr{}); err == nil {
		return conn, nil
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
	if err != nil {
		return nil, fmt.Errorf("open upstream socket: %w", err)
	}
	return conn, nil
}

func (v *ViewerAgent) closeSockets() {
	if v.out != nil {
		v.out.Close()
	}
	if v.conn != nil {
		v.conn.Close()
	}
}

func StartViewer(parent context.Context, cfg ViewerConfig) (*ViewerAgent, error) {
	if cfg.ServerURL == "" || cfg.HostID == "" {
		return nil, fmt.Errorf("viewer agent needs --server and --id")
	}
	if cfg.Mode == "" {
		cfg.Mode = ModeAuto
	}
	if cfg.Mode != ModeAuto && cfg.Mode != ModeDirect && cfg.Mode != ModeRelay && cfg.Mode != ModeCarrier && cfg.Mode != ModeTCP {
		return nil, fmt.Errorf("--mode must be auto, direct, relay, carrier or tcp")
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.ServerPublicKey != "" {
		if _, err := identity.ParsePublicKey(cfg.ServerPublicKey); err != nil {
			return nil, fmt.Errorf("--server-pubkey: %w", err)
		}
	}
	servers, err := newServerPool(cfg.ServerURL, cfg.Servers)
	if err != nil {
		return nil, fmt.Errorf("rendezvous servers: %w", err)
	}
	listen, err := net.ResolveUDPAddr("udp", cfg.ListenAddr)
	if err != nil {
		return nil, fmt.Errorf("listen address %q: %w", cfg.ListenAddr, err)
	}
	conn, err := net.ListenUDP("udp", listen)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", cfg.ListenAddr, err)
	}
	out, err := listenUpstream()
	if err != nil {
		conn.Close()
		return nil, err
	}
	ctx, cancel := context.WithCancel(parent)
	v := &ViewerAgent{
		cfg:     cfg,
		conn:    conn,
		out:     out,
		api:     newAPIClient(servers.Current(), viewerAPIKey(cfg), cfg.TLS, cfg.Proxy),
		servers: servers,
		log:     cfg.Logger.With("agent", "viewer", "id", cfg.HostID),
		ctx:     ctx,
		cancel:  cancel,
		nonce:   newNonce(),
	}
	v.directCtx, v.directCancel = context.WithCancel(ctx)
	v.localAddrs, v.localAddrsV6 = splitByFamily(localCandidates(out.LocalAddr().(*net.UDPAddr)))

	host, err := v.lookupWithFailover(ctx)
	if err != nil {
		v.closeSockets()
		cancel()
		return nil, &viewerStartError{err: fmt.Errorf("lookup: %w", err), retryable: retryableLookupError(err)}
	}
	if err := checkHostBinding(cfg.ServerPublicKey, cfg.HostID, host); err != nil {
		v.closeSockets()
		cancel()
		return nil, err
	}
	if host.PublicKey != "" {
		if pub, err := identity.ParsePublicKey(host.PublicKey); err == nil {
			v.hostPub = pub
		} else {
			v.log.Warn("host public key is not usable for a TCP punch", "error", err)
		}
	}
	if host.ConfigSerial > 0 && len(host.RendezvousServers) > 0 {
		if v.servers.Apply(host.ConfigSerial, host.RendezvousServers) {
			v.log.Info("rendezvous configuration updated",
				"serial", host.ConfigSerial, "servers", v.servers.List())
		}
	}
	if !host.Online {
		v.closeSockets()
		cancel()
		return nil, &viewerStartError{err: fmt.Errorf("host %s is registered but offline", cfg.HostID), retryable: true}
	}
	if cfg.UPnP && cfg.Proxy == nil {
		mapCtx, mapCancel := context.WithTimeout(ctx, 5*time.Second)
		mapper, mapErr := upnp.Map(mapCtx, v.out.LocalAddr().(*net.UDPAddr).Port, "chast viewer")
		mapCancel()
		if mapErr != nil {
			v.log.Warn("UPnP mapping unavailable", "error", mapErr)
		} else {
			v.upnpPort = mapper.ExternalPort()
			v.log.Info("UPnP port mapped", "external_port", v.upnpPort,
				"internal_port", v.out.LocalAddr().(*net.UDPAddr).Port,
				"gateway", mapper.Gateway().ControlURL)
			go renewUPnP(ctx, v.log, mapper)
		}
	}
	if host.Software.updateAvailable() {
		v.log.Info("software update available",
			"version", host.Software.Version, "current", Version, "url", host.Software.URL)
	}
	v.punch, err = resolveUDP(host.Punch)
	if err != nil {
		v.closeSockets()
		cancel()
		return nil, fmt.Errorf("punch address: %w", err)
	}
	v.tcpPunch = resolveTCP(host.TCPPunch)
	// A viewer bound to an IPv6-only socket cannot reach the IPv4 rendezvous
	// port, so it needs the IPv6 endpoint to ask for a punch at all.
	if punch6, err := resolveUDP(host.PunchV6); err == nil && punch6 != nil &&
		familyAllowed(v.out.LocalAddr().(*net.UDPAddr).IP, punch6.IP) {
		v.punch6 = punch6
	}
	candidateTexts := append([]string{}, host.Candidates...)
	if host.Candidate != "" {
		candidateTexts = append(candidateTexts, host.Candidate)
	}
	if host.CandidateV6 != "" {
		candidateTexts = append(candidateTexts, host.CandidateV6)
	}
	candidateTexts = append(candidateTexts, host.LocalAddrs...)
	candidateTexts = append(candidateTexts, host.LocalAddrsV6...)
	if mapped := candidateWithUpnpPort(host.Candidate, host.UpnpPort); mapped != "" {
		candidateTexts = append(candidateTexts, mapped)
	}
	var hostV6 net.IP
	if addr, err := resolveUDP(host.CandidateV6); err == nil && addr != nil {
		hostV6 = addr.IP
	}
	// The v6 endpoint is learned from the route towards the host, exactly as
	// RustDesk's get_ipv6_socket derives local_addr_v6. A v4-bound socket or a
	// host without an IPv6 route leaves it empty, and then the v6 candidates
	// are dropped: there is no path to probe them from.
	v.selfV6 = selfV6Endpoint(v.out.LocalAddr().(*net.UDPAddr), hostV6)
	v.candidates = v.mergeDirect(candidateTexts)
	if len(v.candidates) > 0 {
		v.direct = v.candidates[0]
	}
	v.initDirectHistory()
	v.natType = host.NATType

	v.carrier = host.Carrier
	v.carrierV6 = host.CarrierV6
	v.carrierToken = host.CarrierToken
	v.relayOnly = host.RelayOnly
	v.transport = host.Transport
	relayTexts := host.Relays
	if len(relayTexts) == 0 && host.Relay != "" {
		relayTexts = []string{host.Relay}
	}
	if cfg.Proxy == nil {
		v.relay = selectRelayAddr(v.out.LocalAddr().(*net.UDPAddr), relayTexts)
		if v.relay == nil && len(relayTexts) > 0 {
			v.closeSockets()
			cancel()
			return nil, fmt.Errorf("relay address %q is not reachable from %s", strings.Join(relayTexts, ", "), v.out.LocalAddr().String())
		}
	}
	carrierReady := v.carrierToken != "" && len(v.carrierCandidates()) > 0
	tcpReady := v.tcpPunch != nil && v.hostPub != nil
	useCarrier := false

	if cfg.Proxy != nil {
		if err := proxyViewerModeError(cfg.HostID, cfg.Mode, carrierReady); err != nil {
			v.closeSockets()
			cancel()
			return nil, err
		}
		v.log.Info("outbound proxy forces the TCP carrier", "proxy", cfg.Proxy.Redacted())
		if cfg.UPnP {
			v.log.Info("UPnP skipped while an outbound proxy is configured")
		}
		useCarrier = true
	} else {
		switch cfg.Mode {
		case ModeCarrier:
			if !carrierReady {
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("host has no shared carrier address")
			}
			useCarrier = true
		case ModeRelay:
			if host.Transport == "carrier" && carrierReady {
				useCarrier = true
			} else if v.relay != nil {
				v.remote = v.relay
				v.latched = true
			} else if carrierReady {
				useCarrier = true
			} else {
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("host has no relay or carrier address")
			}
		case ModeDirect:
			if host.RelayOnly {
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("server forces relay; direct mode is unavailable")
			}
			if v.direct == nil && (v.tcpPunch == nil || v.hostPub == nil) {
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("host has no direct candidate")
			}
			if v.direct != nil {
				v.remote = v.direct
				v.latched = true
			}
		case ModeTCP:
			if host.RelayOnly {
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("server forces relay; tcp mode is unavailable")
			}
			if v.tcpPunch == nil || v.hostPub == nil {
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("host has no verifiable TCP punch endpoint")
			}
		default:
			switch {
			case !host.RelayOnly && (v.direct != nil || tcpReady):
				v.remote = v.direct
			case host.Transport == "carrier" && carrierReady:
				useCarrier = true
			case v.relay != nil:
				v.remote = v.relay
				v.latched = true
				v.log.Warn("host has no direct candidate; using relay")
			case carrierReady:
				useCarrier = true
			default:
				v.closeSockets()
				cancel()
				return nil, fmt.Errorf("host has neither a direct, relay nor carrier address")
			}
		}
	}
	v.carrierMode = useCarrier
	startTCP := !useCarrier && cfg.Mode != ModeRelay && !host.RelayOnly && tcpReady
	startUDP := !useCarrier && cfg.Mode != ModeRelay && cfg.Mode != ModeTCP &&
		!host.RelayOnly && v.punch != nil && v.direct != nil
	startOnRelay := v.remote != nil && v.relay != nil && sameAddr(v.remote, v.relay)
	path := "relay " + addrText(v.relay)
	if useCarrier {
		path = "carrier " + v.carrierEndpoint()
	} else if cfg.Mode == ModeTCP || (startTCP && v.direct == nil) {
		path = "tcp punch " + v.tcpPunch.String()
	} else if v.remote != nil && sameAddr(v.remote, v.direct) {
		path = "direct " + addrText(v.direct)
	}
	v.log.Info("viewer agent listening", "on", conn.LocalAddr().String(),
		"upstream", v.out.LocalAddr().String(), "path", path)
	if startTCP {
		v.pendingDirect++
	}
	if startUDP {
		v.pendingDirect++
	}
	if (startTCP || startUDP) && cfg.Mode == ModeAuto && cfg.DirectTimeout > 0 {
		v.mu.Lock()
		v.timer = time.AfterFunc(cfg.DirectTimeout, v.onDirectTimeout)
		v.mu.Unlock()
	}

	go v.readLoop()
	go v.localLoop()
	go v.natProbeLoop()
	if startOnRelay {
		v.startRelayAuth()
	}
	if useCarrier {
		go v.carrierLoop()
	} else {
		if startUDP {
			v.sendPunchRequest()
			go v.punchLoop()
		}
		// The TCP punch races beside the UDP punch, as RustDesk races its TCP
		// direct transport beside UDP. Relay starts after all eligible attempts
		// finish, unless the user explicitly caps the whole attempt.
		if startTCP {
			go v.tcpPunchLoop()
		}
	}
	v.reportPath()
	return v, nil
}

// lookupWithFailover tries every known endpoint once so a viewer is not stuck
// on a broken node from its seed list.
func (v *ViewerAgent) lookupWithFailover(ctx context.Context) (lookupResult, error) {
	var lastErr error
	for attempt := 0; attempt < v.servers.Len(); attempt++ {
		attemptCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		host, err := v.api.lookup(attemptCtx, v.cfg.HostID)
		cancel()
		if err == nil {
			return host, nil
		}
		lastErr = err
		if next, ok := v.servers.Next(); ok {
			v.api.setBase(next)
			v.log.Warn("lookup failed; trying the next rendezvous server", "server", next, "error", err)
		}
	}
	if lastErr == nil {
		lastErr = errors.New("no rendezvous server answered")
	}
	return lookupResult{}, lastErr
}

func (v *ViewerAgent) LocalAddr() net.Addr { return v.conn.LocalAddr() }

// PathInfo returns a snapshot of the forwarding path. It is safe to call from
// the embedding application while the agent keeps running.
func (v *ViewerAgent) PathInfo() PathInfo {
	v.mu.Lock()
	defer v.mu.Unlock()
	return v.pathLocked()
}

func (v *ViewerAgent) pathLocked() PathInfo {
	path := "connecting"
	remote := addrValue(v.remote)
	carrier := v.carrierEndpoint()
	transport := "udp"
	if v.carrierMode {
		path = "carrier"
		remote = carrier
	} else if v.tcpActive.Load() {
		path = "direct"
		remote = v.tcpRemote
		transport = "tcp"
	} else if v.remote != nil {
		if v.relay != nil && sameAddr(v.remote, v.relay) {
			path = "relay"
		} else if v.directOK.Load() {
			path = "direct"
		}
	}
	return PathInfo{
		Path:         path,
		Remote:       remote,
		Direct:       addrValue(v.direct),
		Relay:        addrValue(v.relay),
		NAT:          v.natType,
		OwnNAT:       v.ownNAT,
		Candidates:   len(v.candidates),
		Carrier:      carrier,
		CarrierToken: v.carrierToken,
		Transport:    transport,
	}
}

func (v *ViewerAgent) reportPath() {
	v.mu.Lock()
	info := v.pathLocked()
	key := info.Path + "|" + info.Remote + "|" + info.NAT + "|" + info.OwnNAT + "|" +
		strconv.Itoa(info.Candidates) + "|" + info.Transport
	changed := key != v.lastPathKey
	v.lastPathKey = key
	v.mu.Unlock()
	if changed && v.cfg.OnPath != nil {
		v.cfg.OnPath(info)
	}
}

func addrValue(a *net.UDPAddr) string {
	if a == nil {
		return ""
	}
	return a.String()
}

func (v *ViewerAgent) Close() {
	v.mu.Lock()
	if v.timer != nil {
		v.timer.Stop()
	}
	v.mu.Unlock()
	v.cancel()
	v.tcpMu.Lock()
	if v.tcpConn != nil {
		v.tcpConn.Close()
	}
	v.tcpMu.Unlock()
	v.carrierMu.Lock()
	if v.carrierConn != nil {
		v.carrierConn.Close()
	}
	v.carrierMu.Unlock()
	v.closeSockets()
}

func (v *ViewerAgent) sendPunchRequest() {
	if v.punch == nil && v.punch6 == nil {
		return
	}
	v.mu.Lock()
	ownNAT := v.ownNAT
	v.mu.Unlock()
	msg := wire.Message{Type: wire.TypePunchRequest, HostID: v.cfg.HostID, Nonce: v.nonce,
		UpnpPort: v.upnpPort, NATType: ownNAT, SelfV6: v.selfV6,
		LocalAddrs: v.localAddrs, LocalAddrsV6: v.localAddrsV6}
	if key := viewerAPIKey(v.cfg); key != "" {
		msg.Timestamp = time.Now().Unix()
		msg.Auth = identity.PunchAuth(key, v.cfg.HostID, v.nonce, msg.Timestamp)
	}
	encoded := wire.Encode(msg)
	if v.punch != nil {
		v.out.WriteToUDP(encoded, v.punch)
	}
	if v.punch6 != nil {
		v.out.WriteToUDP(encoded, v.punch6)
	}
}

const (
	relayAuthAttempts = 6
	relayAuthInterval = 500 * time.Millisecond
	// The relay trusts a viewer proof for viewerIdle (60s). Renew well inside
	// that window so an active session is not cut off at the authorization
	// deadline, with room for a lost renewal.
	relayAuthRefresh = 20 * time.Second
)

// startRelayAuth proves the lookup token to the relay before the first QUIC
// datagram. It is safe to call on both the initial relay path and a fallback.
func (v *ViewerAgent) startRelayAuth() {
	if v.relayAddress() == nil {
		return
	}
	v.relayAuth.Do(func() { go v.relayAuthLoop() })
}

func (v *ViewerAgent) relayAddress() *net.UDPAddr {
	v.mu.Lock()
	defer v.mu.Unlock()
	return cloneAddr(v.relay)
}

func (v *ViewerAgent) relayAuthLoop() {
	if v.carrierToken == "" {
		v.log.Warn("relay did not return a viewer token")
		return
	}
	msg := wire.Encode(wire.Message{
		Type:   wire.TypeRelayViewer,
		HostID: v.cfg.HostID,
		Token:  v.carrierToken,
	})
	for attempt := 1; ; attempt++ {
		relay := v.relayAddress()
		if relay == nil {
			return
		}
		if _, err := v.out.WriteToUDP(msg, relay); err != nil {
			v.log.Debug("relay viewer auth write", "error", err)
		}
		wait := relayAuthRefresh
		if attempt < relayAuthAttempts && !v.relayAuthOK.Load() {
			wait = relayAuthInterval
		}
		select {
		case <-v.ctx.Done():
			return
		case <-time.After(wait):
		}
	}
}

const (
	// RustDesk retries the hole punch request three times with 3s, 6s and 9s
	// deadlines; the response normally ends the request phase at the first
	// round trip.
	punchRequestAttempts = 3
	punchRequestStep     = 3 * time.Second
	// punchProbeWindow is RustDesk punch_udp's MAX_TIME. The probe cadence
	// backs off from 20ms by 1.5x up to 200ms.
	punchProbeWindow = 3 * time.Second
	punchProbeMinGap = 20 * time.Millisecond
	punchProbeMaxGap = 200 * time.Millisecond
	punchTIDLen      = 16
)

func (v *ViewerAgent) punchLoop() {
	defer v.directAttemptDone()
	started := time.Now()
	// The first request left with the lookup; later attempts re-ask the server
	// with RustDesk's increasing deadlines while the response is outstanding.
	for attempt := 1; attempt <= punchRequestAttempts; attempt++ {
		if v.punched.Load() {
			break
		}
		if attempt > 1 {
			v.sendPunchRequest()
		}
		if !v.waitPunchReply(time.Duration(attempt) * punchRequestStep) {
			return
		}
	}
	if !v.punched.Load() {
		v.log.Warn("no punch reply from the rendezvous service", "nonce", v.nonce)
		return
	}
	v.probeCandidatesFor(v.connectWait(time.Since(started), v.udpIsLocal()))
}

func (v *ViewerAgent) waitPunchReply(wait time.Duration) bool {
	deadline := time.Now().Add(wait)
	for !v.punched.Load() {
		if v.directContext().Err() != nil {
			return false
		}
		if time.Now().After(deadline) {
			return true
		}
		select {
		case <-v.directContext().Done():
			return false
		case <-time.After(25 * time.Millisecond):
		}
	}
	return true
}

// probeCandidates is RustDesk's punch_udp on the connector side: probe with a
// fresh transaction id, back off from 20ms to 200ms, and stop only when that id
// comes back acknowledged or real peer data arrives. A datagram that merely
// arrived proves nothing about the return path.
func (v *ViewerAgent) probeCandidatesFor(wait time.Duration) {
	tid := newNonce()
	v.mu.Lock()
	v.punchTID = tid
	v.mu.Unlock()
	v.punchActive.Store(true)
	defer v.punchActive.Store(false)
	deadline := time.Now().Add(minDuration(punchProbeWindow, wait))
	gap := punchProbeMinGap
	for {
		if v.directContext().Err() != nil || v.directOK.Load() || time.Now().After(deadline) {
			if v.directContext().Err() == nil && !v.directOK.Load() {
				v.log.Info("direct punch was not confirmed", "window", punchProbeWindow)
			}
			return
		}
		v.sendPunchProbe(tid)
		select {
		case <-v.directContext().Done():
			return
		case <-time.After(gap):
		}
		gap = nextPunchGap(gap)
	}
}

func nextPunchGap(gap time.Duration) time.Duration {
	gap = time.Duration(float64(gap) * 1.5)
	if gap > punchProbeMaxGap {
		return punchProbeMaxGap
	}
	return gap
}

func (v *ViewerAgent) sendPunchProbe(tid string) {
	probe := wire.Encode(wire.Message{Type: wire.TypePunchProbe, Nonce: v.nonce, TID: tid})
	for _, peer := range v.getCandidates() {
		v.out.WriteToUDP(probe, peer)
	}
}

// answerPunchProbe echoes a peer's transaction id while this viewer is
// punching. RustDesk runs the same loop on both ends; a connector that stayed
// mute would leave the host probing a hole it can never confirm.
func (v *ViewerAgent) answerPunchProbe(src *net.UDPAddr, msg wire.Message) {
	if len(msg.TID) != punchTIDLen || !v.punchActive.Load() {
		return
	}
	ack := wire.Encode(wire.Message{Type: wire.TypePunchAck, TID: msg.TID})
	v.out.WriteToUDP(ack, src)
}

// confirmPunchAck accepts only the acknowledgement of this round's probe: it is
// the one message that proves the path carries traffic in both directions.
func (v *ViewerAgent) confirmPunchAck(src *net.UDPAddr, msg wire.Message) {
	v.mu.Lock()
	if v.directOK.Load() {
		if !v.tcpActive.Load() && !v.fellBack && !v.directLost &&
			v.directKeepaliveTID != "" && msg.TID == v.directKeepaliveTID && sameAddr(src, v.direct) {
			v.lastDirectReceive = time.Now()
			v.directKeepaliveTID = ""
		}
		v.mu.Unlock()
		return
	}
	tid := v.punchTID
	v.mu.Unlock()
	if tid == "" || msg.TID != tid {
		return
	}
	v.onDirectPacket(src)
}

const (
	natProbeAttempts = 3
	natProbeTimeout  = 750 * time.Millisecond
)

// natProbeLoop classifies this viewer's own NAT the way the host does: the
// public source the rendezvous port sees is compared with the one the relay
// port sees. A symmetric NAT allocates a different port per destination, so
// punching the observed v4 candidate is hopeless and the relay is the honest
// path. The probe runs beside the first punch attempts; until it finishes the
// punch request carries no NAT type and the host treats it as unknown, which
// is exactly RustDesk's behaviour before its background NAT test completes.
func (v *ViewerAgent) natProbeLoop() {
	if v.cfg.Proxy != nil {
		// A proxy hides the UDP source entirely; RustDesk treats proxied
		// sessions as symmetric for the same reason.
		v.setOwnNAT(NATSymmetric)
		return
	}
	if v.punch == nil || v.relayAddress() == nil {
		return
	}
	for attempt := 0; attempt < natProbeAttempts; attempt++ {
		if v.probeNAT() {
			return
		}
		if v.ctx.Err() != nil {
			return
		}
		select {
		case <-v.ctx.Done():
			return
		case <-time.After(time.Duration(attempt+1) * time.Second):
		}
	}
}

// probeNAT sends one nonce to both observation ports and waits for both
// replies. One reply is not enough: comparing a port with itself would call
// every NAT asymmetric.
func (v *ViewerAgent) probeNAT() bool {
	nonce := newNonce()
	v.natProbeMu.Lock()
	v.natProbeNonce = nonce
	v.natProbeSeen = make(map[string]*net.UDPAddr, 2)
	v.natProbeMu.Unlock()

	probe := wire.Encode(wire.Message{Type: wire.TypeNatProbe, Nonce: nonce})
	v.out.WriteToUDP(probe, v.punch)
	if relay := v.relayAddress(); relay != nil {
		v.out.WriteToUDP(probe, relay)
	}

	deadline := time.Now().Add(natProbeTimeout)
	for time.Now().Before(deadline) {
		v.natProbeMu.Lock()
		seen := make([]*net.UDPAddr, 0, len(v.natProbeSeen))
		for _, addr := range v.natProbeSeen {
			seen = append(seen, cloneAddr(addr))
		}
		v.natProbeMu.Unlock()
		if len(seen) >= 2 {
			if nat := classifyNAT(seen[0], seen[1]); nat != "" {
				v.setOwnNAT(nat)
				return true
			}
			return false
		}
		select {
		case <-v.ctx.Done():
			return false
		case <-time.After(10 * time.Millisecond):
		}
	}
	return false
}

func (v *ViewerAgent) setOwnNAT(nat string) {
	v.mu.Lock()
	if v.ownNAT == nat {
		v.mu.Unlock()
		return
	}
	v.ownNAT = nat
	v.mu.Unlock()
	v.log.Info("viewer NAT type detected", "nat", nat)
	v.reportPath()
}

// noteNatProbeAck records one observation of the NAT test. The source key is
// the observation port, so two replies from the same server endpoint can never
// stand in for the pair.
func (v *ViewerAgent) noteNatProbeAck(src *net.UDPAddr, msg wire.Message) {
	addr, err := resolveUDP(msg.Candidate)
	if err != nil || addr == nil {
		return
	}
	v.natProbeMu.Lock()
	if v.natProbeSeen != nil && msg.Nonce == v.natProbeNonce {
		v.natProbeSeen[src.String()] = addr
	}
	v.natProbeMu.Unlock()
}

const (
	viewerCarrierDial      = 5 * time.Second
	viewerCarrierIdle      = 60 * time.Second
	viewerCarrierKeepalive = 30 * time.Second
	viewerCarrierRetryMax  = 5 * time.Second
)

// carrierCandidates orders the shared-carrier endpoints, preferring the address
// family this machine can reach. The server sends both when the relay listens
// on IPv4 and IPv6.
func (v *ViewerAgent) carrierCandidates() []string {
	return carrierCandidateOrder(v.carrier, v.carrierV6, preferIPv6Outbound())
}

// carrierEndpoint reports the endpoint in use, or the preferred one while the
// link is down.
func (v *ViewerAgent) carrierEndpoint() string {
	if used, ok := v.carrierUsed.Load().(string); ok && used != "" {
		return used
	}
	if candidates := v.carrierCandidates(); len(candidates) > 0 {
		return candidates[0]
	}
	return ""
}

// carrierLoop keeps the viewer's shared-carrier link alive. The link carries
// the same opaque QUIC datagrams as UDP; when it drops, the agent reconnects
// with backoff while the local client retransmits.
func (v *ViewerAgent) carrierLoop() {
	backoff := 500 * time.Millisecond
	for {
		started := time.Now()
		err := v.runCarrierLink()
		if v.ctx.Err() != nil {
			return
		}
		if err != nil {
			v.log.Warn("carrier link lost", "carrier", v.carrierEndpoint(), "error", err)
		}
		if time.Since(started) > 30*time.Second {
			backoff = 500 * time.Millisecond
		}
		select {
		case <-v.ctx.Done():
			return
		case <-time.After(backoff):
		}
		if backoff < viewerCarrierRetryMax {
			backoff *= 2
			if backoff > viewerCarrierRetryMax {
				backoff = viewerCarrierRetryMax
			}
		}
	}
}

func (v *ViewerAgent) runCarrierLink() error {
	var conn net.Conn
	var err error
	endpoint := ""
	for _, candidate := range v.carrierCandidates() {
		dialCtx, dialCancel := context.WithTimeout(v.ctx, viewerCarrierDial)
		conn, err = v.cfg.Proxy.DialContext(dialCtx, "tcp", candidate)
		dialCancel()
		if err == nil {
			endpoint = candidate
			break
		}
		if v.ctx.Err() != nil {
			return err
		}
		v.log.Warn("carrier dial failed", "carrier", candidate, "error", err)
	}
	if conn == nil {
		if err == nil {
			err = errors.New("host has no shared carrier address")
		}
		return err
	}
	hello, err := json.Marshal(carrier.SharedViewerHello{HostID: v.cfg.HostID, Token: v.carrierToken})
	if err != nil {
		conn.Close()
		return err
	}
	_ = conn.SetWriteDeadline(time.Now().Add(viewerCarrierDial))
	if _, err := io.WriteString(conn, carrier.MagicViewer+string(hello)+"\n"); err != nil {
		conn.Close()
		return err
	}
	send := carrier.NewFrameWriter(conn)
	stop := make(chan struct{})
	defer close(stop)
	v.carrierMu.Lock()
	v.carrierConn = conn
	v.carrierSend = send
	v.carrierMu.Unlock()
	v.carrierUsed.Store(endpoint)
	defer func() {
		v.carrierMu.Lock()
		if v.carrierConn == conn {
			v.carrierConn = nil
			v.carrierSend = nil
			v.carrierUsed.Store("")
		}
		v.carrierMu.Unlock()
		conn.Close()
	}()
	v.log.Info("carrier link established", "carrier", endpoint)
	go v.carrierKeepalive(conn, send, stop)

	reader := carrier.NewFrameReader(conn)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(viewerCarrierIdle))
		payload, err := reader.ReadFrame()
		if err != nil {
			return err
		}
		if len(payload) == 0 {
			continue
		}
		v.mu.Lock()
		local := cloneAddr(v.local)
		v.mu.Unlock()
		if local != nil {
			v.conn.WriteToUDP(payload, local)
		}
	}
}

func (v *ViewerAgent) carrierKeepalive(conn net.Conn, send *carrier.FrameWriter, stop <-chan struct{}) {
	ticker := time.NewTicker(viewerCarrierKeepalive)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-v.ctx.Done():
			return
		case <-ticker.C:
			_ = conn.SetWriteDeadline(time.Now().Add(viewerCarrierDial))
			if err := send.WriteFrame(nil); err != nil {
				conn.Close()
				return
			}
		}
	}
}

// writeCarrier forwards one datagram to the host. Data is dropped while the
// link is down; QUIC above the tunnel retransmits what matters.
func (v *ViewerAgent) writeCarrier(payload []byte) {
	v.carrierMu.Lock()
	conn, send := v.carrierConn, v.carrierSend
	v.carrierMu.Unlock()
	if conn == nil || send == nil {
		return
	}
	_ = conn.SetWriteDeadline(time.Now().Add(viewerCarrierDial))
	if err := send.WriteFrame(payload); err != nil {
		v.log.Warn("carrier write failed", "error", err)
		conn.Close()
	}
}

func (v *ViewerAgent) getCandidates() []*net.UDPAddr {
	v.mu.Lock()
	defer v.mu.Unlock()
	out := make([]*net.UDPAddr, 0, len(v.candidates))
	for _, addr := range v.candidates {
		if cloned := cloneAddr(addr); cloned != nil {
			out = append(out, cloned)
		}
	}
	return out
}

// mergeDirect resolves and ranks direct candidates, dropping the IPv6 ones
// when this viewer has no global IPv6 source. RustDesk only punches v6 when
// get_ipv6_socket produced an address, so advertising a v6 candidate here
// would schedule probes that cannot leave the machine.
func (v *ViewerAgent) mergeDirect(texts []string) []*net.UDPAddr {
	merged := mergeCandidates(v.out.LocalAddr().(*net.UDPAddr), texts)
	if v.selfV6 != "" {
		return merged
	}
	kept := merged[:0]
	for _, addr := range merged {
		if addr.IP.To4() != nil {
			kept = append(kept, addr)
		}
	}
	return kept
}

// isCandidate reports whether src is one of the addresses the rendezvous
// service handed out for the host.
func (v *ViewerAgent) isCandidate(src *net.UDPAddr) bool {
	v.mu.Lock()
	defer v.mu.Unlock()
	for _, addr := range v.candidates {
		if sameAddr(addr, src) {
			return true
		}
	}
	return false
}

// writeLocal hands one datagram to the local QUIC client, if it has attached.
func (v *ViewerAgent) writeLocal(payload []byte) {
	v.mu.Lock()
	local := cloneAddr(v.local)
	v.mu.Unlock()
	if local != nil {
		v.conn.WriteToUDP(payload, local)
	}
}

// localLoop reads from the loopback listener and forwards client datagrams
// upstream. Only loopback sources are accepted: the upstream socket is the
// one remote peers know about.
func (v *ViewerAgent) localLoop() {
	buf := make([]byte, 65535)
	firstLocal := true
	for {
		n, src, err := v.conn.ReadFromUDP(buf)
		if err != nil {
			if v.ctx.Err() == nil {
				v.log.Error("viewer local read", "error", err)
			}
			return
		}
		if n == 0 || !src.IP.IsLoopback() {
			continue
		}
		data := buf[:n]
		v.mu.Lock()
		// A reconnect creates a new local source port, so the newest loopback
		// sender always becomes the client instead of being ignored forever.
		if v.local == nil || !sameAddr(v.local, src) {
			v.local = cloneAddr(src)
			if firstLocal {
				firstLocal = false
				v.log.Info("local QUIC client attached", "from", src.String())
			} else {
				v.log.Info("local QUIC client switched", "from", src.String())
			}
		}
		remote := cloneAddr(v.remote)
		useCarrier := v.carrierMode
		v.mu.Unlock()
		if useCarrier {
			v.writeCarrier(data)
			continue
		}
		if v.tcpActive.Load() {
			v.writeTCP(data)
			continue
		}
		if remote == nil {
			continue
		}
		v.out.WriteToUDP(data, remote)
	}
}

// readLoop reads the routable socket: control from the rendezvous and relay,
// punches and data from the host. Everything that reaches the local client is
// written to the loopback listener.
func (v *ViewerAgent) readLoop() {
	buf := make([]byte, 65535)
	for {
		n, src, err := v.out.ReadFromUDP(buf)
		if err != nil {
			if v.ctx.Err() == nil {
				v.log.Error("viewer upstream read", "error", err)
			}
			return
		}
		if n == 0 {
			continue
		}
		data := buf[:n]
		v.mu.Lock()
		punch, punch6, direct, relay := cloneAddr(v.punch), cloneAddr(v.punch6), cloneAddr(v.direct), cloneAddr(v.relay)
		v.mu.Unlock()

		if wire.IsControl(data) {
			if sameAddr(src, punch) || sameAddr(src, punch6) || sameAddr(src, relay) {
				v.handleControl(src, data)
				continue
			}
			if msg, err := wire.Decode(data); err == nil {
				switch msg.Type {
				case wire.TypePunch:
					// Older hosts only announce the hole with the session nonce.
					if msg.Nonce == v.nonce && !v.directOK.Load() {
						v.onDirectPacket(src)
					}
				case wire.TypePunchProbe:
					v.answerPunchProbe(src, msg)
				case wire.TypePunchAck:
					v.confirmPunchAck(src, msg)
				}
			}
			continue
		}
		if sameAddr(src, punch) || sameAddr(src, punch6) {
			continue
		}
		if sameAddr(src, relay) {
			v.mu.Lock()
			onRelay := v.remote != nil && sameAddr(v.remote, v.relay)
			v.mu.Unlock()
			if onRelay {
				v.writeLocal(data)
			}
			continue
		}
		if v.isCandidate(src) || sameAddr(src, direct) {
			// Host data from any announced candidate, or from the address a
			// confirmed punch recorded. Real peer data is also how RustDesk's
			// listener-side punch ends: the peer already speaks the transport.
			if v.onDirectPacket(src) {
				v.writeLocal(data)
			}
			continue
		}
		// Neither the announced host path, the relay nor a nonce-checked punch:
		// dropping avoids feeding a stray datagram to the local client.
	}
}

func (v *ViewerAgent) onDirectPacket(src *net.UDPAddr) bool {
	v.mu.Lock()
	if v.ctx.Err() != nil || v.directLost || v.carrierMode || v.fellBack || v.cfg.Mode == ModeTCP || v.cfg.Mode == ModeRelay || v.tcpActive.Load() ||
		(v.directOK.Load() && !sameAddr(v.direct, src)) {
		// A late UDP probe must not redirect a TCP or fallback session.
		v.mu.Unlock()
		return false
	}
	first := !v.directOK.Swap(true)
	v.lastDirectReceive = time.Now()
	if !sameAddr(v.direct, src) {
		v.direct = cloneAddr(src)
	}
	if v.cfg.Mode != ModeRelay && !v.fellBack {
		if !v.latched || !sameAddr(v.remote, v.direct) {
			v.log.Info("direct path answered", "host", src.String())
		}
		v.latched = true
		v.remote = cloneAddr(src)
		if v.timer != nil {
			v.timer.Stop()
		}
	}
	v.mu.Unlock()
	v.stopDirectAttempts()
	if first {
		v.recordDirectResult(false)
		if v.out != nil {
			go v.udpDirectKeepalive()
		}
	}
	v.reportPath()
	return true
}

func (v *ViewerAgent) onDirectTimeout() {
	v.fallBackToRelay("direct path did not answer")
}

// fallBackToRelay moves a failed direct path onto the UDP relay or carrier. Once the
// fallback happens a late direct packet must not switch the session back:
// QUIC above the tunnel has already retransmitted on the relay path.
func (v *ViewerAgent) fallBackToRelay(reason string) {
	v.mu.Lock()
	if v.latched || v.ctx.Err() != nil || v.carrierMode || v.cfg.Mode != ModeAuto {
		v.mu.Unlock()
		return
	}
	carrierReady := v.carrierToken != "" && len(v.carrierCandidates()) > 0
	useCarrier := carrierReady && (v.transport == "carrier" || v.relay == nil)
	if !useCarrier && (v.relay == nil || sameAddr(v.remote, v.relay)) {
		v.mu.Unlock()
		return
	}
	relay := cloneAddr(v.relay)
	v.latched = true
	v.fellBack = true
	v.carrierMode = useCarrier
	if useCarrier {
		v.remote = nil
	} else {
		v.remote = relay
	}
	if v.timer != nil {
		v.timer.Stop()
	}
	v.mu.Unlock()
	v.stopDirectAttempts()
	v.recordDirectResult(true)
	if useCarrier {
		v.log.Warn(reason+"; using carrier", "carrier", v.carrierEndpoint())
		go v.carrierLoop()
	} else {
		v.log.Warn(reason+"; using relay", "relay", relay.String())
		v.startRelayAuth()
	}
	v.reportPath()
}

func (v *ViewerAgent) handleControl(src *net.UDPAddr, data []byte) {
	msg, err := wire.Decode(data)
	if err != nil {
		return
	}
	switch msg.Type {
	case wire.TypeNatProbeAck:
		v.noteNatProbeAck(src, msg)
	case wire.TypePunchPeer:
		v.mu.Lock()
		finished := v.directLost || v.fellBack || v.directOK.Load()
		v.mu.Unlock()
		if finished {
			return
		}
		if !msg.OK {
			v.log.Warn("punch request refused", "error", msg.Error)
			return
		}
		if msg.Relay != "" {
			if relay, err := resolveUDP(msg.Relay); err == nil && relay != nil {
				v.mu.Lock()
				v.relay = relay
				v.mu.Unlock()
			}
		}
		if msg.Candidate != "" || msg.CandidateV6 != "" || len(msg.LocalAddrs) > 0 || len(msg.LocalAddrsV6) > 0 {
			texts := make([]string, 0, len(msg.LocalAddrs)+len(msg.LocalAddrsV6)+2)
			if msg.Candidate != "" {
				texts = append(texts, msg.Candidate)
			}
			if msg.CandidateV6 != "" {
				texts = append(texts, msg.CandidateV6)
			}
			texts = append(texts, msg.LocalAddrs...)
			texts = append(texts, msg.LocalAddrsV6...)
			if mapped := candidateWithUpnpPort(msg.Candidate, msg.UpnpPort); mapped != "" {
				texts = append(texts, mapped)
			}
			v.mu.Lock()
			merged := v.mergeDirect(append(addrList(v.candidates), texts...))
			if len(merged) > 0 {
				v.candidates = merged
				if !v.directOK.Load() {
					v.direct = cloneAddr(merged[0])
					if !v.latched {
						v.remote = cloneAddr(merged[0])
					}
				}
				if msg.NATType != "" {
					v.natType = msg.NATType
				}
			}
			v.mu.Unlock()
			if len(merged) > 0 {
				v.mu.Lock()
				v.udpLocal = msg.IsLocal
				v.mu.Unlock()
				v.punched.Store(true)
			}
		}
	case wire.TypeBusy:
		v.log.Warn("relay is busy", "error", msg.Error)
	case wire.TypeRelayViewerAck:
		if msg.OK {
			v.relayAuthOK.Store(true)
			v.log.Info("relay viewer token accepted")
		} else {
			v.log.Warn("relay viewer token refused", "error", msg.Error)
		}
	}
	v.reportPath()
}

func addrText(a *net.UDPAddr) string {
	if a == nil {
		return "none"
	}
	return a.String()
}

// checkHostBinding enforces the optional server pin and makes sure the lookup
// fields agree with the signed binding. Without a pin this is a no-op: the
// viewer cannot authenticate the server on its own.
func checkHostBinding(serverPublicKey, hostID string, host lookupResult) error {
	if serverPublicKey == "" {
		return nil
	}
	if host.Binding == "" {
		return fmt.Errorf("server %s did not sign the host binding; check --server-pubkey", host.ServerPublicKey)
	}
	if host.ServerPublicKey != "" && host.ServerPublicKey != serverPublicKey {
		return fmt.Errorf("lookup came from an unexpected server identity")
	}
	binding, err := identity.VerifyHostBinding(host.Binding, serverPublicKey)
	if err != nil {
		return fmt.Errorf("verify host binding: %w", err)
	}
	if binding.ID != hostID {
		return fmt.Errorf("server signed a binding for a different host id")
	}
	if host.PublicKey != binding.PublicKey || host.Fingerprint != binding.Fingerprint {
		return fmt.Errorf("lookup fields do not match the signed host binding")
	}
	return nil
}
