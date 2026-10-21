package agent

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	ModeAuto   = "auto"
	ModeDirect = "direct"
	ModeRelay  = "relay"
)

type ViewerConfig struct {
	ServerURL     string
	ServerKey     string
	HostID        string
	ListenAddr    string
	Mode          string
	DirectTimeout time.Duration
	Logger        *slog.Logger
	// OnPath receives every direct/relay change, starting with the initial
	// path once StartViewer has resolved the host. It is called from the read
	// or timer goroutine, so it must not block.
	OnPath func(PathInfo)
}

// PathInfo is the viewer agent's current forwarding path. Path is "direct",
// "relay" or "connecting" while the first direct probe is still unanswered.
type PathInfo struct {
	Path   string `json:"path"`
	Remote string `json:"remote,omitempty"`
	Direct string `json:"direct,omitempty"`
	Relay  string `json:"relay,omitempty"`
}

type ViewerAgent struct {
	cfg  ViewerConfig
	conn *net.UDPConn
	api  *apiClient
	log  *slog.Logger

	ctx    context.Context
	cancel context.CancelFunc

	punch  *net.UDPAddr
	direct *net.UDPAddr
	relay  *net.UDPAddr

	mu          sync.Mutex
	remote      *net.UDPAddr
	local       *net.UDPAddr
	latched     bool
	timer       *time.Timer
	timerArmed  bool
	directOK    atomic.Bool
	punched     atomic.Bool
	fellBack    bool
	nonce       string
	lastPathKey string
}

func StartViewer(parent context.Context, cfg ViewerConfig) (*ViewerAgent, error) {
	if cfg.ServerURL == "" || cfg.HostID == "" {
		return nil, fmt.Errorf("viewer agent needs --server and --id")
	}
	if cfg.Mode == "" {
		cfg.Mode = ModeAuto
	}
	if cfg.Mode != ModeAuto && cfg.Mode != ModeDirect && cfg.Mode != ModeRelay {
		return nil, fmt.Errorf("--mode must be auto, direct or relay")
	}
	if cfg.DirectTimeout <= 0 {
		cfg.DirectTimeout = 2 * time.Second
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
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
	if !host.Online {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("host %s is registered but offline", cfg.HostID)
	}
	v.punch, err = resolveUDP(host.Punch)
	if err != nil {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("punch address: %w", err)
	}
	v.direct, err = resolveUDP(host.Candidate)
	if err != nil {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("host candidate: %w", err)
	}
	v.relay, err = resolveUDP(host.Relay)
	if err != nil {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("relay address: %w", err)
	}

	switch cfg.Mode {
	case ModeRelay:
		if v.relay == nil {
			conn.Close()
			cancel()
			return nil, fmt.Errorf("host has no relay address")
		}
		v.remote = v.relay
		v.latched = true
	case ModeDirect:
		if v.direct == nil {
			conn.Close()
			cancel()
			return nil, fmt.Errorf("host has no direct candidate")
		}
		v.remote = v.direct
		v.latched = true
	default:
		if v.direct != nil {
			v.remote = v.direct
		} else if v.relay != nil {
			v.remote = v.relay
			v.latched = true
			v.log.Warn("host has no direct candidate; using relay")
		} else {
			conn.Close()
			cancel()
			return nil, fmt.Errorf("host has neither a direct nor a relay address")
		}
	}

	go v.readLoop()
	if cfg.Mode != ModeRelay && v.punch != nil && v.direct != nil {
		v.sendPunchRequest()
		go v.punchLoop()
	}
	path := "relay " + addrText(v.relay)
	if v.remote != nil && sameAddr(v.remote, v.direct) {
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
	if v.remote != nil {
		if v.relay != nil && sameAddr(v.remote, v.relay) {
			path = "relay"
		} else {
			path = "direct"
		}
	}
	return PathInfo{
		Path:   path,
		Remote: addrValue(v.remote),
		Direct: addrValue(v.direct),
		Relay:  addrValue(v.relay),
	}
}

func (v *ViewerAgent) reportPath() {
	v.mu.Lock()
	info := v.pathLocked()
	key := info.Path + "|" + info.Remote
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
	v.conn.Close()
}

func (v *ViewerAgent) sendPunchRequest() {
	if v.punch == nil {
		return
	}
	msg := wire.Message{Type: wire.TypePunchRequest, HostID: v.cfg.HostID, Nonce: v.nonce}
	v.conn.WriteToUDP(wire.Encode(msg), v.punch)
}

func (v *ViewerAgent) punchLoop() {
	for i := 0; i < 6; i++ {
		if v.punched.Load() {
			return
		}
		if i > 0 {
			v.sendPunchRequest()
		}
		v.sendPunchesTo(v.getDirect())
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

func (v *ViewerAgent) sendPunchesTo(peer *net.UDPAddr) {
	if peer == nil {
		return
	}
	punch := wire.Encode(wire.Message{Type: wire.TypePunch, Nonce: v.nonce})
	v.conn.WriteToUDP(punch, peer)
}

func (v *ViewerAgent) getDirect() *net.UDPAddr {
	v.mu.Lock()
	defer v.mu.Unlock()
	return cloneAddr(v.direct)
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
		punch, direct, relay := cloneAddr(v.punch), cloneAddr(v.direct), cloneAddr(v.relay)
		v.mu.Unlock()

		if wire.IsControl(data) {
			if sameAddr(src, punch) || sameAddr(src, relay) {
				v.handleControl(data)
				continue
			}
			if msg, err := wire.Decode(data); err == nil && msg.Type == wire.TypePunch {
				v.onDirectPacket(src)
			}
			continue
		}
		if sameAddr(src, punch) {
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
		if remote == nil {
			continue
		}
		v.conn.WriteToUDP(data, remote)
	}
}

func (v *ViewerAgent) onDirectPacket(src *net.UDPAddr) {
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
		if msg.Candidate != "" {
			peer, err := resolveUDP(msg.Candidate)
			if err == nil && peer != nil {
				v.mu.Lock()
				if !sameAddr(v.direct, peer) {
					v.direct = peer
				}
				v.mu.Unlock()
				v.punched.Store(true)
				go v.sendPunchesTo(peer)
			}
		}
	case wire.TypeBusy:
		v.log.Warn("relay is busy", "error", msg.Error)
	}
	v.reportPath()
}

func addrText(a *net.UDPAddr) string {
	if a == nil {
		return "none"
	}
	return a.String()
}
