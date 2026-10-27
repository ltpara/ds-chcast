// Package agent runs the two tunnel endpoints. The host agent registers the
// machine, maintains its NAT mapping with keepalives, and forwards datagrams
// between chast-host and whichever path a viewer chose. The viewer agent makes
// the opposite choice: direct first when it is asked to, relay when the direct
// path stays silent.
package agent

import (
	"bufio"
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
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/upnp"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

type HostConfig struct {
	ServerURL string
	// Servers seeds the failover list before the first registration. The
	// server's pushed list replaces it after a successful reply.
	Servers   []string
	ServerKey string
	// TLS configures HTTPS to the rendezvous API: a private CA bundle, a
	// client certificate for mTLS, or both. Nil uses the system roots.
	TLS *tls.Config
	// ServerPublicKey pins the rendezvous server identity. When set, the
	// registration reply must contain a valid server-signed host binding.
	ServerPublicKey string
	ID              string
	Key             string
	// OldID is the previous rendezvous id when the operator changed it. The
	// agent proves ownership with its host key and the server moves the
	// durable identity instead of leaving the old peer orphaned.
	OldID       string
	Fingerprint string
	// LocalAddr is chast-host's QUIC listener, usually 127.0.0.1:4433.
	LocalAddr string
	// ListenAddr is the public-facing UDP socket used for all traffic.
	ListenAddr string
	// CarrierAddr is chast-host's local TCP carrier listener, usually
	// 127.0.0.1:(QUIC port + 1). Empty disables the relay's TCP carrier.
	CarrierAddr string
	Keepalive   time.Duration
	Logger      *slog.Logger
	// AuditSpool is a JSONL file chast-host appends inbound session and
	// finished-file events to. The agent signs each line with the host key and
	// posts it, so the spool works while the server is unreachable. Empty
	// disables forwarding.
	AuditSpool string
	// UPnP maps the public UDP socket on the local Internet Gateway Device and
	// reports that external port to the rendezvous service. It is opt-in
	// because it changes router state.
	UPnP bool
	// OnInfo receives registration and viewer-path changes, starting with the
	// relay assigned by the server. It is called from the read or refresh
	// goroutine, so it must not block.
	OnInfo func(HostInfo)
}

// HostInfo is a snapshot of the host agent for an embedding application.
// Path is "direct" or "relay" once a viewer has sent data; Viewer is the
// public address that data came from (the relay endpoint for a relayed
// viewer).
type HostInfo struct {
	ID     string `json:"id,omitempty"`
	Listen string `json:"listen,omitempty"`
	Punch  string `json:"punch,omitempty"`
	Relay  string `json:"relay,omitempty"`
	Viewer string `json:"viewer,omitempty"`
	Path   string `json:"path,omitempty"`
	// NAT is the classification derived from the mappings seen by the
	// rendezvous and relay ports: asymmetric, symmetric or unknown.
	NAT string `json:"nat,omitempty"`
	// UPnPPort is the external port mapped on the local gateway, zero when no
	// mapping is active.
	UPnPPort int `json:"upnp_port,omitempty"`
	// MigratedFrom is the old id the service moved to ID during this start.
	// Empty on ordinary registrations and renewals.
	MigratedFrom string `json:"migrated_from,omitempty"`
}

type HostAgent struct {
	cfg          HostConfig
	conn         *net.UDPConn
	api          *apiClient
	servers      *serverPool
	log          *slog.Logger
	local        *net.UDPAddr
	carrier      *net.TCPAddr
	priv         ed25519.PrivateKey
	publicKey    string
	localAddrs   []string
	localAddrsV6 []string

	ctx    context.Context
	cancel context.CancelFunc

	mu       sync.Mutex
	punch    *net.UDPAddr
	punch6   *net.UDPAddr
	relay    *net.UDPAddr
	relaySet []*net.UDPAddr
	refresh  time.Duration
	// sharedCarrier and sharedCarrierV6 are the server-wide carrier endpoints
	// advertised by the registration reply. When set they replace the per-port
	// relay carrier; the agent prefers the address family it can reach.
	sharedCarrier   string
	sharedCarrierV6 string
	// sharedCarrierToken is reissued on every lease; the host keeps it for
	// diagnostics only, viewers receive it from their own lookup.
	sharedCarrierToken string
	// viewer and viewerPath describe the last path that delivered data. They
	// are reported to the embedding UI; unlike remote they are not used for
	// forwarding.
	viewer     *net.UDPAddr
	viewerPath string
	lastInfo   string
	// migratedFrom is set once at start, when the registration moved an old
	// identity. It is never updated by renewals.
	migratedFrom string
	// softwareSeen remembers the last advertised update version so a renewal
	// every few seconds does not repeat the same notice.
	softwareSeen string
	// observedPunch and observedRelay are this host's public mappings as seen
	// by the rendezvous port and by its relay port. Comparing them classifies
	// the NAT without a second server.
	observedPunch *net.UDPAddr
	observedRelay *net.UDPAddr
	natType       string
	upnpPort      int

	// remote is only touched by the read loop: it is the source of the last
	// datagram that came from a viewer path (direct or via the relay).
	remote *net.UDPAddr

	keepaliveOnce  sync.Once
	relayOnce      sync.Once
	carrierOnce    sync.Once
	keepaliveReady chan struct{}
	relayReady     chan struct{}
	carrierReady   chan struct{}
}

func StartHost(parent context.Context, cfg HostConfig) (*HostAgent, error) {
	if cfg.ServerURL == "" || cfg.ID == "" || len(cfg.Key) < 8 {
		return nil, fmt.Errorf("host agent needs --server, --id and a --key of at least 8 characters")
	}
	if cfg.Keepalive <= 0 {
		cfg.Keepalive = 10 * time.Second
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
	privateKey := identity.PrivateKeyFromSecret(cfg.Key)
	publicKey := identity.PublicKeyText(privateKey.Public().(ed25519.PublicKey))
	local, err := resolveUDP(cfg.LocalAddr)
	if err != nil || local == nil {
		return nil, fmt.Errorf("local chast-host address: %v", err)
	}
	listen, err := net.ResolveUDPAddr("udp", cfg.ListenAddr)
	if err != nil {
		return nil, fmt.Errorf("listen address %q: %w", cfg.ListenAddr, err)
	}
	conn, err := net.ListenUDP("udp", listen)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", cfg.ListenAddr, err)
	}
	var carrierAddr *net.TCPAddr
	if cfg.CarrierAddr != "" {
		carrierAddr, err = net.ResolveTCPAddr("tcp", cfg.CarrierAddr)
		if err != nil || carrierAddr == nil {
			cfg.Logger.Warn("carrier link disabled: bad local carrier address",
				"address", cfg.CarrierAddr, "error", err)
			carrierAddr = nil
		}
	}

	ctx, cancel := context.WithCancel(parent)
	localAddrs, localAddrsV6 := splitByFamily(localCandidates(conn.LocalAddr().(*net.UDPAddr)))
	h := &HostAgent{
		cfg:            cfg,
		conn:           conn,
		api:            newAPIClient(servers.Current(), cfg.ServerKey, cfg.TLS),
		servers:        servers,
		log:            cfg.Logger.With("agent", "host", "id", cfg.ID),
		local:          local,
		carrier:        carrierAddr,
		priv:           privateKey,
		publicKey:      publicKey,
		localAddrs:     localAddrs,
		localAddrsV6:   localAddrsV6,
		ctx:            ctx,
		cancel:         cancel,
		refresh:        20 * time.Second,
		keepaliveReady: make(chan struct{}),
		relayReady:     make(chan struct{}),
		carrierReady:   make(chan struct{}),
	}
	reg, err := h.registerWithFailover(ctx)
	if err != nil {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("register: %w", err)
	}
	if err := h.checkRegistration(reg); err != nil {
		conn.Close()
		cancel()
		return nil, err
	}
	h.applyRegistration(reg)
	h.migratedFrom = reg.MigratedFrom

	if cfg.UPnP {
		mapCtx, mapCancel := context.WithTimeout(ctx, 5*time.Second)
		mapper, mapErr := upnp.Map(mapCtx, conn.LocalAddr().(*net.UDPAddr).Port, "chast host "+cfg.ID)
		mapCancel()
		if mapErr != nil {
			h.log.Warn("UPnP mapping unavailable", "error", mapErr)
		} else {
			h.upnpPort = mapper.ExternalPort()
			h.log.Info("UPnP port mapped", "external_port", h.upnpPort,
				"internal_port", conn.LocalAddr().(*net.UDPAddr).Port,
				"gateway", mapper.Gateway().ControlURL)
			go renewUPnP(ctx, h.log, mapper)
		}
	}

	go h.readLoop()
	go h.keepaliveLoop()
	go h.relayHelloLoop()
	go h.refreshLoop()
	if h.carrier != nil {
		go h.carrierLinkLoop()
	}
	if cfg.AuditSpool != "" {
		go h.auditSpoolLoop(cfg.AuditSpool)
	}

	h.log.Info("host agent listening", "on", conn.LocalAddr().String(), "forwarding_to", local.String(), "relay", reg.Relay)
	h.reportInfo()
	return h, nil
}

func (h *HostAgent) LocalAddr() net.Addr { return h.conn.LocalAddr() }

// Info returns a snapshot of the registration and the last viewer path. Safe
// to call while the agent is running.
func (h *HostAgent) Info() HostInfo {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.infoLocked()
}

func (h *HostAgent) infoLocked() HostInfo {
	return HostInfo{
		ID:           h.cfg.ID,
		Listen:       h.conn.LocalAddr().String(),
		Punch:        addrValue(h.punch),
		Relay:        addrValue(h.relay),
		Viewer:       addrValue(h.viewer),
		Path:         h.viewerPath,
		NAT:          h.natType,
		UPnPPort:     h.upnpPort,
		MigratedFrom: h.migratedFrom,
	}
}

func (h *HostAgent) reportInfo() {
	h.mu.Lock()
	info := h.infoLocked()
	key := info.Relay + "|" + info.Viewer + "|" + info.Path + "|" + info.NAT + "|" + info.MigratedFrom
	changed := key != h.lastInfo
	h.lastInfo = key
	h.mu.Unlock()
	if changed && h.cfg.OnInfo != nil {
		h.cfg.OnInfo(info)
	}
}

func (h *HostAgent) Close() {
	h.cancel()
	h.conn.Close()
}

// WaitReady waits for the first keepalive acknowledgement, which is what makes
// the host's observed address usable for punching. An authenticated shared
// carrier link is accepted instead: it is what keeps the host online when UDP
// cannot leave its network. The relay acknowledgement is waited for briefly but
// is not required: a direct-only deployment may not have a relay.
func (h *HostAgent) WaitReady(ctx context.Context) error {
	viaCarrier := false
	select {
	case <-h.keepaliveReady:
	case <-h.carrierReady:
		viaCarrier = true
		h.log.Warn("keepalive has not answered; continuing on the carrier link")
	case <-ctx.Done():
		return ctx.Err()
	}
	if viaCarrier {
		return nil
	}
	select {
	case <-h.relayReady:
	case <-ctx.Done():
		return ctx.Err()
	case <-time.After(2 * time.Second):
		h.log.Warn("relay did not acknowledge yet; direct connections can still work")
	}
	return nil
}

// registerWithFailover tries every endpoint once. A server that answers with
// an application error still causes a rotation, so a single broken node cannot
// keep the host offline while a healthy one is known.
func (h *HostAgent) registerWithFailover(ctx context.Context) (registerResult, error) {
	var lastErr error
	for attempt := 0; attempt < h.servers.Len(); attempt++ {
		attemptCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		reg, err := h.api.register(attemptCtx, h.cfg.ID, h.cfg.OldID, h.priv, h.cfg.Fingerprint)
		cancel()
		if err == nil {
			return reg, nil
		}
		lastErr = err
		if next, ok := h.servers.Next(); ok {
			h.api.setBase(next)
			h.log.Warn("registration failed; trying the next rendezvous server", "server", next, "error", err)
		}
	}
	if lastErr == nil {
		lastErr = errors.New("no rendezvous server answered")
	}
	return registerResult{}, lastErr
}

func (h *HostAgent) rotateServer(reason string) {
	if next, ok := h.servers.Next(); ok {
		h.api.setBase(next)
		h.log.Warn("switching rendezvous server", "server", next, "reason", reason)
	}
}

// checkRegistration verifies the server's answer when the operator pinned the
// server public key. Without a pin the fields are still checked for internal
// consistency so a confused proxy cannot silently swap the host key.
func (h *HostAgent) checkRegistration(reg registerResult) error {
	if reg.PublicKey != "" && reg.PublicKey != h.publicKey {
		return fmt.Errorf("server registered a different host public key")
	}
	if h.cfg.ServerPublicKey == "" {
		return nil
	}
	if reg.ServerPublicKey != "" && reg.ServerPublicKey != h.cfg.ServerPublicKey {
		return fmt.Errorf("server identity does not match the pinned --server-pubkey")
	}
	binding, err := identity.VerifyHostBinding(reg.Binding, h.cfg.ServerPublicKey)
	if err != nil {
		return fmt.Errorf("verified host binding: %w", err)
	}
	if binding.ID != h.cfg.ID || binding.PublicKey != h.publicKey || binding.Fingerprint != h.cfg.Fingerprint {
		return fmt.Errorf("server signed a binding for a different host identity")
	}
	return nil
}

func (h *HostAgent) applyRegistration(reg registerResult) {
	if reg.ConfigSerial > 0 && len(reg.RendezvousServers) > 0 {
		if h.servers.Apply(reg.ConfigSerial, reg.RendezvousServers) {
			h.log.Info("rendezvous configuration updated",
				"serial", reg.ConfigSerial, "servers", h.servers.List())
		}
	}
	punch, err := resolveUDP(reg.Punch)
	if err != nil {
		h.log.Warn("registration returned an unusable punch address", "punch", reg.Punch, "error", err)
	}
	punch6, err := resolveUDP(reg.PunchV6)
	if err != nil {
		h.log.Warn("registration returned an unusable IPv6 punch address", "punch_v6", reg.PunchV6, "error", err)
	}
	relay, err := resolveUDP(reg.Relay)
	if err != nil {
		h.log.Warn("registration returned an unusable relay address", "relay", reg.Relay, "error", err)
	}
	endpoints := reg.Relays
	if len(endpoints) == 0 && reg.Relay != "" {
		endpoints = []string{reg.Relay}
	}
	relaySet := resolveUDPAll(endpoints)
	h.mu.Lock()
	if punch != nil {
		h.punch = punch
	}
	if punch6 != nil && familyAllowed(h.conn.LocalAddr().(*net.UDPAddr).IP, punch6.IP) {
		if !sameAddr(h.punch6, punch6) {
			h.log.Info("IPv6 rendezvous endpoint updated", "punch_v6", punch6.String())
		}
		h.punch6 = punch6
	}
	if relay != nil && !sameAddr(h.relay, relay) {
		h.relay = relay
		h.log.Info("relay address updated", "relay", relay.String())
	}
	if len(relaySet) > 0 {
		if relay != nil && !containsUDPAddr(relaySet, relay) {
			relaySet = append(relaySet, cloneAddr(relay))
		}
		if !sameUDPAddrSet(h.relaySet, relaySet) {
			h.log.Info("relay challenge set updated", "endpoints", addrList(relaySet))
		}
		h.relaySet = relaySet
	}
	if reg.ExpiresInSec > 0 {
		refresh := time.Duration(reg.ExpiresInSec) * time.Second / 2
		if refresh < 5*time.Second {
			refresh = 5 * time.Second
		}
		h.refresh = refresh
	}
	if reg.Carrier != h.sharedCarrier || reg.CarrierV6 != h.sharedCarrierV6 {
		h.sharedCarrier = reg.Carrier
		h.sharedCarrierV6 = reg.CarrierV6
		h.log.Info("carrier endpoint updated", "carrier", reg.Carrier, "carrier_v6", reg.CarrierV6)
	}
	h.sharedCarrierToken = reg.CarrierToken
	var notice *softwareInfo
	if reg.Software.updateAvailable() {
		if h.softwareSeen != reg.Software.Version {
			h.softwareSeen = reg.Software.Version
			offer := *reg.Software
			notice = &offer
		}
	} else {
		h.softwareSeen = ""
	}
	h.mu.Unlock()
	if notice != nil {
		h.log.Info("software update available",
			"version", notice.Version, "current", Version, "url", notice.URL)
	}
	h.reportInfo()
}

func (h *HostAgent) getPunch() *net.UDPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	return cloneAddr(h.punch)
}

