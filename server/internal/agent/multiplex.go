package agent

import (
	"errors"
	"net"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
)

// Each remote route gets its own local socket, so host replies cannot be
// redirected by the most recently received viewer packet.
type localRoutes struct {
	mu     sync.Mutex
	peers  map[string]*net.UDPConn
	closed bool
}

func (r *localRoutes) forward(key string, target *net.UDPAddr, data []byte, reply func([]byte) error) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return net.ErrClosed
	}
	if r.peers == nil {
		r.peers = make(map[string]*net.UDPConn)
	}
	conn := r.peers[key]
	if conn == nil {
		if len(r.peers) >= carrier.MaxMuxPeers {
			return errors.New("too many remote routes")
		}
		var err error
		conn, err = net.DialUDP("udp", nil, target)
		if err != nil {
			return err
		}
		r.peers[key] = conn
		go func() {
			defer conn.Close()
			defer func() {
				r.mu.Lock()
				if r.peers[key] == conn {
					delete(r.peers, key)
				}
				r.mu.Unlock()
			}()
			buffer := make([]byte, carrier.MaxPayload)
			for {
				_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
				n, err := conn.Read(buffer)
				if err != nil {
					return
				}
				if err := reply(buffer[:n]); err != nil {
					return
				}
			}
		}()
	}
	_ = conn.SetReadDeadline(time.Now().Add(90 * time.Second))
	_, err := conn.Write(data)
	return err
}

func (r *localRoutes) close() {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.closed = true
	for _, conn := range r.peers {
		conn.Close()
	}
}
