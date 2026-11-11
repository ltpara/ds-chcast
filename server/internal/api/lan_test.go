package api_test

import (
	"encoding/json"
	"net"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/lanmask"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func lanMask(t *testing.T, text string) *lanmask.Mask {
	t.Helper()
	mask, err := lanmask.Parse(text)
	if err != nil {
		t.Fatalf("lan mask: %v", err)
	}
	return mask
}

// A LAN host looked up by a WAN viewer must be reported as symmetric so the
// viewer starts on the relay; the relay is still the host's assigned port.
func TestLookupForcesRelayForLANWANPair(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{
		LANMask:    lanMask(t, "127.0.0.0/8"),
		LANLocalIP: "192.168.50.10",
	})
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	// Move the host's observed mapping off the server's LAN while the lookup
	// client stays on it, which is the LAN/WAN pair hbbs forces onto the relay.
	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", nil, "asymmetric"); err != nil {
		t.Fatal(err)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if host.NATType != "symmetric" {
		t.Fatalf("LAN/WAN lookup was not forced onto the relay: %+v", host)
	}
	_, port, err := net.SplitHostPort(host.Relay)
	if err != nil {
		t.Fatalf("relay %q: %v", host.Relay, err)
	}
	want := net.JoinHostPort("192.168.50.10", port)
	if host.Relay != want {
		t.Fatalf("relay = %q, want the local address %q", host.Relay, want)
	}
	if len(host.Relays) == 0 || host.Relays[0] != want {
		t.Fatalf("relays = %v, want the local address first", host.Relays)
	}
}

// Both sides on the server's LAN keep direct candidates; only the relay host
// is rewritten to the local address.
func TestLookupKeepsDirectForLANPair(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{
		LANMask:    lanMask(t, "127.0.0.0/8"),
		LANLocalIP: "192.168.50.10",
	})
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	if err := s.Registry.Touch("devbox", "127.0.0.1:5000", nil, "asymmetric"); err != nil {
		t.Fatal(err)
	}
	_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if host.NATType == "symmetric" {
		t.Fatalf("LAN pair was forced onto the relay: %+v", host)
	}
	if host.Candidate != "127.0.0.1:5000" {
		t.Fatalf("direct candidate was hidden: %+v", host)
	}
	_, port, err := net.SplitHostPort(host.Relay)
	if err != nil {
		t.Fatalf("relay %q: %v", host.Relay, err)
	}
	if host.Relay != net.JoinHostPort("192.168.50.10", port) {
		t.Fatalf("relay = %q, want the local address", host.Relay)
	}
}

// The register reply addresses the host itself, so a LAN host receives the
// local relay address with the public endpoint still listed as a fallback.
func TestRegisterAdvertisesLocalRelayToLANHost(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{
		LANMask:    lanMask(t, "127.0.0.0/8"),
		LANLocalIP: "192.168.50.10",
	})
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg struct {
		Relay   string   `json:"relay"`
		Relays  []string `json:"relays"`
		Carrier string   `json:"carrier"`
	}
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	relayHost, port, err := net.SplitHostPort(reg.Relay)
	if err != nil {
		t.Fatalf("relay %q: %v", reg.Relay, err)
	}
	if relayHost != "192.168.50.10" {
		t.Fatalf("LAN host relay host = %q, want the local address", relayHost)
	}
	public := net.JoinHostPort("127.0.0.1", port)
	local := reg.Relay
	seen := map[string]bool{}
	for _, endpoint := range reg.Relays {
		seen[endpoint] = true
	}
	if !seen[local] || !seen[public] {
		t.Fatalf("relays = %v, want both %q and %q", reg.Relays, local, public)
	}
	carrierHost, _, err := net.SplitHostPort(reg.Carrier)
	if err != nil {
		t.Fatalf("carrier %q: %v", reg.Carrier, err)
	}
	if carrierHost != "192.168.50.10" {
		t.Fatalf("carrier host = %q, want the local address", carrierHost)
	}
	if len(reg.Relays) < 2 {
		t.Fatalf("relays = %v", reg.Relays)
	}
}
