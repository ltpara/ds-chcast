package agent

import (
	"context"
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
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
)

// The native host owns the runtime identity and the viewer connection ids. It
// publishes a complete bounded snapshot into one file; this agent only reads
// that file, signs the snapshot and reports it. The reverse direction is
// separate: the agent persists every polled disconnect command into another
// file before the native side may act on it, and the native side records the
// resulting receipt in its own snapshot file. One writer per file keeps a
// crashed or restarted peer from silently overwriting the other direction.
const liveStateVersion = 1

type nativeLiveState struct {
	Version     int                   `json:"version"`
	RuntimeID   string                `json:"runtime_id"`
	Sequence    uint64                `json:"sequence"`
	Terminal    bool                  `json:"terminal"`
	Connections []liveconn.Connection `json:"connections"`
	Receipts    []liveconn.Receipt    `json:"receipts"`
}

// liveCommandState is the agent's durable inbox for commands handed to the
// native side, plus what the server already accepted so a restart does not
// replay a settled receipt.
type liveCommandState struct {
	Version   int                  `json:"version"`
	RuntimeID string               `json:"runtime_id"`
	Commands  []liveconn.Command   `json:"commands"`
	Acked     map[string]uint64    `json:"acked_receipts,omitempty"`
}

func (h *HostAgent) liveStatePath() string {
	if h.cfg.LiveState != "" {
		return h.cfg.LiveState
	}
	return ""
}

func (h *HostAgent) liveCommandsPath() string {
	path := h.liveStatePath()
	if path == "" {
		return ""
	}
	return path + ".commands"
}

func readLiveFile(path string, dst any) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return err
	}
	if info.Size() > 256<<10 {
		return errors.New("live connection state exceeds its limit")
	}
	decoder := json.NewDecoder(io.LimitReader(f, 256<<10+1))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(dst); err != nil {
		return err
	}
	// Exactly one JSON document: anything after the first value, including a
	// second object or junk, must be rejected rather than silently ignored.
	var extra any
	switch err := decoder.Decode(&extra); {
	case err == nil:
		return errors.New("live connection state has trailing JSON")
	case !errors.Is(err, io.EOF):
		return errors.New("live connection state has trailing JSON")
	}
	return nil
}

func saveLiveFile(path string, value any) error {
	data, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".live-*.tmp")
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

// serverLiveKey returns the pinned server identity. Live command responses are
// useless without it: an unverified reply could disconnect a viewer chosen by
// whoever answers the request.
func (h *HostAgent) serverLiveKey() string {
	h.mu.Lock()
	key := h.controlServerKey
	h.mu.Unlock()
	if h.cfg.ServerPublicKey != "" {
		key = h.cfg.ServerPublicKey
	}
	return key
}

// reportLiveSnapshot reads the native snapshot and reports it verbatim. The
// runtime id is chosen by the native side, so a restarted host process reports
// under a new id instead of reviving the previous one.
func (h *HostAgent) reportLiveSnapshot(ctx context.Context, client *apiClient, now time.Time) error {
	var state nativeLiveState
	err := readLiveFile(h.liveStatePath(), &state)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read native live connections: %w", err)
	}
	if state.Version != liveStateVersion {
		return errors.New("unsupported live connection state version")
	}
	snapshot, err := liveconn.Snapshot{
		RuntimeID: state.RuntimeID, Sequence: state.Sequence, Terminal: state.Terminal,
		Connections: state.Connections,
	}.Normalize()
	if err != nil {
		return err
	}
	digest, err := liveconn.Digest(snapshot)
	if err != nil {
		return err
	}
	timestamp := now.Unix()
	body, err := json.Marshal(struct {
		Snapshot  liveconn.Snapshot `json:"snapshot"`
		Timestamp int64             `json:"timestamp"`
		Signature string            `json:"signature"`
	}{snapshot, timestamp, identity.Sign(h.priv, identity.HostLiveSnapshotMessage(
		h.cfg.ID, snapshot.RuntimeID, snapshot.Sequence, snapshot.Terminal, digest, timestamp))})
	if err != nil {
		return err
	}
	return client.do(ctx, http.MethodPost,
		"/v1/hosts/"+url.PathEscape(h.cfg.ID)+"/live-connections", body, nil)
}

