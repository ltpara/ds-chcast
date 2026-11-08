package agent

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// rendezvousControlLoop carries server-originated instructions independently
// of the relay data plane, including when UDP registration cannot get through.
func (h *HostAgent) rendezvousControlLoop() {
	for h.ctx.Err() == nil {
		endpoint := h.getTCPPunch()
		if endpoint != nil {
			if err := h.runRendezvousControl(endpoint); err != nil && h.ctx.Err() == nil {
				h.log.Debug("rendezvous control disconnected", "error", err)
			}
		}
		if !h.sleep(time.Second) {
			return
		}
	}
}

func (h *HostAgent) runRendezvousControl(endpoint *net.TCPAddr) error {
	dialCtx, cancel := context.WithTimeout(h.ctx, tcpPunchDialWait)
	conn, err := h.cfg.Proxy.DialContext(dialCtx, "tcp", endpoint.String())
	cancel()
	if err != nil {
		return err
	}
	defer conn.Close()
	stopClose := context.AfterFunc(h.ctx, func() { conn.Close() })
	defer stopClose()
	_ = conn.SetDeadline(time.Now().Add(tcpLinkHandshake))
	clientNonce := newNonce()
	if err := writeHandshake(conn, wire.Message{Type: wire.TypeRendezvousControl, HostID: h.cfg.ID, Nonce: clientNonce}); err != nil {
		return err
	}
	br := bufio.NewReader(conn)
	challenge, err := readHandshake(br)
	if err != nil {
		return err
	}
	h.mu.Lock()
	serverKey := h.controlServerKey
	h.mu.Unlock()
	if h.cfg.ServerPublicKey != "" {
		serverKey = h.cfg.ServerPublicKey
	}
	pub, err := identity.ParsePublicKey(serverKey)
	if err != nil {
		return fmt.Errorf("rendezvous control requires the registered server identity: %w", err)
	}
	if challenge.Type != wire.TypeRendezvousControl || challenge.HostID != h.cfg.ID ||
		challenge.TID != clientNonce || len(challenge.Nonce) != 64 ||
		!identity.Verify(pub, identity.RendezvousControlMessage("server", h.cfg.ID, clientNonce, challenge.Nonce), challenge.Signature) {
		return fmt.Errorf("rendezvous control challenge did not verify")
	}
	proof := wire.Message{
		Type: wire.TypeRendezvousProof, HostID: h.cfg.ID, TID: clientNonce, Nonce: challenge.Nonce,
		Signature: identity.Sign(h.priv, identity.RendezvousControlMessage("host", h.cfg.ID, clientNonce, challenge.Nonce)),
	}
	if err := writeHandshake(conn, proof); err != nil {
		return err
	}
	ack, err := readHandshake(br)
	if err != nil {
		return err
	}
	if ack.Type != wire.TypeRendezvousControl || !ack.OK || ack.Nonce != challenge.Nonce {
		return fmt.Errorf("rendezvous control authentication refused")
	}
	_ = conn.SetDeadline(time.Time{})
	h.mu.Lock()
	h.controlConn = conn
	h.mu.Unlock()
	defer func() {
		h.mu.Lock()
		if h.controlConn == conn {
			h.controlConn = nil
		}
		h.mu.Unlock()
	}()
	h.controlOnce.Do(func() { close(h.controlReady) })
	stop := make(chan struct{})
	defer close(stop)
	send := carrier.NewFrameWriter(conn)
	go func() {
		ticker := time.NewTicker(20 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-stop:
				return
			case <-h.ctx.Done():
				return
			case <-ticker.C:
				current := h.getTCPPunch()
				if current == nil || current.String() != endpoint.String() {
					conn.Close()
					return
				}
				_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
				if send.WriteFrame(nil) != nil {
					conn.Close()
					return
				}
			}
		}
	}()
	reader := carrier.NewFrameReader(br)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(60 * time.Second))
		payload, err := reader.ReadFrame()
		if err != nil {
			return err
		}
		if len(payload) == 0 {
			continue
		}
		msg, err := wire.Decode(payload)
		if err != nil {
			return err
		}
		age := time.Now().Unix() - msg.Timestamp
		if msg.Type != wire.TypeTCPPunch || msg.HostID != h.cfg.ID || age < -60 || age > 60 ||
			!identity.Verify(pub, identity.RendezvousTriggerMessage(h.cfg.ID, clientNonce, msg.Nonce, msg.Candidate, msg.NATType, msg.IsLocal, msg.Timestamp), msg.Signature) {
			return fmt.Errorf("rendezvous control trigger did not verify")
		}
		h.startTCPPunch(msg.Nonce, msg.Candidate, msg.NATType, msg.IsLocal)
	}
}
