// Package relay forwards opaque UDP datagrams between a viewer and one host.
//
// Each host is assigned its own relay port while it is registered. The host
// agent keeps that port bound to its current address with relay_hello; a viewer
// simply sends QUIC to the port. Datagrams are copied verbatim, so the QUIC
// connection stays end-to-end between the viewer and chast-host and the relay
// never needs to understand chast.
//
// The same port also accepts TCP and WebSocket carriers for networks that
// block UDP. A host agent keeps one authenticated carrier link to the port and
// the relay pairs it with the current viewer; the relay still only moves
// opaque QUIC datagrams.
package relay

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// Authenticator is implemented by the registry. Challenge proofs are one-time
// and signed with the host's long-term key, so a relay port cannot be claimed
// by replaying a hello or by learning a static secret.
type Authenticator interface {
	VerifyHost(id string, port int) bool
	// HostOnline reports a live registration without requiring a port, which
	// is what the shared carrier link uses.
	HostOnline(id string) bool
	VerifyCarrier(id, token string) bool
	IssueChallenge(id, purpose string) (string, error)
	CheckChallenge(id, purpose, nonce, message, signature string) bool
}

const (
	hostIdle          = 90 * time.Second
	viewerIdle        = 60 * time.Second
	carrierHandshake  = 10 * time.Second
	carrierViewerIdle = 180 * time.Second
	carrierWrite      = 30 * time.Second
	maxDatagram       = carrier.MaxPayload
)

var ErrNoFreePort = errors.New("no free relay port")

type Manager struct {
	mu     sync.Mutex
	ports  map[int]*port
	order  []int
	auth   Authenticator
	log    *slog.Logger
	shared *SharedCarrier
	policy *Policy
	bindIP net.IP
	closed bool
}

type port struct {
	manager *Manager
	number  int

	mu         sync.Mutex
	conn       *net.UDPConn
	tcp        net.Listener
	boundIP    net.IP
	boundHost  string
	hostID     string
	hostAddr   *net.UDPAddr
	hostSeen   time.Time
	viewerAddr *net.UDPAddr
	viewerSeen time.Time
	toHost     uint64
	toViewer   uint64
	busy       uint64
	dropped    uint64

	// hostLink is the host agent's authenticated TCP carrier link. It outlives
	// viewers so the next one can pair without a new handshake.
	hostLink   *hostLink
	viewer     *carrierViewer
	udpSession *Session

	tcpToHost   uint64
	tcpToViewer uint64
	tcpViewers  uint64
	tcpBusy     uint64
	blocked     uint64
}

type hostLink struct {
	conn net.Conn
	send *carrier.FrameWriter
}

// sendFrame refreshes the write deadline so a wedged peer cannot pin the
// forwarding loop forever.
func (l *hostLink) sendFrame(payload []byte) error {
	_ = l.conn.SetWriteDeadline(time.Now().Add(carrierWrite))
	return l.send.WriteFrame(payload)
}

type carrierViewer struct {
	conn   net.Conn
	ws     *carrier.WSConn
	send   *carrier.FrameWriter
	frames *carrier.FrameReader
	peer   string

	mu      sync.Mutex
	session *Session
}

// take charges payload against the viewer's policy session. A nil session
// (closed viewer) forwards without charging.
func (v *carrierViewer) take(payload int) error {
	v.mu.Lock()
	s := v.session
	v.mu.Unlock()
	if s == nil {
		return nil
	}
	return s.Take(payload)
}

func (v *carrierViewer) readFrame() ([]byte, error) {
	if v.ws != nil {
		return v.ws.ReadMessage()
	}
	return v.frames.ReadFrame()
}

func (v *carrierViewer) writeFrame(payload []byte) error {
	_ = v.conn.SetWriteDeadline(time.Now().Add(carrierWrite))
	if v.ws != nil {
		return v.ws.WriteBinary(payload)
	}
	return v.send.WriteFrame(payload)
}