func (h *HostAgent) getPunch6() *net.UDPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	return cloneAddr(h.punch6)
}

func (h *HostAgent) getRelay() *net.UDPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	return cloneAddr(h.relay)
}

// relayEndpoints lists every advertised relay address plus the primary one.
// A host may receive a challenge from any of them because they all reach the
// same relay instance on the lease's port.
func (h *HostAgent) relayEndpoints() []*net.UDPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make([]*net.UDPAddr, 0, len(h.relaySet)+1)
	for _, addr := range h.relaySet {
		out = append(out, cloneAddr(addr))
	}
	if h.relay != nil && !containsUDPAddr(out, h.relay) {
		out = append(out, cloneAddr(h.relay))
	}
	return out
}

func (h *HostAgent) relayAllowed(src *net.UDPAddr) bool {
	for _, addr := range h.relayEndpoints() {
		if sameAddr(src, addr) {
			return true
		}
	}
	return false
}

// acceptsControl admits control datagrams from either rendezvous address or a
// relay endpoint. A dual-stack host keeps an IPv4 and an IPv6 mapping alive, so
// dropping the IPv6 source would leave that mapping unverified.
func (h *HostAgent) acceptsControl(src, punch, punch6 *net.UDPAddr) bool {
	return sameAddr(src, punch) || sameAddr(src, punch6) || h.relayAllowed(src)
}

