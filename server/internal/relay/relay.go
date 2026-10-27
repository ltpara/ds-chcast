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
	"github.com/ltpara/ds-chcast/server/internal/netutil"
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
	ports  map[int]*portSet
	order  []int
	auth   Authenticator
	log    *slog.Logger
	shared *SharedCarrier
	policy *Policy
	bindIP net.IP
	// bind6IP is an optional second listener address. Relays bound this way
	// serve IPv4 and IPv6 on the same port number, each with its own host and
	// viewer association, because a dual-stack peer uses a different source
	// mapping per family.
	bind6IP  net.IP
	bind6Err error
	closed   bool

	// requireViewerToken admits only viewers that first prove the lookup
	// carrier token over the relay port. It is enabled with the server key so
	// unauthenticated deployments keep the original first-source behavior.
	requireViewerToken bool

	wantedReadBuffer int
	readBuffer       int
}

// Option customizes the relay manager before its ports are bound.
type Option func(*Manager)

// WithReadBuffer sets the UDP receive buffer (SO_RCVBUF) requested for every
// relay port, including ports rebound for a lease address. Zero keeps the
// system default.
func WithReadBuffer(size int) Option {
	return func(m *Manager) { m.wantedReadBuffer = size }
}

// WithViewerTokenRequired makes every relay viewer prove the per-lease carrier
// token before its datagrams are forwarded, matching hbbr's key check.
func WithViewerTokenRequired(required bool) Option {
	return func(m *Manager) { m.requireViewerToken = required }
}

// WithBind6 listens on an additional IPv6 address with the same port numbers.
// It is how a single relay serves dual-stack clients without relying on
// v4-mapped sockets, which cannot keep separate host mappings per family.
func WithBind6(bind string) Option {
	return func(m *Manager) {
		bind = strings.TrimSpace(bind)
		if bind == "" {
			return
		}
		ip := net.ParseIP(bind)
		if ip == nil || ip.To4() != nil {
			m.bind6Err = fmt.Errorf("relay IPv6 bind %q is not an IPv6 address", bind)
			return
		}
		m.bind6IP = ip
	}
}

// portSet groups the listeners and per-family state that share one host
// reservation and port number.
type portSet struct {
	manager *Manager
	number  int

	mu        sync.Mutex
	hostID    string
	boundHost string
	members   []*port
}

type port struct {
	set    *portSet
	number int
	family string

	mu         sync.Mutex
	conn       *net.UDPConn
	tcp        net.Listener
	boundIP    net.IP
	hostAddr   *net.UDPAddr
	hostSeen   time.Time
	viewerAddr *net.UDPAddr
	viewerSeen time.Time
	toHost     uint64
	toViewer   uint64
	busy       uint64
	dropped    uint64

	// viewerAuthAddr is the source that proved the carrier token. It is not
	// tied to viewerAddr so a viewer reconnect from a new port can replace a
	// stale association after authenticating again.
	viewerAuthAddr *net.UDPAddr

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
//
// WithBind6 adds a second, IPv6-only listener per port. The two listeners
// share the host reservation but keep separate host and viewer associations,
// since a dual-stack peer reaches them through different NAT mappings.
func NewManager(bind string, ports []int, auth Authenticator, log *slog.Logger, opts ...Option) (*Manager, error) {
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
		ports:  make(map[int]*portSet),
		auth:   auth,
		log:    log,
		policy: policy,
		bindIP: ip,
	}
	for _, opt := range opts {
		opt(m)
	}
	if m.bind6Err != nil {
		return nil, m.bind6Err
	}
	if m.bind6IP != nil && m.bindIP.To4() == nil {
		return nil, errors.New("relay IPv6 bind is redundant when the primary bind is already IPv6")
	}
	for _, wanted := range ports {
		set, err := m.newPortSet(wanted)
		if err != nil {
			m.Close()
			return nil, err
		}
		m.ports[set.number] = set
		m.order = append(m.order, set.number)
	}
	if len(m.ports) == 0 {
		return nil, errors.New("relay needs at least one port")
	}
	return m, nil
}