func (v *carrierViewer) close() {
	v.mu.Lock()
	session := v.session
	v.session = nil
	v.mu.Unlock()
	if session != nil {
		session.Close()
	}
	if v.ws != nil {
		_ = v.ws.Close()
		return
	}
	_ = v.conn.Close()
}

func peerIP(peer string) string {
	host, _, err := net.SplitHostPort(peer)
	if err != nil {
		return peer
	}
	return host
}

// NewManager binds every requested UDP port and serves it for the lifetime of
// the manager. Each port also gets a TCP carrier listener; when it cannot be
// bound the port keeps serving UDP without a carrier. A port of 0 asks the
// kernel for an ephemeral port and is useful in tests; callers use Ports to
// learn the actual numbers.
func NewManager(bind string, ports []int, auth Authenticator, log *slog.Logger) (*Manager, error) {
	if log == nil {
		log = slog.Default()
	}
	ip := net.ParseIP(bind)
	if bind == "" {
		ip = net.IPv4zero
	} else if ip == nil {
		return nil, fmt.Errorf("relay bind %q is not an IP address", bind)
	}
	policy, err := newPolicy(DefaultPolicyConfig())
	if err != nil {
		return nil, err
	}
	m := &Manager{
		ports:  make(map[int]*port),
		auth:   auth,
		log:    log,
		policy: policy,
		bindIP: ip,
	}
	for _, wanted := range ports {
		conn, tcp, err := m.listenPort(ip, wanted)
		if err != nil {
			m.Close()
			return nil, err
		}
		actual := conn.LocalAddr().(*net.UDPAddr).Port
		p := &port{manager: m, conn: conn, tcp: tcp, number: actual, boundIP: ip}
		m.ports[actual] = p
		m.order = append(m.order, actual)
		go p.serve(conn)
		if tcp != nil {
			go p.serveTCP(tcp)
		}
	}
	if len(m.ports) == 0 {
		return nil, errors.New("relay needs at least one port")
	}
	return m, nil
}

func (m *Manager) listenPort(ip net.IP, wanted int) (*net.UDPConn, net.Listener, error) {
	attempts := 1
	if wanted == 0 {
		// An ephemeral UDP port may collide with an unrelated TCP listener;
		// the carrier needs both, so try another candidate.
		attempts = 10
	}
	for attempt := 0; attempt < attempts; attempt++ {
		udp, err := net.ListenUDP("udp", &net.UDPAddr{IP: ip, Port: wanted})
		if err != nil {
			return nil, nil, fmt.Errorf("relay port %d: %w", wanted, err)
		}
		actual := udp.LocalAddr().(*net.UDPAddr).Port
		tcp, err := net.ListenTCP("tcp", &net.TCPAddr{IP: ip, Port: actual})
		if err == nil {
			return udp, tcp, nil
		}
		udp.Close()
		if wanted == 0 {
			continue
		}
		m.log.Warn("relay carrier disabled: cannot bind the TCP twin",
			"port", wanted, "error", err)
		udp, err = net.ListenUDP("udp", &net.UDPAddr{IP: ip, Port: wanted})
		if err != nil {
			return nil, nil, fmt.Errorf("relay port %d: %w", wanted, err)
		}
		return udp, nil, nil
	}
	return nil, nil, errors.New("relay port 0: no UDP port with a free TCP twin")
}

// listenOn binds one UDP port and, when possible, its TCP carrier twin on the
// same address. A missing TCP twin is reported to the caller as nil.
func (m *Manager) listenOn(ip net.IP, number int) (*net.UDPConn, net.Listener, error) {
	udp, err := net.ListenUDP("udp", &net.UDPAddr{IP: ip, Port: number})
	if err != nil {
		return nil, nil, err
	}
	tcp, err := net.ListenTCP("tcp", &net.TCPAddr{IP: ip, Port: number})
	if err != nil {
		m.log.Warn("relay carrier disabled: cannot bind the TCP twin",
			"port", number, "address", ip.String(), "error", err)
		tcp = nil
	}
	return udp, tcp, nil
}