func (h *HostAgent) getNATType() string {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.natType
}

// noteObserved records the public mapping each server port sees. Both probes
// come from the same socket, so a cone NAT keeps the port stable while a
// symmetric NAT allocates a different one per destination.
func (h *HostAgent) noteObserved(primary bool, candidate string) {
	if candidate == "" {
		return
	}
	addr, err := resolveUDP(candidate)
	if err != nil || addr == nil {
		return
	}
	h.mu.Lock()
	if primary {
		h.observedPunch = addr
	} else {
		h.observedRelay = addr
	}
	nat := classifyNAT(h.observedPunch, h.observedRelay)
	changed := nat != "" && nat != h.natType
	if changed {
		h.natType = nat
	}
	h.mu.Unlock()
	if changed {
		h.log.Info("NAT type detected", "nat", nat)
		h.sendKeepalive()
		h.reportInfo()
	}
}

func (h *HostAgent) readLoop() {
	buf := make([]byte, 65535)
	for {
		n, src, err := h.conn.ReadFromUDP(buf)
		if err != nil {
			if h.ctx.Err() == nil {
				h.log.Error("host agent read", "error", err)
			}
			return
		}
		if n == 0 {
			continue
		}
		data := buf[:n]
		h.mu.Lock()
		punch := cloneAddr(h.punch)
		punch6 := cloneAddr(h.punch6)
		h.mu.Unlock()

		if wire.IsControl(data) {
			if h.acceptsControl(src, punch, punch6) {
				h.handleControl(data, src)
			}

			// A punch from a viewer is only a NAT probe; there is nothing to
			// forward. The host's own punch was sent when the server notified it.
			continue
		}
		if sameAddr(src, h.local) {
			if h.remote == nil {
				continue
			}
			h.conn.WriteToUDP(data, h.remote)
			continue
		}
		h.noteViewer(src)
		h.remote = cloneAddr(src)
		h.conn.WriteToUDP(data, h.local)
	}
}

