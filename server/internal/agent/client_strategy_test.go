package agent

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

func TestClientStrategyTransportPersistsAndOnlyForwardsMatchingNativeAck(t *testing.T) {
	h, priv := testHostAgent(t)
	h.publicKey = identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
	h.cfg.PolicyState = filepath.Join(t.TempDir(), "policy.json")
	settings := strategy.Settings{"host.keep_awake": {Mode: strategy.Locked, Value: json.RawMessage("true")}}
	encoded, err := strategy.Encode(settings)
	if err != nil {
		t.Fatal(err)
	}
	var mu sync.Mutex
	snapshot := strategy.Snapshot{PeerID: "devbox", Target: strategy.Host, Revision: 3, PolicyID: "managed", Settings: settings, Digest: strategy.Digest(strategy.Host, encoded)}
	statusCode, posts := http.StatusOK, 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if r.Header.Get("Authorization") != "Bearer secret" {
			t.Error("host credential missing")
			w.WriteHeader(401)
			return
		}
		if r.Method == http.MethodGet {
			ts, _ := strconv.ParseInt(r.URL.Query().Get("timestamp"), 10, 64)
			if !identity.Verify(priv.Public().(ed25519.PublicKey), identity.HostClientStrategyFetchMessage("devbox", ts), r.URL.Query().Get("signature")) {
				t.Error("fetch signature invalid")
				w.WriteHeader(401)
				return
			}
			w.WriteHeader(statusCode)
			json.NewEncoder(w).Encode(snapshot)
			return
		}
		var req struct {
			Receipt   strategy.Receipt `json:"receipt"`
			Timestamp int64            `json:"timestamp"`
			Signature string           `json:"signature"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			t.Error(err)
			w.WriteHeader(400)
			return
		}
		payload, err := strategy.CanonicalReceipt(req.Receipt)
		if err != nil || !identity.Verify(priv.Public().(ed25519.PublicKey), identity.HostClientStrategyReceiptMessage("devbox", payload, req.Timestamp), req.Signature) {
			t.Error("receipt signature invalid")
			w.WriteHeader(401)
			return
		}
		posts++
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()
	h.cfg.ServerURL = server.URL
	h.api = newAPIClient(server.URL, "secret", nil, nil)
	ctx := context.Background()
	path := h.cfg.PolicyState + ".client-strategy"
	if err := h.syncClientStrategy(ctx); err != nil {
		t.Fatal(err)
	}
	var state clientStrategyState
	if err := readStrategyFile(path, &state); err != nil {
		t.Fatal(err)
	}
	if state.Snapshot.Revision != 3 || state.Scope != h.clientStrategyScope() {
		t.Fatalf("state: %+v", state)
	}
	mu.Lock()
	count := posts
	mu.Unlock()
	if count != 0 {
		t.Fatal("delivery reported as applied")
	}
	ack := clientStrategyAck{Scope: state.Scope, Receipt: strategy.Receipt{Revision: 3, Digest: state.Snapshot.Digest, Sequence: 1, Status: strategy.PendingRestart, Results: map[string]strategy.Result{"host.keep_awake": {Status: strategy.PendingRestart}}}}
	writeAck := func() {
		t.Helper()
		body, err := json.Marshal(ack)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path+".ack", body, 0600); err != nil {
			t.Fatal(err)
		}
	}
	ack.Scope = "another-server"
	writeAck()
	if err := h.syncClientStrategy(ctx); err != nil {
		t.Fatal(err)
	}
	mu.Lock()
	count = posts
	mu.Unlock()
	if count != 0 {
		t.Fatal("foreign ack sent")
	}
	ack.Scope = state.Scope
	writeAck()
	if err := h.syncClientStrategy(ctx); err != nil {
		t.Fatal(err)
	}
	mu.Lock()
	count = posts
	mu.Unlock()
	if count != 1 {
		t.Fatal("matching native ack not sent")
	}
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	mu.Lock()
	statusCode = http.StatusServiceUnavailable
	mu.Unlock()
	if err := h.syncClientStrategy(ctx); err == nil {
		t.Fatal("outage ignored")
	}
	after, err := os.ReadFile(path)
	if err != nil || string(after) != string(before) {
		t.Fatal("outage changed cached settings")
	}
	mu.Lock()
	statusCode = http.StatusOK
	snapshot.Revision = 2
	mu.Unlock()
	if err := h.syncClientStrategy(ctx); err == nil {
		t.Fatal("old version accepted")
	}
	mu.Lock()
	snapshot.Revision = 3
	snapshot.PolicyID = "other-policy"
	mu.Unlock()
	if err := h.syncClientStrategy(ctx); err == nil {
		t.Fatal("same version changed identity accepted")
	}
	mu.Lock()
	snapshot.Revision = 4
	snapshot.Settings = strategy.Settings{}
	snapshot.Digest = strategy.Digest(strategy.Host, "{}")
	mu.Unlock()
	if err := h.syncClientStrategy(ctx); err != nil {
		t.Fatal(err)
	}
	if err := readStrategyFile(path, &state); err != nil || len(state.Snapshot.Settings) != 0 || state.Snapshot.Revision != 4 {
		t.Fatalf("removal state: %+v %v", state, err)
	}
	mu.Lock()
	count = posts
	mu.Unlock()
	if count != 1 {
		t.Fatal("old applied ack reused for removal")
	}
	// Restart uses the file generation, not an in-memory counter.
	restarted, _ := testHostAgent(t)
	restarted.cfg, restarted.publicKey, restarted.api = h.cfg, h.publicKey, h.api
	mu.Lock()
	snapshot.Revision = 3
	mu.Unlock()
	if err := restarted.syncClientStrategy(ctx); err == nil {
		t.Fatal("restart accepted old version")
	}
}

func TestClientStrategyStateRejectsOtherTargetsAndTampering(t *testing.T) {
	snapshot := strategy.Snapshot{PeerID: "devbox", Target: strategy.Host, Revision: 1, Settings: strategy.Settings{}, Digest: strategy.Digest(strategy.Host, "{}")}
	if err := snapshot.Validate("devbox", strategy.Host); err != nil {
		t.Fatal(err)
	}
	if err := snapshot.Validate("other", strategy.Host); err == nil {
		t.Fatal("wrong peer accepted")
	}
	if err := snapshot.Validate("devbox", strategy.Viewer); err == nil {
		t.Fatal("wrong target accepted")
	}
	snapshot.Settings["viewer.use_websocket"] = strategy.Setting{Mode: strategy.Locked, Value: json.RawMessage("true")}
	if err := snapshot.Validate("devbox", strategy.Host); err == nil {
		t.Fatal("foreign target setting accepted")
	}
	snapshot.Settings = strategy.Settings{}
	snapshot.Digest = "modified"
	if err := snapshot.Validate("devbox", strategy.Host); err == nil {
		t.Fatal("wrong digest accepted")
	}
}

// This same file is consumed by the native C++ decoder. Keep its identity and
// canonical settings tied to the real Go protocol instead of two mock codecs.
func TestClientStrategyNativeGoldenFixture(t *testing.T) {
	var state clientStrategyState
	if err := readStrategyFile("../../../tests/data/host_client_strategy.json", &state); err != nil {
		t.Fatal(err)
	}
	priv := identity.PrivateKeyFromSecret("native-test-secret")
	h := &HostAgent{cfg: HostConfig{ServerURL: "https://strategy.example.test///", ID: "native-host"},
		publicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey))}
	if state.Version != 1 || state.Scope != h.clientStrategyScope() || state.HostPublicKey != h.publicKey ||
		state.ServerURL != "https://strategy.example.test" || state.ServerPublicKey != "" {
		t.Fatal("native fixture authority differs from agent scope")
	}
	if err := state.Snapshot.Validate(h.cfg.ID, strategy.Host); err != nil {
		t.Fatal(err)
	}
	if state.Snapshot.Revision != 7 || len(state.Snapshot.Settings) != 6 || state.Snapshot.PolicyID != "policy-测试<>&😀" {
		t.Fatal("native fixture no longer covers all six settings and escaped metadata")
	}
	path := filepath.Join(t.TempDir(), "native-cache.json")
	if err := saveClientStrategyState(path, state); err != nil {
		t.Fatal(err)
	}
	var roundTrip clientStrategyState
	if err := readStrategyFile(path, &roundTrip); err != nil {
		t.Fatal(err)
	}
	if roundTrip.Scope != state.Scope || roundTrip.Snapshot.Digest != state.Snapshot.Digest {
		t.Fatal("agent cache writer changed native fixture binding")
	}
}

func TestViewerClientStrategyNativeGoldenFixture(t *testing.T) {
	data, err := os.ReadFile("../../../tests/data/viewer_client_strategy.json")
	if err != nil {
		t.Fatal(err)
	}
	var snapshot strategy.Snapshot
	if err := json.Unmarshal(data, &snapshot); err != nil {
		t.Fatal(err)
	}
	if err := snapshot.Validate("native-host", strategy.Viewer); err != nil {
		t.Fatal(err)
	}
	if snapshot.Revision != 9 || len(snapshot.Settings) != 5 || snapshot.PolicyID != "viewer-policy" {
		t.Fatal("viewer fixture no longer covers all five session settings")
	}
	for key := range snapshot.Settings {
		if !strings.HasPrefix(key, "viewer.") {
			t.Fatalf("viewer fixture contains foreign key %s", key)
		}
	}
}