// BindHost rebinds a port's listeners to the local address behind host, so a
// viewer that dialed that address sees relay replies from the same source
// address. host may be a name; names that do not resolve, or addresses that
// are not local, keep the port on the configured --relay-listen address.
//
// The host agent authenticates on whichever address its lease names, and the
// per-port state is shared, so the switch does not need a new handshake.
func (m *Manager) BindHost(number int, host string) error {
	m.mu.Lock()
	p := m.ports[number]
	m.mu.Unlock()
	if p == nil {
		return fmt.Errorf("relay port %d is not managed here", number)
	}
	host = strings.TrimSpace(strings.Trim(host, "[]"))
	if host == "" {
		return nil
	}
	target := resolveRelayIP(host)
	if target == nil {
		m.log.Warn("relay address does not resolve; using the configured bind",
			"port", number, "address", host)
		target = m.bindIP
	}

	p.mu.Lock()
	if p.boundHost == host && p.conn != nil {
		p.mu.Unlock()
		return nil
	}
	if p.boundIP != nil && p.boundIP.Equal(target) && p.conn != nil {
		p.boundHost = host
		p.mu.Unlock()
		return nil
	}
	oldConn, oldTCP := p.conn, p.tcp
	p.conn, p.tcp = nil, nil
	// Keep the requested name so renewals of the same lease are a no-op even
	// after falling back to the configured bind.
	p.boundHost = host
	p.mu.Unlock()

	if oldConn != nil {
		oldConn.Close()
	}
	if oldTCP != nil {
		oldTCP.Close()
	}

	udp, tcp, err := m.listenOn(target, number)
	if err != nil && !target.Equal(m.bindIP) {
		m.log.Warn("relay address is not local; using the configured bind",
			"port", number, "address", host, "error", err)
		target = m.bindIP
		udp, tcp, err = m.listenOn(target, number)
	}
	if err != nil {
		m.log.Error("relay port lost its listener", "port", number, "error", err)
		return err
	}
	if m.isClosed() {
		udp.Close()
		if tcp != nil {
			tcp.Close()
		}
		return errors.New("relay manager is closed")
	}

	p.mu.Lock()
	if p.conn != nil {
		// A concurrent bind won the race; keep its listeners.
		p.mu.Unlock()
		udp.Close()
		if tcp != nil {
			tcp.Close()
		}
		return nil
	}
	p.conn, p.tcp, p.boundIP = udp, tcp, target
	p.mu.Unlock()
	go p.serve(udp)
	if tcp != nil {
		go p.serveTCP(tcp)
	}
	return nil
}

func (m *Manager) isClosed() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.closed
}

// resolveRelayIP resolves a relay list entry for binding. Names can point at
// several addresses; the first one is used, matching the viewer's resolution.
func resolveRelayIP(host string) net.IP {
	if ip := net.ParseIP(host); ip != nil {
		return ip
	}
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	addrs, err := net.DefaultResolver.LookupIPAddr(ctx, host)
	if err != nil || len(addrs) == 0 {
		return nil
	}
	return addrs[0].IP
}

func (m *Manager) Ports() []int {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]int, len(m.order))
	copy(out, m.order)
	return out
}

// Allocate reserves a free port for hostID, or returns the port it already
// holds. The reservation lasts until Release or the registry lease expires.
func (m *Manager) Allocate(hostID string) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, number := range m.order {
		p := m.ports[number]
		p.mu.Lock()
		held := p.hostID
		p.mu.Unlock()
		if held == hostID {
			return number, nil
		}
	}
	for _, number := range m.order {
		p := m.ports[number]
		p.mu.Lock()
		free := p.hostID == ""
		if free {
			p.hostID = hostID
			p.hostAddr = nil
			p.viewerAddr = nil
			p.hostSeen = time.Time{}
			p.viewerSeen = time.Time{}
			if p.udpSession != nil {
				p.udpSession.Close()
				p.udpSession = nil
			}
		}
		p.mu.Unlock()
		if free {
			return number, nil
		}
	}
	return 0, ErrNoFreePort
}