// noteViewer records a new viewer path and reports a change. A relayed viewer
// is seen by this agent as one of the relay endpoints, while a direct viewer
// keeps its own public address.
func (h *HostAgent) noteViewer(src *net.UDPAddr) {
	path := "direct"
	for _, relay := range h.relayEndpoints() {
		if sameAddr(src, relay) {
			path = "relay"
			break
		}
	}
	h.mu.Lock()
	first := h.viewer == nil
	changed := !sameAddr(h.viewer, src) || h.viewerPath != path
	if changed {
		h.viewer = cloneAddr(src)
		h.viewerPath = path
	}
	h.mu.Unlock()
	if !changed {
		return
	}
	if first {
		h.log.Info("viewer path established", "from", src.String(), "path", path)
	} else {
		h.log.Info("viewer path switched", "from", src.String(), "path", path)
	}
	h.reportInfo()
}

func (h *HostAgent) handleControl(data []byte, src *net.UDPAddr) {
	msg, err := wire.Decode(data)
	if err != nil {
		return
	}
	switch msg.Type {
	case wire.TypeKeepaliveChallenge:
		if (!sameAddr(src, h.getPunch()) && !sameAddr(src, h.getPunch6())) ||
			msg.HostID != h.cfg.ID || (msg.Candidate == "" && msg.CandidateV6 == "") || msg.Nonce == "" {
			h.log.Debug("ignoring keepalive challenge", "from", src.String(), "host", msg.HostID,
				"candidate", msg.Candidate, "candidate_v6", msg.CandidateV6)
			return
		}
		candidate := msg.Candidate
		if candidate == "" {
			candidate = msg.CandidateV6
		}
		signature := identity.Sign(h.priv, identity.KeepaliveMessage(h.cfg.ID, candidate, msg.Nonce))
		h.conn.WriteToUDP(wire.Encode(wire.Message{
			Type:         wire.TypeKeepaliveAuth,
			HostID:       h.cfg.ID,
			Nonce:        msg.Nonce,
			Candidate:    msg.Candidate,
			CandidateV6:  msg.CandidateV6,
			Signature:    signature,
			LocalAddrs:   h.localAddrs,
			LocalAddrsV6: h.localAddrsV6,
			NATType:      h.getNATType(),
			UpnpPort:     h.upnpPort,
		}), src)
	case wire.TypeKeepaliveAck:
		if !msg.OK {
			h.log.Warn("keepalive refused", "error", msg.Error)
			return
		}
		if msg.Candidate != "" {
			h.noteObserved(true, msg.Candidate)
		}
		h.keepaliveOnce.Do(func() { close(h.keepaliveReady) })
	case wire.TypePunchPeer:
		peer, err := resolveUDP(msg.Candidate)
		if err != nil || peer == nil {
			peer, err = resolveUDP(msg.CandidateV6)
			if err != nil || peer == nil {
				return
			}
		}
		h.log.Info("punch request from viewer", "candidate", peer.String())
		go h.sendPunches(peer, msg.Nonce)
		// The viewer's gateway mapping stays reachable even when its observed
		// source port differs per destination (symmetric NAT).
		if mapped, err := resolveUDP(candidateWithUpnpPort(msg.Candidate, msg.UpnpPort)); err == nil &&
			mapped != nil && !sameAddr(mapped, peer) {
			h.log.Info("punching the viewer's UPnP mapping", "candidate", mapped.String())
			go h.sendPunches(mapped, msg.Nonce)
		}
	case wire.TypeRelayChallenge:
		if !h.relayAllowed(src) || msg.HostID != h.cfg.ID ||
			msg.Candidate == "" || msg.Nonce == "" {
			return
		}
		signature := identity.Sign(h.priv, identity.RelayMessage(h.cfg.ID, src.Port, msg.Candidate, msg.Nonce))
		h.noteObserved(false, msg.Candidate)
		h.conn.WriteToUDP(wire.Encode(wire.Message{
			Type:      wire.TypeRelayAuth,
			HostID:    h.cfg.ID,
			Nonce:     msg.Nonce,
			Candidate: msg.Candidate,
			Signature: signature,
		}), src)
	case wire.TypeRelayAck:
		if !msg.OK {
			h.log.Warn("relay refused the host", "error", msg.Error)
			return
		}
		h.relayOnce.Do(func() { close(h.relayReady) })
	}
}