// pollLiveCommands fetches, verifies and persists the exact disconnect
// commands for this runtime. Nothing reaches the native side before it is on
// disk, so a crash between the HTTP reply and the native read cannot lose a
// command that the server already considers delivered.
func (h *HostAgent) pollLiveCommands(ctx context.Context, client *apiClient, runtimeID string, now time.Time) error {
	timestamp := now.Unix()
	query := url.Values{
		"timestamp": {strconv.FormatInt(timestamp, 10)},
		"signature": {identity.Sign(h.priv, identity.HostLiveCommandsFetchMessage(h.cfg.ID, runtimeID, timestamp))},
	}
	path := "/v1/hosts/" + url.PathEscape(h.cfg.ID) + "/live-connections/" +
		url.PathEscape(runtimeID) + "/commands?" + query.Encode()
	var reply liveconn.SignedCommands
	if err := client.do(ctx, http.MethodGet, path, nil, &reply); err != nil {
		return err
	}
	if reply.Timestamp == 0 {
		return errors.New("live command response has no timestamp")
	}
	signedAt := time.Unix(reply.Timestamp, 0)
	if now.Sub(signedAt) > 2*time.Minute || signedAt.Sub(now) > 2*time.Minute {
		return errors.New("live command response timestamp is outside the allowed window")
	}
	digest, err := liveconn.CommandsDigest(reply.Commands)
	if err != nil {
		return err
	}
	key := h.serverLiveKey()
	if key == "" {
		return errors.New("live commands require the pinned server identity")
	}
	pub, err := identity.ParsePublicKey(key)
	if err != nil {
		return fmt.Errorf("live commands require the registered server identity: %w", err)
	}
	if !identity.Verify(pub, identity.ServerLiveCommandsResponseMessage(
		h.cfg.ID, runtimeID, reply.Timestamp, digest), reply.Signature) {
		return errors.New("live command response did not verify")
	}
	state, err := readLiveCommandState(h.liveCommandsPath(), runtimeID)
	if err != nil {
		return err
	}
	deadline := now.UnixMilli()
	changed := state.prune(deadline)
	for _, command := range reply.Commands {
		if command.HostID != h.cfg.ID || command.RuntimeID != runtimeID {
			return errors.New("live command response named another host or runtime")
		}
		if command.DeadlineMS <= deadline {
			continue
		}
		if state.add(command) {
			changed = true
		}
	}
	if !changed {
		return nil
	}
	return saveLiveFile(h.liveCommandsPath(), state)
}

func readLiveCommandState(path, runtimeID string) (liveCommandState, error) {
	var state liveCommandState
	err := readLiveFile(path, &state)
	if errors.Is(err, os.ErrNotExist) {
		return liveCommandState{
			Version: liveStateVersion, RuntimeID: runtimeID, Acked: map[string]uint64{},
		}, nil
	}
	if err != nil {
		return liveCommandState{}, fmt.Errorf("read live command queue: %w", err)
	}
	if state.Version != liveStateVersion {
		return liveCommandState{}, errors.New("unsupported live command queue version")
	}
	if state.RuntimeID != runtimeID {
		// A new runtime must not inherit another process's commands.
		return liveCommandState{
			Version: liveStateVersion, RuntimeID: runtimeID, Acked: map[string]uint64{},
		}, nil
	}
	if state.Acked == nil {
		state.Acked = map[string]uint64{}
	}
	return state, nil
}

// prune drops settled and expired commands. True means the file changed.
func (s *liveCommandState) prune(nowMS int64) bool {
	kept := s.Commands[:0]
	changed := false
	for _, command := range s.Commands {
		if _, settled := s.Acked[command.CommandID]; settled || command.DeadlineMS <= nowMS {
			changed = true
			continue
		}
		kept = append(kept, command)
	}
	s.Commands = kept
	return changed
}

// add merges one command idempotently. A repeated poll of the same command id
// is not a change, so a lost response cannot rewrite the queue in a loop.
func (s *liveCommandState) add(command liveconn.Command) bool {
	for _, existing := range s.Commands {
		if existing.CommandID == command.CommandID {
			return false
		}
	}
	s.Commands = append(s.Commands, command)
	return true
}

