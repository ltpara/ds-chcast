package agent

import (
	"bufio"
	"context"
	"net"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// RustDesk pre-punch and connection limits; datagrams use our framing.
const (
	tcpPunchDialWait    = 5 * time.Second
	tcpPunchReplyWait   = 5 * time.Second
	tcpPunchWindow      = 18 * time.Second
	tcpPunchMinGap      = 150 * time.Millisecond
	tcpPunchMaxGap      = 2 * time.Second
	tcpPunchDialTimeout = 3 * time.Second
	tcpPrePunch         = 30 * time.Millisecond
	tcpLinkIdle         = directPathIdle
	tcpLinkKeepalive    = directPathKeepalive
	tcpLinkWrite        = 5 * time.Second
	tcpLinkHandshake    = 10 * time.Second
	tcpPunchNonceTTL    = 30 * time.Second
	tcpPunchMaxRunning  = 8
	tcpLinkMax          = 64
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

// tcpPunchLoop races the UDP path. Coordination has its own 3/6/9s budget;
// the connection budget starts only after the host's pre-punch report.
func (v *ViewerAgent) tcpPunchLoop() {
	defer v.directAttemptDone()
	for {
		ctx := v.directContext()
		conn, br, ok := v.tcpPunchRound(ctx)
		if !ok {
			return
		}
		if !v.attachTCP(conn) {
			conn.Close()
			return
		}
		v.tcpReadLoop(conn, br)
		v.detachTCP(conn)
		if v.cfg.Mode == ModeAuto {
			v.fallBackToRelay("TCP direct path was lost")
			return
		}
		// A new session must look up current addresses and relay credentials.
		return
	}
}

type tcpCoordinationReply struct {
	msg wire.Message
	err error
}

// Keep a single reader alive while retransmitting the same request on the same
// socket. Read deadlines between frames would discard partial frame headers.
func coordinateTCP(ctx context.Context, conn net.Conn, msg wire.Message, step time.Duration) (wire.Message, error) {
	responses := make(chan tcpCoordinationReply, 1)
	go func() {
		reply, err := readHandshake(bufio.NewReader(conn))
		responses <- tcpCoordinationReply{reply, err}
	}()
	for attempt := 1; attempt <= punchRequestAttempts; attempt++ {
		_ = conn.SetWriteDeadline(time.Now().Add(tcpLinkWrite))
		if err := writeHandshake(conn, msg); err != nil {
			return wire.Message{}, err
		}
		timer := time.NewTimer(time.Duration(attempt) * step)
		select {
		case response := <-responses:
			timer.Stop()
			return response.msg, response.err
		case <-ctx.Done():
			timer.Stop()
			return wire.Message{}, ctx.Err()
		case <-timer.C:
		}
	}
	return wire.Message{}, context.DeadlineExceeded
}

func (v *ViewerAgent) tcpPunchRound(ctx context.Context) (net.Conn, *bufio.Reader, bool) {
	if v.tcpPunch == nil || ctx.Err() != nil {
		return nil, nil, false
	}
	started := time.Now()
	announce, err := tcpDialReuseContext(ctx, nil, v.tcpPunch, tcpPunchWindow)
	if err != nil {
		return nil, nil, false
	}
	defer announce.Close()
	stopClose := context.AfterFunc(ctx, func() { announce.Close() })
	defer stopClose()
	local, ok := announce.LocalAddr().(*net.TCPAddr)
	if !ok {
		return nil, nil, false
	}
	nonce := newNonce()
	ts := time.Now().Unix()
	v.mu.Lock()
	ownNAT := v.ownNAT
	v.mu.Unlock()
	msg := wire.Message{Type: wire.TypeTCPPunch, Role: wire.RoleViewer,
		HostID: v.cfg.HostID, Nonce: nonce, Timestamp: ts, NATType: ownNAT}
	if v.cfg.ServerKey != "" {
		msg.Auth = identity.PunchAuth(v.cfg.ServerKey, v.cfg.HostID, nonce, ts)
	}
	reply, err := coordinateTCP(ctx, announce, msg, punchRequestStep)
	if err != nil || !reply.OK || reply.Type != wire.TypeTCPPunch || reply.Nonce != nonce {
		v.log.Debug("TCP punch coordination failed", "error", err, "reply", reply.Error)
		return nil, nil, false
	}
	hostAddr := resolveTCP(reply.Candidate)
	if hostAddr == nil {
		return nil, nil, false
	}
	v.mu.Lock()
	if reply.NATType != "" {
		v.natType = reply.NATType
	}
	v.mu.Unlock()
	wait := v.connectWait(time.Since(started), reply.IsLocal)
	// RustDesk drops the rendezvous socket before connecting from its port.
	announce.Close()
	connectCtx, cancel := context.WithTimeout(ctx, wait)
	defer cancel()
	deadline, _ := connectCtx.Deadline()
	gap := tcpPunchMinGap
	for connectCtx.Err() == nil {
		conn, err := tcpDialReuseContext(connectCtx, local, hostAddr, minDuration(tcpPunchDialTimeout, time.Until(deadline)))
		if err == nil {
			stopConn := context.AfterFunc(connectCtx, func() { conn.Close() })
			_ = conn.SetDeadline(deadline)
			br := bufio.NewReader(conn)
			valid := v.tcpHandshake(conn, br, nonce)
			// Cancellation must not close the selected connection after attachTCP.
			detached := stopConn()
			if valid && detached && connectCtx.Err() == nil {
				return conn, br, true
			}
			conn.Close()
		}
		timer := time.NewTimer(gap)
		select {
		case <-connectCtx.Done():
			timer.Stop()
		case <-timer.C:
		}
		gap = nextTCPGap(gap)
	}
	return nil, nil, false
}

func (v *ViewerAgent) fellBackNow() bool {
	v.mu.Lock()
	defer v.mu.Unlock()
	return v.fellBack
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

func (v *ViewerAgent) attachTCP(conn net.Conn) bool {
	send := carrier.NewFrameWriter(conn)
	stop := make(chan struct{})

	// Commit the path switch and the fallback decision under the same lock.
	// Otherwise a relay timeout can win after the punch handshake but before
	// this link is published, letting the viewer switch back to TCP after QUIC
	// has already retransmitted on the relay.
	v.mu.Lock()
	if v.ctx.Err() != nil || v.directLost || v.fellBack || v.directOK.Load() || v.carrierMode {
		v.mu.Unlock()
		return false
	}
	v.tcpMu.Lock()
	v.tcpConn = conn
	v.tcpSend = send
	v.tcpStop = stop
	v.tcpMu.Unlock()

	v.tcpRemote = conn.RemoteAddr().String()
	v.tcpActive.Store(true)
	v.directOK.Store(true)
	v.latched = true
	if v.timer != nil {
		v.timer.Stop()
	}
	remote := v.tcpRemote
	v.mu.Unlock()

	v.stopDirectAttempts()
	v.recordDirectResult(false)
	v.log.Info("TCP direct path established", "host", remote)
	v.reportPath()
	go v.tcpKeepalive(conn, send, stop)
	return true
}

func (v *ViewerAgent) detachTCP(conn net.Conn) {
	v.mu.Lock()
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
	v.directLost = true
	v.tcpRemote = ""
	v.remote = nil
	// The TCP link was the direct path, so a relay fallback must be allowed
	// again. UDP confirmation never races this: it is ignored while TCP owns
	// the path.
	if !v.fellBack {
		v.latched = false
	}
	v.directOK.Store(false)
	v.mu.Unlock()
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
func (h *HostAgent) startTCPPunch(nonce, candidate, peerNAT string, localPeer bool) {
	peer := resolveTCP(candidate)
	if peer == nil || len(nonce) < 8 || len(nonce) > 64 {
		return
	}
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
	if _, busy := h.tcpPunches[nonce]; busy || h.tcpPunchRunning >= tcpPunchMaxRunning || len(h.tcpPunches) >= 1024 {
		h.mu.Unlock()
		return
	}
	h.tcpPunches[nonce] = now.Add(tcpPunchNonceTTL)
	h.tcpPunchRunning++
	h.mu.Unlock()
	go func() {
		defer func() {
			h.mu.Lock()
			h.tcpPunchRunning--
			h.mu.Unlock()
		}()
		if h.cfg.Proxy != nil || (!localPeer && (h.getNATType() == NATSymmetric || peerNAT == NATSymmetric)) {
			h.declineTCPPunch(nonce, endpoint)
			return
		}
		h.runTCPPunch(nonce, endpoint, peer, localPeer)
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

func (h *HostAgent) declineTCPPunch(nonce string, endpoint *net.TCPAddr) {
	ctx, cancel := context.WithTimeout(h.ctx, tcpPunchDialWait)
	defer cancel()
	conn, err := h.cfg.Proxy.DialContext(ctx, "tcp", endpoint.String())
	if err != nil {
		return
	}
	defer conn.Close()
	stop := context.AfterFunc(ctx, func() { conn.Close() })
	defer stop()
	ts := time.Now().Unix()
	reason := "host requires relay for IPv4 TCP"
	_ = writeHandshake(conn, wire.Message{Type: wire.TypeTCPPunch, Role: wire.RoleHost,
		HostID: h.cfg.ID, Nonce: nonce, Timestamp: ts, Error: reason,
		Signature: identity.Sign(h.priv, identity.TCPPunchDeclineMessage(h.cfg.ID, nonce, reason, ts)),
	})
}

// runTCPPunch follows RustDesk's handle_punch_hole: open the coordination
// socket, pre-punch from its local port, then send the host report. Only this
// report releases the viewer's response, so its SYN follows the pre-punch.
func (h *HostAgent) runTCPPunch(nonce string, endpoint, peer *net.TCPAddr, localPeer bool) {
	announce, err := tcpDialReuseContext(h.ctx, nil, endpoint, tcpPunchDialWait)
	if err != nil {
		return
	}
	defer announce.Close()
	stopClose := context.AfterFunc(h.ctx, func() { announce.Close() })
	defer stopClose()
	local, ok := announce.LocalAddr().(*net.TCPAddr)
	if !ok {
		return
	}
	if !localPeer {
		if pc, err := tcpDialReuseContext(h.ctx, local, peer, tcpPrePunch); err == nil {
			pc.Close()
		}
	}
	ts := time.Now().Unix()
	_ = announce.SetWriteDeadline(time.Now().Add(tcpPunchReplyWait))
	if err := writeHandshake(announce, wire.Message{
		Type: wire.TypeTCPPunch, Role: wire.RoleHost, HostID: h.cfg.ID,
		Nonce: nonce, Timestamp: ts, Candidate: local.String(),
		Signature: identity.Sign(h.priv, identity.TCPPunchLocalMessage(h.cfg.ID, nonce, local.String(), ts)),
	}); err != nil {
		return
	}
	announce.Close()
	h.runTCPPunchWindow(nonce, local, peer)
}

func (h *HostAgent) runTCPPunchWindow(nonce string, local, peer *net.TCPAddr) {
	listener, err := tcpListenReuse(local)
	if err != nil {
		h.log.Warn("TCP punch listener failed", "local", local.String(), "error", err)
		return
	}
	defer listener.Close()
	stopClose := context.AfterFunc(h.ctx, func() { listener.Close() })
	defer stopClose()
	if tl, ok := listener.(*net.TCPListener); ok {
		_ = tl.SetDeadline(time.Now().Add(tcpPunchWindow))
	}
	conn, err := listener.Accept()
	if err != nil {
		h.log.Debug("nothing connected to the TCP punch", "peer", peer.String(), "error", err)
		return
	}
	go h.serveTCPViewer(conn, nonce)
}

// serveTCPViewer authenticates the punched socket and then carries framed
// datagrams between it and chast-host. One link has its own local route, so it
// cannot redirect or revoke another viewer's path.
func (h *HostAgent) serveTCPViewer(conn net.Conn, nonce string) {
	defer conn.Close()
	stopClose := context.AfterFunc(h.ctx, func() { conn.Close() })
	defer stopClose()
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
	if !h.addTCPLinkLocked(conn, remote) {
		h.mu.Unlock()
		h.log.Warn("TCP viewer path refused", "from", remote, "reason", "link limit reached")
		return
	}
	h.mu.Unlock()
	h.log.Info("TCP viewer path established", "from", remote)
	h.reportInfo()
	route := "tcp:" + remote
	defer func() {
		h.mu.Lock()
		h.removeTCPLinkLocked(conn, remote)
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

func (h *HostAgent) addTCPLinkLocked(conn net.Conn, remote string) bool {
	if len(h.tcpLinks) >= tcpLinkMax {
		return false
	}
	if h.tcpLinks == nil {
		h.tcpLinks = make(map[net.Conn]string)
	}
	h.tcpLinks[conn] = remote
	if h.tcpViewer == "" {
		h.tcpViewer = remote
	}
	h.viewerTransport = "tcp"
	return true
}

func (h *HostAgent) removeTCPLinkLocked(conn net.Conn, remote string) {
	delete(h.tcpLinks, conn)
	if h.tcpViewer != remote {
		return
	}
	h.tcpViewer = ""
	for _, remaining := range h.tcpLinks {
		h.tcpViewer = remaining
		break
	}
	if h.tcpViewer != "" {
		return
	}
	h.viewerTransport = ""
	if h.viewer != nil {
		h.viewerTransport = "udp"
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
