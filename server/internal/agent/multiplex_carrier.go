package agent

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
)

type localMuxPeer struct {
	ctx      context.Context
	cancel   context.CancelFunc
	outbound chan []byte
}

func (h *HostAgent) runMuxCarrier(conn net.Conn, frames *carrier.FrameReader, send *carrier.FrameWriter, address string) error {
	ctx, cancel := context.WithCancel(h.ctx)
	defer cancel()
	stopClose := context.AfterFunc(ctx, func() { conn.Close() })
	defer func() { cancel(); stopClose(); conn.Close() }()
	var mu, sendMu sync.Mutex
	peers := make(map[uint64]*localMuxPeer)
	write := func(payload []byte) error {
		sendMu.Lock()
		defer sendMu.Unlock()
		if ctx.Err() != nil {
			return ctx.Err()
		}
		_ = conn.SetWriteDeadline(time.Now().Add(carrierWriteTimeout))
		err := send.WriteFrame(payload)
		if err != nil {
			cancel()
		}
		return err
	}
	serve := func(id uint64, route *localMuxPeer) {
		defer route.cancel()
		defer func() {
			mu.Lock()
			current := peers[id] == route
			if current {
				delete(peers, id)
			}
			mu.Unlock()
			if current {
				_ = write(carrier.EncodeMux(id, nil))
			}
		}()
		dialer := net.Dialer{Timeout: carrierDial}
		peer, err := dialer.DialContext(route.ctx, "tcp", h.carrier.String())
		if err != nil {
			return
		}
		defer peer.Close()
		stopPeer := context.AfterFunc(route.ctx, func() { peer.Close() })
		defer stopPeer()
		_ = peer.SetWriteDeadline(time.Now().Add(carrierWriteTimeout))
		if _, err := io.WriteString(peer, carrier.MagicRaw); err != nil {
			return
		}
		readDone := make(chan struct{})
		go func() {
			defer close(readDone)
			defer route.cancel()
			reader := carrier.NewFrameReader(bufio.NewReader(peer))
			for {
				_ = peer.SetReadDeadline(time.Now().Add(carrierViewerTimeout))
				payload, err := reader.ReadFrame()
				if err != nil {
					return
				}
				if route.ctx.Err() != nil {
					return
				}
				if len(payload) == 0 {
					continue
				}
				encoded := carrier.EncodeMux(id, payload)
				if len(encoded) == 0 || write(encoded) != nil {
					return
				}
			}
		}()
		defer func() { route.cancel(); peer.Close(); <-readDone }()
		writer := carrier.NewFrameWriter(peer)
		for {
			select {
			case <-route.ctx.Done():
				return
			case payload := <-route.outbound:
				_ = peer.SetWriteDeadline(time.Now().Add(carrierWriteTimeout))
				if writer.WriteFrame(payload) != nil {
					return
				}
			}
		}
	}
	go func() {
		ticker := time.NewTicker(carrierKeepalive)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if !h.carrierTargetActive(address) || write(nil) != nil {
					cancel()
					return
				}
			}
		}
	}()
	for {
		_ = conn.SetReadDeadline(time.Now().Add(carrierReadIdle))
		frame, err := frames.ReadFrame()
		if err != nil {
			return err
		}
		if len(frame) == 0 {
			continue
		}
		id, payload, ok := carrier.DecodeMux(frame)
		if !ok {
			return fmt.Errorf("malformed multiplex frame")
		}
		mu.Lock()
		route := peers[id]
		if len(payload) == 0 {
			if route != nil {
				delete(peers, id)
				route.cancel()
			}
			mu.Unlock()
			continue
		}
		if route == nil {
			if len(peers) >= carrier.MaxMuxPeers {
				mu.Unlock()
				return fmt.Errorf("too many multiplex peers")
			}
			peerCtx, peerCancel := context.WithCancel(ctx)
			route = &localMuxPeer{ctx: peerCtx, cancel: peerCancel, outbound: make(chan []byte, 32)}
			peers[id] = route
			go serve(id, route)
		}
		mu.Unlock()
		if route.ctx.Err() != nil {
			continue
		}
		// Dialing and writing to one local QUIC carrier never stalls the shared
		// reader. A blocked route gets a bounded queue, then closes independently.
		select {
		case route.outbound <- append([]byte(nil), payload...):
		default:
			route.cancel()
		}
	}
}

const carrierWriteTimeout = 30 * time.Second
const carrierViewerTimeout = 180 * time.Second
