package agent

import (
	"context"
	"encoding/json"
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
	"github.com/ltpara/ds-chcast/server/internal/upnp"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	ModeAuto    = "auto"
	ModeDirect  = "direct"
	ModeRelay   = "relay"
	ModeCarrier = "carrier"
)

type ViewerConfig struct {
	ServerURL string
	ServerKey string
	// ServerPublicKey pins the rendezvous server identity. When set, lookups
	// must carry a valid server-signed binding for the host.
	ServerPublicKey string
	HostID          string
	ListenAddr      string
	Mode            string
	DirectTimeout   time.Duration
	Logger          *slog.Logger
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
// unanswered. NAT is the host's advertised NAT classification; Candidates is
// the number of direct addresses the viewer is probing. Carrier and
// CarrierToken are the shared-carrier endpoint and the per-lease credential
// for it, empty when the server has no shared carrier.
type PathInfo struct {
	Path         string `json:"path"`
	Remote       string `json:"remote,omitempty"`
	Direct       string `json:"direct,omitempty"`
	Relay        string `json:"relay,omitempty"`
	NAT          string `json:"nat,omitempty"`
	Candidates   int    `json:"candidates,omitempty"`
	Carrier      string `json:"carrier,omitempty"`
	CarrierToken string `json:"carrier_token,omitempty"`
}

type ViewerAgent struct {
	cfg  ViewerConfig
	conn *net.UDPConn
	api  *apiClient
	log  *slog.Logger

	ctx    context.Context
	cancel context.CancelFunc

	punch  *net.UDPAddr
	punch6 *net.UDPAddr
	direct *net.UDPAddr
	relay  *net.UDPAddr

	// carrier and carrierToken come from the authenticated lookup. The token
	// authorizes this viewer's slot while the host's lease is live.
	carrier      string
	carrierToken string
	// relayOnly means the server hides direct candidates; transport is its
	// hint (direct, relay or carrier). carrierMode forwards local datagrams
	// through the shared TCP carrier instead of a UDP address.
	relayOnly   bool
	transport   string
	carrierMode bool
	carrierConn net.Conn
	carrierMu   sync.Mutex
	carrierSend *carrier.FrameWriter

	mu          sync.Mutex
	remote      *net.UDPAddr
	local       *net.UDPAddr
	latched     bool
	timer       *time.Timer
	timerArmed  bool
	directOK    atomic.Bool
	punched     atomic.Bool
	relayAuthOK atomic.Bool
	relayAuth   sync.Once
	fellBack    bool
	nonce       string
	lastPathKey string
	candidates  []*net.UDPAddr
	natType     string
	upnpPort    int
}

func StartViewer(parent context.Context, cfg ViewerConfig) (*ViewerAgent, error) {
	if cfg.ServerURL == "" || cfg.HostID == "" {
		return nil, fmt.Errorf("viewer agent needs --server and --id")
	}
	if cfg.Mode == "" {
		cfg.Mode = ModeAuto
	}
	if cfg.Mode != ModeAuto && cfg.Mode != ModeDirect && cfg.Mode != ModeRelay && cfg.Mode != ModeCarrier {
		return nil, fmt.Errorf("--mode must be auto, direct, relay or carrier")
	}
	if cfg.DirectTimeout <= 0 {
		cfg.DirectTimeout = 2 * time.Second
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.ServerPublicKey != "" {
		if _, err := identity.ParsePublicKey(cfg.ServerPublicKey); err != nil {
			return nil, fmt.Errorf("--server-pubkey: %w", err)
		}
	}
	listen, err := net.ResolveUDPAddr("udp", cfg.ListenAddr)
	if err != nil {
		return nil, fmt.Errorf("listen address %q: %w", cfg.ListenAddr, err)
	}
	conn, err := net.ListenUDP("udp", listen)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", cfg.ListenAddr, err)
	}
	ctx, cancel := context.WithCancel(parent)
	v := &ViewerAgent{
		cfg:    cfg,
		conn:   conn,
		api:    newAPIClient(cfg.ServerURL, cfg.ServerKey),
		log:    cfg.Logger.With("agent", "viewer", "id", cfg.HostID),
		ctx:    ctx,
		cancel: cancel,
		nonce:  newNonce(),
	}

	lookupCtx, lookupCancel := context.WithTimeout(ctx, 10*time.Second)
	host, err := v.api.lookup(lookupCtx, cfg.HostID)
	lookupCancel()
	if err != nil {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("lookup: %w", err)
	}
	if err := checkHostBinding(cfg.ServerPublicKey, cfg.HostID, host); err != nil {
		conn.Close()
		cancel()
		return nil, err
	}
	if !host.Online {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("host %s is registered but offline", cfg.HostID)
	}
	if cfg.UPnP {
		mapCtx, mapCancel := context.WithTimeout(ctx, 5*time.Second)
		mapper, mapErr := upnp.Map(mapCtx, conn.LocalAddr().(*net.UDPAddr).Port, "chast viewer")
		mapCancel()
		if mapErr != nil {
			v.log.Warn("UPnP mapping unavailable", "error", mapErr)
		} else {
			v.upnpPort = mapper.ExternalPort()
			v.log.Info("UPnP port mapped", "external_port", v.upnpPort,
				"internal_port", conn.LocalAddr().(*net.UDPAddr).Port,
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
		conn.Close()
		cancel()
		return nil, fmt.Errorf("punch address: %w", err)
	}
	// A viewer bound to an IPv6-only socket cannot reach the IPv4 rendezvous
	// port, so it needs the IPv6 endpoint to ask for a punch at all.
	if punch6, err := resolveUDP(host.PunchV6); err == nil && punch6 != nil &&
		familyAllowed(conn.LocalAddr().(*net.UDPAddr).IP, punch6.IP) {
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
	v.candidates = mergeCandidates(conn.LocalAddr().(*net.UDPAddr), candidateTexts)
	if len(v.candidates) > 0 {
		v.direct = v.candidates[0]
	}
	v.natType = host.NATType
	v.carrier = host.Carrier
	v.carrierToken = host.CarrierToken
	v.relayOnly = host.RelayOnly
	v.transport = host.Transport
	relayTexts := host.Relays
	if len(relayTexts) == 0 && host.Relay != "" {
		relayTexts = []string{host.Relay}
	}
	v.relay = selectRelayAddr(conn.LocalAddr().(*net.UDPAddr), relayTexts)
	if v.relay == nil && len(relayTexts) > 0 {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("relay address %q is not reachable from %s", strings.Join(relayTexts, ", "), cfg.ListenAddr)
	}
	carrierReady := v.carrier != "" && v.carrierToken != ""
	useCarrier := false

	switch cfg.Mode {
	case ModeCarrier:
		if !carrierReady {
			conn.Close()
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
			conn.Close()
			cancel()
			return nil, fmt.Errorf("host has no relay or carrier address")
		}
	case ModeDirect:
		if host.RelayOnly {
			conn.Close()
			cancel()
			return nil, fmt.Errorf("server forces relay; direct mode is unavailable")
		}
		if v.direct == nil {
			conn.Close()
			cancel()
			return nil, fmt.Errorf("host has no direct candidate")
		}
		v.remote = v.direct
		v.latched = true
	default:
		switch {
		case !host.RelayOnly && v.direct != nil:
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
			conn.Close()
			cancel()
			return nil, fmt.Errorf("host has neither a direct, relay nor carrier address")
		}
	}
	v.carrierMode = useCarrier

	go v.readLoop()
	if v.remote != nil && v.relay != nil && sameAddr(v.remote, v.relay) {
		v.startRelayAuth()
	}
	if v.carrierMode {
		go v.carrierLoop()
	} else if cfg.Mode != ModeRelay && !host.RelayOnly && v.punch != nil && v.direct != nil {
		v.sendPunchRequest()
		go v.punchLoop()
	}
	path := "relay " + addrText(v.relay)
	if v.carrierMode {
		path = "carrier " + v.carrier
	} else if v.remote != nil && sameAddr(v.remote, v.direct) {
		path = "direct " + addrText(v.direct)
	}
	v.log.Info("viewer agent listening", "on", conn.LocalAddr().String(), "path", path)
	v.reportPath()
	return v, nil
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
	if v.carrierMode {
		path = "carrier"
		remote = v.carrier
	} else if v.remote != nil {
		if v.relay != nil && sameAddr(v.remote, v.relay) {
			path = "relay"
		} else {
			path = "direct"
		}
	}
	return PathInfo{
		Path:         path,
		Remote:       remote,
		Direct:       addrValue(v.direct),
		Relay:        addrValue(v.relay),
		NAT:          v.natType,
		Candidates:   len(v.candidates),
		Carrier:      v.carrier,
		CarrierToken: v.carrierToken,
	}
}

func (v *ViewerAgent) reportPath() {
	v.mu.Lock()
	info := v.pathLocked()
	key := info.Path + "|" + info.Remote + "|" + info.NAT + "|" + strconv.Itoa(info.Candidates)
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
	v.carrierMu.Lock()
	if v.carrierConn != nil {
		v.carrierConn.Close()
	}
	v.carrierMu.Unlock()
	v.conn.Close()
}

func (v *ViewerAgent) sendPunchRequest() {
	if v.punch == nil && v.punch6 == nil {
		return
	}
	msg := wire.Message{Type: wire.TypePunchRequest, HostID: v.cfg.HostID, Nonce: v.nonce, UpnpPort: v.upnpPort}
	if v.cfg.ServerKey != "" {
		msg.Timestamp = time.Now().Unix()
		msg.Auth = identity.PunchAuth(v.cfg.ServerKey, v.cfg.HostID, v.nonce, msg.Timestamp)
	}
	encoded := wire.Encode(msg)
	if v.punch != nil {
		v.conn.WriteToUDP(encoded, v.punch)
	}
	if v.punch6 != nil {
		v.conn.WriteToUDP(encoded, v.punch6)
	}
}

const (
	relayAuthAttempts = 6
	relayAuthInterval = 500 * time.Millisecond
)

// startRelayAuth proves the lookup token to the relay before the first QUIC
// datagram. It is safe to call on both the initial relay path and a fallback.
func (v *ViewerAgent) startRelayAuth() {
	if v.relay == nil {
		return
	}
	v.relayAuth.Do(func() { go v.relayAuthLoop() })
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
	for i := 0; i < relayAuthAttempts; i++ {
		if v.relayAuthOK.Load() {
			return
		}
		if _, err := v.conn.WriteToUDP(msg, v.relay); err != nil {
			v.log.Debug("relay viewer auth write", "error", err)
		}
		select {
		case <-v.ctx.Done():
			return
		case <-time.After(relayAuthInterval):
		}
	}
}

func (v *ViewerAgent) punchLoop() {
	for i := 0; i < 6; i++ {
		if v.punched.Load() {
			return
		}
		if i > 0 {
			v.sendPunchRequest()
		}
		v.sendPunchesTo(v.getCandidates())
		select {
		case <-v.ctx.Done():
			return
		case <-time.After(500 * time.Millisecond):
		}
	}
	if !v.punched.Load() {
		v.log.Warn("no punch reply from the rendezvous service", "nonce", v.nonce)
	}
}

const (
	viewerCarrierDial      = 5 * time.Second
	viewerCarrierIdle      = 60 * time.Second
	viewerCarrierKeepalive = 30 * time.Second
	viewerCarrierRetryMax  = 5 * time.Second
)

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
			v.log.Warn("carrier link lost", "carrier", v.carrier, "error", err)
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
	dialer := &net.Dialer{Timeout: viewerCarrierDial}
	conn, err := dialer.DialContext(v.ctx, "tcp", v.carrier)
	if err != nil {
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
	defer func() {
		v.carrierMu.Lock()
		if v.carrierConn == conn {
			v.carrierConn = nil
			v.carrierSend = nil
		}
		v.carrierMu.Unlock()
		conn.Close()
	}()
	v.log.Info("carrier link established", "carrier", v.carrier)
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

func (v *ViewerAgent) sendPunchesTo(peers []*net.UDPAddr) {
	if len(peers) == 0 {
		return
	}
	punch := wire.Encode(wire.Message{Type: wire.TypePunch, Nonce: v.nonce})
	for _, peer := range peers {
		if peer != nil {
			v.conn.WriteToUDP(punch, peer)
		}
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

func (v *ViewerAgent) readLoop() {
	buf := make([]byte, 65535)
	firstLocal := true
	for {
		n, src, err := v.conn.ReadFromUDP(buf)
		if err != nil {
			if v.ctx.Err() == nil {
				v.log.Error("viewer agent read", "error", err)
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
				v.handleControl(data)
				continue
			}
			if msg, err := wire.Decode(data); err == nil && msg.Type == wire.TypePunch {
				v.onDirectPacket(src)
			}
			continue
		}
		if sameAddr(src, punch) || sameAddr(src, punch6) {
			continue
		}
		if sameAddr(src, relay) {
			v.mu.Lock()
			if v.remote == nil || !sameAddr(v.remote, v.relay) {
				v.mu.Unlock()
				continue
			}
			local := cloneAddr(v.local)
			v.mu.Unlock()
			if local != nil {
				v.conn.WriteToUDP(data, local)
			}
			continue
		}
		if sameAddr(src, direct) || sameAddr(src, relay) || !src.IP.IsLoopback() {
			// Host data, either from the announced candidate or from the
			// address the host's NAT actually mapped us to.
			v.onDirectPacket(src)
			v.mu.Lock()
			local := cloneAddr(v.local)
			v.mu.Unlock()
			if local != nil {
				v.conn.WriteToUDP(data, local)
			}
			continue
		}
		// Everything else on a loopback-only listener is the local QUIC client.
		// A reconnect creates a new local source port, so the newest loopback
		// sender always becomes the client instead of being ignored forever.
		v.mu.Lock()
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
		if v.cfg.Mode == ModeAuto && !v.timerArmed && remote != nil && !sameAddr(remote, v.relay) {
			v.timerArmed = true
			v.timer = time.AfterFunc(v.cfg.DirectTimeout, v.onDirectTimeout)
		}
		v.mu.Unlock()
		if v.carrierMode {
			v.writeCarrier(data)
			continue
		}
		if remote == nil {
			continue
		}
		v.conn.WriteToUDP(data, remote)
	}
}

func (v *ViewerAgent) onDirectPacket(src *net.UDPAddr) {
	if v.carrierMode {
		return
	}
	v.directOK.Store(true)
	v.mu.Lock()
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
	v.reportPath()
}

func (v *ViewerAgent) onDirectTimeout() {
	v.mu.Lock()
	if v.latched || v.ctx.Err() != nil || v.relay == nil ||
		v.remote == nil || sameAddr(v.remote, v.relay) {
		v.mu.Unlock()
		return
	}
	v.latched = true
	v.fellBack = true
	v.remote = cloneAddr(v.relay)
	v.log.Warn("direct path did not answer; using relay", "relay", v.relay.String())
	v.mu.Unlock()
	v.startRelayAuth()
	v.reportPath()
}

func (v *ViewerAgent) handleControl(data []byte) {
	msg, err := wire.Decode(data)
	if err != nil {
		return
	}
	switch msg.Type {
	case wire.TypePunchPeer:
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
			merged := mergeCandidates(v.conn.LocalAddr().(*net.UDPAddr), addrList(v.candidates), texts)
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
				v.punched.Store(true)
				go v.sendPunchesTo(merged)
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