// Release drops the host binding, any viewer association and the carrier link.
// The listener stays up so the port can be handed to the next host.
func (m *Manager) Release(hostID string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, number := range m.order {
		p := m.ports[number]
		p.mu.Lock()
		if p.hostID != hostID {
			p.mu.Unlock()
			continue
		}
		p.hostID = ""
		p.hostAddr = nil
		p.viewerAddr = nil
		p.hostSeen = time.Time{}
		p.viewerSeen = time.Time{}
		if p.udpSession != nil {
			p.udpSession.Close()
			p.udpSession = nil
		}
		link, viewer := p.hostLink, p.viewer
		p.hostLink, p.viewer = nil, nil
		p.mu.Unlock()
		if link != nil {
			link.conn.Close()
		}
		if viewer != nil {
			viewer.close()
		}
	}
}

func (m *Manager) Close() {
	m.mu.Lock()
	m.closed = true
	for _, p := range m.ports {
		p.mu.Lock()
		conn, tcp := p.conn, p.tcp
		p.conn, p.tcp = nil, nil
		if p.udpSession != nil {
			p.udpSession.Close()
			p.udpSession = nil
		}
		link, viewer := p.hostLink, p.viewer
		p.hostLink, p.viewer = nil, nil
		p.mu.Unlock()
		if conn != nil {
			conn.Close()
		}
		if tcp != nil {
			tcp.Close()
		}
		if link != nil {
			link.conn.Close()
		}
		if viewer != nil {
			viewer.close()
		}
	}
	shared := m.shared
	m.shared = nil
	m.mu.Unlock()
	if shared != nil {
		shared.Close()
	}
}

// EnableSharedCarrier starts the single-port carrier used by hosts whose
// registration names it. It is optional: per-port TCP carriers stay available.
func (m *Manager) EnableSharedCarrier(bind string) error {
	sc, err := newSharedCarrier(bind, m.auth, m.policy, m.log)
	if err != nil {
		return err
	}
	m.mu.Lock()
	if m.shared != nil {
		m.mu.Unlock()
		sc.Close()
		return errors.New("the shared carrier is already enabled")
	}
	m.shared = sc
	m.mu.Unlock()
	return nil
}

// CarrierAddr is the bound address of the shared carrier, empty when it is
// disabled.
func (m *Manager) CarrierAddr() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.shared == nil {
		return ""
	}
	return m.shared.Addr()
}

func (m *Manager) CarrierPort() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.shared == nil {
		return 0
	}
	return m.shared.Port()
}

// HostLinked reports whether the host has an authenticated shared-carrier
// link. The API treats this as an online signal when UDP keepalives cannot
// reach the rendezvous port.
func (m *Manager) HostLinked(hostID string) bool {
	m.mu.Lock()
	shared := m.shared
	m.mu.Unlock()
	return shared != nil && shared.HostLinked(hostID)
}

func (m *Manager) SharedCarrierStats() (SharedCarrierStats, bool) {
	m.mu.Lock()
	sc := m.shared
	m.mu.Unlock()
	if sc == nil {
		return SharedCarrierStats{}, false
	}
	return sc.Stats(), true
}

type PortStats struct {
	Port        int    `json:"port"`
	HostID      string `json:"host_id,omitempty"`
	HostOnline  bool   `json:"host_online"`
	Viewer      string `json:"viewer,omitempty"`
	ToHost      uint64 `json:"to_host_bytes"`
	ToViewer    uint64 `json:"to_viewer_bytes"`
	Busy        uint64 `json:"busy"`
	Dropped     uint64 `json:"dropped"`
	TCPToHost   uint64 `json:"tcp_to_host_bytes"`
	TCPToViewer uint64 `json:"tcp_to_viewer_bytes"`
	TCPViewers  uint64 `json:"tcp_viewers"`
	TCPBusy     uint64 `json:"tcp_busy"`
	Blocked     uint64 `json:"blocked_sessions"`
}

type Stats struct {
	Ports         []PortStats         `json:"ports"`
	SharedCarrier *SharedCarrierStats `json:"shared_carrier,omitempty"`
	Policy        PolicyStats         `json:"policy"`
	Sessions      []SessionUsage      `json:"sessions,omitempty"`
}

