package agent

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
)

const (
	liveTestRuntime    = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	liveTestConnection = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
)

type liveServer struct {
	mu         sync.Mutex
	posts      int
	commands   int
	receipts   int
	failNext   int
	reply      liveconn.SignedCommands
	onSnap     func(t *testing.T, snapshot liveconn.Snapshot, timestamp int64, signature string)
	onRecpt    func(t *testing.T, receipt liveconn.Receipt, timestamp int64, signature string)
	serverPriv ed25519.PrivateKey
	hostPub    ed25519.PublicKey
}

func (s *liveServer) handler(t *testing.T) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		defer s.mu.Unlock()
		if r.Header.Get("Authorization") != "Bearer secret" {
			t.Error("host credential missing")
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		switch {
		case r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/live-connections"):
			var req struct {
				Snapshot  liveconn.Snapshot `json:"snapshot"`
				Timestamp int64             `json:"timestamp"`
				Signature string            `json:"signature"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				t.Errorf("snapshot decode: %v", err)
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			s.posts++
			if s.onSnap != nil {
				s.onSnap(t, req.Snapshot, req.Timestamp, req.Signature)
			}
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte("{}"))
		case r.Method == http.MethodGet && strings.HasSuffix(r.URL.Path, "/commands"):
			s.commands++
			payload, err := json.Marshal(s.reply)
			if err != nil {
				t.Errorf("command encode: %v", err)
				w.WriteHeader(http.StatusInternalServerError)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			w.Write(payload)
		case r.Method == http.MethodPost && strings.HasSuffix(r.URL.Path, "/receipts"):
			s.receipts++
			if s.failNext > 0 {
				s.failNext--
				w.WriteHeader(http.StatusInternalServerError)
				return
			}
			var req struct {
				Receipt   liveconn.Receipt `json:"receipt"`
				Timestamp int64            `json:"timestamp"`
				Signature string           `json:"signature"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				t.Errorf("receipt decode: %v", err)
				w.WriteHeader(http.StatusBadRequest)
				return
			}
			if s.onRecpt != nil {
				s.onRecpt(t, req.Receipt, req.Timestamp, req.Signature)
			}
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte("{}"))
		default:
			t.Errorf("unexpected request %s %s", r.Method, r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
}

// newLiveTestHost wires a host agent, a native snapshot file and a fake server
// that shares one signing key for the command response direction.
func newLiveTestHost(t *testing.T) (*HostAgent, *liveServer, string) {
	t.Helper()
	h, hostPriv := testHostAgent(t)
	serverPriv := identity.PrivateKeyFromSecret("server-secret")
	live := &liveServer{
		serverPriv: serverPriv,
		hostPub:    hostPriv.Public().(ed25519.PublicKey),
	}
	server := httptest.NewServer(live.handler(t))
	t.Cleanup(server.Close)
	h.cfg.ServerURL = server.URL
	h.cfg.ServerPublicKey = identity.PublicKeyText(serverPriv.Public().(ed25519.PublicKey))
	h.cfg.LiveState = filepath.Join(t.TempDir(), "live-connections.json")
	h.api = newAPIClient(server.URL, "secret", nil, nil)
	return h, live, h.cfg.LiveState
}

func writeNativeLive(t *testing.T, path string, state nativeLiveState) {
	t.Helper()
	if err := saveLiveFile(path, state); err != nil {
		t.Fatal(err)
	}
}

func liveCommand(status string, deadline time.Time) liveconn.Command {
	issued := deadline.Add(-time.Minute)
	if issued.After(time.Now()) {
		issued = time.Now()
	}
	return liveconn.Command{
		CommandID:    "cmd-1",
		HostID:       "devbox",
		RuntimeID:    liveTestRuntime,
		ConnectionID: liveTestConnection,
		Operation:    liveconn.OperationDisconnect,
		RequestKey:   "request-key-0001",
		Status:       status,
		IssuedAtMS:   issued.UnixMilli(),
		DeadlineMS:   deadline.UnixMilli(),
	}
}

func signCommands(t *testing.T, live *liveServer, commands []liveconn.Command) liveconn.SignedCommands {
	t.Helper()
	digest, err := liveconn.CommandsDigest(commands)
	if err != nil {
		t.Fatal(err)
	}
	stamp := time.Now().Unix()
	return liveconn.SignedCommands{
		Commands: commands, Timestamp: stamp,
		Signature: identity.Sign(live.serverPriv, identity.ServerLiveCommandsResponseMessage(
			"devbox", liveTestRuntime, stamp, digest)),
	}
}

func readCommandFile(t *testing.T, path string) liveCommandState {
	t.Helper()
	var state liveCommandState
	if err := readLiveFile(path+".commands", &state); err != nil {
		t.Fatal(err)
	}
	return state
}

func TestLiveSnapshotReportsNativeState(t *testing.T) {
	h, live, path := newLiveTestHost(t)
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 4,
		Connections: []liveconn.Connection{{
			ConnectionID: liveTestConnection, PeerAddress: "203.0.113.7:51000",
			Viewer: "alice", Mode: "control", Route: "relay",
			StartedAtMS: time.Now().Add(-time.Minute).UnixMilli(),
		}},
	})
	live.reply = signCommands(t, live, nil)
	var seen bool
	live.onSnap = func(t *testing.T, snapshot liveconn.Snapshot, timestamp int64, signature string) {
		seen = true
		if snapshot.RuntimeID != liveTestRuntime || snapshot.Sequence != 4 || len(snapshot.Connections) != 1 {
			t.Errorf("snapshot: %+v", snapshot)
		}
		digest, err := liveconn.Digest(snapshot)
		if err != nil {
			t.Fatal(err)
		}
		if !identity.Verify(live.hostPub, identity.HostLiveSnapshotMessage(
			"devbox", snapshot.RuntimeID, snapshot.Sequence, snapshot.Terminal,
			digest, timestamp), signature) {
			t.Error("snapshot signature did not verify")
		}
	}
	if err := h.syncLiveConnections(context.Background()); err != nil {
		t.Fatal(err)
	}
	if !seen {
		t.Fatal("snapshot was not reported")
	}
}