func (h *HostAgent) sendPunches(peer *net.UDPAddr, nonce string) {
	punch := wire.Encode(wire.Message{Type: wire.TypePunch, Nonce: nonce})
	for i := 0; i < 3; i++ {
		if _, err := h.conn.WriteToUDP(punch, peer); err != nil {
			return
		}
		select {
		case <-h.ctx.Done():
			return
		case <-time.After(200 * time.Millisecond):
		}
	}
}

func (h *HostAgent) sendKeepalive() {
	punch := h.getPunch()
	if punch == nil {
		return
	}
	msg := wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: h.cfg.ID})
	if _, err := h.conn.WriteToUDP(msg, punch); err != nil {
		h.log.Debug("IPv4 keepalive write failed", "punch", punch.String(), "error", err)
	}
	// A second keepalive over IPv6 keeps that mapping alive independently;
	// without it the server can only ever hand out the IPv4 mapping.
	if punch6 := h.getPunch6(); punch6 != nil {
		if _, err := h.conn.WriteToUDP(msg, punch6); err != nil {
			h.log.Debug("IPv6 keepalive write failed", "punch6", punch6.String(), "error", err)
		}
	}
}

func (h *HostAgent) keepaliveLoop() {
	ticker := time.NewTicker(h.cfg.Keepalive)
	defer ticker.Stop()
	h.sendKeepalive()
	for {
		select {
		case <-h.ctx.Done():
			return
		case <-ticker.C:
			h.sendKeepalive()
		}
	}
}

