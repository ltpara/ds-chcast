package agent

import (
	"bufio"
	"errors"
	"net"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// TCP punch timings mirror RustDesk. The coordination server pairs the two
// announcements in milliseconds; the window is what the peer has to complete
// the punch itself, and it is the same CONNECT_TIMEOUT hbbs uses.
const (
	tcpPunchDialWait    = 5 * time.Second
	tcpPunchReplyWait   = 5 * time.Second
	tcpPunchWindow      = 18 * time.Second
	tcpPunchGrace       = 3 * time.Second
	tcpPunchMinGap      = 150 * time.Millisecond
	tcpPunchMaxGap      = 2 * time.Second
	tcpPunchDialTimeout = 3 * time.Second
	tcpPunchRetryWait   = 500 * time.Millisecond
	tcpPrePunch         = 30 * time.Millisecond
	tcpLinkIdle         = 60 * time.Second
	tcpLinkKeepalive    = 20 * time.Second
	tcpLinkWrite        = 5 * time.Second
	tcpLinkHandshake    = 10 * time.Second
	tcpPunchNonceTTL    = 30 * time.Second
	tcpPunchMaxRunning  = 8
)

func minDuration(a, b time.Duration) time.Duration {
	if a < b {
		return a
	}
	return b
}

func nextTCPGap(gap time.Duration) time.Duration {
	gap = gap * 3 / 2
	if gap > tcpPunchMaxGap {
		return tcpPunchMaxGap
	}
	return gap
}

func writeHandshake(conn net.Conn, msg wire.Message) error {
	return carrier.NewFrameWriter(conn).WriteFrame(wire.Encode(msg))
}

func readHandshake(br *bufio.Reader) (wire.Message, error) {
	payload, err := carrier.NewFrameReader(br).ReadFrame()
	if err != nil {
		return wire.Message{}, err
	}
	return wire.Decode(payload)
}

func resolveTCP(text string) *net.TCPAddr {
	if text == "" {
		return nil
	}
	addr, err := net.ResolveTCPAddr("tcp", text)
	if err != nil || addr == nil || addr.Port == 0 {
		return nil
	}
	return addr
}

// tcpWindow is how long a viewer keeps trying to complete the TCP punch. With
// a relay behind it the window matches the UDP direct attempt, because the
// relay takes over at the same deadline; direct-only and TCP-only sessions get
// RustDesk's full connect timeout.
func (v *ViewerAgent) tcpWindow() time.Duration {
	if v.cfg.Mode == ModeAuto {
		return v.directWait
	}
	return tcpPunchWindow
}

// tcpPunchLoop coordinates a punch and then owns the link until it dies. In
// auto mode a lost link falls back to the relay; direct and tcp modes keep
// re-punching for the life of the agent.
func (v *ViewerAgent) tcpPunchLoop() {
	v.directTrying.Store(true)
	defer v.directTrying.Store(false)
	deadline := time.Now().Add(v.tcpWindow())
	for {
		if v.ctx.Err() != nil || v.fellBack || v.directOK.Load() {
			return
		}
		conn, br, ok := v.tcpPunchRound(deadline)
		if !ok {
			if time.Now().After(deadline) {
				v.log.Info("TCP punch was not confirmed", "window", v.tcpWindow())
				return
			}
			select {
			case <-v.ctx.Done():
				return
			case <-time.After(tcpPunchRetryWait):
			}
			continue
		}
		v.attachTCP(conn)
		v.tcpReadLoop(conn, br)
		v.detachTCP(conn)
		if v.cfg.Mode == ModeAuto {
			if !v.fellBack {
				v.fallBackToRelay("TCP direct path was lost; using relay")
			}
			return
		}
		deadline = time.Now().Add(v.tcpWindow())
	}
}

// tcpPunchRound announces one nonce, learns the host's observed TCP endpoint
// and dials it from the address the announcement was sent from. Keeping that
// address is the whole trick: it is the mapping the host was told to punch.
func (v *ViewerAgent) tcpPunchRound(deadline time.Time) (net.Conn, *bufio.Reader, bool) {
	if v.tcpPunch == nil {
		return nil, nil, false
	}
	wait := minDuration(tcpPunchReplyWait, time.Until(deadline))
	if wait <= 0 {
		return nil, nil, false
	}
	announce, err := tcpDialReuse(nil, v.tcpPunch, minDuration(tcpPunchDialWait, wait))
	if err != nil {
		v.log.Debug("TCP punch announce failed", "server", v.tcpPunch.String(), "error", err)
		return nil, nil, false
	}
	local, ok := announce.LocalAddr().(*net.TCPAddr)
	if !ok {
		announce.Close()
		return nil, nil, false
	}
	nonce := newNonce()
	_ = announce.SetDeadline(time.Now().Add(wait + time.Second))
	ts := time.Now().Unix()
	msg := wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleViewer,
		HostID:    v.cfg.HostID,
		Nonce:     nonce,
		Timestamp: ts,
	}
	if v.cfg.ServerKey != "" {
		msg.Auth = identity.PunchAuth(v.cfg.ServerKey, v.cfg.HostID, nonce, ts)
	}
	if err := writeHandshake(announce, msg); err != nil {
		announce.Close()
		return nil, nil, false
	}
	buf := bufio.NewReader(announce)
	reply, err := readHandshake(buf)
	if err != nil || !reply.OK || (reply.Nonce != "" && reply.Nonce != nonce) {
		if err != nil {
			v.log.Debug("TCP punch announce failed", "server", v.tcpPunch.String(), "error", err)
		} else {
			v.log.Debug("TCP punch announce refused", "error", reply.Error)
		}
		announce.Close()
		return nil, nil, false
	}
	hostAddr := resolveTCP(reply.Candidate)
	if hostAddr == nil {
		v.log.Debug("TCP punch reply carried no endpoint", "candidate", reply.Candidate)
		announce.Close()
		return nil, nil, false
	}
	v.log.Debug("TCP punch paired", "host", hostAddr.String(), "local", local.String())
	// The announce socket stays open for the whole dial phase: it is the
	// mapping the host punches, and closing it early can drop that mapping
	// before the peer's SYN arrives.
	gap := tcpPunchMinGap
	for {
		left := time.Until(deadline)
		if left <= 0 || v.ctx.Err() != nil || v.fellBack || v.directOK.Load() {
			break
		}
		conn, err := tcpDialReuse(local, hostAddr, minDuration(tcpPunchDialTimeout, left))
		if err == nil {
			_ = conn.SetDeadline(time.Now().Add(tcpLinkHandshake))
			cbr := bufio.NewReader(conn)
			if v.tcpHandshake(conn, cbr, nonce) {
				announce.Close()
				return conn, cbr, true
			}
			conn.Close()
		}
		select {
		case <-v.ctx.Done():
		case <-time.After(gap):
		}
		gap = nextTCPGap(gap)
	}
	announce.Close()
	return nil, nil, false
}

