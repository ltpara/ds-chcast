package relay

import (
	"bufio"
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
)

// SharedCarrier is one TCP/WebSocket listener that serves every host. Host
// links authenticate with the same signed challenge as the per-port carrier;
// viewers select a host with the token issued by the authenticated lookup.
// This is the chast equivalent of RustDesk's single-port relay keyed by UUID,
// except that only the control plane is multiplexed: each host still keeps its
// own UDP relay port because the relay cannot demultiplex opaque QUIC.
type SharedCarrier struct {
	ln     net.Listener
	auth   Authenticator
	policy *Policy
	log    *slog.Logger

	mu       sync.Mutex
	hosts    map[string]*sharedHost
	viewers  uint64
	busy     uint64
	rejected uint64
	blocked  uint64
	links    uint64
	toHost   uint64
	toViewer uint64
	closed   bool

	closeOnce sync.Once
}

type sharedHost struct {
	carrier *SharedCarrier
	id      string
	conn    net.Conn
	send    *carrier.FrameWriter
	viewer  *carrierViewer
}

type SharedCarrierStats struct {
	Addr          string `json:"addr,omitempty"`
	Hosts         int    `json:"hosts"`
	HostLinks     uint64 `json:"host_links"`
	Viewers       uint64 `json:"viewers"`
	Busy          uint64 `json:"busy"`
	Rejected      uint64 `json:"rejected"`
	Blocked       uint64 `json:"blocked_sessions"`
	ToHostBytes   uint64 `json:"to_host_bytes"`
	ToViewerBytes uint64 `json:"to_viewer_bytes"`
}

func newSharedCarrier(bind string, auth Authenticator, policy *Policy, log *slog.Logger) (*SharedCarrier, error) {
	ln, err := net.Listen("tcp", bind)
	if err != nil {
		return nil, fmt.Errorf("carrier listen %q: %w", bind, err)
	}
	sc := &SharedCarrier{
		ln:     ln,
		auth:   auth,
		policy: policy,
		log:    log,
		hosts:  make(map[string]*sharedHost),
	}
	go sc.serve()
	return sc, nil
}

func (sc *SharedCarrier) Addr() string { return sc.ln.Addr().String() }

// HostLinked reports whether the host currently holds an authenticated link.
// The link is removed when its read deadline expires, so a dead TCP path stops
// counting as online within hostIdle.
func (sc *SharedCarrier) HostLinked(hostID string) bool {
	sc.mu.Lock()
	defer sc.mu.Unlock()
	return sc.hosts[hostID] != nil
}

func (sc *SharedCarrier) Port() int {
	if addr, ok := sc.ln.Addr().(*net.TCPAddr); ok {
		return addr.Port
	}
	return 0
}

func (sc *SharedCarrier) Stats() SharedCarrierStats {
	sc.mu.Lock()
	defer sc.mu.Unlock()
	return SharedCarrierStats{
		Addr:          sc.Addr(),
		Hosts:         len(sc.hosts),
		HostLinks:     sc.links,
		Viewers:       sc.viewers,
		Busy:          sc.busy,
		Rejected:      sc.rejected,
		Blocked:       sc.blocked,
		ToHostBytes:   sc.toHost,
		ToViewerBytes: sc.toViewer,
	}
}

func (sc *SharedCarrier) Close() {
	sc.closeOnce.Do(func() {
		_ = sc.ln.Close()
		sc.mu.Lock()
		sc.closed = true
		hosts := make([]*sharedHost, 0, len(sc.hosts))
		viewers := make([]*carrierViewer, 0, len(sc.hosts))
		for id, host := range sc.hosts {
			hosts = append(hosts, host)
			if host.viewer != nil {
				viewers = append(viewers, host.viewer)
				host.viewer = nil
			}
			delete(sc.hosts, id)
		}
		sc.mu.Unlock()
		for _, host := range hosts {
			host.conn.Close()
		}
		for _, viewer := range viewers {
			viewer.close()
		}
	})
}

