package relay

import (
	"fmt"
	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"net"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/tracing"
)

type udpPeer struct {
	id      uint64
	address *net.UDPAddr
	seen    time.Time
	session *Session
	span    *tracing.Span
}

type udpAuthorization struct {
	seen   time.Time
	parent tracing.TraceContext
}

func (p *port) clearMuxLocked() {
	for _, peer := range p.muxPeers {
		p.removeMuxLocked(peer)
	}
	p.muxPeers = nil
	p.muxIDs = nil
	p.muxAuth = nil
}
func (p *port) removeMuxLocked(peer *udpPeer) {
	delete(p.muxPeers, peer.address.String())
	delete(p.muxIDs, peer.id)
	peer.session.Close()
	p.set.manager.tracer.EndSpan(peer.span,
		tracing.Int("relay.port", p.number),
		tracing.String("relay.family", p.family),
		tracing.String("relay.transport", "udp"),
		tracing.String("relay.viewer_ip", peer.address.IP.String()),
		tracing.String("relay.session_id", fmt.Sprint(peer.id)))
}
func (p *port) handleMux(conn *net.UDPConn, data []byte, src *net.UDPAddr, now time.Time, fromHost bool) {
	for _, peer := range p.muxPeers {
		if now.Sub(peer.seen) > viewerIdle {
			p.removeMuxLocked(peer)
		}
	}
	if fromHost {
		p.hostSeen = now
		id, payload, ok := carrier.DecodeMux(data)
		if !ok {
			p.dropped++
			return
		}
		peer := p.muxIDs[id]
		if peer == nil {
			p.dropped++
			return
		}
		allowed, err := peer.session.Drop(len(payload))
		if err != nil || !allowed {
			if err != nil {
				p.removeMuxLocked(peer)
			}
			p.dropped++
			return
		}
		if _, err := conn.WriteToUDP(payload, peer.address); err != nil {
			p.dropped++
			return
		}
		p.toViewer += uint64(len(payload))
		return
	}
	if p.hostAddr == nil || now.Sub(p.hostSeen) > hostIdle {
		p.dropped++
		return
	}
	peer := p.muxPeers[src.String()]
	if peer == nil {
		if p.set.manager.requireViewerToken && now.Sub(p.muxAuth[src.String()].seen) > viewerIdle {
			p.dropped++
			return
		}
		if len(p.muxPeers) >= carrier.MaxMuxPeers {
			p.busy++
			return
		}
		p.nextMuxID++
		session, err := p.set.manager.policy.Open(fmt.Sprintf("udp:%d/%s/%d", p.number, p.family, p.nextMuxID), src.IP.String())
		if err != nil {
			p.blocked++
			p.dropped++
			return
		}
		peer = &udpPeer{id: p.nextMuxID, address: cloneAddr(src), session: session}
		peer.span = p.set.manager.tracer.StartSpan("relay.udp_session", p.muxAuth[src.String()].parent)
		if p.muxPeers == nil {
			p.muxPeers = make(map[string]*udpPeer)
			p.muxIDs = make(map[uint64]*udpPeer)
		}
		p.muxPeers[src.String()] = peer
		p.muxIDs[peer.id] = peer
	}
	peer.seen = now
	allowed, err := peer.session.Drop(len(data))
	if err != nil || !allowed {
		if err != nil {
			p.removeMuxLocked(peer)
		}
		p.dropped++
		return
	}
	encoded := carrier.EncodeMux(peer.id, data)
	if len(encoded) == 0 {
		p.dropped++
		return
	}
	if _, err := conn.WriteToUDP(encoded, p.hostAddr); err != nil {
		p.dropped++
		return
	}
	p.toHost += uint64(len(data))
}
