package relaypool

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

const testKey = "node-key"

var testLog = slog.New(slog.NewTextHandler(io.Discard, nil))

type fakeNode struct {
	server *httptest.Server

	mu          sync.Mutex
	healthy     bool
	leases      int
	linked      []string
	leaseStatus int
	port        int
	relayHost   string
	carrier     string
	requests    []leaseRequest
	released    []string
	badAuth     int
}

func newFakeNode(t *testing.T, relayHost string) *fakeNode {
	t.Helper()
	return newNode(t, relayHost, false)
}

func newNode(t *testing.T, relayHost string, useTLS bool) *fakeNode {
	t.Helper()
	node := &fakeNode{
		healthy:   true,
		port:      41000,
		relayHost: relayHost,
		carrier:   "carrier.example:8448",
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {
		node.mu.Lock()
		healthy, leases, linked := node.healthy, node.leases, append([]string(nil), node.linked...)
		node.mu.Unlock()
		if !healthy {
			http.Error(w, "unavailable", http.StatusServiceUnavailable)
			return
		}
		json.NewEncoder(w).Encode(healthResponse{OK: true, Leases: leases, Linked: linked})
	})
	mux.HandleFunc("POST /v1/leases", func(w http.ResponseWriter, r *http.Request) {
		node.mu.Lock()
		status := node.leaseStatus
		if r.Header.Get("Authorization") != "Bearer "+testKey {
			node.badAuth++
			node.mu.Unlock()
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		var req leaseRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			node.mu.Unlock()
			http.Error(w, "bad json", http.StatusBadRequest)
			return
		}
		node.requests = append(node.requests, req)
		if status != 0 {
			node.mu.Unlock()
			http.Error(w, "lease refused", status)
			return
		}
		node.port++
		port := node.port
		relayHost, carrier := node.relayHost, node.carrier
		node.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(Lease{
			Port:         port,
			Relay:        relayHost + ":" + strconv.Itoa(port),
			Carrier:      carrier,
			ExpiresInSec: 120,
		})
	})
	mux.HandleFunc("DELETE /v1/leases/{id}", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer "+testKey {
			node.mu.Lock()
			node.badAuth++
			node.mu.Unlock()
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		node.mu.Lock()
		node.released = append(node.released, r.PathValue("id"))
		node.mu.Unlock()
		w.WriteHeader(http.StatusNoContent)
	})
	if useTLS {
		node.server = httptest.NewTLSServer(mux)
	} else {
		node.server = httptest.NewServer(mux)
	}
	t.Cleanup(node.server.Close)
	return node
}

func (n *fakeNode) setHealthy(healthy bool) {
	n.mu.Lock()
	n.healthy = healthy
	n.mu.Unlock()
}

func (n *fakeNode) setLeaseStatus(status int) {
	n.mu.Lock()
	n.leaseStatus = status
	n.mu.Unlock()
}

func (n *fakeNode) setLinked(linked ...string) {
	n.mu.Lock()
	n.linked = linked
	n.mu.Unlock()
}

func (n *fakeNode) leaseRequests() []leaseRequest {
	n.mu.Lock()
	defer n.mu.Unlock()
	return append([]leaseRequest(nil), n.requests...)
}

func newPool(t *testing.T, nodes ...*fakeNode) *Pool {
	t.Helper()
	urls := make([]string, 0, len(nodes))
	for _, node := range nodes {
		urls = append(urls, node.server.URL)
	}
	pool, err := New(Config{Nodes: urls, Key: testKey, Interval: 20 * time.Millisecond, Timeout: time.Second, Logger: testLog})
	if err != nil {
		t.Fatalf("pool: %v", err)
	}
	t.Cleanup(pool.Close)
	return pool
}