// reportLiveReceipts forwards the receipts the native side recorded. A receipt
// is only removed from the queue once the server accepted it, and an already
// accepted sequence is never re-posted.
func (h *HostAgent) reportLiveReceipts(ctx context.Context, client *apiClient, runtimeID string, now time.Time) error {
	var native nativeLiveState
	err := readLiveFile(h.liveStatePath(), &native)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("read native live connections: %w", err)
	}
	if native.RuntimeID != runtimeID || len(native.Receipts) == 0 {
		return nil
	}
	state, err := readLiveCommandState(h.liveCommandsPath(), runtimeID)
	if err != nil {
		return err
	}
	changed := false
	for _, receipt := range native.Receipts {
		normalized, err := receipt.Normalize()
		if err != nil {
			return err
		}
		if previous, ok := state.Acked[normalized.CommandID]; ok && previous >= normalized.ReceiptSequence {
			continue
		}
		timestamp := time.Now().Unix()
		body, err := json.Marshal(struct {
			Receipt   liveconn.Receipt `json:"receipt"`
			Timestamp int64            `json:"timestamp"`
			Signature string           `json:"signature"`
		}{normalized, timestamp, identity.Sign(h.priv, identity.HostLiveReceiptMessage(
			h.cfg.ID, runtimeID, normalized.ReceiptSequence, normalized.CommandID,
			normalized.Status, normalized.ConfirmedAbsent, normalized.Detail, timestamp))})
		if err != nil {
			return err
		}
		if err := client.do(ctx, http.MethodPost, "/v1/hosts/"+url.PathEscape(h.cfg.ID)+
			"/live-connections/"+url.PathEscape(runtimeID)+"/receipts", body, nil); err != nil {
			// Keep the receipt queued: a later tick retries the same sequence,
			// which the server treats as an idempotent repeat.
			return err
		}
		state.Acked[normalized.CommandID] = normalized.ReceiptSequence
		changed = true
	}
	if !changed {
		return nil
	}
	state.prune(now.UnixMilli())
	return saveLiveFile(h.liveCommandsPath(), state)
}

// syncLiveConnections reports the native snapshot, drains commands and uploads
// native receipts. It uses one captured endpoint so a relay failover cannot
// split the three calls across two servers.
func (h *HostAgent) syncLiveConnections(ctx context.Context) error {
	if h.liveStatePath() == "" {
		return nil
	}
	client := &apiClient{base: h.api.endpoint(), serverKey: h.api.serverKey, http: h.api.http}
	now := time.Now()
	if err := h.reportLiveSnapshot(ctx, client, now); err != nil {
		return err
	}
	runtimeID, ok, err := readLiveRuntimeID(h.liveStatePath())
	if err != nil || !ok {
		return err
	}
	if err := h.pollLiveCommands(ctx, client, runtimeID, now); err != nil {
		return err
	}
	return h.reportLiveReceipts(ctx, client, runtimeID, now)
}

func readLiveRuntimeID(path string) (string, bool, error) {
	var state nativeLiveState
	err := readLiveFile(path, &state)
	if errors.Is(err, os.ErrNotExist) {
		return "", false, nil
	}
	if err != nil {
		return "", false, fmt.Errorf("read native live connections: %w", err)
	}
	if !liveconn.ValidRuntimeID(state.RuntimeID) {
		return "", false, errors.New("native live connection state has an invalid runtime id")
	}
	if state.Terminal {
		// A stopped runtime takes no more commands; the next process reports a
		// new id and a new runtime row.
		return "", false, nil
	}
	return strings.ToLower(strings.TrimSpace(state.RuntimeID)), true, nil
}

func (h *HostAgent) liveConnectionsLoop() {
	timer := time.NewTicker(liveconn.HeartbeatInterval)
	defer timer.Stop()
	lastError := ""
	for {
		ctx, cancel := context.WithTimeout(h.ctx, 10*time.Second)
		err := h.syncLiveConnections(ctx)
		cancel()
		if h.ctx.Err() != nil {
			return
		}
		if err != nil {
			message := err.Error()
			if message != lastError {
				h.log.Warn("live connection synchronization failed", "error", err)
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