// tcpHandshake proves the peer owns the host id before any datagram reaches
// the local QUIC client. The nonce ties the socket to this punch round.
func (v *ViewerAgent) tcpHandshake(conn net.Conn, br *bufio.Reader, nonce string) bool {
	msg, err := readHandshake(br)
	if err != nil {
		return false
	}
	if msg.Type != wire.TypeTCPDirect || msg.Role != wire.RoleHost ||
		msg.HostID != v.cfg.HostID || msg.Nonce != nonce {
		return false
	}
	if v.hostPub == nil ||
		!identity.Verify(v.hostPub, identity.TCPDirectMessage(msg.HostID, nonce, msg.Timestamp), msg.Signature) {
		v.log.Warn("TCP direct hello did not verify", "from", conn.RemoteAddr().String())
		return false
	}
	if err := writeHandshake(conn, wire.Message{
		Type:      wire.TypeTCPDirect,
		Role:      wire.RoleViewer,
		HostID:    v.cfg.HostID,
		Nonce:     nonce,
		Timestamp: time.Now().Unix(),
	}); err != nil {
		return false
	}
	_ = conn.SetDeadline(time.Time{})
	return true
}

func (v *ViewerAgent) attachTCP(conn net.Conn) {
	v.tcpMu.Lock()
	send := carrier.NewFrameWriter(conn)
	stop := make(chan struct{})
	v.tcpConn = conn
	v.tcpSend = send
	v.tcpStop = stop
	v.tcpMu.Unlock()

	v.mu.Lock()
	v.tcpRemote = conn.RemoteAddr().String()
	v.tcpActive.Store(true)
	v.directOK.Store(true)
	v.directTrying.Store(false)
	v.latched = true
	if v.timer != nil {
		v.timer.Stop()
		v.timerArmed = true
	}
	remote := v.tcpRemote
	v.mu.Unlock()

	v.log.Info("TCP direct path established", "host", remote)
	v.reportPath()
	go v.tcpKeepalive(conn, send, stop)
}