func TestLiveSnapshotMissingFileIsNotAnError(t *testing.T) {
	h, _, path := newLiveTestHost(t)
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	if err := h.reportLiveSnapshot(context.Background(), &apiClient{
		base: h.api.endpoint(), serverKey: "secret", http: h.api.http,
	}, time.Now()); err != nil {
		t.Fatal(err)
	}
}

func TestLiveCommandResponseMustVerify(t *testing.T) {
	command := liveCommand(liveconn.CommandDelivered, time.Now().Add(time.Minute))
	for _, tc := range []struct {
		name   string
		mutate func(live *liveServer, reply *liveconn.SignedCommands)
	}{
		{"bad signature", func(_ *liveServer, reply *liveconn.SignedCommands) {
			reply.Signature = strings.Repeat("0", 128)
		}},
		{"stale timestamp", func(live *liveServer, reply *liveconn.SignedCommands) {
			reply.Timestamp = time.Now().Add(-10 * time.Minute).Unix()
			digest, _ := liveconn.CommandsDigest(reply.Commands)
			reply.Signature = identity.Sign(live.serverPriv,
				identity.ServerLiveCommandsResponseMessage("devbox", liveTestRuntime, reply.Timestamp, digest))
		}},
		{"no timestamp", func(_ *liveServer, reply *liveconn.SignedCommands) {
			reply.Timestamp = 0
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			h, live, path := newLiveTestHost(t)
			writeNativeLive(t, path, nativeLiveState{
				Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 1,
			})
			live.reply = signCommands(t, live, []liveconn.Command{command})
			tc.mutate(live, &live.reply)
			if err := h.syncLiveConnections(context.Background()); err == nil {
				t.Fatal("unverified command response was accepted")
			}
			if _, err := os.Stat(path + ".commands"); !os.IsNotExist(err) {
				t.Fatal("unverified command reached the native inbox")
			}
		})
	}
}

