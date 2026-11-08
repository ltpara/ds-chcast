package rendezvous

import (
	"crypto/rand"
	"encoding/hex"
	"net"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const controlIdle = 60 * time.Second

type hostControl struct {
	conn      net.Conn
	send      *carrier.FrameWriter
	mu        sync.Mutex
	publicKey string
	nonce     string
}

func (c *hostControl) write(payload []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	return c.send.WriteFrame(payload)
}

// HostControlLinked is independent of relay liveness: the host speaks to the
// rendezvous server itself, as in RustDesk's TCP rendezvous mediator.
func (s *Server) HostControlLinked(id string) bool {
	if s.tcpPunch == nil {
		return false
	}
	host, ok := s.reg.Lookup(id)
	if !ok {
		return false
	}
	p := s.tcpPunch
	p.mu.Lock()
	link := p.controls[id]
	p.mu.Unlock()
	return link != nil && link.publicKey == host.PublicKey
}

func (p *tcpPuncher) serveHostControl(conn net.Conn, hello wire.Message) {
	if p.srv.identity == nil || len(hello.HostID) > 128 || len(hello.Nonce) < 8 || len(hello.Nonce) > 64 {
		return
	}
	var entropy [32]byte
	if _, err := rand.Read(entropy[:]); err != nil {
		return
	}
	nonce := hex.EncodeToString(entropy[:])
	challenge := wire.Message{
		Type: wire.TypeRendezvousControl, HostID: hello.HostID,
		TID: hello.Nonce, Nonce: nonce,
		Signature: p.srv.identity.SignRendezvousChallenge(hello.HostID, hello.Nonce, nonce),
	}
	link := &hostControl{conn: conn, send: carrier.NewFrameWriter(conn), nonce: hello.Nonce}
	if link.write(wire.Encode(challenge)) != nil {
		return
	}
	proof, err := readAnnounce(conn)
	if err != nil {
		return
	}
	host, exists := p.srv.reg.Lookup(hello.HostID)
	pub, keyErr := identity.ParsePublicKey(host.PublicKey)
	if !exists || keyErr != nil || proof.Type != wire.TypeRendezvousProof ||
		proof.HostID != hello.HostID || proof.Nonce != nonce || proof.TID != hello.Nonce ||
		!identity.Verify(pub, identity.RendezvousControlMessage("host", hello.HostID, hello.Nonce, nonce), proof.Signature) {
		_ = link.write(wire.Encode(wire.Message{Type: wire.TypeRendezvousControl, Error: "punch authentication failed"}))
		return
	}
	link.publicKey = host.PublicKey
	p.mu.Lock()
	select {
	case <-p.srv.done:
		p.mu.Unlock()
		return
	default:
	}
	if p.controls == nil {
		p.controls = make(map[string]*hostControl)
	}
	old := p.controls[hello.HostID]
	if old == nil && len(p.controls) >= tcpPunchMaxSessions {
		p.mu.Unlock()
		return
	}
	p.controls[hello.HostID] = link
	p.mu.Unlock()
	if old != nil {
		old.conn.Close()
	}
	defer func() {
		p.mu.Lock()
		if p.controls[hello.HostID] == link {
			delete(p.controls, hello.HostID)
		}
		p.mu.Unlock()
	}()
	if link.write(wire.Encode(wire.Message{Type: wire.TypeRendezvousControl, OK: true, Nonce: nonce})) != nil {
		return
	}
	reader := carrier.NewFrameReader(conn)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(controlIdle))
		payload, err := reader.ReadFrame()
		if err != nil || len(payload) != 0 {
			return
		}
		current, ok := p.srv.reg.Lookup(hello.HostID)
		if !ok || current.PublicKey != link.publicKey {
			return
		}
		if link.write(nil) != nil {
			return
		}
	}
}

func (p *tcpPuncher) triggerControl(msg wire.Message) bool {
	p.mu.Lock()
	link := p.controls[msg.HostID]
	p.mu.Unlock()
	host, ok := p.srv.reg.Lookup(msg.HostID)
	if link == nil || !ok || link.publicKey != host.PublicKey {
		return false
	}
	msg.Timestamp = time.Now().Unix()
	msg.Signature = p.srv.identity.SignRendezvousTrigger(msg.HostID, link.nonce, msg.Nonce, msg.Candidate, msg.NATType, msg.IsLocal, msg.Timestamp)
	if err := link.write(wire.Encode(msg)); err != nil {
		link.conn.Close()
		return false
	}
	return true
}