func (h *HostAgent) relayHelloLoop() {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	send := func() {
		raw := wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: h.cfg.ID})
		// Every advertised endpoint is a way into the same lease. A dual-stack
		// host authenticates on each family so a viewer can pick either one.
		for _, relay := range h.relayEndpoints() {
			h.conn.WriteToUDP(raw, relay)
		}
	}
	send()
	for {
		select {
		case <-h.ctx.Done():
			return
		case <-ticker.C:
			send()
		}
	}
}

const (
	carrierDial      = 5 * time.Second
	carrierReadIdle  = 60 * time.Second
	carrierKeepalive = 20 * time.Second
	carrierRetryMax  = 10 * time.Second
)

// carrierLinkLoop keeps one authenticated TCP link to the relay so viewers on
// networks that block UDP can still reach this host. It reconnects after any
// failure until the agent closes.
func (h *HostAgent) carrierLinkLoop() {
	backoff := time.Second
	for {
		if h.ctx.Err() != nil {
			return
		}
		targets := h.carrierTargets()
		if len(targets) == 0 {
			if !h.sleep(2 * time.Second) {
				return
			}
			continue
		}
		started := time.Now()
		target := targets[0]
		established := false
		var err error
		for _, candidate := range targets {
			target = candidate
			established, err = h.runCarrierLink(candidate)
			if h.ctx.Err() != nil {
				return
			}
			if err == nil || established {
				break
			}
			h.log.Warn("carrier dial failed", "carrier", candidate, "error", err)
		}
		if err != nil {
			h.log.Warn("carrier link lost", "carrier", target, "error", err)
		}
		if time.Since(started) > 30*time.Second {
			backoff = time.Second
		}
		if !h.sleep(backoff) {
			return
		}
		backoff *= 2
		if backoff > carrierRetryMax {
			backoff = carrierRetryMax
		}
	}
}