func waitStatus(t *testing.T, pool *Pool, check func(Status) bool) Status {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		status := pool.Status()
		if check(status) {
			return status
		}
		if time.Now().After(deadline) {
			t.Fatalf("relay pool status did not settle: %+v", status)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestNewValidatesNodes(t *testing.T) {
	cases := []Config{
		{},
		{Key: testKey},
		{Nodes: []string{"ftp://relay.example"}, Key: testKey},
		{Nodes: []string{"http://"}, Key: testKey},
		{Nodes: []string{"http://relay.example/path"}, Key: testKey},
		{Nodes: []string{"http://relay.example/?x=1"}, Key: testKey},
		{Nodes: []string{"http://relay.example/#frag"}, Key: testKey},
		{Nodes: []string{"http://user:pass@relay.example"}, Key: testKey},
		{Nodes: []string{"http://relay.example"}},
	}
	for _, cfg := range cases {
		if _, err := New(cfg); err == nil {
			t.Fatalf("New(%+v) accepted an invalid config", cfg)
		}
	}
}

func TestNewNormalizesAndDeduplicatesNodes(t *testing.T) {
	pool, err := New(Config{
		Nodes:  []string{"HTTP://Relay.Example:8447/", "http://relay.example:8447", " https://relay2.example "},
		Key:    testKey,
		Logger: testLog,
	})
	if err != nil {
		t.Fatalf("pool: %v", err)
	}
	defer pool.Close()
	urls := pool.NodeURLs()
	if len(urls) != 2 || urls[0] != "http://relay.example:8447" || urls[1] != "https://relay2.example" {
		t.Fatalf("nodes = %v", urls)
	}
}

func TestHealthSnapshotFeedsLinked(t *testing.T) {
	node := newFakeNode(t, "node1.example")
	node.mu.Lock()
	node.leases = 3
	node.linked = []string{"devbox", "devbox2"}
	node.mu.Unlock()
	pool := newPool(t, node)
	pool.Start()

	status := waitStatus(t, pool, func(s Status) bool {
		return s.Healthy == 1 && s.Nodes[0].Checked && s.Nodes[0].Leases == 3 && s.Nodes[0].Linked == 2
	})
	if status.Nodes[0].URL != node.server.URL {
		t.Fatalf("node url = %q", status.Nodes[0].URL)
	}
	if !pool.HostLinked(node.server.URL, "devbox") || !pool.HostLinked(node.server.URL, "devbox2") {
		t.Fatal("linked snapshot is missing hosts")
	}
	if pool.HostLinked(node.server.URL, "ghost") || pool.HostLinked("http://unknown.example", "devbox") {
		t.Fatal("linked snapshot accepted an unknown host or node")
	}

	node.setHealthy(false)
	status = waitStatus(t, pool, func(s Status) bool { return !s.Nodes[0].Healthy })
	if status.Nodes[0].Error == "" || pool.HostLinked(node.server.URL, "devbox") {
		t.Fatalf("failed probe kept stale state: %+v", status)
	}

	node.setHealthy(true)
	node.setLinked("devbox")
	waitStatus(t, pool, func(s Status) bool { return s.Healthy == 1 && s.Nodes[0].Linked == 1 })
}

func TestProvisionRoundRobinAndSticky(t *testing.T) {
	first := newFakeNode(t, "node1.example")
	second := newFakeNode(t, "node2.example")
	pool := newPool(t, first, second)

	lease, err := pool.Provision("devbox", "pk", "token", time.Minute, "")
	if err != nil {
		t.Fatalf("first provision: %v", err)
	}
	if lease.Node != first.server.URL || lease.Port == 0 || lease.Relay != fmt.Sprintf("node1.example:%d", lease.Port) {
		t.Fatalf("first lease = %+v", lease)
	}
	if lease.Carrier != "carrier.example:8448" || lease.ExpiresInSec != 120 {
		t.Fatalf("first lease fields = %+v", lease)
	}
	lease, err = pool.Provision("devbox2", "pk", "token", time.Minute, "")
	if err != nil {
		t.Fatalf("second provision: %v", err)
	}
	if lease.Node != second.server.URL {
		t.Fatalf("second lease went to %s, want %s", lease.Node, second.server.URL)
	}

	// A preferred node wins even when round robin would pick the other one.
	lease, err = pool.Provision("devbox", "pk", "token", time.Minute, first.server.URL)
	if err != nil {
		t.Fatalf("sticky provision: %v", err)
	}
	if lease.Node != first.server.URL {
		t.Fatalf("sticky lease went to %s", lease.Node)
	}

	requests := first.leaseRequests()
	if len(requests) != 2 {
		t.Fatalf("first node saw %d leases", len(requests))
	}
	want := leaseRequest{ID: "devbox", PublicKey: "pk", CarrierToken: "token", TTLSeconds: 60}
	if requests[1] != want {
		t.Fatalf("lease request = %+v, want %+v", requests[1], want)
	}
	first.mu.Lock()
	badAuth := first.badAuth
	first.mu.Unlock()
	second.mu.Lock()
	badAuth += second.badAuth
	second.mu.Unlock()
	if badAuth != 0 {
		t.Fatalf("%d requests used the wrong control key", badAuth)
	}
}

func TestPreviewNextHealthyNode(t *testing.T) {
	first := newFakeNode(t, "node1.example")
	second := newFakeNode(t, "node2.example")
	pool := newPool(t, first, second)
	pool.Start()
	waitStatus(t, pool, func(s Status) bool { return s.Healthy == 2 })

	node, ok := pool.Preview()
	if !ok || node.URL != first.server.URL {
		t.Fatalf("preview = %+v (ok=%v)", node, ok)
	}
	// Preview must not consume the rotation.
	lease, err := pool.Provision("devbox", "pk", "token", time.Minute, "")
	if err != nil {
		t.Fatalf("provision: %v", err)
	}
	if lease.Node != first.server.URL {
		t.Fatalf("provision after preview went to %s", lease.Node)
	}
	if node, ok := pool.Preview(); !ok || node.URL != second.server.URL {
		t.Fatalf("second preview = %+v (ok=%v)", node, ok)
	}

	first.setHealthy(false)
	waitStatus(t, pool, func(s Status) bool { return !s.Nodes[0].Healthy })
	if node, ok := pool.Preview(); !ok || node.URL != second.server.URL {
		t.Fatalf("preview with a failed node = %+v (ok=%v)", node, ok)
	}
	second.setHealthy(false)
	waitStatus(t, pool, func(s Status) bool { return s.Healthy == 0 })
	if node, ok := pool.Preview(); ok {
		t.Fatalf("preview with no healthy node = %+v", node)
	}
}

func TestProvisionFailsOverAndSkipsUnhealthy(t *testing.T) {
	first := newFakeNode(t, "node1.example")
	second := newFakeNode(t, "node2.example")
	first.setLeaseStatus(http.StatusInternalServerError)
	pool := newPool(t, first, second)

	lease, err := pool.Provision("devbox", "pk", "token", time.Minute, "")
	if err != nil {
		t.Fatalf("provision: %v", err)
	}
	if lease.Node != second.server.URL {
		t.Fatalf("lease went to %s after the first node failed", lease.Node)
	}
	status := pool.Status()
	for _, node := range status.Nodes {
		switch node.URL {
		case first.server.URL:
			if node.Healthy || !node.Checked || node.Error == "" {
				t.Fatalf("failed node status = %+v", node)
			}
		case second.server.URL:
			if !node.Healthy {
				t.Fatalf("serving node status = %+v", node)
			}
		}
	}

	lease, err = pool.Provision("devbox2", "pk", "token", time.Minute, first.server.URL)
	if err != nil {
		t.Fatalf("second provision: %v", err)
	}
	if lease.Node != second.server.URL {
		t.Fatalf("unhealthy node was retried: %s", lease.Node)
	}

	first.setLeaseStatus(0)
	pool.Start()
	waitStatus(t, pool, func(s Status) bool {
		for _, node := range s.Nodes {
			if node.URL == first.server.URL && node.Healthy {
				return true
			}
		}
		return false
	})
}

func TestProvisionWithoutHealthyNode(t *testing.T) {
	only := newFakeNode(t, "node1.example")
	only.setLeaseStatus(http.StatusBadGateway)
	pool := newPool(t, only)

	if _, err := pool.Provision("devbox", "pk", "token", time.Minute, ""); err == nil ||
		!strings.Contains(err.Error(), "all relay nodes failed") {
		t.Fatalf("all-failed provision = %v", err)
	}
	if _, err := pool.Provision("devbox", "pk", "token", time.Minute, ""); err == nil ||
		!strings.Contains(err.Error(), "no healthy relay node") {
		t.Fatalf("empty-candidate provision = %v", err)
	}
}

func TestRelease(t *testing.T) {
	node := newFakeNode(t, "node1.example")
	pool := newPool(t, node)

	pool.Release(node.server.URL, "devbox")
	node.mu.Lock()
	released := append([]string(nil), node.released...)
	node.mu.Unlock()
	if len(released) != 1 || released[0] != "devbox" {
		t.Fatalf("released = %v", released)
	}

	pool.Release("http://unknown.example", "devbox")
	pool.ReleaseAsync(node.server.URL, "devbox2")
	deadline := time.Now().Add(2 * time.Second)
	for {
		node.mu.Lock()
		released = append([]string(nil), node.released...)
		node.mu.Unlock()
		if len(released) == 2 && released[1] == "devbox2" {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("async release did not arrive: %v", released)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestTLSNodeUsesConfiguredRoots(t *testing.T) {
	node := newNode(t, "node1.example", true)

	untrusted, err := New(Config{Nodes: []string{node.server.URL}, Key: testKey, Interval: 20 * time.Millisecond, Timeout: time.Second, Logger: testLog})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(untrusted.Close)
	untrusted.Start()
	status := waitStatus(t, untrusted, func(s Status) bool { return s.Nodes[0].Checked })
	if status.Nodes[0].Healthy || status.Nodes[0].Error == "" {
		t.Fatalf("self-signed node verified without a CA: %+v", status.Nodes[0])
	}

	roots := x509.NewCertPool()
	roots.AddCert(node.server.Certificate())
	pool, err := New(Config{
		Nodes: []string{node.server.URL}, Key: testKey, Interval: 20 * time.Millisecond,
		Timeout: time.Second, Logger: testLog, TLS: &tls.Config{RootCAs: roots},
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(pool.Close)
	pool.Start()
	waitStatus(t, pool, func(s Status) bool { return s.Nodes[0].Checked && s.Nodes[0].Healthy })

	lease, err := pool.Provision("devbox", "pk", "token", time.Minute, "")
	if err != nil {
		t.Fatalf("provision over TLS: %v", err)
	}
	if lease.Node != node.server.URL || lease.Port == 0 {
		t.Fatalf("lease = %+v", lease)
	}
}

func TestCloseIsIdempotent(t *testing.T) {
	node := newFakeNode(t, "node1.example")
	pool := newPool(t, node)
	pool.Start()
	pool.Close()
	pool.Close()
	if _, err := pool.Provision("devbox", "pk", "token", time.Minute, ""); err == nil {
		t.Fatal("closed pool provisioned a lease")
	}
}
