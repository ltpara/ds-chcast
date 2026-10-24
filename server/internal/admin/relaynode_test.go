package admin

import (
	"crypto/ed25519"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func startHealthNode(t *testing.T, linked ...string) (*httptest.Server, *atomic.Bool) {
	t.Helper()
	var up atomic.Bool
	up.Store(true)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/healthz" || !up.Load() {
			http.Error(w, "down", http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"ok":     true,
			"leases": 1,
			"linked": linked,
		})
	}))
	t.Cleanup(srv.Close)
	return srv, &up
}

func waitPool(t *testing.T, pool *relaypool.Pool, what string, cond func(relaypool.Status) bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond(pool.Status()) {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s: %+v", what, pool.Status())
}

func TestConsoleRelayNodes(t *testing.T) {
	s := testutil.Start(t)
	node, up := startHealthNode(t, "devbox")
	pool, err := relaypool.New(relaypool.Config{
		Nodes:    []string{node.URL},
		Key:      "node-key",
		Interval: 20 * time.Millisecond,
		Timeout:  time.Second,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	if err != nil {
		t.Fatalf("pool: %v", err)
	}
	pool.Start()
	t.Cleanup(pool.Close)
	waitPool(t, pool, "healthy node", func(status relaypool.Status) bool {
		return status.Healthy == 1
	})

	priv := identity.PrivateKeyFromSecret("node backed host")
	if _, err := s.Registry.Register(registry.RegisterRequest{
		ID:        "devbox",
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		TTL:       time.Minute,
	}); err != nil {
		t.Fatalf("register host: %v", err)
	}
	s.Registry.SetRemoteRelay("devbox", node.URL, 41001, "relay-a.example", "relay-a.example:8448")

	relays, err := relayaddr.New([]string{"127.0.0.1"})
	if err != nil {
		t.Fatalf("relay addresses: %v", err)
	}
	c := startConsole(t, Config{
		Registry:   s.Registry,
		Relay:      s.Relay,
		RelayAddrs: relays,
		RelayPool:  pool,
	})

	stats := send(t, c.Addr(), "stats")
	if !strings.Contains(stats, "relay_nodes=1 healthy=1") {
		t.Fatalf("stats output:\n%s", stats)
	}
	hosts := send(t, c.Addr(), "hosts")
	for _, want := range []string{
		"devbox online=true",
		"relay_host=relay-a.example",
		"node=" + node.URL,
		"carrier=true",
	} {
		if !strings.Contains(hosts, want) {
			t.Fatalf("hosts output missing %q:\n%s", want, hosts)
		}
	}
	rs := send(t, c.Addr(), "rs")
	for _, want := range []string{"relay_nodes=1 healthy=1", "node=" + node.URL + " status=up", "127.0.0.1"} {
		if !strings.Contains(rs, want) {
			t.Fatalf("rs output missing %q:\n%s", want, rs)
		}
	}
	if got := send(t, c.Addr(), "tg 203.0.113.1"); !strings.Contains(got, "next="+node.URL) {
		t.Fatalf("tg with a healthy node: %q", got)
	}

	up.Store(false)
	waitPool(t, pool, "node failure", func(status relaypool.Status) bool {
		return status.Healthy == 0
	})
	rs = send(t, c.Addr(), "rs")
	if !strings.Contains(rs, "status=down") {
		t.Fatalf("rs after failure:\n%s", rs)
	}
	if got := send(t, c.Addr(), "tg 203.0.113.1"); !strings.Contains(got, "next=127.0.0.1") {
		t.Fatalf("tg fallback: %q", got)
	}
}