// carrierTargets lists the endpoints worth dialing, preferred address family
// first. The server-wide carrier replaces the relay port's own TCP twin; the
// per-host port stays valid for older servers.
func (h *HostAgent) carrierTargets() []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.sharedCarrier != "" || h.sharedCarrierV6 != "" {
		return carrierCandidateOrder(h.sharedCarrier, h.sharedCarrierV6, preferIPv6Outbound())
	}
	if h.relay == nil {
		return nil
	}
	return []string{net.JoinHostPort(h.relay.IP.String(), strconv.Itoa(h.relay.Port))}
}

// carrierTargetActive reports whether target is still among the current
// candidates, so an established link only drops when its endpoint really went
// away rather than when the preferred family changed.
func (h *HostAgent) carrierTargetActive(target string) bool {
	for _, candidate := range h.carrierTargets() {
		if candidate == target {
			return true
		}
	}
	return false
}

func (h *HostAgent) sleep(d time.Duration) bool {
	select {
	case <-h.ctx.Done():
		return false
	case <-time.After(d):
		return true
	}
}

// runCarrierLink dials one carrier endpoint and pumps frames until the link
// closes. The boolean reports whether the handshake completed, so the caller
// only falls back to another endpoint when the dial itself failed.
func (h *HostAgent) runCarrierLink(relayAddr string) (bool, error) {
	local, err := net.DialTimeout("tcp", h.carrier.String(), carrierDial)
	if err != nil {
		return false, fmt.Errorf("local carrier %s: %w", h.carrier, err)
	}
	defer local.Close()
	if _, err := io.WriteString(local, carrier.MagicRaw); err != nil {
		return false, fmt.Errorf("local carrier magic: %w", err)
	}
	localFrames := carrier.NewFrameReader(bufio.NewReader(local))
	localSend := carrier.NewFrameWriter(local)

	conn, err := net.DialTimeout("tcp", relayAddr, carrierDial)
	if err != nil {
		return false, fmt.Errorf("relay carrier %s: %w", relayAddr, err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
	hello, err := json.Marshal(carrier.HostHello{HostID: h.cfg.ID})
	if err != nil {
		return false, err
	}
	if _, err := io.WriteString(conn, carrier.MagicHost+string(hello)+"\n"); err != nil {
		return false, fmt.Errorf("relay carrier hello: %w", err)
	}
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		return false, fmt.Errorf("relay carrier handshake: %w", err)
	}
	challengeText, ok := strings.CutPrefix(line, carrier.MagicChallenge)
	if !ok {
		return false, fmt.Errorf("relay carrier refused: %s", strings.TrimSpace(strings.TrimPrefix(line+"\n", carrier.MagicErrorPrefix)))
	}
	var challenge carrier.HostChallenge
	if err := json.Unmarshal([]byte(challengeText), &challenge); err != nil || challenge.Nonce == "" {
		return false, fmt.Errorf("relay carrier sent a malformed challenge")
	}
	auth, err := json.Marshal(carrier.HostAuth{
		HostID:    h.cfg.ID,
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(h.priv, identity.CarrierHostMessage(h.cfg.ID, "", challenge.Nonce)),
	})
	if err != nil {
		return false, err
	}
	if _, err := io.WriteString(conn, carrier.MagicAuth+string(auth)+"\n"); err != nil {
		return false, fmt.Errorf("relay carrier proof: %w", err)
	}
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		return false, fmt.Errorf("relay carrier handshake: %w", err)
	}
	if line+"\n" != carrier.MagicOK {
		reason := strings.TrimSpace(strings.TrimPrefix(line+"\n", carrier.MagicErrorPrefix))
		if reason == "" {
			reason = line
		}
		return false, fmt.Errorf("relay carrier refused: %s", reason)
	}
	_ = conn.SetDeadline(time.Time{})
	relayFrames := carrier.NewFrameReader(br)
	relaySend := carrier.NewFrameWriter(conn)

	h.carrierOnce.Do(func() { close(h.carrierReady) })
	h.log.Info("carrier link established", "relay", relayAddr, "local", h.carrier.String())
	defer h.log.Info("carrier link closed", "relay", relayAddr)

	errs := make(chan error, 2)
	go func() {
		for {
			_ = conn.SetReadDeadline(time.Now().Add(carrierReadIdle))
			payload, err := relayFrames.ReadFrame()
			if err != nil {
				errs <- err
				return
			}
			if len(payload) == 0 {
				continue
			}
			if err := localSend.WriteFrame(payload); err != nil {
				errs <- err
				return
			}
		}
	}()
	go func() {
		for {
			payload, err := localFrames.ReadFrame()
			if err != nil {
				errs <- err
				return
			}
			if len(payload) == 0 {
				continue
			}
			if err := relaySend.WriteFrame(payload); err != nil {
				errs <- err
				return
			}
		}
	}()

	ticker := time.NewTicker(carrierKeepalive)
	defer ticker.Stop()
	for {
		select {
		case <-h.ctx.Done():
			return true, h.ctx.Err()
		case err := <-errs:
			return true, err
		case <-ticker.C:
			if !h.carrierTargetActive(relayAddr) {
				return true, fmt.Errorf("carrier endpoint changed")
			}
			if err := relaySend.WriteFrame(nil); err != nil {
				return true, err
			}
			if err := localSend.WriteFrame(nil); err != nil {
				return true, err
			}
		}
	}
}

func (h *HostAgent) refreshLoop() {
	for {
		h.mu.Lock()
		every := h.refresh
		h.mu.Unlock()
		select {
		case <-h.ctx.Done():
			return
		case <-time.After(every):
		}
		ctx, cancel := context.WithTimeout(h.ctx, 5*time.Second)
		reg, err := h.api.register(ctx, h.cfg.ID, h.cfg.OldID, h.priv, h.cfg.Fingerprint)
		cancel()
		if err != nil {
			h.log.Warn("registration refresh failed", "error", err)
			h.rotateServer("registration refresh failed")
			h.mu.Lock()
			h.refresh = 2 * time.Second
			h.mu.Unlock()
			continue
		}
		if err := h.checkRegistration(reg); err != nil {
			h.log.Warn("registration refresh was not trusted", "error", err)
			h.rotateServer("registration refresh was not trusted")
			h.mu.Lock()
			h.refresh = 5 * time.Second
			h.mu.Unlock()
			continue
		}
		h.applyRegistration(reg)
	}
}
