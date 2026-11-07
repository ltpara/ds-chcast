package rendezvous

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	// tcpPunchWait bounds how long one side of a punch waits for the other. The
	// viewer announces first and the host is triggered by UDP, so a host that
	// never answers costs one wait per round.
	tcpPunchWait = 5 * time.Second
	// tcpPunchSessionTTL reaps sessions whose sides never paired.
	tcpPunchSessionTTL = 30 * time.Second
	// tcpPunchMaxSessions caps memory and the number of open handshake sockets.
	tcpPunchMaxSessions = 1024
	// tcpPunchRateMax caps announces per source IP per ten seconds.
	tcpPunchRateMax = 60
)

// tcpWait is one side of a punch waiting for its peer. The channel holds at
// most one pair result or error, so notifying never blocks a handler.
type tcpWait struct {
	ch       chan tcpPair
	observed string
}

type tcpPair struct {
	peer string
	err  string
}

type tcpSession struct {
	viewer  *tcpWait
	host    *tcpWait
	created time.Time
}

// tcpPuncher pairs the two halves of a TCP punch. Both agents open a short TCP
// connection to this port and announce themselves; the server answers each with
// the endpoint it observed for the other. It never forwards traffic: the punch
// itself is between the agents, exactly as on RustDesk's hbbs punch-hole path.
type tcpPuncher struct {
	srv     *Server
	ln      net.Listener
	limiter *limiter

	mu        sync.Mutex
	sessions  map[string]*tcpSession
	lastSwept time.Time
}

// enableTCPPunch binds the punch coordination listener. A bind failure is not
// fatal: UDP punching and relay still work, only the TCP fallback is lost.
func (s *Server) enableTCPPunch(bind string) {
	ln, err := net.Listen("tcp", bind)
	if err != nil {
		s.log.Warn("TCP punch listener not started", "addr", bind, "error", err)
		return
	}
	s.tcpPunch = &tcpPuncher{
		srv:      s,
		ln:       ln,
		limiter:  newLimiter(tcpPunchRateMax, defaultRateSpan),
		sessions: make(map[string]*tcpSession),
	}
	s.log.Info("TCP punch listening", "addr", ln.Addr().String())
	go s.tcpPunch.serve()
}

// TCPAddr is the advertised TCP punch endpoint, or "" when the listener is
// disabled or failed to bind.
func (s *Server) TCPAddr() string {
	if s.tcpPunch == nil {
		return ""
	}
	return s.tcpPunch.ln.Addr().String()
}