func (sc *SharedCarrier) serve() {
	for {
		conn, err := sc.ln.Accept()
		if err != nil {
			if !errors.Is(err, net.ErrClosed) {
				sc.log.Warn("shared carrier accept", "error", err)
			}
			return
		}
		go sc.handle(conn)
	}
}

func (sc *SharedCarrier) handle(conn net.Conn) {
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
			sc.log.Debug("shared carrier upgrade refused",
				"remote", conn.RemoteAddr().String(), "error", err)
			conn.Close()
			return
		}
		body, err := ws.ReadMessage()
		if err != nil {
			conn.Close()
			return
		}
		var hello carrier.SharedViewerHello
		if json.Unmarshal(body, &hello) != nil {
			conn.Close()
			return
		}
		_ = conn.SetDeadline(time.Time{})
		sc.serveViewer(&carrierViewer{
			conn: conn,
			ws:   ws,
			peer: conn.RemoteAddr().String(),
		}, hello)
	case strings.HasPrefix(string(prefix), "CHAS"):
		line, err := carrier.ReadLine(br, 64)
		if err != nil {
			conn.Close()
			return
		}
		switch line + "\n" {
		case carrier.MagicHost:
			sc.serveHost(conn, br)
		case carrier.MagicViewer:
			helloLine, err := carrier.ReadLine(br, carrier.MaxHandshake)
			if err != nil {
				conn.Close()
				return
			}
			var hello carrier.SharedViewerHello
			if json.Unmarshal([]byte(helloLine), &hello) != nil {
				conn.Close()
				return
			}
			_ = conn.SetDeadline(time.Time{})
			sc.serveViewer(&carrierViewer{
				conn:   conn,
				send:   carrier.NewFrameWriter(conn),
				frames: carrier.NewFrameReader(br),
				peer:   conn.RemoteAddr().String(),
			}, hello)
		default:
			conn.Close()
		}
	default:
		conn.Close()
	}
}

func (sc *SharedCarrier) serveHost(conn net.Conn, br *bufio.Reader) {
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		conn.Close()
		return
	}
	var hello carrier.HostHello
	if err := json.Unmarshal([]byte(line), &hello); err != nil || hello.HostID == "" {
		sc.refuse(conn, "malformed host hello")
		return
	}
	if sc.auth == nil || !sc.auth.HostOnline(hello.HostID) {
		sc.refuse(conn, "host was not accepted")
		return
	}
	nonce, err := sc.auth.IssueChallenge(hello.HostID, identity.DomainCarrierHost)
	if err != nil {
		sc.refuse(conn, "host has no live registration")
		return
	}
	challenge, err := json.Marshal(carrier.HostChallenge{Nonce: nonce})
	if err != nil {
		sc.refuse(conn, "could not issue a challenge")
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
		sc.refuse(conn, "host did not answer the challenge")
		return
	}
	var proof carrier.HostAuth
	if err := json.Unmarshal([]byte(authText), &proof); err != nil {
		sc.refuse(conn, "malformed host proof")
		return
	}
	message := identity.CarrierHostMessage(hello.HostID, "", proof.Nonce)
	if proof.HostID != hello.HostID || proof.Nonce != nonce ||
		!sc.auth.CheckChallenge(hello.HostID, identity.DomainCarrierHost, proof.Nonce, string(message), proof.Signature) {
		sc.refuse(conn, "host proof was not accepted")
		return
	}
	if _, err := io.WriteString(conn, carrier.MagicOK); err != nil {
		conn.Close()
		return
	}
	_ = conn.SetDeadline(time.Time{})

	link := &sharedHost{carrier: sc, id: hello.HostID, conn: conn, send: carrier.NewFrameWriter(conn)}
	sc.mu.Lock()
	if sc.closed {
		sc.mu.Unlock()
		conn.Close()
		return
	}
	old := sc.hosts[hello.HostID]
	var oldViewer *carrierViewer
	if old != nil {
		oldViewer = old.viewer
		old.viewer = nil
		if oldViewer != nil && sc.viewers > 0 {
			sc.viewers--
		}
	}
	sc.hosts[hello.HostID] = link
	sc.links++
	sc.mu.Unlock()
	if old != nil {
		old.conn.Close()
	}
	if oldViewer != nil {
		oldViewer.close()
	}
	sc.log.Info("shared carrier host link", "host", hello.HostID)
	sc.hostLoop(link, br)
}

