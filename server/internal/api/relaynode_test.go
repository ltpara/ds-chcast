package api_test

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// stubNode speaks the relay node control protocol so API tests can drive
// health, lease failures and carrier-online snapshots without a data plane.
type stubNode struct {
	url       string
	relayHost string
	carrier   string
	server    *httptest.Server

	mu            sync.Mutex
	healthy       bool
	leaseStatus   int
	nextPort      int
	ports         map[string]int
	ttls          map[string]int64
	carrierTokens map[string]string
	linked        map[string]bool
	releases      []string
	requests      int
	lastPublicKey string
	badAuth       int
}

func newStubNode(t *testing.T, relayHost, carrier string) *stubNode {
	t.Helper()
	n := &stubNode{
		relayHost:     relayHost,
		carrier:       carrier,
		healthy:       true,
		nextPort:      41001,
		ports:         make(map[string]int),
		ttls:          make(map[string]int64),
		carrierTokens: make(map[string]string),
		linked:        make(map[string]bool),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", n.handleHealth)
	mux.HandleFunc("POST /v1/leases", n.handleLease)
	mux.HandleFunc("DELETE /v1/leases/{id}", n.handleRelease)
	n.server = httptest.NewServer(mux)
	n.url = n.server.URL
	t.Cleanup(n.server.Close)
	return n
}

func (n *stubNode) setHealthy(healthy bool) {
	n.mu.Lock()
	n.healthy = healthy
	n.mu.Unlock()
}

func (n *stubNode) setLeaseStatus(status int) {
	n.mu.Lock()
	n.leaseStatus = status
	n.mu.Unlock()
}

func (n *stubNode) setLinked(id string) {
	n.mu.Lock()
	n.linked[id] = true
	n.mu.Unlock()
}

func (n *stubNode) releaseCount(id string) int {
	n.mu.Lock()
	defer n.mu.Unlock()
	count := 0
	for _, released := range n.releases {
		if released == id {
			count++
		}
	}
	return count
}

func (n *stubNode) leaseRequest(id string) (int64, string, int) {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.ttls[id], n.lastPublicKey, n.requests
}

func (n *stubNode) handleHealth(w http.ResponseWriter, _ *http.Request) {
	n.mu.Lock()
	defer n.mu.Unlock()
	if !n.healthy {
		http.Error(w, "down", http.StatusServiceUnavailable)
		return
	}
	linked := make([]string, 0, len(n.linked))
	for id := range n.linked {
		linked = append(linked, id)
	}
	writeStubJSON(w, http.StatusOK, map[string]any{
		"ok":     true,
		"leases": len(n.ports),
		"linked": linked,
	})
}

func (n *stubNode) handleLease(w http.ResponseWriter, r *http.Request) {
	if !n.checkKey(w, r) {
		return
	}
	var req struct {
		ID           string `json:"id"`
		PublicKey    string `json:"public_key"`
		CarrierToken string `json:"carrier_token"`
		TTLSeconds   int64  `json:"ttl_sec"`
	}
	body, _ := io.ReadAll(io.LimitReader(r.Body, 8<<10))
	if err := json.Unmarshal(body, &req); err != nil {
		writeStubJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
		return
	}
	n.mu.Lock()
	n.requests++
	n.lastPublicKey = req.PublicKey
	if n.leaseStatus != 0 {
		status := n.leaseStatus
		n.mu.Unlock()
		writeStubJSON(w, status, map[string]string{"error": "lease refused"})
		return
	}
	port := n.ports[req.ID]
	if port == 0 {
		port = n.nextPort
		n.nextPort++
		n.ports[req.ID] = port
	}
	n.ttls[req.ID] = req.TTLSeconds
	n.carrierTokens[req.ID] = req.CarrierToken
	carrier := n.carrier
	n.mu.Unlock()
	reply := map[string]any{
		"id":             req.ID,
		"port":           port,
		"relay":          joinStub(n.relayHost, port),
		"expires_in_sec": req.TTLSeconds,
	}
	if carrier != "" {
		reply["carrier"] = carrier
	}
	writeStubJSON(w, http.StatusOK, reply)
}

func (n *stubNode) handleRelease(w http.ResponseWriter, r *http.Request) {
	if !n.checkKey(w, r) {
		return
	}
	id := r.PathValue("id")
	n.mu.Lock()
	n.releases = append(n.releases, id)
	delete(n.ports, id)
	delete(n.ttls, id)
	delete(n.carrierTokens, id)
	delete(n.linked, id)
	n.mu.Unlock()
	w.WriteHeader(http.StatusNoContent)
}

func (n *stubNode) checkKey(w http.ResponseWriter, r *http.Request) bool {
	if r.Header.Get("Authorization") != "Bearer "+testutil.RelayNodeKey {
		n.mu.Lock()
		n.badAuth++
		n.mu.Unlock()
		writeStubJSON(w, http.StatusUnauthorized, map[string]string{"error": "key"})
		return false
	}
	return true
}

func joinStub(host string, port int) string {
	return net.JoinHostPort(host, strconv.Itoa(port))
}

func writeStubJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func waitUntil(t *testing.T, timeout time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestRemoteRelayRegistrationAndLookup(t *testing.T) {
	node := newStubNode(t, "relay-a.example", "relay-a.example:8448")
	node.setLinked("devbox")
	s := testutil.StartWith(t, testutil.Options{
		RelayNodes:      []string{node.url},
		RelayNodeHealth: 20 * time.Millisecond,
	})
	now := time.Now().Unix()

	register := func() struct {
		Relay        string   `json:"relay"`
		Relays       []string `json:"relays"`
		Carrier      string   `json:"carrier"`
		CarrierToken string   `json:"carrier_token"`
	} {
		t.Helper()
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("register: %d %s", status, body)
		}
		var reply struct {
			Relay        string   `json:"relay"`
			Relays       []string `json:"relays"`
			Carrier      string   `json:"carrier"`
			CarrierToken string   `json:"carrier_token"`
		}
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatal(err)
		}
		return reply
	}

	first := register()
	if first.Relay != "relay-a.example:41001" {
		t.Fatalf("remote relay = %q", first.Relay)
	}
	if len(first.Relays) != 1 || first.Relays[0] != first.Relay {
		t.Fatalf("remote relay list: %+v", first.Relays)
	}
	if first.Carrier != "relay-a.example:8448" {
		t.Fatalf("remote carrier = %q", first.Carrier)
	}
	ttl, pub, calls := node.leaseRequest("devbox")
	if calls != 1 || ttl != 120 {
		t.Fatalf("lease request: ttl=%d calls=%d", ttl, calls)
	}
	_, wantPub := hostKey("correct horse")
	if pub != wantPub {
		t.Fatalf("node saw public key %q", pub)
	}
	lease, ok := s.Registry.Lookup("devbox")
	if !ok || lease.RelayNode != node.url || lease.RelayHost != "relay-a.example" ||
		lease.RelayPort != 41001 || lease.Carrier != "relay-a.example:8448" {
		t.Fatalf("registry lease: %+v (found=%v)", lease, ok)
	}

	// The carrier health snapshot alone can prove the host online.
	waitUntil(t, 2*time.Second, "node carrier snapshot", func() bool {
		return s.Pool.HostLinked(node.url, "devbox")
	})
	status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if !host.Online || host.Transport != "carrier" || host.Relay != first.Relay ||
		host.Carrier != first.Carrier || host.CarrierToken != first.CarrierToken {
		t.Fatalf("lookup after registration: %+v", host)
	}

	// A renewal is sticky: the same node returns the same port and nothing is
	// released.
	second := register()
	if second.Relay != first.Relay || second.CarrierToken != first.CarrierToken {
		t.Fatalf("renewal moved the lease: %+v -> %+v", first, second)
	}
	if _, _, calls := node.leaseRequest("devbox"); calls != 2 {
		t.Fatalf("renewal did not ask the node again: %d", calls)
	}
	if released := node.releaseCount("devbox"); released != 0 {
		t.Fatalf("renewal released the lease %d times", released)
	}

	_, infoBody := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	var info struct {
		RelayNodes struct {
			Healthy int `json:"healthy"`
			Nodes   []struct {
				URL     string `json:"url"`
				Healthy bool   `json:"healthy"`
			} `json:"nodes"`
		} `json:"relay_nodes"`
	}
	if err := json.Unmarshal(infoBody, &info); err != nil {
		t.Fatal(err)
	}
	if len(info.RelayNodes.Nodes) != 1 || info.RelayNodes.Nodes[0].URL != node.url ||
		info.RelayNodes.Healthy != 1 || !info.RelayNodes.Nodes[0].Healthy {
		t.Fatalf("server info relay nodes: %+v", info.RelayNodes)
	}

	var stats struct {
		RelayNodes struct {
			Healthy int `json:"healthy"`
			Nodes   []struct {
				Leases int `json:"leases"`
				Linked int `json:"linked"`
			} `json:"nodes"`
		} `json:"relay_nodes"`
	}
	waitUntil(t, 2*time.Second, "health probe with the lease", func() bool {
		status := s.Pool.Status()
		return len(status.Nodes) == 1 && status.Nodes[0].Leases == 1
	})
	_, statsBody := request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
	if err := json.Unmarshal(statsBody, &stats); err != nil {
		t.Fatal(err)
	}
	if stats.RelayNodes.Healthy != 1 || len(stats.RelayNodes.Nodes) != 1 ||
		stats.RelayNodes.Nodes[0].Leases != 1 || stats.RelayNodes.Nodes[0].Linked != 1 {
		t.Fatalf("stats relay nodes: %+v", stats.RelayNodes)
	}

	_, metrics := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey)
	text := string(metrics)
	for _, want := range []string{"chast_relay_nodes 1", "chast_relay_nodes_healthy 1"} {
		if !contains(text, want) {
			t.Fatalf("metrics missing %q:\n%s", want, text)
		}
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey); status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", status, body)
	}
	waitUntil(t, 2*time.Second, "remote lease release", func() bool {
		return node.releaseCount("devbox") == 1
	})
}

