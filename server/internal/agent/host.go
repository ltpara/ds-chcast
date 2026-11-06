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
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/updater"
	"github.com/ltpara/ds-chcast/server/internal/upnp"
	"github.com/ltpara/ds-chcast/server/internal/wake"
	"github.com/ltpara/ds-chcast/server/internal/wire"
	"github.com/ltpara/ds-chcast/server/internal/wol"
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
	// Proxy routes the HTTP control plane, update download and the relay
	// carrier link through an outbound proxy. Inbound UDP paths are unchanged.
	Proxy *netproxy.Proxy
	// ServerPublicKey pins the rendezvous server identity. When set, the
	// registration reply must contain a valid server-signed host binding.
	ServerPublicKey string
	// PublisherPublicKey pins the release publisher key. When set, every
	// update notice must carry a matching publisher key id and signature.
	PublisherPublicKey string
	// RequirePublisherSignature refuses updates without a publisher signature.
	// It is implied by PublisherPublicKey and rejects a notice that carries
	// none instead of silently downgrading.
	RequirePublisherSignature bool
	ID                        string
	Key                       string
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
	// AutoUpdate stages and launches the installer advertised by the server.
	// It requires ServerPublicKey: without a pin the notice cannot be
	// verified, so the agent refuses to run a downloaded binary.
	AutoUpdate bool
	// UpdateDir is where the installer is staged. Empty uses an "updates"
	// directory beside the agent executable, which inherits its ACL.
	UpdateDir string
	// WakeRelay makes this agent a cross-subnet Wake-on-LAN proxy: the server
	// may hand it jobs for targets whose last reported link shares an endpoint
	// with this machine's link. It is opt-in because it sends packets to
	// addresses outside this host.
	WakeRelay bool
	// WakeTargets overrides the broadcast destinations for diagnostics (for
	// example 127.0.0.1:9 to observe the packet locally). Empty uses the
	// directed broadcast of each IPv4 interface, which is also what the agent
	// reports to the server as its link.
	WakeTargets []string
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
	routes localRoutes
	cfg    HostConfig
	// conn carries rendezvous, relay and viewer traffic; localOut exists only
	// when the local control address needs the other address family.
	conn         *net.UDPConn
	localOut     *net.UDPConn
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
	// updatesTried keeps the versions this process already staged or
	// rejected, so a flapping server cannot loop the download.
	updatesTried map[string]bool
	// wakeSeen remembers relayed wake job ids so a duplicate delivery from a
	// restarted or failed-over server does not broadcast twice.
	wakeSeen map[string]bool
	// observedPunch and observedRelay are this host's public mappings as seen
	// by the rendezvous port and by its relay port. Comparing them classifies
	// the NAT without a second server.
	observedPunch *net.UDPAddr
	observedRelay *net.UDPAddr
	natType       string
	upnpPort      int

	// remote is guarded by mu: both read loops use it. It is the source of the
	// last datagram that came from a viewer path (direct or via the relay).
	remote *net.UDPAddr
	// punchPeers are viewer addresses the rendezvous service asked this host
	// to punch, with an expiry. Only these, the current remote and relay
	// endpoints may become the forwarding target, so one stray datagram
	// cannot steal the return path.
	punchPeers map[string]time.Time

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
	if cfg.AutoUpdate && cfg.ServerPublicKey == "" {
		return nil, fmt.Errorf("--auto-update needs --server-pubkey so the update notice can be verified")
	}
	if cfg.PublisherPublicKey != "" {
		if _, err := identity.ParsePublicKey(cfg.PublisherPublicKey); err != nil {
			return nil, fmt.Errorf("--publisher-pubkey: %w", err)
		}
		if _, err := identity.PublisherKeyID(cfg.PublisherPublicKey); err != nil {
			return nil, fmt.Errorf("--publisher-pubkey: %w", err)
		}
	}
	if cfg.RequirePublisherSignature && cfg.PublisherPublicKey == "" {
		return nil, fmt.Errorf("--require-publisher-signature needs --publisher-pubkey")
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
	localOut, err := localForwardConn(conn, local)
	if err != nil {
		cfg.Logger.Warn("local forwarding stays on the punch socket", "error", err)
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
		localOut:       localOut,
		api:            newAPIClient(servers.Current(), cfg.ServerKey, cfg.TLS, cfg.Proxy),
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
		punchPeers:     make(map[string]time.Time),
		keepaliveReady: make(chan struct{}),
		relayReady:     make(chan struct{}),
		carrierReady:   make(chan struct{}),
	}
	reg, err := h.registerWithFailover(ctx)
	if err != nil {
		conn.Close()
		if localOut != nil {
			localOut.Close()
		}
		cancel()
		return nil, fmt.Errorf("register: %w", err)
	}
	if err := h.checkRegistration(reg); err != nil {
		conn.Close()
		if localOut != nil {
			localOut.Close()
		}
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
	if h.localOut != nil {
		go h.localLoop()
	}
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

// localForwardConn opens a second datagram socket when the punch socket is
// bound to one address family and the local control address uses the other:
// a socket bound to ::1 cannot write to 127.0.0.1. A wildcard IPv6 bind is
// treated as dual-stack, which the supported platforms provide by default.
func localForwardConn(conn *net.UDPConn, local *net.UDPAddr) (*net.UDPConn, error) {
	bind, ok := conn.LocalAddr().(*net.UDPAddr)
	if !ok || local == nil || local.IP == nil {
		return nil, nil
	}
	connV4 := bind.IP.To4() != nil
	localV4 := local.IP.To4() != nil
	if connV4 == localV4 || (!connV4 && bind.IP.IsUnspecified()) {
		return nil, nil
	}
	network := "udp4"
	if !localV4 {
		network = "udp6"
	}
	return net.ListenUDP(network, nil)
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
	h.routes.close()
	h.cancel()
	h.conn.Close()
	if h.localOut != nil {
		h.localOut.Close()
	}
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
		reg, err := h.api.register(attemptCtx, h.cfg.ID, h.cfg.OldID, h.priv, h.cfg.Fingerprint,
			h.cfg.WakeRelay, h.wakeTargets())
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
	h.relayWakeJobs(reg.Wake)
	if notice != nil {
		h.log.Info("software update available",
			"version", notice.Version, "current", Version, "url", notice.URL)
		h.startUpdate(*notice)
	}
	h.reportInfo()
}

// startUpdate stages the advertised release at most once per version. The
// download runs off the read loop: a slow or capped link must never stall
// keepalives. Verification happens before anything is written, and the
// installer only runs after the digest matches.
func (h *HostAgent) startUpdate(n softwareInfo) {
	if !h.cfg.AutoUpdate {
		return
	}
	h.mu.Lock()
	if h.updatesTried[n.Version] {
		h.mu.Unlock()
		return
	}
	if h.updatesTried == nil {
		h.updatesTried = make(map[string]bool)
	}
	h.updatesTried[n.Version] = true
	h.mu.Unlock()

	notice := updater.Notice{
		Version:            n.Version,
		URL:                n.URL,
		SHA256:             n.SHA256,
		Size:               n.Size,
		Signature:          n.Signature,
		PublisherKeyID:     n.PublisherKeyID,
		PublisherSignature: n.PublisherSignature,
	}
	go func() {
		if err := updater.Verify(notice, h.cfg.ServerPublicKey); err != nil {
			h.log.Error("software update rejected", "version", notice.Version, "error", err)
			return
		}
		if h.cfg.PublisherPublicKey != "" {
			if err := updater.VerifyPublisher(notice, h.cfg.PublisherPublicKey); err != nil {
				h.log.Error("software update publisher rejected", "version", notice.Version, "error", err)
				return
			}
		}
		client := &http.Client{Timeout: 15 * time.Minute}
		if h.cfg.TLS != nil || h.cfg.Proxy != nil {
			client.Transport = h.cfg.Proxy.HTTPTransport(h.cfg.TLS)
		}
		ctx, cancel := context.WithTimeout(h.ctx, 15*time.Minute)
		defer cancel()
		path, err := updater.Fetch(ctx, client, notice, h.cfg.UpdateDir)
		if err != nil {
			h.log.Error("software update download failed", "version", notice.Version, "error", err)
			return
		}
		h.log.Info("software update staged", "version", notice.Version,
			"path", path, "size", notice.Size, "sha256", notice.SHA256)
		if err := updater.VerifyFile(path, notice.SHA256, notice.Size); err != nil {
			h.log.Error("software update staged file no longer matches", "version", notice.Version,
				"path", path, "error", err)
			return
		}
		if err := updater.Launch(path); err != nil {
			h.log.Error("software update launch failed", "version", notice.Version,
				"path", path, "error", err)
			return
		}
		h.log.Info("software update installer started", "version", notice.Version, "path", path)
	}()
}

// relayWakeJobs broadcasts queued Wake-on-LAN jobs on the target's link. The
// server already scopes delivery to relay agents whose reported link matches
// the target's, but the agent re-checks against its current interfaces so a
// delivery queued before a DHCP change is dropped instead of leaking a packet
// onto a different LAN. An explicit --wake-targets override is used as-is for
// diagnostics. The server marks a job delivered per agent, so the local set of
// seen ids only guards against a duplicate after a server restart or failover.
// A relay failure is logged and the job dropped: the request already returned
// 202 and the target's MAC does not change, so the caller can simply retry.
func (h *HostAgent) relayWakeJobs(jobs []wake.Job) {
	if !h.cfg.WakeRelay || len(jobs) == 0 {
		return
	}
	override := len(h.cfg.WakeTargets) > 0
	local := h.wakeTargets()
	for _, job := range jobs {
		targets := local
		if !override {
			targets = sharedTargets(job.Broadcasts, local)
		}
		if len(targets) == 0 {
			h.log.Warn("wake relay skipped: no link shared with the target",
				"target", job.Target, "job", job.ID)
			continue
		}
		h.mu.Lock()
		seen := h.wakeSeen[job.ID]
		if !seen {
			if h.wakeSeen == nil || len(h.wakeSeen) >= 512 {
				h.wakeSeen = make(map[string]bool)
			}
			h.wakeSeen[job.ID] = true
		}
		h.mu.Unlock()
		if seen {
			continue
		}
		sent, err := wol.SendTo(job.MAC, targets)
		if err != nil {
			h.log.Warn("wake relay failed", "target", job.Target, "mac", job.MAC, "error", err)
			continue
		}
		h.log.Info("wake relayed", "target", job.Target, "mac", job.MAC,
			"packets", sent, "job", job.ID)
	}
}

// wakeTargets reports the directed broadcast endpoints this agent can reach.
// An explicit override wins; otherwise every up, non-loopback IPv4 interface
// contributes its directed broadcast. The limited broadcast is never reported
// because every host would report the same value, so it cannot identify a
// link.
func (h *HostAgent) wakeTargets() []string {
	if len(h.cfg.WakeTargets) > 0 {
		return wol.NormalizeTargets(h.cfg.WakeTargets)
	}
	return append([]string{}, wol.DirectedTargets(wol.Port)...)
}

// sharedTargets returns the endpoints present in both lists, preserving order
// and duplicates from a. An empty result means the job must not be sent.
func sharedTargets(a, b []string) []string {
	if len(a) == 0 || len(b) == 0 {
		return nil
	}
	set := make(map[string]struct{}, len(b))
	for _, value := range b {
		set[value] = struct{}{}
	}
	var out []string
	for _, value := range a {
		if _, ok := set[value]; ok {
			out = append(out, value)
		}
	}
	return out
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
	return h.relayEndpointsLocked()
}

func (h *HostAgent) relayEndpointsLocked() []*net.UDPAddr {
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

// acceptViewerSourceLocked reports whether data from src may become the
// forwarding target. The current remote, a relay endpoint and a recently
// punched viewer address qualify; anything else would let a single stray
// datagram steal the return path.
func (h *HostAgent) acceptViewerSourceLocked(src *net.UDPAddr) bool {
	if h.remote != nil && sameAddr(h.remote, src) {
		return true
	}
	for _, relay := range h.relayEndpointsLocked() {
		if sameAddr(src, relay) {
			return true
		}
	}
	key := src.String()
	expiry, ok := h.punchPeers[key]
	if !ok {
		return false
	}
	if time.Now().After(expiry) {
		delete(h.punchPeers, key)
		return false
	}
	return true
}

// rememberPunchPeer records a viewer address the rendezvous service vouched
// for, with a short expiry.
func (h *HostAgent) rememberPunchPeer(addr *net.UDPAddr) {
	if addr == nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.punchPeers == nil {
		h.punchPeers = make(map[string]time.Time)
	}
	if len(h.punchPeers) >= 64 {
		now := time.Now()
		for key, expiry := range h.punchPeers {
			if now.After(expiry) {
				delete(h.punchPeers, key)
			}
		}
		if len(h.punchPeers) >= 64 {
			return
		}
	}
	h.punchPeers[addr.String()] = time.Now().Add(punchTargetTTL)
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
			h.mu.Lock()
			remote := cloneAddr(h.remote)
			h.mu.Unlock()
			if remote == nil {
				continue
			}
			h.conn.WriteToUDP(data, remote)
			continue
		}
		h.mu.Lock()
		accepted := h.acceptViewerSourceLocked(src)
		if accepted {
			h.remote = cloneAddr(src)
		}
		h.mu.Unlock()
		if !accepted {
			// Not the punched viewer and not the relay: forwarding replies
			// here would let one datagram redirect the whole session.
			continue
		}
		h.noteViewer(src)
		route := src.String()
		id, payload, multiplex := carrier.DecodeMux(data)
		if multiplex {
			if !h.relayAllowed(src) {
				continue
			}
			route += "/" + strconv.FormatUint(id, 10)
			data = payload
		}
		peer := cloneAddr(src)
		_ = h.routes.forward(route, h.local, data, func(reply []byte) error {
			if multiplex {
				reply = carrier.EncodeMux(id, reply)
			}
			if len(reply) == 0 {
				return nil
			}
			_, err := h.conn.WriteToUDP(reply, peer)
			return err
		})
	}
}

// localLoop carries replies from local control addresses that cannot be
// reached from the punch socket's address family; the punch socket still
// carries them to the viewer.
func (h *HostAgent) localLoop() {
	buf := make([]byte, 65535)
	for {
		n, src, err := h.localOut.ReadFromUDP(buf)
		if err != nil {
			if h.ctx.Err() == nil {
				h.log.Error("host agent local read", "error", err)
			}
			return
		}
		if n == 0 || !sameAddr(src, h.local) {
			continue
		}
		h.mu.Lock()
		remote := cloneAddr(h.remote)
		h.mu.Unlock()
		if remote == nil {
			continue
		}
		h.conn.WriteToUDP(buf[:n], remote)
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
	fromPunch := sameAddr(src, h.getPunch()) || sameAddr(src, h.getPunch6())
	fromRelay := h.relayAllowed(src)
	switch msg.Type {
	case wire.TypeKeepaliveChallenge:
		if !fromPunch ||
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
		if !fromPunch {
			return
		}
		if !msg.OK {
			h.log.Warn("keepalive refused", "error", msg.Error)
			return
		}
		if msg.Candidate != "" {
			h.noteObserved(true, msg.Candidate)
		}
		h.keepaliveOnce.Do(func() { close(h.keepaliveReady) })
	case wire.TypePunchPeer:
		if !fromPunch {
			return
		}
		peer, err := resolveUDP(msg.Candidate)
		if err != nil || peer == nil {
			peer, err = resolveUDP(msg.CandidateV6)
			if err != nil || peer == nil {
				return
			}
		}
		h.rememberPunchPeer(peer)
		h.log.Info("punch request from viewer", "candidate", peer.String())
		go h.sendPunches(peer, msg.Nonce)
		// The viewer's gateway mapping stays reachable even when its observed
		// source port differs per destination (symmetric NAT).
		if mapped, err := resolveUDP(candidateWithUpnpPort(msg.Candidate, msg.UpnpPort)); err == nil &&
			mapped != nil && !sameAddr(mapped, peer) {
			h.rememberPunchPeer(mapped)
			h.log.Info("punching the viewer's UPnP mapping", "candidate", mapped.String())
			go h.sendPunches(mapped, msg.Nonce)
		}
	case wire.TypeRelayChallenge:
		if !fromRelay || msg.HostID != h.cfg.ID ||
			msg.Candidate == "" || msg.Nonce == "" {
			return
		}
		candidate := msg.Candidate
		if msg.Multiplex {
			candidate += "\nMUX1"
		}
		signature := identity.Sign(h.priv, identity.RelayMessage(h.cfg.ID, src.Port, candidate, msg.Nonce))
		h.noteObserved(false, msg.Candidate)
		h.conn.WriteToUDP(wire.Encode(wire.Message{
			Type:      wire.TypeRelayAuth,
			Multiplex: msg.Multiplex,
			HostID:    h.cfg.ID,
			Nonce:     msg.Nonce,
			Candidate: msg.Candidate,
			Signature: signature,
		}), src)
	case wire.TypeRelayAck:
		if !fromRelay {
			return
		}
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
		raw := wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: h.cfg.ID, Multiplex: true})
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
	// punchTargetTTL bounds how long a rendezvous-announced viewer address may
	// become the forwarding target. The viewer connects right after the punch,
	// so a longer window would only widen the spoofing window.
	punchTargetTTL = 2 * time.Minute
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

	dialCtx, dialCancel := context.WithTimeout(h.ctx, carrierDial)
	conn, err := h.cfg.Proxy.DialContext(dialCtx, "tcp", relayAddr)
	dialCancel()
	if err != nil {
		return false, fmt.Errorf("relay carrier %s: %w", relayAddr, err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
	hello, err := json.Marshal(carrier.HostHello{HostID: h.cfg.ID, Multiplex: true})
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
	capability := ""
	if challenge.Multiplex {
		capability = "MUX1"
	}
	auth, err := json.Marshal(carrier.HostAuth{
		Multiplex: challenge.Multiplex,
		HostID:    h.cfg.ID,
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(h.priv, identity.CarrierHostMessage(h.cfg.ID, capability, challenge.Nonce)),
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
	if challenge.Multiplex {
		local.Close()
		return true, h.runMuxCarrier(conn, relayFrames, relaySend, relayAddr)
	}
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
		reg, err := h.api.register(ctx, h.cfg.ID, h.cfg.OldID, h.priv, h.cfg.Fingerprint,
			h.cfg.WakeRelay, h.wakeTargets())
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