func (v *ViewerAgent) detachTCP(conn net.Conn) {
	v.tcpMu.Lock()
	if v.tcpConn == conn {
		v.tcpConn = nil
		v.tcpSend = nil
		if v.tcpStop != nil {
			close(v.tcpStop)
			v.tcpStop = nil
		}
	}
	v.tcpMu.Unlock()
	v.tcpActive.Store(false)
	v.mu.Lock()
	v.tcpRemote = ""
	// The TCP link was the direct path, so a relay fallback must be allowed
	// again. UDP confirmation never races this: it is ignored while TCP owns
	// the path.
	v.latched = false
	v.mu.Unlock()
	v.directOK.Store(false)
	conn.Close()
	v.log.Warn("TCP direct path lost")
	v.reportPath()
}

func (v *ViewerAgent) tcpReadLoop(conn net.Conn, br *bufio.Reader) {
	reader := carrier.NewFrameReader(br)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(tcpLinkIdle))
		payload, err := reader.ReadFrame()
		if err != nil {
			return
		}
		if len(payload) == 0 {
			continue
		}
		v.writeLocal(payload)
	}
}

func (v *ViewerAgent) tcpKeepalive(conn net.Conn, send *carrier.FrameWriter, stop <-chan struct{}) {
	ticker := time.NewTicker(tcpLinkKeepalive)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-v.ctx.Done():
			return
		case <-ticker.C:
			_ = conn.SetWriteDeadline(time.Now().Add(tcpLinkWrite))
			if err := send.WriteFrame(nil); err != nil {
				conn.Close()
				return
			}
		}
	}
}

// writeTCP forwards one datagram over the punched TCP link.
func (v *ViewerAgent) writeTCP(payload []byte) {
	v.tcpMu.Lock()
	conn, send := v.tcpConn, v.tcpSend
	v.tcpMu.Unlock()
	if conn == nil || send == nil {
		return
	}
	_ = conn.SetWriteDeadline(time.Now().Add(tcpLinkWrite))
	if err := send.WriteFrame(payload); err != nil {
		v.log.Warn("TCP direct write failed", "error", err)
		conn.Close()
	}
}

// startTCPPunch deduplicates one coordination trigger and runs the host half
// off the read loop.
func (h *HostAgent) startTCPPunch(nonce string) {
	endpoint := h.getTCPPunch()
	if endpoint == nil {
		return
	}
	now := time.Now()
	h.mu.Lock()
	if h.tcpPunches == nil {
		h.tcpPunches = make(map[string]time.Time)
	}
	for key, expiry := range h.tcpPunches {
		if now.After(expiry) {
			delete(h.tcpPunches, key)
		}
	}
	if _, busy := h.tcpPunches[nonce]; busy || len(h.tcpPunches) >= tcpPunchMaxRunning {
		h.mu.Unlock()
		return
	}
	h.tcpPunches[nonce] = now.Add(tcpPunchNonceTTL)
	h.mu.Unlock()
	go func() {
		defer func() {
			h.mu.Lock()
			delete(h.tcpPunches, nonce)
			h.mu.Unlock()
		}()
		h.runTCPPunch(nonce, endpoint)
	}()
}

func (h *HostAgent) getTCPPunch() *net.TCPAddr {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.tcpPunch == nil {
		return nil
	}
	out := *h.tcpPunch
	return &out
}