// Policy exposes the relay policy for console commands and tests.
func (m *Manager) Policy() *Policy { return m.policy }

// SetPolicyConfig applies bandwidth, downgrade and address-list settings.
func (m *Manager) SetPolicyConfig(cfg PolicyConfig) error {
	return m.policy.SetConfig(cfg)
}

func (m *Manager) Stats() Stats {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := time.Now()
	out := Stats{Ports: make([]PortStats, 0, len(m.order))}
	for _, number := range m.order {
		p := m.ports[number]
		p.mu.Lock()
		ps := PortStats{
			Port:        p.number,
			HostID:      p.hostID,
			ToHost:      p.toHost,
			ToViewer:    p.toViewer,
			Busy:        p.busy,
			Dropped:     p.dropped,
			TCPToHost:   p.tcpToHost,
			TCPToViewer: p.tcpToViewer,
			TCPViewers:  p.tcpViewers,
			TCPBusy:     p.tcpBusy,
			Blocked:     p.blocked,
		}
		if (p.hostAddr != nil && now.Sub(p.hostSeen) <= hostIdle) || p.hostLink != nil {
			ps.HostOnline = true
		}
		if p.viewer != nil {
			ps.Viewer = p.viewer.peer
		} else if p.viewerAddr != nil && now.Sub(p.viewerSeen) <= viewerIdle {
			ps.Viewer = p.viewerAddr.String()
		}
		p.mu.Unlock()
		out.Ports = append(out.Ports, ps)
	}
	if m.shared != nil {
		shared := m.shared.Stats()
		out.SharedCarrier = &shared
	}
	out.Policy = m.policy.Stats()
	out.Sessions = m.policy.Usage()
	return out
}

func (p *port) serve(conn *net.UDPConn) {
	buf := make([]byte, maxDatagram)
	for {
		n, src, err := conn.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			p.manager.log.Warn("relay read", "port", p.number, "error", err)
			continue
		}
		if n == 0 {
			continue
		}
		p.handle(conn, buf[:n], src, time.Now())
	}
}

// detachUDPViewerLocked releases the viewer slot after its session was
// blocklisted, so a different address can attach once the list is cleared.
func (p *port) detachUDPViewerLocked() {
	if p.udpSession != nil {
		p.udpSession.Close()
		p.udpSession = nil
	}
	p.viewerAddr = nil
	p.viewerSeen = time.Time{}
}

func (p *port) handle(conn *net.UDPConn, data []byte, src *net.UDPAddr, now time.Time) {
	p.mu.Lock()
	defer p.mu.Unlock()

	if msg, err := wire.Decode(data); err == nil {
		switch msg.Type {
		case wire.TypeRelayHello:
			p.handleHello(conn, msg, src, now)
			return
		case wire.TypeRelayAuth:
			p.handleAuth(conn, msg, src, now)
			return
		}
	}

	fromHost := p.hostAddr != nil && sameAddr(p.hostAddr, src)
	if fromHost {
		p.hostSeen = now
		if p.viewerAddr == nil || now.Sub(p.viewerSeen) > viewerIdle || p.udpSession == nil {
			p.dropped++
			return
		}
		if allowed, err := p.udpSession.Drop(len(data)); err != nil || !allowed {
			if errors.Is(err, ErrBlocked) {
				p.detachUDPViewerLocked()
			}
			p.dropped++
			return
		}
		if _, err := conn.WriteToUDP(data, p.viewerAddr); err != nil {
			p.dropped++
			return
		}
		p.toViewer += uint64(len(data))
		return
	}

	if p.hostAddr == nil || now.Sub(p.hostSeen) > hostIdle {
		p.dropped++
		return
	}
	if p.viewerAddr == nil || now.Sub(p.viewerSeen) > viewerIdle {
		session, err := p.manager.policy.Open(fmt.Sprintf("udp:%d", p.number), src.IP.String())
		if err != nil {
			if p.udpSession != nil {
				p.udpSession.Close()
				p.udpSession = nil
			}
			p.blocked++
			p.dropped++
			return
		}
		if p.udpSession != nil {
			p.udpSession.Close()
		}
		p.udpSession = session
		p.viewerAddr = cloneAddr(src)
		p.viewerSeen = now
	} else if !sameAddr(p.viewerAddr, src) {
		// A second viewer would have its QUIC packets interleaved with the
		// current connection at the host's single relay socket.
		p.busy++
		conn.WriteToUDP(wire.Encode(wire.Message{
			Type:  wire.TypeBusy,
			Error: "the relay is serving another viewer",
		}), src)
		return
	}
	if p.udpSession == nil {
		p.dropped++
		return
	}
	if allowed, err := p.udpSession.Drop(len(data)); err != nil || !allowed {
		if errors.Is(err, ErrBlocked) {
			p.detachUDPViewerLocked()
		}
		p.dropped++
		return
	}
	if _, err := conn.WriteToUDP(data, p.hostAddr); err != nil {
		p.dropped++
		return
	}
	p.toHost += uint64(len(data))
}