func (sc *SharedCarrier) hostLoop(link *sharedHost, br *bufio.Reader) {
	defer link.shutdown()
	frames := carrier.NewFrameReader(br)
	for {
		_ = link.conn.SetReadDeadline(time.Now().Add(hostIdle))
		payload, err := frames.ReadFrame()
		if err != nil {
			return
		}
		sc.mu.Lock()
		current := sc.hosts[link.id] == link
		var viewer *carrierViewer
		if current {
			viewer = link.viewer
		}
		sc.mu.Unlock()
		if !current {
			return
		}
		if len(payload) == 0 {
			// Keepalive from the host agent; answer so both sides can refresh
			// their deadlines.
			if err := link.sendFrame(nil); err != nil {
				return
			}
			continue
		}
		if viewer == nil {
			continue
		}
		if err := viewer.take(len(payload)); err != nil {
			viewer.close()
			sc.detachViewer(link, viewer)
			continue
		}
		if err := viewer.writeFrame(payload); err != nil {
			viewer.close()
			sc.detachViewer(link, viewer)
			continue
		}
		sc.mu.Lock()
		sc.toViewer += uint64(len(payload))
		sc.mu.Unlock()
	}
}

func (sc *SharedCarrier) serveViewer(v *carrierViewer, hello carrier.SharedViewerHello) {
	defer v.close()
	if hello.HostID == "" || sc.auth == nil || !sc.auth.VerifyCarrier(hello.HostID, hello.Token) {
		sc.mu.Lock()
		sc.rejected++
		sc.mu.Unlock()
		return
	}
	sc.mu.Lock()
	if sc.closed {
		sc.mu.Unlock()
		return
	}
	host := sc.hosts[hello.HostID]
	if host == nil {
		sc.mu.Unlock()
		return
	}
	if host.viewer != nil {
		sc.busy++
		sc.mu.Unlock()
		return
	}
	session, err := sc.policy.Open("shared:"+hello.HostID, peerIP(v.peer))
	if err != nil {
		sc.blocked++
		sc.mu.Unlock()
		return
	}
	v.mu.Lock()
	v.session = session
	v.mu.Unlock()
	host.viewer = v
	sc.viewers++
	sc.mu.Unlock()
	sc.log.Info("shared carrier viewer attached", "host", hello.HostID, "remote", v.peer)

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
		sc.mu.Lock()
		sc.toHost += uint64(len(payload))
		sc.mu.Unlock()
	}
	sc.detachViewer(host, v)
}

func (sc *SharedCarrier) detachViewer(host *sharedHost, v *carrierViewer) {
	sc.mu.Lock()
	if host.viewer == v {
		host.viewer = nil
		if sc.viewers > 0 {
			sc.viewers--
		}
	}
	sc.mu.Unlock()
}

func (h *sharedHost) sendFrame(payload []byte) error {
	_ = h.conn.SetWriteDeadline(time.Now().Add(carrierWrite))
	return h.send.WriteFrame(payload)
}

func (h *sharedHost) shutdown() {
	sc := h.carrier
	sc.mu.Lock()
	var viewer *carrierViewer
	if sc.hosts[h.id] == h {
		delete(sc.hosts, h.id)
		viewer = h.viewer
		h.viewer = nil
		if viewer != nil && sc.viewers > 0 {
			sc.viewers--
		}
	}
	sc.mu.Unlock()
	h.conn.Close()
	if viewer != nil {
		viewer.close()
	}
}

func (sc *SharedCarrier) refuse(conn net.Conn, reason string) {
	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, _ = io.WriteString(conn, carrier.MagicErrorPrefix+reason+"\n")
	conn.Close()
}