// runTCPPunch announces the host half, pre-punches the viewer from the
// announced socket's local port, then hands that port to a listener. RustDesk
// does the same: the mapping the host's own SYN opened is what lets the
// viewer's SYN back in.
func (h *HostAgent) runTCPPunch(nonce string, endpoint *net.TCPAddr) {
	announce, err := tcpDialReuse(nil, endpoint, tcpPunchDialWait)
	if err != nil {
		h.log.Debug("TCP punch coordination dial failed", "server", endpoint.String(), "error", err)
		return
	}
	local, ok := announce.LocalAddr().(*net.TCPAddr)
	if !ok {
		announce.Close()
		return
	}
	ts := time.Now().Unix()
	signature := identity.Sign(h.priv, identity.TCPPunchMessage(h.cfg.ID, nonce, wire.RoleHost, ts))
	_ = announce.SetDeadline(time.Now().Add(tcpPunchReplyWait + time.Second))
	if err := writeHandshake(announce, wire.Message{
		Type:      wire.TypeTCPPunch,
		Role:      wire.RoleHost,
		HostID:    h.cfg.ID,
		Nonce:     nonce,
		Timestamp: ts,
		Signature: signature,
	}); err != nil {
		announce.Close()
		return
	}
	reply, err := readHandshake(bufio.NewReader(announce))
	if err != nil {
		h.log.Debug("TCP punch announcement failed", "error", err)
		announce.Close()
		return
	}
	if !reply.OK {
		h.log.Debug("TCP punch announcement refused", "error", reply.Error)
		announce.Close()
		return
	}
	peer := resolveTCP(reply.Candidate)
	if peer == nil {
		announce.Close()
		return
	}
	// Not awaited past its 30ms budget: the SYN only has to leave, opening the
	// gateway mapping before the controller's own SYN can arrive.
	if pc, err := tcpDialReuse(local, peer, tcpPrePunch); err == nil {
		pc.Close()
	}
	announce.Close()
	h.runTCPPunchWindow(nonce, local, peer)
}

func (h *HostAgent) runTCPPunchWindow(nonce string, local, peer *net.TCPAddr) {
	until := time.Now().Add(tcpPunchWindow)
	listener, err := tcpListenReuse(local)
	if err != nil {
		// Not fatal: the dial half can still meet the controller's SYN.
		h.log.Warn("TCP punch listener failed; dialing only", "local", local.String(), "error", err)
	}
	acceptCh := make(chan net.Conn)
	if listener != nil {
		go func() {
			for {
				left := time.Until(until)
				if left <= 0 {
					return
				}
				if tl, ok := listener.(*net.TCPListener); ok {
					_ = tl.SetDeadline(time.Now().Add(left))
				}
				conn, err := listener.Accept()
				if err != nil {
					if errors.Is(err, net.ErrClosed) {
						return
					}
					// A timeout ends the window; a transient accept error would
					// otherwise spin, so back off one deadline's worth.
					if ne, ok := err.(net.Error); ok && ne.Timeout() {
						return
					}
					select {
					case <-h.ctx.Done():
						return
					case <-time.After(tcpPunchRetryWait):
					}
					continue
				}
				select {
				case acceptCh <- conn:
				default:
					conn.Close()
				}
			}
		}()
	}
	dialCh := make(chan net.Conn)
	stopDial := make(chan struct{})
	go func() {
		gap := tcpPunchMinGap
		for {
			left := time.Until(until)
			if left <= 0 {
				return
			}
			conn, err := tcpDialReuse(local, peer, minDuration(tcpPunchDialTimeout, left))
			if err == nil {
				select {
				case dialCh <- conn:
				case <-stopDial:
					conn.Close()
				case <-h.ctx.Done():
					conn.Close()
				}
				return
			}
			select {
			case <-h.ctx.Done():
				return
			case <-stopDial:
				return
			case <-time.After(minDuration(gap, left)):
			}
			gap = nextTCPGap(gap)
		}
	}()
	var conn net.Conn
	timer := time.NewTimer(time.Until(until) + tcpPunchGrace)
	defer timer.Stop()
	select {
	case conn = <-dialCh:
	case conn = <-acceptCh:
		// Prefer a crossing punch when both are ready: it is the one known to
		// have met the viewer at the address the server observed.
		select {
		case crossed := <-dialCh:
			conn.Close()
			conn = crossed
		default:
		}
	case <-timer.C:
		conn = nil
	case <-h.ctx.Done():
		conn = nil
	}
	if listener != nil {
		listener.Close()
	}
	if conn == nil {
		close(stopDial)
		h.log.Info("nothing connected to the TCP punch", "peer", peer.String())
		return
	}
	select {
	case extra := <-acceptCh:
		extra.Close()
	default:
	}
	select {
	case extra := <-dialCh:
		extra.Close()
	default:
	}
	// The dial half may still be connecting; the stop makes it drop its socket
	// instead of leaving a blocked send while this link is served.
	close(stopDial)
	h.serveTCPViewer(conn, nonce)
}

