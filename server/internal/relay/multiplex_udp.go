package relay

import (
	"fmt"
	"net"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

type udpPeer struct {
	id      uint64
	address *net.UDPAddr
	seen    time.Time
	session *Session
}

type udpAuthorization struct {
	seen time.Time
}

func (p *port) clearMuxLocked() {
	for _, peer := range p.muxPeers {
		p.removeMuxLocked(peer)
	}
	p.muxPeers = nil
	p.muxIDs = nil
	p.muxAuth = nil
	p.nextMuxID = 0
}

func (p *port) removeMuxLocked(peer *udpPeer) {
	delete(p.muxPeers, peer.address.String())
	delete(p.muxIDs, peer.id)
	peer.session.Close()
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
	if p.set.manager.requireViewerToken {
		auth, ok := p.muxAuth[src.String()]
		if !ok || now.Sub(auth.seen) > viewerIdle {
			p.dropped++
			return
		}
	}
	if wire.IsControl(data) {
		// The relay address is trusted by the host agent, so a viewer must not
		// be able to inject control datagrams through its multiplex route.
		p.dropped++
		return
	}
	peer := p.muxPeers[src.String()]
	if peer == nil {
		if len(p.muxPeers) >= carrier.MaxMuxPeers {
			p.busy++
			return
		}
		p.nextMuxID++
		key := fmt.Sprintf("udp:%d/%s/%d", p.number, p.family, p.nextMuxID)
		session, err := p.set.manager.policy.OpenTraced(sessionTraceID(p.hostID, key), key, src.IP.String(),
			tracing.String("chast.host_id", p.hostID),
			tracing.String("relay.transport", "udp"),
			tracing.String("relay.family", p.family),
			tracing.Int("relay.port", int64(p.number)))
		if err != nil {
			p.blocked++
			p.dropped++
			return
		}
		peer = &udpPeer{id: p.nextMuxID, address: cloneAddr(src), session: session}
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