func TestLiveCommandForAnotherRuntimeIsRejected(t *testing.T) {
	h, live, path := newLiveTestHost(t)
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 1,
	})
	foreign := liveCommand(liveconn.CommandDelivered, time.Now().Add(time.Minute))
	foreign.HostID = "otherbox"
	live.reply = signCommands(t, live, []liveconn.Command{foreign})
	if err := h.syncLiveConnections(context.Background()); err == nil {
		t.Fatal("command naming another host was accepted")
	}
	if _, err := os.Stat(path + ".commands"); !os.IsNotExist(err) {
		t.Fatal("foreign command reached the native inbox")
	}
}

func TestLiveCommandsPersistOnceAndRepeatPollIsIdempotent(t *testing.T) {
	h, live, path := newLiveTestHost(t)
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 1,
	})
	live.reply = signCommands(t, live, []liveconn.Command{
		liveCommand(liveconn.CommandDelivered, time.Now().Add(time.Minute)),
	})
	ctx := context.Background()
	if err := h.syncLiveConnections(ctx); err != nil {
		t.Fatal(err)
	}
	first, err := os.ReadFile(path + ".commands")
	if err != nil {
		t.Fatal(err)
	}
	state := readCommandFile(t, path)
	if len(state.Commands) != 1 || state.Commands[0].CommandID != "cmd-1" {
		t.Fatalf("inbox: %+v", state)
	}
	if state.RuntimeID != liveTestRuntime {
		t.Fatalf("inbox runtime: %q", state.RuntimeID)
	}
	if err := h.syncLiveConnections(ctx); err != nil {
		t.Fatal(err)
	}
	second, err := os.ReadFile(path + ".commands")
	if err != nil {
		t.Fatal(err)
	}
	if string(first) != string(second) {
		t.Fatal("a repeated poll rewrote the persisted command queue")
	}
}

func TestLiveCommandsDropExpiredAndSettled(t *testing.T) {
	h, live, path := newLiveTestHost(t)
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 1,
	})
	live.reply = signCommands(t, live, []liveconn.Command{
		liveCommand(liveconn.CommandDelivered, time.Now().Add(-time.Minute)),
	})
	if err := h.syncLiveConnections(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path + ".commands"); !os.IsNotExist(err) {
		t.Fatal("expired command was queued for the native side")
	}
}

func TestLiveReceiptsRetryUntilAcceptedThenStop(t *testing.T) {
	h, live, path := newLiveTestHost(t)
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 2,
		Receipts: []liveconn.Receipt{{
			ReceiptSequence: 1, CommandID: "cmd-1",
			Status: liveconn.CommandApplied, ConfirmedAbsent: true,
		}},
	})
	live.reply = signCommands(t, live, nil)
	live.failNext = 1
	var verified int
	live.onRecpt = func(t *testing.T, receipt liveconn.Receipt, timestamp int64, signature string) {
		if !identity.Verify(live.hostPub, identity.HostLiveReceiptMessage(
			"devbox", liveTestRuntime, receipt.ReceiptSequence, receipt.CommandID,
			receipt.Status, receipt.ConfirmedAbsent, receipt.Detail, timestamp), signature) {
			t.Error("receipt signature did not verify")
		}
		verified++
	}
	ctx := context.Background()
	if err := h.syncLiveConnections(ctx); err == nil {
		t.Fatal("failed receipt post was treated as accepted")
	}
	if _, err := os.Stat(path + ".commands"); !os.IsNotExist(err) {
		state := readCommandFile(t, path)
		if _, ok := state.Acked["cmd-1"]; ok {
			t.Fatal("receipt marked acknowledged before the server accepted it")
		}
	}
	if err := h.syncLiveConnections(ctx); err != nil {
		t.Fatal(err)
	}
	state := readCommandFile(t, path)
	if got := state.Acked["cmd-1"]; got != 1 {
		t.Fatalf("acknowledged sequence: %d", got)
	}
	live.mu.Lock()
	before := live.receipts
	live.mu.Unlock()
	if err := h.syncLiveConnections(ctx); err != nil {
		t.Fatal(err)
	}
	live.mu.Lock()
	after := live.receipts
	live.mu.Unlock()
	if after != before {
		t.Fatalf("accepted receipt was posted again: %d -> %d", before, after)
	}
	if verified == 0 {
		t.Fatal("receipt signature was never checked")
	}
}

