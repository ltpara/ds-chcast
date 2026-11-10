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
	// viewer retries on one connection at 3s and 9s; keep its session alive
	// throughout all three response windows (18s total).
	tcpPunchWait = 20 * time.Second
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
	local    string
}

type tcpPair struct {
	peer  string
	local bool
	err   string
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
	controls  map[string]*hostControl
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
	p.mu.Lock()
	for _, control := range p.controls {
		control.conn.Close()
	}
	p.mu.Unlock()
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
	conn.SetDeadline(time.Now().Add(7 * time.Second))
	msg, err := readAnnounce(conn)
	if err != nil {
		return
	}
	if msg.Type == wire.TypeRendezvousControl {
		p.serveHostControl(conn, msg)
		return
	}
	if err := p.checkAnnounce(msg); err != nil {
		writeAnnounce(conn, wire.Message{Type: wire.TypeTCPPunch, Nonce: msg.Nonce, Error: err.Error()})
		return
	}
	key := msg.HostID + "\x00" + msg.Nonce
	if msg.Role == wire.RoleHost && msg.Error != "" {
		p.mu.Lock()
		if sess := p.sessions[key]; sess != nil && sess.viewer != nil {
			tryNotify(sess.viewer, tcpPair{err: msg.Error})
			delete(p.sessions, key)
		}
		p.mu.Unlock()
		return
	}
	own, err := p.join(key, msg.Role, remote.String(), msg.Candidate)
	if err != nil {
		writeAnnounce(conn, wire.Message{Type: wire.TypeTCPPunch, Nonce: msg.Nonce, Error: err.Error()})
		return
	}
	defer p.leave(key, msg.Role, own)
	_ = conn.SetDeadline(time.Now().Add(tcpPunchWait))
	// A single reader preserves partially received frames across retry windows.
	requests := make(chan wire.Message)
	disconnected := make(chan struct{})
	finished := make(chan struct{})
	defer close(finished)
	if msg.Role == wire.RoleViewer {
		go func() {
			defer close(disconnected)
			for {
				retry, err := readAnnounce(conn)
				if err != nil {
					return
				}
				select {
				case requests <- retry:
				case <-finished:
					return
				}
			}
		}()
		go p.triggerHost(msg.HostID, msg.Nonce, remote.String(), msg.NATType)
	}
	timer := time.NewTimer(tcpPunchWait)
	defer timer.Stop()
	for {
		select {
		case retry := <-requests:
			if retry.Role != msg.Role || retry.HostID != msg.HostID || retry.Nonce != msg.Nonce ||
				!p.limiter.allow(remote.IP.String()) || p.checkAnnounce(retry) != nil {
				return
			}
			go p.triggerHost(msg.HostID, msg.Nonce, remote.String(), retry.NATType)
		case <-disconnected:
			return
		case result := <-own.ch:
			host, _ := p.srv.reg.Lookup(msg.HostID)
			writeAnnounce(conn, wire.Message{
				Type: wire.TypeTCPPunch, Nonce: msg.Nonce,
				OK: result.err == "", Error: result.err,
				Candidate: result.peer, NATType: host.NATType, IsLocal: result.local,
			})
			return
		case <-timer.C:
			writeAnnounce(conn, wire.Message{Type: wire.TypeTCPPunch, Nonce: msg.Nonce,
				Error: "peer did not join the punch in time"})
			return
		case <-p.srv.done:
			return
		}
	}
}

