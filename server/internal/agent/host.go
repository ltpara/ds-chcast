// Package agent runs the two tunnel endpoints. The host agent registers the
// machine, maintains its NAT mapping with keepalives, and forwards datagrams
// between chast-host and whichever path a viewer chose. The viewer agent makes
// the opposite choice: direct first when it is asked to, relay when the direct
// path stays silent.
package agent

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

type HostConfig struct {
	ServerURL   string
	ServerKey   string
	ID          string
	Key         string
	Fingerprint string
	// LocalAddr is chast-host's QUIC listener, usually 127.0.0.1:4433.
	LocalAddr string
	// ListenAddr is the public-facing UDP socket used for all traffic.
	ListenAddr string
	Keepalive  time.Duration
	Logger     *slog.Logger
}

type HostAgent struct {
	cfg   HostConfig
	conn  *net.UDPConn
	api   *apiClient
	log   *slog.Logger
	local *net.UDPAddr

	ctx    context.Context
	cancel context.CancelFunc

	mu      sync.Mutex
	punch   *net.UDPAddr
	relay   *net.UDPAddr
	refresh time.Duration

	// remote is only touched by the read loop: it is the source of the last
	// datagram that came from a viewer path (direct or via the relay).
	remote *net.UDPAddr

	keepaliveOnce  sync.Once
	relayOnce      sync.Once
	keepaliveReady chan struct{}
	relayReady     chan struct{}
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

	ctx, cancel := context.WithCancel(parent)
	h := &HostAgent{
		cfg:            cfg,
		conn:           conn,
		api:            newAPIClient(cfg.ServerURL, cfg.ServerKey),
		log:            cfg.Logger.With("agent", "host", "id", cfg.ID),
		local:          local,
		ctx:            ctx,
		cancel:         cancel,
		refresh:        20 * time.Second,
		keepaliveReady: make(chan struct{}),
		relayReady:     make(chan struct{}),
	}
	registerCtx, registerCancel := context.WithTimeout(ctx, 10*time.Second)
	reg, err := h.api.register(registerCtx, cfg.ID, cfg.Key, cfg.Fingerprint)
	registerCancel()
	if err != nil {
		conn.Close()
		cancel()
		return nil, fmt.Errorf("register: %w", err)
	}
	h.applyRegistration(reg)

	go h.readLoop()
	go h.keepaliveLoop()
	go h.relayHelloLoop()
	go h.refreshLoop()

	h.log.Info("host agent listening", "on", conn.LocalAddr().String(), "forwarding_to", local.String(), "relay", reg.Relay)
	return h, nil
}

func (h *HostAgent) LocalAddr() net.Addr { return h.conn.LocalAddr() }

func (h *HostAgent) Close() {
	h.cancel()
	h.conn.Close()
}

// WaitReady waits for the first keepalive acknowledgement, which is what makes
// the host's observed address usable for punching. The relay acknowledgement is
// waited for briefly but is not required: a direct-only deployment may not have
// a relay.
func (h *HostAgent) WaitReady(ctx context.Context) error {
	select {
	case <-h.keepaliveReady:
	case <-ctx.Done():
		return ctx.Err()
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

func (h *HostAgent) applyRegistration(reg registerResult) {
	punch, err := resolveUDP(reg.Punch)
	if err != nil {
		h.log.Warn("registration returned an unusable punch address", "punch", reg.Punch, "error", err)
	}
	relay, err := resolveUDP(reg.Relay)
	if err != nil {
		h.log.Warn("registration returned an unusable relay address", "relay", reg.Relay, "error", err)
	}
	h.mu.Lock()
	if punch != nil {
		h.punch = punch
	}
	if relay != nil && !sameAddr(h.relay, relay) {
		h.relay = relay
		h.log.Info("relay address updated", "relay", relay.String())
	}
	if reg.ExpiresInSec > 0 {
		refresh := time.Duration(reg.ExpiresInSec) * time.Second / 2
		if refresh < 5*time.Second {
			refresh = 5 * time.Second
		}
		h.refresh = refresh
	}
	h.mu.Unlock()
}

func (h *HostAgent) getPunch() *net.UDPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	return cloneAddr(h.punch)
}

func (h *HostAgent) getRelay() *net.UDPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	return cloneAddr(h.relay)
}

func (h *HostAgent) readLoop() {
	buf := make([]byte, 65535)
	firstViewer := true
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
		punch, relay := cloneAddr(h.punch), cloneAddr(h.relay)
		h.mu.Unlock()

		if wire.IsControl(data) {
			if sameAddr(src, punch) || sameAddr(src, relay) {
				h.handleControl(data)
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
		if firstViewer {
			firstViewer = false
			h.log.Info("viewer path established", "from", src.String())
		}
		h.remote = cloneAddr(src)
		h.conn.WriteToUDP(data, h.local)
	}
}

func (h *HostAgent) handleControl(data []byte) {
	msg, err := wire.Decode(data)
	if err != nil {
		return
	}
	switch msg.Type {
	case wire.TypeKeepaliveAck:
		if !msg.OK {
			h.log.Warn("keepalive refused", "error", msg.Error)
			return
		}
		h.keepaliveOnce.Do(func() { close(h.keepaliveReady) })
	case wire.TypePunchPeer:
		peer, err := resolveUDP(msg.Candidate)
		if err != nil || peer == nil {
			return
		}
		h.log.Info("punch request from viewer", "candidate", peer.String())
		go h.sendPunches(peer, msg.Nonce)
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

func (h *HostAgent) keepaliveLoop() {
	ticker := time.NewTicker(h.cfg.Keepalive)
	defer ticker.Stop()
	send := func() {
		punch := h.getPunch()
		if punch == nil {
			return
		}
		msg := wire.Message{Type: wire.TypeKeepalive, HostID: h.cfg.ID, KeyHash: keyHash(h.cfg.Key)}
		h.conn.WriteToUDP(wire.Encode(msg), punch)
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

func (h *HostAgent) relayHelloLoop() {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	send := func() {
		relay := h.getRelay()
		if relay == nil {
			return
		}
		msg := wire.Message{Type: wire.TypeRelayHello, HostID: h.cfg.ID, KeyHash: keyHash(h.cfg.Key)}
		h.conn.WriteToUDP(wire.Encode(msg), relay)
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
		reg, err := h.api.register(ctx, h.cfg.ID, h.cfg.Key, h.cfg.Fingerprint)
		cancel()
		if err != nil {
			h.log.Warn("registration refresh failed", "error", err)
			h.mu.Lock()
			h.refresh = 2 * time.Second
			h.mu.Unlock()
			continue
		}
		h.applyRegistration(reg)
	}
}
