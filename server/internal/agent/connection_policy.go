package agent

import (
	"context"
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

func (v *ViewerAgent) directContext() context.Context {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.directCtx != nil {
		return v.directCtx
	}
	return v.ctx
}

// directConnectTimeout follows client.rs::connect, including its branch order.
// In particular own symmetric NAT only selects MIN when the peer is asymmetric.
func directConnectTimeout(punch time.Duration, peer, own string, local, relay, failed bool) time.Duration {
	if local || peer == NATSymmetric {
		return time.Second
	}
	if !relay {
		return tcpPunchWindow
	}
	wait := time.Duration(0)
	if peer == NATAsymmetric {
		if own == NATAsymmetric {
			wait = tcpPunchWindow
			if failed {
				wait = punch * 6
			}
		} else if own == NATSymmetric {
			wait = time.Second
		}
	}
	if wait == 0 {
		factor := time.Duration(6)
		if failed {
			factor = 3
		}
		wait = punch * factor
	}
	if wait < time.Second {
		wait = time.Second
	}
	return wait
}

func (v *ViewerAgent) connectWait(punch time.Duration, local bool) time.Duration {
	// RustDesk gives its background NAT test up to 100ms when an asymmetric
	// peer would otherwise be paired with an unknown local NAT type.
	deadline := time.Now().Add(100 * time.Millisecond)
	for !local && time.Now().Before(deadline) {
		v.mu.Lock()
		unknown := v.natType == NATAsymmetric && v.ownNAT == ""
		v.mu.Unlock()
		if !unknown {
			break
		}
		select {
		case <-v.directContext().Done():
			break
		case <-time.After(5 * time.Millisecond):
			continue
		}
		break
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	relay := v.cfg.Mode == ModeAuto && (v.relay != nil ||
		(v.carrierCredentialReady() && len(v.carrierCandidates()) > 0))
	return directConnectTimeout(punch, v.natType, v.ownNAT, local, relay, v.directFailed)
}

func (v *ViewerAgent) directAttemptDone() {
	v.mu.Lock()
	if v.pendingDirect > 0 {
		v.pendingDirect--
	}
	done := v.pendingDirect == 0 && !v.directOK.Load()
	v.mu.Unlock()
	if done {
		v.fallBackToRelay("all direct connection attempts failed")
	}
}

// Call only after detaching cancellation from a winning TCP socket.
func (v *ViewerAgent) stopDirectAttempts() {
	v.mu.Lock()
	cancel := v.directCancel
	v.mu.Unlock()
	if cancel != nil {
		cancel()
	}
}

func DefaultDirectHistoryDir() string {
	dir, err := os.UserConfigDir()
	if err != nil {
		return ""
	}
	return filepath.Join(dir, "ds-chcast", "direct-history")
}

func (v *ViewerAgent) initDirectHistory() {
	if v.cfg.DirectHistoryDir == "" {
		return
	}
	// Scope history to the server, peer ID and peer identity, never raw paths.
	key := sha256.Sum256([]byte(v.cfg.ServerURL + "\x00" + v.cfg.HostID + "\x00" + string(v.hostPub)))
	v.historyPath = filepath.Join(v.cfg.DirectHistoryDir, fmt.Sprintf("%x", key))
	data, err := os.ReadFile(v.historyPath)
	v.directFailed = err == nil && string(data) == "1\n"
}

func (v *ViewerAgent) recordDirectResult(failed bool) {
	if v.historyPath == "" {
		return
	}
	v.historyMu.Lock()
	defer v.historyMu.Unlock()
	if err := os.MkdirAll(filepath.Dir(v.historyPath), 0700); err != nil {
		v.log.Debug("cannot save direct connection history", "error", err)
		return
	}
	value := "0\n"
	if failed {
		value = "1\n"
	}
	// Each peer has a separate file, so simultaneous peers cannot lose updates.
	if err := os.WriteFile(v.historyPath, []byte(value), 0600); err != nil {
		v.log.Debug("cannot save direct connection history", "error", err)
	}
}

func (v *ViewerAgent) udpIsLocal() bool {
	v.mu.Lock()
	defer v.mu.Unlock()
	return v.udpLocal
}