func TestLiveCommandQueueIsNotInheritedByAnotherRuntime(t *testing.T) {
	path := filepath.Join(t.TempDir(), "queue.json")
	command := liveCommand(liveconn.CommandDelivered, time.Now().Add(time.Minute))
	if err := saveLiveFile(path, liveCommandState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime,
		Commands: []liveconn.Command{command},
	}); err != nil {
		t.Fatal(err)
	}
	next := strings.Repeat("c", 32)
	state, err := readLiveCommandState(path, next)
	if err != nil {
		t.Fatal(err)
	}
	if len(state.Commands) != 0 || state.RuntimeID != next {
		t.Fatalf("new runtime inherited the previous inbox: %+v", state)
	}
	original, err := readLiveCommandState(path, liveTestRuntime)
	if err != nil {
		t.Fatal(err)
	}
	if len(original.Commands) != 1 {
		t.Fatal("reading another runtime's inbox destroyed the original")
	}
}

func TestTerminalOrInvalidRuntimeSkipsPolling(t *testing.T) {
	h, live, path := newLiveTestHost(t)
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: liveTestRuntime, Sequence: 9, Terminal: true,
	})
	if err := h.syncLiveConnections(context.Background()); err != nil {
		t.Fatal(err)
	}
	live.mu.Lock()
	commands := live.commands
	live.mu.Unlock()
	if commands != 0 {
		t.Fatal("a terminal runtime kept polling for commands")
	}
	if _, ok, err := readLiveRuntimeID(path); err != nil || ok {
		t.Fatalf("terminal runtime reported as live: ok=%v err=%v", ok, err)
	}
	writeNativeLive(t, path, nativeLiveState{
		Version: liveStateVersion, RuntimeID: "not-a-runtime-id", Sequence: 10,
	})
	if _, ok, err := readLiveRuntimeID(path); err == nil || ok {
		t.Fatal("invalid runtime id was accepted")
	}
}

func TestLiveStatePathDisabledByDefault(t *testing.T) {
	h, _ := testHostAgent(t)
	if h.liveStatePath() != "" || h.liveCommandsPath() != "" {
		t.Fatal("live connections must stay disabled without a configured path")
	}
	if err := h.syncLiveConnections(context.Background()); err != nil {
		t.Fatal(err)
	}
}

func TestLiveFileRejectsOversizedAndTrailingJSON(t *testing.T) {
	dir := t.TempDir()
	big := filepath.Join(dir, "big.json")
	if err := os.WriteFile(big, []byte(strings.Repeat("x", 300<<10)), 0600); err != nil {
		t.Fatal(err)
	}
	var state nativeLiveState
	if err := readLiveFile(big, &state); err == nil {
		t.Fatal("oversized state file was accepted")
	}
	trailing := filepath.Join(dir, "trailing.json")
	if err := os.WriteFile(trailing, []byte(`{"version":1} {"version":1}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := readLiveFile(trailing, &state); err == nil {
		t.Fatal("trailing JSON was accepted")
	}
	unknown := filepath.Join(dir, "unknown.json")
	if err := os.WriteFile(unknown, []byte(`{"version":1,"surprise":true}`), 0600); err != nil {
		t.Fatal(err)
	}
	if err := readLiveFile(unknown, &state); err == nil {
		t.Fatal("unknown field was accepted")
	}
}