// checkAnnounce authenticates one half of a punch. Viewers present either a
// short-lived transport credential or the legacy link/server key HMAC; hosts
// prove their registered host key, because only that key can speak for an id.
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
	now := time.Now()
	delta := now.Unix() - msg.Timestamp
	if delta < 0 {
		delta = -delta
	}
	if delta > int64(punchAuthWindow/time.Second) {
		return errors.New("announce is stale")
	}
	// Authenticate the caller before looking up the host. In particular, a
	// viewer without a connection credential must not be able to use this port
	// to probe whether an ID exists or is online.
	if msg.Role == wire.RoleViewer {
		if !p.srv.checkViewerPunch(msg, now) {
			return errors.New("punch authentication failed")
		}
	}
	host, ok := p.srv.reg.Lookup(msg.HostID)
	if !ok {
		if msg.Role == wire.RoleHost {
			return errors.New("punch authentication failed")
		}
		return errors.New("host is offline")
	}
	if msg.Role == wire.RoleHost {
		// Until the signature verifies, unknown IDs and invalid keys/proofs
		// must be indistinguishable, including when the host is offline.
		pub, err := identity.ParsePublicKey(host.PublicKey)
		if err != nil {
			return errors.New("punch authentication failed")
		}
		message := identity.TCPPunchMessage(msg.HostID, msg.Nonce, wire.RoleHost, msg.Timestamp)
		if msg.Candidate != "" {
			message = identity.TCPPunchLocalMessage(msg.HostID, msg.Nonce, msg.Candidate, msg.Timestamp)
		}
		if msg.Error != "" {
			message = identity.TCPPunchDeclineMessage(msg.HostID, msg.Nonce, msg.Error, msg.Timestamp)
		}
		if !identity.Verify(pub, message, msg.Signature) {
			return errors.New("punch authentication failed")
		}
	}
	online := p.srv.reg.Online(host)
	if !online {
		online = p.srv.HostControlLinked(msg.HostID)
	}
	if !online && p.srv.hostCarrierLinked != nil {
		online = p.srv.hostCarrierLinked(msg.HostID)
	}
	if !online {
		return errors.New("host is offline")
	}
	return nil
}

// join registers one side of a punch and wakes both sides when they pair. A
// repeated announce for the same role replaces the older socket: a
// retransmitted trigger or a retried round leaves the previous wait stale, and
// letting it block the fresh announce would waste the whole window.
func (p *tcpPuncher) join(key, role, observed, local string) (*tcpWait, error) {
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
	own := &tcpWait{ch: make(chan tcpPair, 1), observed: observed, local: local}
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
		tryNotify(peer, tcpPeerResult(own, peer))
		tryNotify(own, tcpPeerResult(peer, own))
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
// receives the server instruction over its authenticated control link, or
// over UDP when that link is unavailable. Retries come from the viewer.
func (p *tcpPuncher) triggerHost(hostID, nonce, candidate, nat string) {
	host, ok := p.srv.reg.Lookup(hostID)
	if !ok {
		return
	}
	msg := wire.Message{Type: wire.TypeTCPPunch, HostID: hostID, Nonce: nonce, Candidate: candidate, NATType: nat}
	p.mu.Lock()
	link := p.controls[hostID]
	p.mu.Unlock()
	hostObserved := host.Candidate
	if link != nil {
		hostObserved = link.conn.RemoteAddr().String()
	}
	msg.IsLocal = sameObservedIP(hostObserved, candidate)
	if p.triggerControl(msg) {
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
	for _, addr := range targets {
		p.srv.reply(addr, msg)
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

func sameObservedIP(a, b string) bool {
	ah, _, ae := net.SplitHostPort(a)
	bh, _, be := net.SplitHostPort(b)
	ai, bi := net.ParseIP(ah), net.ParseIP(bh)
	return ae == nil && be == nil && ai != nil && bi != nil && ai.Equal(bi)
}

func tcpPeerResult(peer, own *tcpWait) tcpPair {
	result := tcpPair{peer: peer.observed}
	if sameObservedIP(peer.observed, own.observed) && peer.local != "" {
		host, port, err := net.SplitHostPort(peer.local)
		ip := net.ParseIP(host)
		if err == nil && port != "0" && ip != nil && !ip.IsUnspecified() && !ip.IsMulticast() {
			result.peer, result.local = peer.local, true
		}
	}
	return result
}