func (p *port) handleHello(conn *net.UDPConn, msg wire.Message, src *net.UDPAddr, now time.Time) {
	if p.hostID == "" || msg.HostID != p.hostID {
		p.reply(conn, src, wire.Message{Type: wire.TypeRelayAck, Error: "no relay reservation for this host"})
		return
	}
	if p.manager.auth == nil || !p.manager.auth.VerifyHost(msg.HostID, p.number) {
		p.reply(conn, src, wire.Message{Type: wire.TypeRelayAck, Error: "host has no live relay reservation"})
		return
	}
	nonce, err := p.manager.auth.IssueChallenge(msg.HostID, identity.DomainRelay)
	if err != nil {
		p.reply(conn, src, wire.Message{Type: wire.TypeRelayAck, Error: "host has no live registration"})
		return
	}
	p.reply(conn, src, wire.Message{
		Type:      wire.TypeRelayChallenge,
		HostID:    msg.HostID,
		Nonce:     nonce,
		Candidate: src.String(),
	})
}

func (p *port) handleAuth(conn *net.UDPConn, msg wire.Message, src *net.UDPAddr, now time.Time) {
	ack := wire.Message{Type: wire.TypeRelayAck}
	if p.hostID == "" || msg.HostID != p.hostID {
		ack.Error = "no relay reservation for this host"
		p.reply(conn, src, ack)
		return
	}
	candidate := src.String()
	if msg.Candidate != "" && msg.Candidate != candidate {
		ack.Error = "relay proof arrived from a different address than the challenge"
		p.reply(conn, src, ack)
		return
	}
	message := identity.RelayMessage(msg.HostID, p.number, candidate, msg.Nonce)
	if p.manager.auth == nil ||
		!p.manager.auth.CheckChallenge(msg.HostID, identity.DomainRelay, msg.Nonce, string(message), msg.Signature) ||
		!p.manager.auth.VerifyHost(msg.HostID, p.number) {
		ack.Error = "relay proof was not accepted"
		p.reply(conn, src, ack)
		return
	}
	if p.hostAddr == nil || !sameAddr(p.hostAddr, src) {
		// The agent's outbound mapping changed. Any QUIC session through the
		// old mapping is gone, so drop the viewer instead of mixing sources.
		p.viewerAddr = nil
		p.viewerSeen = time.Time{}
		if p.udpSession != nil {
			p.udpSession.Close()
			p.udpSession = nil
		}
	}
	p.hostAddr = cloneAddr(src)
	p.hostSeen = now
	ack.OK = true
	p.reply(conn, src, ack)
}

func (p *port) reply(conn *net.UDPConn, to *net.UDPAddr, m wire.Message) {
	conn.WriteToUDP(wire.Encode(m), to)
}

// serveTCP accepts both raw carriers and WebSocket upgrades on the relay port.
func (p *port) serveTCP(tcp net.Listener) {
	for {
		conn, err := tcp.Accept()
		if err != nil {
			if !errors.Is(err, net.ErrClosed) {
				p.manager.log.Warn("relay carrier accept", "port", p.number, "error", err)
			}
			return
		}
		go p.handleTCP(conn)
	}
}