// newPortSet binds one port on every configured address family. Port 0 is
// retried because the ephemeral UDP port chosen for one family must also be
// free for the other family's TCP carrier twin.
func (m *Manager) newPortSet(wanted int) (*portSet, error) {
	attempts := 1
	if wanted == 0 {
		attempts = 10
	}
	var lastErr error
	for attempt := 0; attempt < attempts; attempt++ {
		udp, tcp, number, err := m.listenPort(m.bindIP, wanted)
		if err != nil {
			return nil, err
		}
		set := &portSet{manager: m, number: number}
		set.members = append(set.members, &port{
			set: set, number: number, family: ipFamily(m.bindIP),
			conn: udp, tcp: tcp, boundIP: m.bindIP,
		})
		if m.bind6IP != nil {
			udp6, tcp6, _, err := m.listenPort(m.bind6IP, number)
			if err != nil {
				closePortMember(set.members[0])
				lastErr = fmt.Errorf("relay IPv6 port %d: %w", number, err)
				if wanted == 0 {
					continue
				}
				return nil, lastErr
			}
			set.members = append(set.members, &port{
				set: set, number: number, family: ipFamily(m.bind6IP),
				conn: udp6, tcp: tcp6, boundIP: m.bind6IP,
			})
		}
		for _, p := range set.members {
			go p.serve(p.conn)
			if p.tcp != nil {
				go p.serveTCP(p.tcp)
			}
		}
		return set, nil
	}
	if lastErr != nil {
		return nil, lastErr
	}
	return nil, errors.New("relay port 0: no UDP port with a free TCP twin")
}

// listenPort binds one UDP port and, when possible, its TCP carrier twin.
// Port 0 asks for an ephemeral port; a fixed port keeps serving UDP when only
// the TCP twin is taken.
func (m *Manager) listenPort(ip net.IP, wanted int) (*net.UDPConn, net.Listener, int, error) {
	udpNet, tcpNet := familyNetworks(ip)
	udp, err := net.ListenUDP(udpNet, &net.UDPAddr{IP: ip, Port: wanted})
	if err != nil {
		return nil, nil, 0, fmt.Errorf("relay port %d: %w", wanted, err)
	}
	actual := udp.LocalAddr().(*net.UDPAddr).Port
	m.tuneReadBuffer(udp, actual)
	tcp, err := net.ListenTCP(tcpNet, &net.TCPAddr{IP: ip, Port: actual})
	if err == nil {
		return udp, tcp, actual, nil
	}
	udp.Close()
	if wanted == 0 {
		return nil, nil, 0, fmt.Errorf("relay port 0: cannot bind the TCP twin: %w", err)
	}
	m.log.Warn("relay carrier disabled: cannot bind the TCP twin",
		"port", wanted, "network", tcpNet, "error", err)
	udp, err = net.ListenUDP(udpNet, &net.UDPAddr{IP: ip, Port: wanted})
	if err != nil {
		return nil, nil, 0, fmt.Errorf("relay port %d: %w", wanted, err)
	}
	m.tuneReadBuffer(udp, wanted)
	return udp, nil, wanted, nil
}

// listenOn binds one UDP port and, when possible, its TCP carrier twin on the
// same address. A missing TCP twin is reported to the caller as nil.
func (m *Manager) listenOn(ip net.IP, number int) (*net.UDPConn, net.Listener, error) {
	udpNet, tcpNet := familyNetworks(ip)
	udp, err := net.ListenUDP(udpNet, &net.UDPAddr{IP: ip, Port: number})
	if err != nil {
		return nil, nil, err
	}
	m.tuneReadBuffer(udp, number)
	tcp, err := net.ListenTCP(tcpNet, &net.TCPAddr{IP: ip, Port: number})
	if err != nil {
		m.log.Warn("relay carrier disabled: cannot bind the TCP twin",
			"port", number, "address", ip.String(), "error", err)
		tcp = nil
	}
	return udp, tcp, nil
}

func familyNetworks(ip net.IP) (udpNet, tcpNet string) {
	if ip != nil && ip.To4() == nil {
		return "udp6", "tcp6"
	}
	return "udp4", "tcp4"
}

func ipFamily(ip net.IP) string {
	if ip != nil && ip.To4() == nil {
		return "v6"
	}
	return "v4"
}

func closePortMember(p *port) {
	if p.conn != nil {
		p.conn.Close()
	}
	if p.tcp != nil {
		p.tcp.Close()
	}
}

