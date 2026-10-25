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
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	punchTTL        = 30 * time.Second
	punchAuthWindow = time.Minute
	DefaultRateMax  = 30
	defaultRateSpan = 10 * time.Second
)

type options struct {
	readBuffer int
	rateMax    int
	serverKey  string
}

// Option customizes the rendezvous socket without changing the call sites that
// only need the defaults.
type Option func(*options)

// WithReadBuffer sets the UDP receive buffer (SO_RCVBUF) requested at bind
// time. Zero keeps the system default.
func WithReadBuffer(size int) Option {
	return func(o *options) { o.readBuffer = size }
}

// WithPunchRateMax caps punch requests per client IP per ten seconds. A
// non-positive value disables the limiter.
func WithPunchRateMax(max int) Option {
	return func(o *options) { o.rateMax = max }
}

// WithServerKey requires viewers to prove knowledge of the server key before a
// punch is coordinated. An empty key keeps public rendezvous behavior.
func WithServerKey(key string) Option {
	return func(o *options) { o.serverKey = key }
}

type Server struct {
	conn       *net.UDPConn
	reg        *registry.Registry
	relayAddr  func(port int) string
	identity   *identity.Server
	punchLog   *punchlog.Log
	ipChanges  *ipchange.Tracker
	log        *slog.Logger
	limiter    *limiter
	serverKey  string
	readBuffer int
	done       chan struct{}
	closeOnce  sync.Once
}

// New binds the meeting port. bind is an IP:port; port 0 lets the kernel pick.
func New(bind string, reg *registry.Registry, relayAddr func(port int) string, log *slog.Logger, opts ...Option) (*Server, error) {
	cfg := options{rateMax: DefaultRateMax}
	for _, opt := range opts {
		opt(&cfg)
	}
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
	readBuffer, err := netutil.SetUDPReadBuffer(conn, cfg.readBuffer)
	if err != nil {
		log.Warn("punch UDP receive buffer was not applied",
			"requested", cfg.readBuffer, "error", err)
	}
	return &Server{
		conn:       conn,
		reg:        reg,
		relayAddr:  relayAddr,
		log:        log,
		limiter:    newLimiter(cfg.rateMax, defaultRateSpan),
		serverKey:  cfg.serverKey,
		readBuffer: readBuffer,
		done:       make(chan struct{}),
	}, nil
}

// ReadBuffer is the SO_RCVBUF value reported by the kernel, or zero when the
// default was kept or the option could not be read back.
func (s *Server) ReadBuffer() int { return s.readBuffer }

func (s *Server) Addr() string { return s.conn.LocalAddr().String() }

// SetIdentity enables server-signed host bindings in punch replies.
func (s *Server) SetIdentity(server *identity.Server) { s.identity = server }

// SetPunchLog records successful punch coordination for the admin console.
func (s *Server) SetPunchLog(log *punchlog.Log) { s.punchLog = log }

// SetIPChanges audits public address changes seen in authenticated keepalives.
func (s *Server) SetIPChanges(tracker *ipchange.Tracker) { s.ipChanges = tracker }

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
	case wire.TypeKeepaliveAuth:
		s.handleKeepaliveAuth(msg, src)
	case wire.TypePunchRequest:
		s.handlePunchRequest(msg, src)
	}
}

// handleKeepalive only starts the exchange. The host must prove its key over a
// one-time nonce before the observed address is allowed to move, so a spoofed
// source cannot redirect a host's mapping.
func (s *Server) handleKeepalive(msg wire.Message, src *net.UDPAddr) {
	nonce, err := s.reg.IssueChallenge(msg.HostID, identity.DomainKeepalive)
	if err != nil {
		s.reply(src, wire.Message{Type: wire.TypeKeepaliveAck, Error: "host has no live registration"})
		return
	}
	s.reply(src, wire.Message{
		Type:      wire.TypeKeepaliveChallenge,
		HostID:    msg.HostID,
		Nonce:     nonce,
		Candidate: src.String(),
	})
}

func (s *Server) handleKeepaliveAuth(msg wire.Message, src *net.UDPAddr) {
	ack := wire.Message{Type: wire.TypeKeepaliveAck}
	candidate := src.String()
	if msg.Candidate != "" && msg.Candidate != candidate {
		ack.Error = "keepalive arrived from a different address than the challenge"
		s.reply(src, ack)
		return
	}
	message := identity.KeepaliveMessage(msg.HostID, candidate, msg.Nonce)
	if err := s.reg.TouchChallenge(msg.HostID, msg.Nonce, candidate, msg.Signature,
		string(message), msg.LocalAddrs, msg.NATType); err != nil {
		ack.Error = err.Error()
		s.reply(src, ack)
		return
	}
	if s.ipChanges != nil {
		s.ipChanges.Observe(msg.HostID, src.IP.String())
	}
	host, ok := s.reg.Lookup(msg.HostID)
	if !ok {
		ack.Error = "host is no longer registered"
		s.reply(src, ack)
		return
	}
	ack.OK = true
	ack.Candidate = candidate
	ack.LocalAddrs = host.LocalAddrs
	ack.NATType = host.NATType
	ack.Fingerprint = host.Fingerprint
	if host.RelayPort != 0 {
		ack.Relay = s.relayFor(host)
	}
	s.reply(src, ack)
}

func (s *Server) relayFor(host registry.Host) string {
	if host.RelayPort == 0 {
		return ""
	}
	if host.RelayHost != "" {
		return net.JoinHostPort(host.RelayHost, strconv.Itoa(host.RelayPort))
	}
	return s.relayAddr(host.RelayPort)
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
	if !identity.CheckPunchAuth(s.serverKey, msg.HostID, msg.Nonce, msg.Auth,
		msg.Timestamp, time.Now(), punchAuthWindow) {
		reply.Error = "punch authentication failed"
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
	// Record only requests that can actually be coordinated, matching hbbs's
	// record point after the target's online check.
	if s.punchLog != nil {
		s.punchLog.Record(src.IP.String(), hostAddr.IP.String(), host.ID)
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
	reply.LocalAddrs = host.LocalAddrs
	reply.NATType = host.NATType
	reply.PublicKey = host.PublicKey
	reply.Fingerprint = host.Fingerprint
	if s.identity != nil && host.PublicKey != "" {
		if pub, err := identity.ParsePublicKey(host.PublicKey); err == nil {
			if binding, err := s.identity.SignHostBinding(host.ID, pub, host.Fingerprint, time.Now()); err == nil {
				reply.Binding = binding
			}
		}
	}
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
	if l.max <= 0 {
		return true
	}
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