func (p *port) handleTCP(conn net.Conn) {
	_ = conn.SetDeadline(time.Now().Add(carrierHandshake))
	br := bufio.NewReaderSize(conn, 4096)
	prefix, err := br.Peek(4)
	if err != nil {
		conn.Close()
		return
	}
	switch {
	case string(prefix) == "GET ":
		ws, err := carrier.UpgradeServer(conn, br)
		if err != nil {
			p.manager.log.Debug("relay carrier upgrade refused",
				"port", p.number, "remote", conn.RemoteAddr().String(), "error", err)
			conn.Close()
			return
		}
		_ = conn.SetDeadline(time.Time{})
		p.serveViewer(&carrierViewer{
			conn: conn,
			ws:   ws,
			peer: conn.RemoteAddr().String(),
		})
	case strings.HasPrefix(string(prefix), "CHAS"):
		line, err := carrier.ReadLine(br, 64)
		if err != nil {
			conn.Close()
			return
		}
		switch line + "\n" {
		case carrier.MagicRaw:
			_ = conn.SetDeadline(time.Time{})
			p.serveViewer(&carrierViewer{
				conn:   conn,
				send:   carrier.NewFrameWriter(conn),
				frames: carrier.NewFrameReader(br),
				peer:   conn.RemoteAddr().String(),
			})
		case carrier.MagicHost:
			p.serveHost(conn, br)
		default:
			conn.Close()
		}
	default:
		conn.Close()
	}
}

func (p *port) serveHost(conn net.Conn, br *bufio.Reader) {
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		conn.Close()
		return
	}
	var hello carrier.HostHello
	if err := json.Unmarshal([]byte(line), &hello); err != nil {
		p.refuseHost(conn, "malformed host hello")
		return
	}
	p.mu.Lock()
	reserved := p.hostID
	p.mu.Unlock()
	if reserved == "" || hello.HostID != reserved || p.manager.auth == nil ||
		!p.manager.auth.VerifyHost(hello.HostID, p.number) {
		p.refuseHost(conn, "host was not accepted")
		return
	}
	nonce, err := p.manager.auth.IssueChallenge(hello.HostID, identity.DomainCarrierHost)
	if err != nil {
		p.refuseHost(conn, "host has no live registration")
		return
	}
	challenge, err := json.Marshal(carrier.HostChallenge{Nonce: nonce})
	if err != nil {
		p.refuseHost(conn, "could not issue a challenge")
		return
	}
	if _, err := io.WriteString(conn, carrier.MagicChallenge+string(challenge)+"\n"); err != nil {
		conn.Close()
		return
	}
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		conn.Close()
		return
	}
	authText, ok := strings.CutPrefix(line, carrier.MagicAuth)
	if !ok {
		p.refuseHost(conn, "host did not answer the challenge")
		return
	}
	var auth carrier.HostAuth
	if err := json.Unmarshal([]byte(authText), &auth); err != nil {
		p.refuseHost(conn, "malformed host proof")
		return
	}
	message := identity.CarrierHostMessage(hello.HostID, "", auth.Nonce)
	if auth.HostID != hello.HostID || auth.Nonce != nonce ||
		!p.manager.auth.CheckChallenge(hello.HostID, identity.DomainCarrierHost, auth.Nonce, string(message), auth.Signature) {
		p.refuseHost(conn, "host proof was not accepted")
		return
	}
	if _, err := io.WriteString(conn, carrier.MagicOK); err != nil {
		conn.Close()
		return
	}
	_ = conn.SetDeadline(time.Time{})

	link := &hostLink{conn: conn, send: carrier.NewFrameWriter(conn)}
	p.mu.Lock()
	if p.hostID != reserved {
		p.mu.Unlock()
		conn.Close()
		return
	}
	oldLink, oldViewer := p.hostLink, p.viewer
	p.hostLink = link
	p.viewer = nil
	p.hostSeen = time.Now()
	p.mu.Unlock()
	if oldLink != nil {
		oldLink.conn.Close()
	}
	if oldViewer != nil {
		oldViewer.close()
	}
	p.manager.log.Info("relay carrier host link", "port", p.number, "host", hello.HostID)

	frames := carrier.NewFrameReader(br)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(hostIdle))
		payload, err := frames.ReadFrame()
		if err != nil {
			break
		}
		now := time.Now()
		p.mu.Lock()
		if p.hostLink != link {
			p.mu.Unlock()
			break
		}
		p.hostSeen = now
		viewer := p.viewer
		p.mu.Unlock()
		if len(payload) == 0 {
			// The agent's keepalive; answer it so its read side has a
			// deadline to refresh as well.
			if err := link.sendFrame(nil); err != nil {
				break
			}
			continue
		}
		if viewer == nil {
			continue
		}
		if err := viewer.take(len(payload)); err != nil {
			viewer.close()
			p.mu.Lock()
			if p.viewer == viewer {
				p.viewer = nil
			}
			p.mu.Unlock()
			continue
		}
		if err := viewer.writeFrame(payload); err != nil {
			viewer.close()
			p.mu.Lock()
			if p.viewer == viewer {
				p.viewer = nil
			}
			p.mu.Unlock()
			continue
		}
		p.mu.Lock()
		p.tcpToViewer += uint64(len(payload))
		p.mu.Unlock()
	}

	p.mu.Lock()
	var viewer *carrierViewer
	if p.hostLink == link {
		p.hostLink = nil
		viewer = p.viewer
		p.viewer = nil
	}
	p.mu.Unlock()
	if viewer != nil {
		viewer.close()
	}
	conn.Close()
	p.manager.log.Info("relay carrier host link closed", "port", p.number)
}