func TestRemoteRelayFailoverAndLocalFallback(t *testing.T) {
	nodeA := newStubNode(t, "relay-a.example", "relay-a.example:8448")
	nodeB := newStubNode(t, "relay-b.example", "relay-b.example:8448")
	nodeA.setLeaseStatus(http.StatusServiceUnavailable)
	s := testutil.StartWith(t, testutil.Options{
		RelayNodes:      []string{nodeA.url, nodeB.url},
		RelayNodeHealth: 20 * time.Millisecond,
	})
	now := time.Now().Unix()

	register := func(id string) (string, string) {
		t.Helper()
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, id, id+" horse", "", now), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("register %s: %d %s", id, status, body)
		}
		var reply struct {
			Relay   string `json:"relay"`
			Carrier string `json:"carrier"`
		}
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatal(err)
		}
		return reply.Relay, reply.Carrier
	}

	relay, carrier := register("alpha")
	if relay != "relay-b.example:41001" || carrier != "relay-b.example:8448" {
		t.Fatalf("failover assignment: relay=%q carrier=%q", relay, carrier)
	}
	if nodeA.leaseRequestCount() == 0 {
		t.Fatal("the failing node was never tried")
	}

	// The last healthy node fails too, and the local relay takes over.
	nodeA.setHealthy(false)
	nodeB.setLeaseStatus(http.StatusServiceUnavailable)
	waitUntil(t, 2*time.Second, "node A offline", func() bool {
		for _, node := range s.Pool.Status().Nodes {
			if node.URL == nodeA.url && node.Healthy {
				return false
			}
		}
		return true
	})
	relay, carrier = register("beta")
	if !hasHostPrefix(relay, "127.0.0.1:") || !hasHostPrefix(carrier, "127.0.0.1:") {
		t.Fatalf("local fallback: relay=%q carrier=%q", relay, carrier)
	}
	lease, ok := s.Registry.Lookup("beta")
	if !ok || lease.RelayNode != "" || lease.RelayHost != "127.0.0.1" || lease.RelayPort == 0 {
		t.Fatalf("fallback registry lease: %+v (found=%v)", lease, ok)
	}
	if !localPortAssigned(s, "beta", lease.RelayPort) {
		t.Fatalf("local relay port %d was not assigned to beta", lease.RelayPort)
	}

	// Once the node is healthy again the next registration moves the host off
	// the local relay.
	nodeB.setLeaseStatus(0)
	waitUntil(t, 2*time.Second, "node B healthy", func() bool {
		return poolNodeHealthy(s, nodeB.url)
	})
	relay, carrier = register("beta")
	if !hasHostPrefix(relay, "relay-b.example:") || !hasHostPrefix(carrier, "relay-b.example:") {
		t.Fatalf("recovery assignment: relay=%q carrier=%q", relay, carrier)
	}
	lease, _ = s.Registry.Lookup("beta")
	if lease.RelayNode != nodeB.url {
		t.Fatalf("recovered registry lease: %+v", lease)
	}
	for _, port := range s.Relay.Stats().Ports {
		if port.HostID == "beta" {
			t.Fatalf("local relay port %d still holds beta", port.Port)
		}
	}

	// A preferred node that fails at renewal moves the host and releases the
	// old lease.
	nodeA.setHealthy(true)
	nodeA.setLeaseStatus(0)
	nodeB.setLeaseStatus(http.StatusServiceUnavailable)
	waitUntil(t, 2*time.Second, "node A healthy", func() bool {
		for _, node := range s.Pool.Status().Nodes {
			if node.URL == nodeA.url && node.Healthy {
				return true
			}
		}
		return false
	})
	relay, _ = register("alpha")
	if !hasHostPrefix(relay, "relay-a.example:") {
		t.Fatalf("node switch assignment = %q", relay)
	}
	waitUntil(t, 2*time.Second, "old node release", func() bool {
		return nodeB.releaseCount("alpha") == 1
	})
}

func (n *stubNode) leaseRequestCount() int {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.requests
}

func localPortAssigned(s *testutil.Server, id string, port int) bool {
	for _, stats := range s.Relay.Stats().Ports {
		if stats.Port == port {
			return stats.HostID == id
		}
	}
	return false
}

func poolNodeHealthy(s *testutil.Server, url string) bool {
	for _, node := range s.Pool.Status().Nodes {
		if node.URL == url {
			return node.Healthy
		}
	}
	return false
}

func hasHostPrefix(endpoint, prefix string) bool {
	return strings.HasPrefix(endpoint, prefix)
}

func contains(text, want string) bool {
	return strings.Contains(text, want)
}