// serveTCPViewer authenticates the punched socket and then carries framed
// datagrams between it and chast-host. One link has its own local route, so it
// cannot redirect or revoke another viewer's path.
func (h *HostAgent) serveTCPViewer(conn net.Conn, nonce string) {
	defer conn.Close()
	remote := conn.RemoteAddr().String()
	_ = conn.SetDeadline(time.Now().Add(tcpLinkHandshake))
	ts := time.Now().Unix()
	signature := identity.Sign(h.priv, identity.TCPDirectMessage(h.cfg.ID, nonce, ts))
	if err := writeHandshake(conn, wire.Message{
		Type:      wire.TypeTCPDirect,
		Role:      wire.RoleHost,
		HostID:    h.cfg.ID,
		Nonce:     nonce,
		Timestamp: ts,
		Signature: signature,
	}); err != nil {
		return
	}
	br := bufio.NewReader(conn)
	hello, err := readHandshake(br)
	if err != nil {
		return
	}
	if hello.Type != wire.TypeTCPDirect || hello.Role != wire.RoleViewer ||
		hello.HostID != h.cfg.ID || hello.Nonce != nonce {
		h.log.Warn("TCP direct hello rejected", "from", remote)
		return
	}
	_ = conn.SetDeadline(time.Time{})
	h.mu.Lock()
	if h.tcpLinks == nil {
		h.tcpLinks = make(map[net.Conn]struct{})
	}
	h.tcpLinks[conn] = struct{}{}
	h.tcpViewer = remote
	h.viewerTransport = "tcp"
	h.mu.Unlock()
	h.log.Info("TCP viewer path established", "from", remote)
	h.reportInfo()
	route := "tcp:" + remote
	defer func() {
		h.mu.Lock()
		delete(h.tcpLinks, conn)
		if h.tcpViewer == remote {
			h.tcpViewer = ""
			h.viewerTransport = ""
			if h.viewer != nil {
				h.viewerTransport = "udp"
			}
		}
		h.mu.Unlock()
		h.routes.remove(route)
		h.log.Info("TCP viewer path closed", "from", remote)
		h.reportInfo()
	}()

	send := carrier.NewFrameWriter(conn)
	stop := make(chan struct{})
	defer close(stop)
	go h.tcpLinkKeepalive(conn, send, stop)
	reader := carrier.NewFrameReader(br)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(tcpLinkIdle))
		payload, err := reader.ReadFrame()
		if err != nil {
			return
		}
		if len(payload) == 0 {
			continue
		}
		if err := h.routes.forward(route, h.local, payload, func(reply []byte) error {
			if len(reply) == 0 {
				return nil
			}
			_ = conn.SetWriteDeadline(time.Now().Add(tcpLinkWrite))
			return send.WriteFrame(reply)
		}); err != nil {
			h.log.Warn("TCP viewer forward failed", "from", remote, "error", err)
			return
		}
	}
}

func (h *HostAgent) tcpLinkKeepalive(conn net.Conn, send *carrier.FrameWriter, stop <-chan struct{}) {
	ticker := time.NewTicker(tcpLinkKeepalive)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-h.ctx.Done():
			return
		case <-ticker.C:
			_ = conn.SetWriteDeadline(time.Now().Add(tcpLinkWrite))
			if err := send.WriteFrame(nil); err != nil {
				conn.Close()
				return
			}
		}
	}
}