func (p *port) refuseHost(conn net.Conn, reason string) {
	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, _ = io.WriteString(conn, carrier.MagicErrorPrefix+reason+"\n")
	conn.Close()
}

func (p *port) serveViewer(v *carrierViewer) {
	p.mu.Lock()
	if p.hostLink == nil {
		p.mu.Unlock()
		v.close()
		return
	}
	if p.viewer != nil {
		p.tcpBusy++
		p.mu.Unlock()
		v.close()
		return
	}
	session, err := p.manager.policy.Open(fmt.Sprintf("carrier:%d", p.number), peerIP(v.peer))
	if err != nil {
		p.blocked++
		p.mu.Unlock()
		v.close()
		return
	}
	v.mu.Lock()
	v.session = session
	v.mu.Unlock()
	p.viewer = v
	p.tcpViewers++
	host := p.hostLink
	p.mu.Unlock()
	p.manager.log.Info("relay carrier viewer attached", "port", p.number, "remote", v.peer)

	for {
		_ = v.conn.SetReadDeadline(time.Now().Add(carrierViewerIdle))
		payload, err := v.readFrame()
		if err != nil {
			break
		}
		if len(payload) == 0 {
			// Answer keepalives so the viewer can tell an idle host from a
			// silently broken TCP path.
			if err := v.writeFrame(nil); err != nil {
				break
			}
			continue
		}
		if err := v.take(len(payload)); err != nil {
			break
		}
		if err := host.sendFrame(payload); err != nil {
			host.conn.Close()
			break
		}
		p.mu.Lock()
		p.tcpToHost += uint64(len(payload))
		p.mu.Unlock()
	}

	p.mu.Lock()
	stillPaired := p.viewer == v
	if stillPaired {
		p.viewer = nil
	}
	p.mu.Unlock()
	v.close()
	if stillPaired {
		p.manager.log.Info("relay carrier viewer left", "port", p.number, "remote", v.peer)
	}
}

func sameAddr(a, b *net.UDPAddr) bool {
	return a != nil && b != nil && a.Port == b.Port && a.IP.Equal(b.IP)
}

func cloneAddr(a *net.UDPAddr) *net.UDPAddr {
	if a == nil {
		return nil
	}
	out := *a
	out.IP = append(net.IP(nil), a.IP...)
	return &out
}
