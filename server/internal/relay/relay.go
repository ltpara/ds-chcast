// Package relay forwards opaque UDP datagrams between a viewer and one host.
//
// Each host is assigned its own relay port while it is registered. The host
// agent keeps that port bound to its current address with relay_hello; a viewer
// simply sends QUIC to the port. Datagrams are copied verbatim, so the QUIC
// connection stays end-to-end between the viewer and chast-host and the relay
// never needs to understand chast.
package relay

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// Authenticator is implemented by the registry.
type Authenticator interface {
	VerifyHost(id, keyHash string, port int) bool
}

const (
	hostIdle    = 90 * time.Second
	viewerIdle  = 60 * time.Second
	maxDatagram = 65535
)

var ErrNoFreePort = errors.New("no free relay port")

type Manager struct {
	mu    sync.Mutex
	ports map[int]*port
	order []int
	auth  Authenticator
	log   *slog.Logger
}

type port struct {
	manager *Manager
	conn    *net.UDPConn
	number  int

	mu         sync.Mutex
	hostID     string
	hostAddr   *net.UDPAddr
	hostSeen   time.Time
	viewerAddr *net.UDPAddr
	viewerSeen time.Time
	toHost     uint64
	toViewer   uint64
	busy       uint64
	dropped    uint64
}

// NewManager binds every requested UDP port and serves it for the lifetime of
// the manager. A port of 0 asks the kernel for an ephemeral port and is useful
// in tests; callers use Ports to learn the actual numbers.
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
	m := &Manager{
		ports: make(map[int]*port),
		auth:  auth,
		log:   log,
	}
	for _, wanted := range ports {
		conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: ip, Port: wanted})
		if err != nil {
			m.Close()
			return nil, fmt.Errorf("relay port %d: %w", wanted, err)
		}
		actual := conn.LocalAddr().(*net.UDPAddr).Port
		p := &port{manager: m, conn: conn, number: actual}
		m.ports[actual] = p
		m.order = append(m.order, actual)
		go p.serve()
	}
	if len(m.ports) == 0 {
		return nil, errors.New("relay needs at least one port")
	}
	return m, nil
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
		}
		p.mu.Unlock()
		if free {
			return number, nil
		}
	}
	return 0, ErrNoFreePort
}

// Release drops the host binding and any viewer association. The listener stays
// up so the port can be handed to the next host.
func (m *Manager) Release(hostID string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, number := range m.order {
		p := m.ports[number]
		p.mu.Lock()
		if p.hostID == hostID {
			p.hostID = ""
			p.hostAddr = nil
			p.viewerAddr = nil
			p.hostSeen = time.Time{}
			p.viewerSeen = time.Time{}
		}
		p.mu.Unlock()
	}
}

func (m *Manager) Close() {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, p := range m.ports {
		p.conn.Close()
	}
}

type PortStats struct {
	Port       int    `json:"port"`
	HostID     string `json:"host_id,omitempty"`
	HostOnline bool   `json:"host_online"`
	Viewer     string `json:"viewer,omitempty"`
	ToHost     uint64 `json:"to_host_bytes"`
	ToViewer   uint64 `json:"to_viewer_bytes"`
	Busy       uint64 `json:"busy"`
	Dropped    uint64 `json:"dropped"`
}

type Stats struct {
	Ports []PortStats `json:"ports"`
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
			Port:     p.number,
			HostID:   p.hostID,
			ToHost:   p.toHost,
			ToViewer: p.toViewer,
			Busy:     p.busy,
			Dropped:  p.dropped,
		}
		if p.hostAddr != nil && now.Sub(p.hostSeen) <= hostIdle {
			ps.HostOnline = true
		}
		if p.viewerAddr != nil && now.Sub(p.viewerSeen) <= viewerIdle {
			ps.Viewer = p.viewerAddr.String()
		}
		p.mu.Unlock()
		out.Ports = append(out.Ports, ps)
	}
	return out
}

func (p *port) serve() {
	buf := make([]byte, maxDatagram)
	for {
		n, src, err := p.conn.ReadFromUDP(buf)
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
		p.handle(buf[:n], src, time.Now())
	}
}

func (p *port) handle(data []byte, src *net.UDPAddr, now time.Time) {
	p.mu.Lock()
	defer p.mu.Unlock()

	if msg, err := wire.Decode(data); err == nil && msg.Type == wire.TypeRelayHello {
		p.handleHello(msg, src, now)
		return
	}

	fromHost := p.hostAddr != nil && sameAddr(p.hostAddr, src)
	if fromHost {
		p.hostSeen = now
		if p.viewerAddr == nil || now.Sub(p.viewerSeen) > viewerIdle {
			p.dropped++
			return
		}
		if _, err := p.conn.WriteToUDP(data, p.viewerAddr); err != nil {
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
	if p.viewerAddr == nil || now.Sub(p.viewerSeen) > viewerIdle || sameAddr(p.viewerAddr, src) {
		p.viewerAddr = cloneAddr(src)
		p.viewerSeen = now
	} else {
		// A second viewer would have its QUIC packets interleaved with the
		// current connection at the host's single relay socket.
		p.busy++
		p.conn.WriteToUDP(wire.Encode(wire.Message{
			Type:  wire.TypeBusy,
			Error: "the relay is serving another viewer",
		}), src)
		return
	}
	if _, err := p.conn.WriteToUDP(data, p.hostAddr); err != nil {
		p.dropped++
		return
	}
	p.toHost += uint64(len(data))
}

func (p *port) handleHello(msg wire.Message, src *net.UDPAddr, now time.Time) {
	if p.hostID == "" || msg.HostID != p.hostID {
		p.reply(src, wire.Message{Type: wire.TypeRelayAck, Error: "no relay reservation for this host"})
		return
	}
	if p.manager.auth == nil || !p.manager.auth.VerifyHost(msg.HostID, msg.KeyHash, p.number) {
		p.reply(src, wire.Message{Type: wire.TypeRelayAck, Error: "host key was not accepted"})
		return
	}
	if p.hostAddr == nil || !sameAddr(p.hostAddr, src) {
		// The agent's outbound mapping changed. Any QUIC session through the
		// old mapping is gone, so drop the viewer instead of mixing sources.
		p.viewerAddr = nil
		p.viewerSeen = time.Time{}
	}
	p.hostAddr = cloneAddr(src)
	p.hostSeen = now
	p.reply(src, wire.Message{Type: wire.TypeRelayAck, OK: true})
}

func (p *port) reply(to *net.UDPAddr, m wire.Message) {
	p.conn.WriteToUDP(wire.Encode(m), to)
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
