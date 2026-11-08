package agent

import (
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// Leave room before MsQuic's loss-disconnect deadline as well as its 30s
// idle timeout. Relay setup still cannot guarantee migration on every network.
const (
	directPathIdle      = 8 * time.Second
	directPathKeepalive = 2 * time.Second
)

func (v *ViewerAgent) udpDirectKeepalive() {
	ticker := time.NewTicker(directPathKeepalive)
	defer ticker.Stop()
	for {
		select {
		case <-v.ctx.Done():
			return
		case now := <-ticker.C:
			if !v.checkUDPDirect(now) {
				return
			}
		}
	}
}

// Only a response from the selected endpoint refreshes liveness. Sending a
// packet, rendezvous replies and old punch acknowledgements prove nothing.
func (v *ViewerAgent) checkUDPDirect(now time.Time) bool {
	v.mu.Lock()
	if v.ctx.Err() != nil || v.directLost || v.fellBack || v.carrierMode ||
		v.tcpActive.Load() || !v.directOK.Load() {
		v.mu.Unlock()
		return false
	}
	if now.Sub(v.lastDirectReceive) >= directPathIdle {
		v.directLost = true
		v.directOK.Store(false)
		v.latched = false
		v.remote = nil
		v.directKeepaliveTID = ""
		v.mu.Unlock()
		v.log.Warn("UDP direct path was lost")
		v.fallBackToRelay("UDP direct path was lost")
		v.reportPath()
		return false
	}
	peer := cloneAddr(v.direct)
	tid := newNonce()
	v.directKeepaliveTID = tid
	v.mu.Unlock()
	if peer != nil && v.out != nil {
		_, _ = v.out.WriteToUDP(wire.Encode(wire.Message{
			Type: wire.TypePunchProbe, Nonce: v.nonce, TID: tid,
		}), peer)
	}
	return true
}
