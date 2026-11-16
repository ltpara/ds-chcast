package agent

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

// Separate from the admission policy marker and acknowledgement. The native
// host must write a strategy ack after applying settings, not after reading
// this file. Cached overlays are retained on network or authentication errors.
type clientStrategyState struct {
	Version         int               `json:"version"`
	Scope           string            `json:"scope"`
	ServerURL       string            `json:"server_url"`
	ServerPublicKey string            `json:"server_public_key"`
	HostPublicKey   string            `json:"host_public_key"`
	Snapshot        strategy.Snapshot `json:"snapshot"`
}

type clientStrategyAck struct {
	Scope   string           `json:"scope"`
	Receipt strategy.Receipt `json:"receipt"`
}

func (h *HostAgent) clientStrategyScope() string {
	// A key rotation or server change must not reuse another authority's cache
	// or local acknowledgements even if it assigns the same revision number.
	data, _ := json.Marshal([]string{strings.TrimRight(h.cfg.ServerURL, "/"), h.cfg.ServerPublicKey, h.cfg.ID, h.publicKey})
	digest := sha256.Sum256(data)
	return hex.EncodeToString(digest[:])
}

func saveClientStrategyState(path string, state clientStrategyState) error {
	data, err := json.Marshal(state)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".strategy-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	if err := f.Chmod(0600); err != nil {
		return err
	}
	if _, err := f.Write(append(data, '\n')); err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), path)
}

func readStrategyFile(path string, dst any) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	if info.Size() > 64<<10 {
		return errors.New("strategy state file exceeds limit")
	}
	decoder := json.NewDecoder(io.LimitReader(f, (64<<10)+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(dst); err != nil {
		return err
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return errors.New("strategy state has trailing JSON")
	}
	return nil
}

// syncClientStrategy does not apply host settings itself. It transports the
// desired overlay and forwards only a native consumer's exact acknowledgement.
func (h *HostAgent) syncClientStrategy(ctx context.Context) error {
	path := h.cfg.PolicyState + ".client-strategy"
	scope := h.clientStrategyScope()
	// Use a captured endpoint for both fetch and receipt, even if relay
	// failover changes h.api.base while this operation is in flight.
	client := &apiClient{base: h.api.endpoint(), serverKey: h.api.serverKey, http: h.api.http}
	timestamp := time.Now().Unix()
	query := url.Values{"timestamp": {strconv.FormatInt(timestamp, 10)}, "signature": {identity.Sign(h.priv, identity.HostClientStrategyFetchMessage(h.cfg.ID, timestamp))}}
	endpoint := "/v1/hosts/" + url.PathEscape(h.cfg.ID) + "/client-strategy"
	var snapshot strategy.Snapshot
	if err := client.do(ctx, http.MethodGet, endpoint+"?"+query.Encode(), nil, &snapshot); err != nil {
		return err
	}
	if err := snapshot.Validate(h.cfg.ID, strategy.Host); err != nil {
		return err
	}
	var previous clientStrategyState
	err := readStrategyFile(path, &previous)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("read cached client strategy: %w", err)
	}
	unchanged := false
	if err == nil && previous.Scope == scope {
		if previous.Version != 1 {
			return errors.New("unsupported client strategy cache version")
		}
		if err := previous.Snapshot.Validate(h.cfg.ID, strategy.Host); err != nil {
			return err
		}
		if snapshot.Revision < previous.Snapshot.Revision {
			return errors.New("server returned an older client strategy")
		}
		if snapshot.Revision == previous.Snapshot.Revision {
			if snapshot.Digest != previous.Snapshot.Digest || snapshot.PolicyID != previous.Snapshot.PolicyID {
				return errors.New("client strategy changed without a new revision")
			}
			unchanged = true
		}
	}
	if !unchanged {
		state := clientStrategyState{Version: 1, Scope: scope, ServerURL: strings.TrimRight(h.cfg.ServerURL, "/"), ServerPublicKey: h.cfg.ServerPublicKey, HostPublicKey: h.publicKey, Snapshot: snapshot}
		if err := saveClientStrategyState(path, state); err != nil {
			return err
		}
	}
	var ack clientStrategyAck
	if err := readStrategyFile(path+".ack", &ack); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return err
	}
	if ack.Scope != scope || ack.Receipt.Revision != snapshot.Revision || ack.Receipt.Digest != snapshot.Digest {
		return nil
	}
	if err := ack.Receipt.Validate(snapshot.Settings); err != nil {
		return err
	}
	encoded, err := strategy.CanonicalReceipt(ack.Receipt)
	if err != nil {
		return err
	}
	timestamp = time.Now().Unix()
	body, err := json.Marshal(struct {
		Receipt   strategy.Receipt `json:"receipt"`
		Timestamp int64            `json:"timestamp"`
		Signature string           `json:"signature"`
	}{ack.Receipt, timestamp, identity.Sign(h.priv, identity.HostClientStrategyReceiptMessage(h.cfg.ID, encoded, timestamp))})
	if err != nil {
		return err
	}
	return client.do(ctx, http.MethodPost, endpoint+"/receipt", body, nil)
}

func (h *HostAgent) clientStrategyLoop() {
	timer := time.NewTicker(15 * time.Second)
	defer timer.Stop()
	lastError := ""
	for {
		ctx, cancel := context.WithTimeout(h.ctx, 10*time.Second)
		err := h.syncClientStrategy(ctx)
		cancel()
		if h.ctx.Err() != nil {
			return
		}
		if err != nil {
			message := err.Error()
			if message != lastError {
				h.log.Warn("client strategy synchronization failed; keeping cached settings", "error", err)
				lastError = message
			}
		} else {
			lastError = ""
		}
		select {
		case <-h.ctx.Done():
			return
		case <-timer.C:
		}
	}
}