func (p *tcpPuncher) serve() {
	for {
		conn, err := p.ln.Accept()
		if err != nil {
			select {
			case <-p.srv.done:
				return
			default:
			}
			if errors.Is(err, net.ErrClosed) {
				return
			}
			p.srv.log.Warn("TCP punch accept", "error", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		go p.handle(conn)
	}
}

func (p *tcpPuncher) close() {
	if p.ln != nil {
		p.ln.Close()
	}
}

func (p *tcpPuncher) handle(conn net.Conn) {
	defer conn.Close()
	remote, ok := conn.RemoteAddr().(*net.TCPAddr)
	if !ok {
		return
	}
	if !p.limiter.allow(remote.IP.String()) {
		return
	}
	conn.SetDeadline(time.Now().Add(tcpPunchWait + 2*time.Second))
	msg, err := readAnnounce(conn)
	if err != nil {
		return
	}
	if err := p.checkAnnounce(msg); err != nil {
		writeAnnounce(conn, wire.Message{Type: wire.TypeTCPPunch, Nonce: msg.Nonce, Error: err.Error()})
		return
	}
	key := msg.HostID + "\x00" + msg.Nonce
	own, err := p.join(key, msg.Role, remote.String())
	if err != nil {
		writeAnnounce(conn, wire.Message{Type: wire.TypeTCPPunch, Nonce: msg.Nonce, Error: err.Error()})
		return
	}
	if msg.Role == wire.RoleViewer {
		// Best effort and off the request path: the host is asleep until this
		// tells it which nonce to announce under.
		go p.triggerHost(msg.HostID, msg.Nonce)
	}
	select {
	case result := <-own.ch:
		if result.err != "" {
			writeAnnounce(conn, wire.Message{Type: wire.TypeTCPPunch, Nonce: msg.Nonce, Error: result.err})
			return
		}
		writeAnnounce(conn, wire.Message{
			Type:  wire.TypeTCPPunch,
			Nonce: msg.Nonce,
			OK:    true,
			// The address this side must punch. It is the peer's observed
			// source, not its registration address: a TCP mapping is per
			// socket, and this is the only one proven reachable.
			Candidate: result.peer,
		})
	case <-time.After(tcpPunchWait):
		p.leave(key, msg.Role, own)
		writeAnnounce(conn, wire.Message{
			Type:  wire.TypeTCPPunch,
			Nonce: msg.Nonce,
			Error: "peer did not join the punch in time",
		})
	case <-p.srv.done:
		p.leave(key, msg.Role, own)
	}
}

// checkAnnounce authenticates one half of a punch. Viewers prove the server key
// with the same HMAC as a punch request; hosts prove their host key, because
// only the registered key can speak for an id.
func (p *tcpPuncher) checkAnnounce(msg wire.Message) error {
	if p.srv.relayOnly != nil && p.srv.relayOnly() {
		return errors.New("direct connections are disabled")
	}
	if msg.Type != wire.TypeTCPPunch || (msg.Role != wire.RoleViewer && msg.Role != wire.RoleHost) {
		return errors.New("not a TCP punch announce")
	}
	if len(msg.Nonce) < 8 || len(msg.Nonce) > 64 {
		return errors.New("nonce must be 8..64 characters")
	}
	host, ok := p.srv.reg.Lookup(msg.HostID)
	if !ok || !p.srv.reg.Online(host) {
		return errors.New("host is offline")
	}
	now := time.Now()
	delta := now.Unix() - msg.Timestamp
	if delta < 0 {
		delta = -delta
	}
	if delta > int64(punchAuthWindow/time.Second) {
		return errors.New("announce is stale")
	}
	if msg.Role == wire.RoleViewer {
		if !identity.CheckPunchAuth(p.srv.serverKey, msg.HostID, msg.Nonce, msg.Auth,
			msg.Timestamp, now, punchAuthWindow) {
			return errors.New("punch authentication failed")
		}
		return nil
	}
	pub, err := identity.ParsePublicKey(host.PublicKey)
	if err != nil {
		return errors.New("host has no public key")
	}
	message := identity.TCPPunchMessage(msg.HostID, msg.Nonce, wire.RoleHost, msg.Timestamp)
	if !identity.Verify(pub, message, msg.Signature) {
		return errors.New("host signature does not match")
	}
	return nil
}

// join registers one side of a punch and wakes both sides when they pair. A
// repeated announce for the same role replaces the older socket: a
// retransmitted trigger or a retried round leaves the previous wait stale, and
// letting it block the fresh announce would waste the whole window.
func (p *tcpPuncher) join(key, role, observed string) (*tcpWait, error) {
	now := time.Now()
	p.mu.Lock()
	defer p.mu.Unlock()
	p.sweepLocked(now)
	sess := p.sessions[key]
	if sess == nil {
		if len(p.sessions) >= tcpPunchMaxSessions {
			return nil, errors.New("too many punches in progress")
		}
		sess = &tcpSession{created: now}
		p.sessions[key] = sess
	}
	own := &tcpWait{ch: make(chan tcpPair, 1), observed: observed}
	var peer *tcpWait
	if role == wire.RoleViewer {
		if sess.viewer != nil {
			tryNotify(sess.viewer, tcpPair{err: "superseded by a newer announce"})
		}
		peer = sess.host
		sess.viewer = own
	} else {
		if sess.host != nil {
			tryNotify(sess.host, tcpPair{err: "superseded by a newer announce"})
		}
		peer = sess.viewer
		sess.host = own
	}
	if peer != nil {
		delete(p.sessions, key)
		tryNotify(peer, tcpPair{peer: observed})
		tryNotify(own, tcpPair{peer: peer.observed})
	}
	return own, nil
}

// leave removes a side that timed out, so a late join is not paired with a
// socket nobody is reading any more.
func (p *tcpPuncher) leave(key, role string, own *tcpWait) {
	p.mu.Lock()
	defer p.mu.Unlock()
	sess := p.sessions[key]
	if sess == nil {
		return
	}
	if role == wire.RoleViewer && sess.viewer == own {
		sess.viewer = nil
	}
	if role == wire.RoleHost && sess.host == own {
		sess.host = nil
	}
	if sess.viewer == nil && sess.host == nil {
		delete(p.sessions, key)
	}
}

func (p *tcpPuncher) sweepLocked(now time.Time) {
	if now.Sub(p.lastSwept) < 5*time.Second {
		return
	}
	p.lastSwept = now
	for key, sess := range p.sessions {
		if now.Sub(sess.created) > tcpPunchSessionTTL {
			if sess.viewer != nil {
				tryNotify(sess.viewer, tcpPair{err: "punch expired"})
			}
			if sess.host != nil {
				tryNotify(sess.host, tcpPair{err: "punch expired"})
			}
			delete(p.sessions, key)
		}
	}
}

func tryNotify(w *tcpWait, result tcpPair) {
	select {
	case w.ch <- result:
	default:
	}
}

// triggerHost tells the host to announce itself under this nonce. The host
// keeps its UDP punch mapping open for keepalives, and this is the only
// reliable place to reach it: a TCP punch listener alone cannot be discovered
// through a NAT without an outbound packet first.
func (p *tcpPuncher) triggerHost(hostID, nonce string) {
	host, ok := p.srv.reg.Lookup(hostID)
	if !ok {
		return
	}
	var targets []*net.UDPAddr
	for _, text := range []string{host.Candidate, host.CandidateV6} {
		if text == "" {
			continue
		}
		addr, err := net.ResolveUDPAddr("udp", text)
		if err == nil && addr != nil {
			targets = append(targets, addr)
		}
	}
	msg := wire.Message{Type: wire.TypeTCPPunch, HostID: hostID, Nonce: nonce}
	for i := 0; i < 3; i++ {
		for _, addr := range targets {
			p.srv.reply(addr, msg)
		}
		if i < 2 {
			time.Sleep(250 * time.Millisecond)
		}
	}
}

func readAnnounce(conn net.Conn) (wire.Message, error) {
	payload, err := carrier.NewFrameReader(conn).ReadFrame()
	if err != nil {
		return wire.Message{}, err
	}
	return wire.Decode(payload)
}

func writeAnnounce(conn net.Conn, msg wire.Message) {
	if err := carrier.NewFrameWriter(conn).WriteFrame(wire.Encode(msg)); err != nil {
		slog.Debug("TCP punch reply", "to", conn.RemoteAddr(), "error", err)
	}
}

// deriveTCPPunchListen derives the coordination listener address from the
// bound UDP address unless an explicit one was configured. The same numeric
// port keeps one firewall opening useful for both halves.
func deriveTCPPunchListen(udp *net.UDPAddr) string {
	if udp == nil || udp.Port == 0 {
		return ""
	}
	return net.JoinHostPort(udp.IP.String(), fmt.Sprint(udp.Port))
}
