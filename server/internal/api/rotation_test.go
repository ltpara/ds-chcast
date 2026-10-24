package api_test

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type rotateReply struct {
	ID      string   `json:"id"`
	Relay   string   `json:"relay"`
	Relays  []string `json:"relays"`
	Carrier string   `json:"carrier"`
}

func registerFrom(t *testing.T, s *testutil.Server, body, forwardedFor string) (int, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, s.URL+"/v1/hosts", strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+testutil.ServerKey)
	req.Header.Set("Content-Type", "application/json")
	if forwardedFor != "" {
		req.Header.Set("X-Forwarded-For", forwardedFor)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, data
}

func relayHostOf(t *testing.T, endpoint string) string {
	t.Helper()
	host, _, err := net.SplitHostPort(endpoint)
	if err != nil {
		t.Fatalf("relay endpoint %q: %v", endpoint, err)
	}
	return host
}

func TestRelayRotationAndStickyLease(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RelayServers: []string{"relay-a.example", "Relay-B.Example"}})
	now := time.Now().Unix()
	register := func(id, secret string) rotateReply {
		t.Helper()
		status, body := registerFrom(t, s, registerBody(t, id, secret, "", now), "")
		if status != http.StatusOK {
			t.Fatalf("register %s: %d %s", id, status, body)
		}
		var reply rotateReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatal(err)
		}
		return reply
	}

	alpha := register("alpha", "alpha horse")
	if len(alpha.Relays) != 2 {
		t.Fatalf("relays: %+v", alpha)
	}
	alphaPort := relayPortOf(t, alpha.Relay)
	if got := relayHostOf(t, alpha.Relay); got != "relay-a.example" {
		t.Fatalf("first assignment = %q, want relay-a.example", got)
	}
	for _, endpoint := range alpha.Relays {
		if relayPortOf(t, endpoint) != alphaPort {
			t.Fatalf("relay list ports differ from %d: %+v", alphaPort, alpha.Relays)
		}
	}
	if got := relayHostOf(t, alpha.Relays[1]); got != "relay-b.example" {
		t.Fatalf("second relay list entry = %q", got)
	}
	if !strings.HasPrefix(alpha.Carrier, "relay-a.example:") {
		t.Fatalf("carrier did not use the assigned relay host: %q", alpha.Carrier)
	}

	// A renewal keeps the lease on the same relay even though the list rotates.
	renewed := register("alpha", "alpha horse")
	if renewed.Relay != alpha.Relay || renewed.Carrier != alpha.Carrier {
		t.Fatalf("renewal moved the lease: %+v -> %+v", alpha, renewed)
	}
	if len(renewed.Relays) != 2 || renewed.Relays[0] != alpha.Relays[0] {
		t.Fatalf("renewal relay list: %+v", renewed.Relays)
	}

	beta := register("beta", "beta horse")
	if got := relayHostOf(t, beta.Relay); got != "relay-b.example" {
		t.Fatalf("second host assignment = %q, want relay-b.example", got)
	}

	lease, ok := s.Registry.Lookup("beta")
	if !ok || lease.RelayHost != "relay-b.example" || lease.RelayPort != relayPortOf(t, beta.Relay) {
		t.Fatalf("stored lease: %+v (found=%v)", lease, ok)
	}

	// Lookup and the carrier endpoint follow the lease, not the default host.
	if err := s.Registry.Touch("beta", "198.51.100.9:5000", []string{"192.168.1.9:5000"}, "port-restricted"); err != nil {
		t.Fatal(err)
	}
	_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/beta", "", "")
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if host.Relay != beta.Relay || !strings.HasPrefix(host.Carrier, "relay-b.example:") {
		t.Fatalf("lookup after rotation: %+v", host)
	}

	_, infoBody := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	var info struct {
		RelayServers []string `json:"relay_servers"`
	}
	if err := json.Unmarshal(infoBody, &info); err != nil {
		t.Fatal(err)
	}
	if len(info.RelayServers) != 2 || info.RelayServers[0] != "relay-a.example" || info.RelayServers[1] != "relay-b.example" {
		t.Fatalf("server info relays: %+v", info.RelayServers)
	}

	_, statsBody := request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
	var stats struct {
		RelayServers []string `json:"relay_servers"`
		IPChanges    struct {
			Tracked int    `json:"tracked"`
			Changes uint64 `json:"changes"`
		} `json:"ip_changes"`
	}
	if err := json.Unmarshal(statsBody, &stats); err != nil {
		t.Fatal(err)
	}
	if len(stats.RelayServers) != 2 {
		t.Fatalf("stats relays: %+v", stats.RelayServers)
	}

	_, metrics := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey)
	if !strings.Contains(string(metrics), "chast_relay_servers 2") {
		t.Fatalf("metrics relay servers:\n%s", metrics)
	}

	// Freeing a relay port lets a new host wrap the rotation back to the first
	// address.
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/beta", "", testutil.ServerKey); status != http.StatusNoContent {
		t.Fatalf("delete beta: %d %s", status, body)
	}
	gamma := register("gamma", "gamma horse")
	if got := relayHostOf(t, gamma.Relay); got != "relay-a.example" {
		t.Fatalf("wrapped assignment = %q, want relay-a.example", got)
	}
}

func relayPortOf(t *testing.T, endpoint string) int {
	t.Helper()
	_, port, err := net.SplitHostPort(endpoint)
	if err != nil {
		t.Fatalf("relay endpoint %q: %v", endpoint, err)
	}
	n, err := strconv.Atoi(port)
	if err != nil {
		t.Fatalf("relay port %q: %v", port, err)
	}
	return n
}

func TestRegistrationIPChangeAudit(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{ProxyHeader: "X-Forwarded-For"})
	now := time.Now().Unix()
	body := registerBody(t, "devbox", "correct horse", "AB12", now)
	register := func(ip string) {
		t.Helper()
		if status, data := registerFrom(t, s, body, ip); status != http.StatusOK {
			t.Fatalf("register from %s: %d %s", ip, status, data)
		}
	}

	ipStats := func() (tracked int, changes uint64) {
		t.Helper()
		_, data := request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
		var stats struct {
			IPChanges struct {
				Tracked int    `json:"tracked"`
				Changes uint64 `json:"changes"`
			} `json:"ip_changes"`
		}
		if err := json.Unmarshal(data, &stats); err != nil {
			t.Fatal(err)
		}
		return stats.IPChanges.Tracked, stats.IPChanges.Changes
	}

	register("203.0.113.5")
	if tracked, changes := ipStats(); tracked != 0 || changes != 0 {
		t.Fatalf("first observation should only seed the baseline: tracked=%d changes=%d", tracked, changes)
	}
	register("203.0.113.5")
	if tracked, changes := ipStats(); tracked != 0 || changes != 0 {
		t.Fatalf("same address should not count: tracked=%d changes=%d", tracked, changes)
	}
	register("198.51.100.7")
	if tracked, changes := ipStats(); tracked != 1 || changes != 1 {
		t.Fatalf("after the address change: tracked=%d changes=%d", tracked, changes)
	}

	entries := s.IPChanges.Snapshot(0, 10)
	if len(entries) != 1 || entries[0].ID != "devbox" || len(entries[0].IPs) != 2 {
		t.Fatalf("audit entries: %+v", entries)
	}

	_, metrics := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey)
	text := string(metrics)
	for _, want := range []string{"chast_ip_change_events_total 1", "chast_ip_change_tracked 1"} {
		if !strings.Contains(text, want) {
			t.Fatalf("metrics missing %q:\n%s", want, text)
		}
	}
}
