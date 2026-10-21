// Package rendezvous is the UDP meet-up point for hosts and viewers.
//
// Hosts prove liveness with keepalives; the address the server sees is the only
// public candidate it hands out. A viewer's punch_request makes the server tell
// both sides about each other so they can send probes from the same sockets they
// will use for QUIC. The server never forwards video data on this port.
package rendezvous

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	punchTTL        = 30 * time.Second
	defaultRateMax  = 30
	defaultRateSpan = 10 * time.Second
)

type Server struct {
	conn      *net.UDPConn
	reg       *registry.Registry
	relayAddr func(port int) string
	log       *slog.Logger
	limiter   *limiter
	done      chan struct{}
	closeOnce sync.Once
}

// New binds the meeting port. bind is an IP:port; port 0 lets the kernel pick.
func New(bind string, reg *registry.Registry, relayAddr func(port int) string, log *slog.Logger) (*Server, error) {
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
	return &Server{
		conn:      conn,
		reg:       reg,
		relayAddr: relayAddr,
		log:       log,
		limiter:   newLimiter(defaultRateMax, defaultRateSpan),
		done:      make(chan struct{}),
	}, nil
}

func (s *Server) Addr() string { return s.conn.LocalAddr().String() }

func (s *Server) Close() {
	s.closeOnce.Do(func() {
		close(s.done)
		s.conn.Close()
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
	case wire.TypePunchRequest:
		s.handlePunchRequest(msg, src)
	}
}

func (s *Server) handleKeepalive(msg wire.Message, src *net.UDPAddr) {
	ack := wire.Message{Type: wire.TypeKeepaliveAck}
	if err := s.reg.Touch(msg.HostID, msg.KeyHash, src.String()); err != nil {
		ack.Error = err.Error()
		s.reply(src, ack)
		return
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok {
		ack.Error = "host is no longer registered"
		s.reply(src, ack)
		return
	}
	ack.OK = true
	ack.Candidate = src.String()
	if host.RelayPort != 0 {
		ack.Relay = s.relayAddr(host.RelayPort)
	}
	s.reply(src, ack)
}

func (s *Server) handlePunchRequest(msg wire.Message, src *net.UDPAddr) {
	reply := wire.Message{Type: wire.TypePunchPeer, Nonce: msg.Nonce}
	if !s.limiter.allow(src.IP.String()) {
		reply.Error = "too many punch requests; retry later"
		s.reply(src, reply)
		return
	}
	if len(msg.Nonce) < 8 || len(msg.Nonce) > 64 {
		reply.Error = "nonce must be 8..64 characters"
		s.reply(src, reply)
		return
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok {
		reply.Error = "host is not registered"
		s.reply(src, reply)
		return
	}
	if !s.reg.Online(host) {
		reply.Error = "host is offline"
		s.reply(src, reply)
		return
	}
	hostAddr, err := net.ResolveUDPAddr("udp", host.Candidate)
	if err != nil {
		reply.Error = "host has no usable address"
		s.reply(src, reply)
		return
	}
	// Tell the host to punch towards the viewer. Best effort: the viewer is
	// also told the host candidate and will probe first.
	s.reply(hostAddr, wire.Message{
		Type:      wire.TypePunchPeer,
		Nonce:     msg.Nonce,
		Candidate: src.String(),
		ExpiresMS: punchTTL.Milliseconds(),
	})
	reply.OK = true
	reply.Candidate = host.Candidate
	if host.RelayPort != 0 {
		reply.Relay = s.relayAddr(host.RelayPort)
	}
	reply.ExpiresMS = punchTTL.Milliseconds()
	s.reply(src, reply)
}

func (s *Server) reply(to *net.UDPAddr, m wire.Message) {
	if _, err := s.conn.WriteToUDP(wire.Encode(m), to); err != nil {
		s.log.Debug("rendezvous reply", "to", to, "error", err)
	}
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