// BindHost rebinds each family listener to the local address behind host, so a
// viewer that dialed that address sees relay replies from the same source
// address. host may be a name; names that do not resolve, or addresses that
// are not local, keep the listener on its configured --relay-listen address.
//
// The host agent authenticates on every advertised address, and the per-port
// state is shared per family, so the switch does not need a new handshake.
func (m *Manager) BindHost(number int, host string) error {
	m.mu.Lock()
	set := m.ports[number]
	m.mu.Unlock()
	if set == nil {
		return fmt.Errorf("relay port %d is not managed here", number)
	}
	host = strings.TrimSpace(strings.Trim(host, "[]"))
	if host == "" {
		return nil
	}
	set.mu.Lock()
	if set.boundHost == host {
		ready := true
		for _, p := range set.members {
			p.mu.Lock()
			if p.conn == nil {
				ready = false
			}
			p.mu.Unlock()
		}
		if ready {
			set.mu.Unlock()
			return nil
		}
	}
	// Keep the requested name so renewals of the same lease are a no-op even
	// after falling back to the configured bind.
	set.boundHost = host
	members := append([]*port(nil), set.members...)
	set.mu.Unlock()

	var firstErr error
	for _, p := range members {
		if err := p.rebind(host); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

// rebind switches one family listener to the address of that family behind
// host. An address of the other family leaves the listener untouched.
func (p *port) rebind(host string) error {
	m := p.set.manager
	fallback := m.bindFor(p.family)
	target := resolveRelayIPFamily(host, p.family)
	if target == nil {
		m.log.Warn("relay address does not resolve for this family; using the configured bind",
			"port", p.number, "family", p.family, "address", host)
		target = fallback
	}

	p.mu.Lock()
	if p.boundIP != nil && p.boundIP.Equal(target) && p.conn != nil {
		p.mu.Unlock()
		return nil
	}
	oldConn, oldTCP := p.conn, p.tcp
	p.conn, p.tcp = nil, nil
	p.mu.Unlock()

	if oldConn != nil {
		oldConn.Close()
	}
	if oldTCP != nil {
		oldTCP.Close()
	}

	udp, tcp, err := m.listenOn(target, p.number)
	if err != nil && !target.Equal(fallback) {
		m.log.Warn("relay address is not local; using the configured bind",
			"port", p.number, "family", p.family, "address", host, "error", err)
		target = fallback
		udp, tcp, err = m.listenOn(target, p.number)
	}
	if err != nil {
		m.log.Error("relay port lost its listener", "port", p.number, "family", p.family, "error", err)
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

// bindFor returns the configured listen address for one address family.
func (m *Manager) bindFor(family string) net.IP {
	if family == "v6" && m.bind6IP != nil {
		return m.bind6IP
	}
	return m.bindIP
}

// tuneReadBuffer applies the configured receive buffer to one relay socket.
// Failure is advisory, matching hbbr: the port keeps serving with the system
// default instead of preventing startup.
func (m *Manager) tuneReadBuffer(conn *net.UDPConn, port int) {
	if m.wantedReadBuffer <= 0 {
		return
	}
	size, err := netutil.SetUDPReadBuffer(conn, m.wantedReadBuffer)
	if err != nil {
		m.log.Warn("relay UDP receive buffer was not applied",
			"port", port, "requested", m.wantedReadBuffer, "error", err)
		return
	}
	if size <= 0 {
		return
	}
	m.mu.Lock()
	if m.readBuffer == 0 || size < m.readBuffer {
		m.readBuffer = size
	}
	m.mu.Unlock()
}

// ReadBuffer is the smallest SO_RCVBUF value reported by the kernel across the
// relay sockets, or zero when the default was kept or could not be read back.
func (m *Manager) ReadBuffer() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.readBuffer
}

func (m *Manager) isClosed() bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.closed
}

// resolveRelayIPFamily resolves a relay list entry for binding, restricted to
// one address family. Names can point at several addresses; the first address
// of the requested family is used.
func resolveRelayIPFamily(host, family string) net.IP {
	wantV4 := family != "v6"
	if ip := net.ParseIP(host); ip != nil {
		if (ip.To4() != nil) == wantV4 {
			return ip
		}
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancel()
	addrs, err := net.DefaultResolver.LookupIPAddr(ctx, host)
	if err != nil {
		return nil
	}
	for _, addr := range addrs {
		if (addr.IP.To4() != nil) == wantV4 {
			return addr.IP
		}
	}
	return nil
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
		set := m.ports[number]
		set.mu.Lock()
		held := set.hostID
		set.mu.Unlock()
		if held == hostID {
			return number, nil
		}
	}
	for _, number := range m.order {
		set := m.ports[number]
		set.mu.Lock()
		free := set.hostID == ""
		if free {
			set.hostID = hostID
			for _, p := range set.members {
				p.mu.Lock()
				p.hostAddr = nil
				p.viewerAddr = nil
				p.viewerAuthAddr = nil
				p.hostSeen = time.Time{}
				p.viewerSeen = time.Time{}
				if p.udpSession != nil {
					p.udpSession.Close()
					p.udpSession = nil
				}
				p.mu.Unlock()
			}
		}
		set.mu.Unlock()
		if free {
			return number, nil
		}
	}
	return 0, ErrNoFreePort
}

// Release drops the host binding, any viewer association and the carrier link
// on every family listener. The listeners stay up so the port can be handed to
// the next host.
func (m *Manager) Release(hostID string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, number := range m.order {
		set := m.ports[number]
		set.mu.Lock()
		if set.hostID != hostID {
			set.mu.Unlock()
			continue
		}
		set.hostID = ""
		var links []*hostLink
		var viewers []*carrierViewer
		for _, p := range set.members {
			p.mu.Lock()
			p.hostAddr = nil
			p.viewerAddr = nil
			p.viewerAuthAddr = nil
			p.hostSeen = time.Time{}
			p.viewerSeen = time.Time{}
			if p.udpSession != nil {
				p.udpSession.Close()
				p.udpSession = nil
			}
			if p.hostLink != nil {
				links = append(links, p.hostLink)
			}
			if p.viewer != nil {
				viewers = append(viewers, p.viewer)
			}
			p.hostLink, p.viewer = nil, nil
			p.mu.Unlock()
		}
		set.mu.Unlock()
		for _, link := range links {
			link.conn.Close()
		}
		for _, viewer := range viewers {
			viewer.close()
		}
	}
}

func (m *Manager) Close() {
	m.mu.Lock()
	m.closed = true
	for _, set := range m.ports {
		for _, p := range set.members {
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
	// ReadBuffer is the effective SO_RCVBUF on the relay sockets; zero means
	// the system default.
	ReadBuffer int `json:"udp_read_buffer_bytes"`
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
	out := Stats{Ports: make([]PortStats, 0, len(m.order)), ReadBuffer: m.readBuffer}
	for _, number := range m.order {
		set := m.ports[number]
		set.mu.Lock()
		ps := PortStats{Port: number, HostID: set.hostID}
		for _, p := range set.members {
			p.mu.Lock()
			ps.ToHost += p.toHost
			ps.ToViewer += p.toViewer
			ps.Busy += p.busy
			ps.Dropped += p.dropped
			ps.TCPToHost += p.tcpToHost
			ps.TCPToViewer += p.tcpToViewer
			ps.TCPViewers += p.tcpViewers
			ps.TCPBusy += p.tcpBusy
			ps.Blocked += p.blocked
			if (p.hostAddr != nil && now.Sub(p.hostSeen) <= hostIdle) || p.hostLink != nil {
				ps.HostOnline = true
			}
			if ps.Viewer == "" {
				if p.viewer != nil {
					ps.Viewer = p.viewer.peer
				} else if p.viewerAddr != nil && now.Sub(p.viewerSeen) <= viewerIdle {
					ps.Viewer = p.viewerAddr.String()
				}
			}
			p.mu.Unlock()
		}
		set.mu.Unlock()
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
			p.set.manager.log.Warn("relay read", "port", p.number, "family", p.family, "error", err)
			continue
		}
		if n == 0 {
			continue
		}
		p.handle(conn, buf[:n], src, time.Now())
	}
}

// reservedHost reads the current lease without taking the per-listener lock.
// Control handling calls it before locking, keeping the set -> port lock order
// used by Allocate and Release.
func (p *port) reservedHost() string {
	p.set.mu.Lock()
	defer p.set.mu.Unlock()
	return p.set.hostID
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
	var control *wire.Message
	if msg, err := wire.Decode(data); err == nil {
		switch msg.Type {
		case wire.TypeRelayHello, wire.TypeRelayAuth, wire.TypeRelayViewer:
			control = &msg
		}
	}
	// Read the reservation before taking the listener lock so this path keeps
	// the set -> port lock order used by allocation.
	hostID := ""
	if control != nil {
		hostID = p.reservedHost()
	}

	p.mu.Lock()
	defer p.mu.Unlock()

	if control != nil {
		switch control.Type {
		case wire.TypeRelayHello:
			p.handleHello(conn, *control, src, now, hostID)
			return
		case wire.TypeRelayAuth:
			p.handleAuth(conn, *control, src, now, hostID)
			return
		case wire.TypeRelayViewer:
			p.handleViewerAuth(conn, *control, src, hostID)
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
	if p.set.manager.requireViewerToken &&
		(p.viewerAuthAddr == nil || !sameAddr(p.viewerAuthAddr, src)) {
		p.dropped++
		return
	}
	if p.viewerAddr == nil || now.Sub(p.viewerSeen) > viewerIdle {
		session, err := p.set.manager.policy.Open(fmt.Sprintf("udp:%d/%s", p.number, p.family), src.IP.String())
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

func (p *port) handleHello(conn *net.UDPConn, msg wire.Message, src *net.UDPAddr, now time.Time, hostID string) {
	if hostID == "" || msg.HostID != hostID {
		p.reply(conn, src, wire.Message{Type: wire.TypeRelayAck, Error: "no relay reservation for this host"})
		return
	}
	if p.set.manager.auth == nil || !p.set.manager.auth.VerifyHost(msg.HostID, p.number) {
		p.reply(conn, src, wire.Message{Type: wire.TypeRelayAck, Error: "host has no live relay reservation"})
		return
	}
	nonce, err := p.set.manager.auth.IssueChallenge(msg.HostID, identity.DomainRelay)
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

func (p *port) handleAuth(conn *net.UDPConn, msg wire.Message, src *net.UDPAddr, now time.Time, hostID string) {
	ack := wire.Message{Type: wire.TypeRelayAck}
	if hostID == "" || msg.HostID != hostID {
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
	if p.set.manager.auth == nil ||
		!p.set.manager.auth.CheckChallenge(msg.HostID, identity.DomainRelay, msg.Nonce, string(message), msg.Signature) ||
		!p.set.manager.auth.VerifyHost(msg.HostID, p.number) {
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

// handleViewerAuth is the UDP counterpart of the carrier viewer hello. A valid
// token reserves the source address, and a new valid source supersedes a stale
// viewer association so a reconnect does not have to wait for viewerIdle.
func (p *port) handleViewerAuth(conn *net.UDPConn, msg wire.Message, src *net.UDPAddr, hostID string) {
	ack := wire.Message{Type: wire.TypeRelayViewerAck}
	switch {
	case hostID == "" || msg.HostID != hostID:
		ack.Error = "no relay reservation for this host"
	case p.set.manager.auth == nil || !p.set.manager.auth.VerifyCarrier(msg.HostID, msg.Token):
		ack.Error = "viewer token was not accepted"
	default:
		if p.viewerAuthAddr == nil || !sameAddr(p.viewerAuthAddr, src) {
			if p.viewerAddr != nil && !sameAddr(p.viewerAddr, src) {
				p.detachUDPViewerLocked()
			}
			p.viewerAuthAddr = cloneAddr(src)
		}
		p.set.manager.log.Info("relay viewer authenticated", "port", p.number, "remote", src.String())
		ack.OK = true
	}
	p.reply(conn, src, ack)
}

// serveTCP accepts both raw carriers and WebSocket upgrades on the relay port.
func (p *port) serveTCP(tcp net.Listener) {
	for {
		conn, err := tcp.Accept()
		if err != nil {
			if !errors.Is(err, net.ErrClosed) {
				p.set.manager.log.Warn("relay carrier accept", "port", p.number, "error", err)
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
			p.set.manager.log.Debug("relay carrier upgrade refused",
				"port", p.number, "remote", conn.RemoteAddr().String(), "error", err)
			conn.Close()
			return
		}
		if p.set.manager.requireViewerToken {
			body, err := ws.ReadMessage()
			if err != nil || !p.viewerHelloOK(body) {
				conn.Close()
				return
			}
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
			if p.set.manager.requireViewerToken {
				hello, err := carrier.ReadLine(br, carrier.MaxHandshake)
				if err != nil || !p.viewerHelloTextOK(hello) {
					p.refuseConn(conn, "viewer token was not accepted")
					return
				}
			}
			_ = conn.SetDeadline(time.Time{})
			p.serveViewer(&carrierViewer{
				conn:   conn,
				send:   carrier.NewFrameWriter(conn),
				frames: carrier.NewFrameReader(br),
				peer:   conn.RemoteAddr().String(),
			})
		case carrier.MagicViewer:
			hello, err := carrier.ReadLine(br, carrier.MaxHandshake)
			if err != nil || !p.viewerHelloTextOK(hello) {
				p.refuseConn(conn, "viewer token was not accepted")
				return
			}
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
		p.refuseConn(conn, "malformed host hello")
		return
	}
	reserved := p.reservedHost()
	if reserved == "" || hello.HostID != reserved || p.set.manager.auth == nil ||
		!p.set.manager.auth.VerifyHost(hello.HostID, p.number) {
		p.refuseConn(conn, "host was not accepted")
		return
	}
	nonce, err := p.set.manager.auth.IssueChallenge(hello.HostID, identity.DomainCarrierHost)
	if err != nil {
		p.refuseConn(conn, "host has no live registration")
		return
	}
	challenge, err := json.Marshal(carrier.HostChallenge{Nonce: nonce})
	if err != nil {
		p.refuseConn(conn, "could not issue a challenge")
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
		p.refuseConn(conn, "host did not answer the challenge")
		return
	}
	var auth carrier.HostAuth
	if err := json.Unmarshal([]byte(authText), &auth); err != nil {
		p.refuseConn(conn, "malformed host proof")
		return
	}
	message := identity.CarrierHostMessage(hello.HostID, "", auth.Nonce)
	if auth.HostID != hello.HostID || auth.Nonce != nonce ||
		!p.set.manager.auth.CheckChallenge(hello.HostID, identity.DomainCarrierHost, auth.Nonce, string(message), auth.Signature) {
		p.refuseConn(conn, "host proof was not accepted")
		return
	}
	if _, err := io.WriteString(conn, carrier.MagicOK); err != nil {
		conn.Close()
		return
	}
	_ = conn.SetDeadline(time.Time{})

	link := &hostLink{conn: conn, send: carrier.NewFrameWriter(conn)}
	p.set.mu.Lock()
	if p.set.hostID != reserved {
		p.set.mu.Unlock()
		conn.Close()
		return
	}
	p.mu.Lock()
	p.set.mu.Unlock()
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
	p.set.manager.log.Info("relay carrier host link", "port", p.number, "host", hello.HostID)

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
	p.set.manager.log.Info("relay carrier host link closed", "port", p.number)
}

func (p *port) refuseConn(conn net.Conn, reason string) {
	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, _ = io.WriteString(conn, carrier.MagicErrorPrefix+reason+"\n")
	conn.Close()
}

func (p *port) viewerHelloTextOK(line string) bool {
	var hello carrier.SharedViewerHello
	if json.Unmarshal([]byte(line), &hello) != nil {
		return false
	}
	return p.viewerTokenOK(hello.HostID, hello.Token)
}

func (p *port) viewerHelloOK(body []byte) bool {
	var hello carrier.SharedViewerHello
	if json.Unmarshal(body, &hello) != nil {
		return false
	}
	return p.viewerTokenOK(hello.HostID, hello.Token)
}

func (p *port) viewerTokenOK(id, token string) bool {
	reserved := p.reservedHost()
	return reserved != "" && id == reserved && p.set.manager.auth != nil &&
		p.set.manager.auth.VerifyCarrier(id, token)
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
	session, err := p.set.manager.policy.Open(fmt.Sprintf("carrier:%d/%s", p.number, p.family), peerIP(v.peer))
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
	p.set.manager.log.Info("relay carrier viewer attached", "port", p.number, "remote", v.peer)

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
		p.set.manager.log.Info("relay carrier viewer left", "port", p.number, "remote", v.peer)
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
