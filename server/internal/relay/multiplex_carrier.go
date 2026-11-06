package relay

import (
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
)

var nextMuxCarrier atomic.Uint64

type muxViewer struct {
	*carrierViewer
	outbound chan []byte
}

type muxCarrier struct {
	mu      sync.Mutex
	viewers map[uint64]*muxViewer
	next    uint64
	closed  bool
	policy  *Policy
	hostID  string
	key     string
	family  string
	port    int
	send    func([]byte) error
	count   func(bool, int)
}

// newMuxCarrier creates the per-host demultiplexer used when the agent
// advertises the multiplex capability during the carrier handshake.
func newMuxCarrier(policy *Policy, hostID, key, family string, port int, send func([]byte) error, count func(bool, int)) *muxCarrier {
	// Replacing a host link can overlap its predecessor's shutdown. Keep their
	// bandwidth/audit sessions distinct even though wire IDs restart at one.
	key = fmt.Sprintf("%s/%d", key, nextMuxCarrier.Add(1))
	return &muxCarrier{viewers: make(map[uint64]*muxViewer), policy: policy, hostID: hostID,
		key: key, family: family, port: port, send: send, count: count}
}

func (m *muxCarrier) close() {
	m.mu.Lock()
	m.closed = true
	for _, v := range m.viewers {
		v.close()
	}
	m.viewers = make(map[uint64]*muxViewer)
	m.mu.Unlock()
}

func (m *muxCarrier) addresses() []string {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]string, 0, len(m.viewers))
	for _, v := range m.viewers {
		out = append(out, v.peer)
	}
	return out
}

func (m *muxCarrier) receive(frame []byte) {
	id, payload, ok := carrier.DecodeMux(frame)
	if !ok {
		return
	}
	m.mu.Lock()
	v := m.viewers[id]
	m.mu.Unlock()
	if v == nil {
		return
	}
	if len(payload) == 0 {
		v.close()
		return
	}
	// A slow viewer must not block the shared host reader or other viewers.
	select {
	case v.outbound <- append([]byte(nil), payload...):
	default:
		v.close()
	}
}

func (m *muxCarrier) serve(v *carrierViewer) {
	defer v.close()
	m.mu.Lock()
	if m.closed || len(m.viewers) >= carrier.MaxMuxPeers {
		m.mu.Unlock()
		return
	}
	m.next++
	id := m.next
	key := fmt.Sprintf("%s/%d", m.key, id)
	attrs := []tracing.Attribute{
		tracing.String("chast.host_id", m.hostID),
		tracing.String("relay.transport", "carrier"),
		tracing.Int("relay.port", int64(m.port)),
	}
	if m.family != "" {
		attrs = append(attrs, tracing.String("relay.family", m.family))
	}
	session, err := m.policy.OpenTraced(sessionTraceID(m.hostID, key), key, peerIP(v.peer), attrs...)
	if err != nil {
		m.mu.Unlock()
		return
	}
	v.mu.Lock()
	v.session = session
	v.mu.Unlock()
	outbound := make(chan []byte, 32)
	m.viewers[id] = &muxViewer{carrierViewer: v, outbound: outbound}
	m.mu.Unlock()
	done := make(chan struct{})
	defer close(done)
	go func() {
		for {
			select {
			case <-done:
				return
			case payload := <-outbound:
				if v.take(len(payload)) != nil || v.writeFrame(payload) != nil {
					v.close()
					return
				}
				if m.count != nil {
					m.count(false, len(payload))
				}
			}
		}
	}()
	defer func() {
		m.mu.Lock()
		delete(m.viewers, id)
		m.mu.Unlock()
		_ = m.send(carrier.EncodeMux(id, nil))
	}()
	for {
		_ = v.conn.SetReadDeadline(time.Now().Add(carrierViewerIdle))
		payload, err := v.readFrame()
		if err != nil {
			return
		}
		if len(payload) == 0 {
			if v.writeFrame(nil) != nil {
				return
			}
			continue
		}
		if v.take(len(payload)) != nil {
			return
		}
		frame := carrier.EncodeMux(id, payload)
		if len(frame) == 0 {
			return
		}
		if m.send(frame) != nil {
			return
		}
		if m.count != nil {
			m.count(true, len(payload))
		}
	}
}
