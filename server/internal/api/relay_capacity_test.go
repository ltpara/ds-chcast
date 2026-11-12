package api_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type registerReply struct {
	ID      string   `json:"id"`
	Relay   string   `json:"relay"`
	Relays  []string `json:"relays"`
	Carrier string   `json:"carrier"`
}

func registerHost(t *testing.T, s *testutil.Server, id, secret string) (int, registerReply) {
	t.Helper()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, id, secret, "", time.Now().Unix()), testutil.ServerKey)
	var reply registerReply
	if status == http.StatusOK {
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("register %s: %v", id, err)
		}
	}
	return status, reply
}

// A one-port pool makes the capacity limit reachable: the second host must stay
// reachable by direct punching instead of failing registration the way hbbs
// never would, because hbbr relays every peer through one listener.
func TestExhaustedRelayPoolKeepsHostsOnline(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RelayPorts: []int{0}})

	status, first := registerHost(t, s, "relay-a", "secret for a")
	if status != http.StatusOK || first.Relay == "" {
		t.Fatalf("first register: %d %+v", status, first)
	}

	status, second := registerHost(t, s, "relay-b", "secret for b")
	if status != http.StatusOK {
		t.Fatalf("second register with an exhausted pool = %d, want 200", status)
	}
	if second.Relay != "" || len(second.Relays) != 0 {
		t.Fatalf("second host got a relay despite the exhausted pool: %+v", second)
	}

	// The lease exists but carries no relay, so the lookup must not advertise
	// an endpoint the host never bound.
	status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/relay-b", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if host.Relay != "" || len(host.Relays) != 0 {
		t.Fatalf("lookup advertised a relay for a direct-only lease: %+v", host)
	}

	_, metrics := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey)
	if !strings.Contains(string(metrics), "chast_relay_lease_missing_total 1") {
		t.Fatalf("metrics did not count the degraded registration:\n%s", metrics)
	}

	// Freeing the first host hands its port to the next registration.
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/relay-a", "", testutil.ServerKey); status != http.StatusNoContent {
		t.Fatalf("delete first host: %d %s", status, body)
	}
	status, again := registerHost(t, s, "relay-b", "secret for b")
	if status != http.StatusOK || again.Relay == "" {
		t.Fatalf("register after release: %d %+v", status, again)
	}
}

// Relay-only deployments have no direct path, so an exhausted pool must keep
// failing instead of registering a host that nobody can reach.
func TestRelayOnlyRefusesWhenPoolExhausted(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RelayPorts: []int{0}, RelayOnly: true})

	if status, first := registerHost(t, s, "only-a", "secret for a"); status != http.StatusOK || first.Relay == "" {
		t.Fatalf("first register: %d %+v", status, first)
	}
	if status, _ := registerHost(t, s, "only-b", "secret for b"); status != http.StatusServiceUnavailable {
		t.Fatalf("second register under relay-only = %d, want 503", status)
	}
}
