package api_test

import (
	"crypto/ed25519"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type hostReply struct {
	ID              string   `json:"id"`
	Online          bool     `json:"online"`
	Candidate       string   `json:"candidate"`
	Candidates      []string `json:"candidates"`
	Relay           string   `json:"relay"`
	Punch           string   `json:"punch"`
	Carrier         string   `json:"carrier"`
	CarrierToken    string   `json:"carrier_token"`
	Fingerprint     string   `json:"fingerprint"`
	PublicKey       string   `json:"public_key"`
	ServerPublicKey string   `json:"server_public_key"`
	Binding         string   `json:"binding"`
	LocalAddrs      []string `json:"local_addrs"`
	LastSeenMS      int64    `json:"last_seen_ms"`
	ExpiresInMS     int64    `json:"expires_in_ms"`
	NATType         string   `json:"nat_type"`
	RelayOnly       bool     `json:"relay_only"`
	Transport       string   `json:"transport"`
}

func hostKey(secret string) (ed25519.PrivateKey, string) {
	priv := identity.PrivateKeyFromSecret(secret)
	return priv, identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
}

func registerBody(t *testing.T, id, secret, fingerprint string, timestamp int64) string {
	t.Helper()
	priv, pub := hostKey(secret)
	body, err := json.Marshal(map[string]any{
		"id":          id,
		"public_key":  pub,
		"fingerprint": fingerprint,
		"timestamp":   timestamp,
		"signature":   identity.Sign(priv, identity.RegisterMessage(id, fingerprint, timestamp)),
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

func request(t *testing.T, method, url, body, key string) (int, []byte) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatal(err)
	}
	if key != "" {
		req.Header.Set("Authorization", "Bearer "+key)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
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

func TestRegisterLookupAndDelete(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()

	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg struct {
		ID              string `json:"id"`
		Relay           string `json:"relay"`
		Punch           string `json:"punch"`
		Carrier         string `json:"carrier"`
		CarrierToken    string `json:"carrier_token"`
		PublicKey       string `json:"public_key"`
		ServerPublicKey string `json:"server_public_key"`
		Binding         string `json:"binding"`
	}
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	_, pub := hostKey("correct horse")
	if reg.ID != "devbox" || reg.Relay == "" || reg.Punch != s.Punch || reg.PublicKey != pub {
		t.Fatalf("register reply: %+v", reg)
	}
	if reg.Carrier != s.Carrier || reg.CarrierToken == "" {
		t.Fatalf("register carrier: %+v", reg)
	}
	if reg.ServerPublicKey != s.Identity.PublicKeyText() {
		t.Fatalf("server public key = %q", reg.ServerPublicKey)
	}
	binding, err := identity.VerifyHostBinding(reg.Binding, s.Identity.PublicKeyText())
	if err != nil {
		t.Fatalf("binding: %v", err)
	}
	if binding.ID != "devbox" || binding.PublicKey != pub || binding.Fingerprint != "AB12" {
		t.Fatalf("binding contents: %+v", binding)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var host hostReply
	json.Unmarshal(body, &host)
	if host.Online || host.Candidate != "" {
		t.Fatalf("host without keepalive: %+v", host)
	}
	if host.Carrier != "" || host.CarrierToken != "" {
		t.Fatalf("offline lookup leaked a carrier credential: %+v", host)
	}
	if host.Relay != reg.Relay || host.Punch != reg.Punch || host.Fingerprint != "AB12" || host.PublicKey != pub {
		t.Fatalf("lookup reply: %+v", host)
	}
	if _, err := identity.VerifyHostBinding(host.Binding, s.Identity.PublicKeyText()); err != nil {
		t.Fatalf("lookup binding: %v", err)
	}

	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", []string{"192.168.1.9:5000"}, "port-restricted"); err != nil {
		t.Fatal(err)
	}
	_, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
	json.Unmarshal(body, &host)
	if !host.Online || host.Candidate != "198.51.100.9:5000" || host.LastSeenMS < 0 {
		t.Fatalf("host after keepalive: %+v", host)
	}
	if host.Carrier != s.Carrier || host.CarrierToken != reg.CarrierToken {
		t.Fatalf("online lookup carrier: %+v", host)
	}
	if len(host.Candidates) != 2 || len(host.LocalAddrs) != 1 {
		t.Fatalf("candidates: %+v", host)
	}
	if host.RelayOnly || host.Transport != "direct" {
		t.Fatalf("transport: %+v", host)
	}
	if host.ExpiresInMS <= 0 || host.ExpiresInMS > int64(time.Minute/time.Millisecond) {
		t.Fatalf("expiry = %d", host.ExpiresInMS)
	}

	status, body = request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", status, body)
	}
	// The live lease is gone but the ID -> key binding outlives it.
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
	if status != http.StatusOK {
		t.Fatalf("lookup after delete: %d %s", status, body)
	}
	json.Unmarshal(body, &host)
	if host.Online || host.PublicKey != pub || host.Binding == "" {
		t.Fatalf("offline identity after delete: %+v", host)
	}
	for _, p := range s.Relay.Stats().Ports {
		if p.HostID == "devbox" {
			t.Fatalf("relay port still reserved: %+v", p)
		}
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/identities/devbox", "", testutil.ServerKey); status != http.StatusNoContent {
		t.Fatalf("reset identity: %d %s", status, body)
	}
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", ""); status != http.StatusNotFound {
		t.Fatalf("lookup after identity reset: %d", status)
	}
}

func TestLookupRelayOnly(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RelayOnly: true})
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", []string{"192.168.1.9:5000"}, "port-restricted"); err != nil {
		t.Fatal(err)
	}
	_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if !host.Online || !host.RelayOnly {
		t.Fatalf("relay-only lookup: %+v", host)
	}
	if host.NATType != "symmetric" || host.Candidate != "" || len(host.Candidates) != 0 || len(host.LocalAddrs) != 0 {
		t.Fatalf("relay-only lookup leaked a direct path: %+v", host)
	}
	if host.Relay == "" || host.Transport != "relay" {
		t.Fatalf("relay-only transport: %+v", host)
	}
}

func registerRaw(t *testing.T, url, body string) (*http.Response, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, url+"/v1/hosts", strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+testutil.ServerKey)
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp, data
}

func TestRegistrationBlocker(t *testing.T) {
	blocker := abuse.New(abuse.Config{Window: time.Minute, MaxPerWindow: 2, IDWindow: time.Hour, MaxIDs: 10})
	s := testutil.StartWith(t, testutil.Options{Registration: blocker})
	body := registerBody(t, "devbox", "correct horse", "", time.Now().Unix())
	for i := 0; i < 2; i++ {
		if resp, data := registerRaw(t, s.URL, body); resp.StatusCode != http.StatusOK {
			t.Fatalf("registration %d: %d %s", i+1, resp.StatusCode, data)
		}
	}
	resp, data := registerRaw(t, s.URL, body)
	if resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("blocked registration: %d %s", resp.StatusCode, data)
	}
	if got := resp.Header.Get("Retry-After"); got != "1" {
		t.Fatalf("Retry-After = %q", got)
	}
	if !strings.Contains(string(data), "too many registration attempts") {
		t.Fatalf("blocked body: %s", data)
	}

	_, statsBody := request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
	var stats struct {
		Registration struct {
			TrackedIPs  int    `json:"tracked_ips"`
			RateBlocked uint64 `json:"rate_blocked"`
			IDBlocked   uint64 `json:"id_blocked"`
		} `json:"registration_blocked"`
		Punch struct {
			Stored int `json:"stored"`
		} `json:"punch_requests"`
	}
	if err := json.Unmarshal(statsBody, &stats); err != nil {
		t.Fatal(err)
	}
	if stats.Registration.TrackedIPs != 1 || stats.Registration.RateBlocked != 1 || stats.Punch.Stored != 0 {
		t.Fatalf("stats: %+v", stats)
	}

	_, metrics := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey)
	text := string(metrics)
	for _, want := range []string{
		`chast_register_blocked_total{reason="rate"} 1`,
		`chast_registration_tracked_ips 1`,
		"chast_punch_log_entries 0",
		"chast_relay_single_bandwidth_bps 134217728",
		"chast_relay_total_bandwidth_bps 1073741824",
		"chast_relay_blacklist_speed_bps 33554432",
		"chast_relay_blocked_sessions_total 0",
		"chast_relay_rate_dropped_total{unit=\"datagrams\"} 0",
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("metrics missing %q:\n%s", want, text)
		}
	}
}

func TestRegistrationIDFlood(t *testing.T) {
	blocker := abuse.New(abuse.Config{Window: time.Minute, MaxPerWindow: 50, IDWindow: time.Hour, MaxIDs: 1})
	s := testutil.StartWith(t, testutil.Options{Registration: blocker})
	now := time.Now().Unix()
	if resp, data := registerRaw(t, s.URL, registerBody(t, "devbox", "correct horse", "", now)); resp.StatusCode != http.StatusOK {
		t.Fatalf("first id: %d %s", resp.StatusCode, data)
	}
	if resp, data := registerRaw(t, s.URL, registerBody(t, "relaybox", "another horse", "", now)); resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("new id after the limit: %d %s", resp.StatusCode, data)
	}
	// Renewing an ID seen inside the window must survive the flood guard.
	if resp, data := registerRaw(t, s.URL, registerBody(t, "devbox", "correct horse", "", now)); resp.StatusCode != http.StatusOK {
		t.Fatalf("known id renewal: %d %s", resp.StatusCode, data)
	}
	if stats := blocker.Stats(); stats.IDBlocked != 1 {
		t.Fatalf("blocker stats: %+v", stats)
	}
}

func TestRuntimeRelayOnlyToggle(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if resp, data := registerRaw(t, s.URL, registerBody(t, "devbox", "correct horse", "", now)); resp.StatusCode != http.StatusOK {
		t.Fatalf("register: %d %s", resp.StatusCode, data)
	}
	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", []string{"192.168.1.9:5000"}, "port-restricted"); err != nil {
		t.Fatal(err)
	}
	lookup := func() hostReply {
		t.Helper()
		_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
		var host hostReply
		if err := json.Unmarshal(body, &host); err != nil {
			t.Fatal(err)
		}
		return host
	}
	if host := lookup(); host.RelayOnly || host.Transport != "direct" || host.Candidate == "" {
		t.Fatalf("direct lookup: %+v", host)
	}
	s.RelayOnly.Store(true)
	if host := lookup(); !host.RelayOnly || host.Transport != "relay" || host.NATType != "symmetric" ||
		host.Candidate != "" || len(host.Candidates) != 0 || len(host.LocalAddrs) != 0 {
		t.Fatalf("toggled relay-only lookup: %+v", host)
	}
	_, info := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	var serverInfo struct {
		RelayOnly bool `json:"relay_only"`
	}
	if err := json.Unmarshal(info, &serverInfo); err != nil || !serverInfo.RelayOnly {
		t.Fatalf("server info after toggle: %s (%v)", info, err)
	}
	s.RelayOnly.Store(false)
	if host := lookup(); host.RelayOnly || host.Candidate == "" || host.Transport != "direct" {
		t.Fatalf("lookup after restoring direct mode: %+v", host)
	}
}

func TestRegistrationValidation(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	valid := registerBody(t, "devbox", "correct horse", "", now)
	_, pub := hostKey("correct horse")
	badSigBody, _ := json.Marshal(map[string]any{
		"id":         "devbox",
		"public_key": pub,
		"timestamp":  now,
		"signature":  identity.Sign(identity.PrivateKeyFromSecret("another horse"), identity.RegisterMessage("devbox", "", now)),
	})
	cases := []struct {
		name   string
		body   string
		key    string
		status int
	}{
		{"no server key", valid, "", http.StatusUnauthorized},
		{"wrong server key", valid, "nope", http.StatusUnauthorized},
		{"bad id", registerBody(t, "two words", "correct horse", "", now), testutil.ServerKey, http.StatusBadRequest},
		{"missing public key", `{"id":"devbox","timestamp":` + itoa(now) + `,"signature":"x"}`, testutil.ServerKey, http.StatusBadRequest},
		{"bad public key", `{"id":"devbox","public_key":"abc","timestamp":` + itoa(now) + `,"signature":"x"}`, testutil.ServerKey, http.StatusBadRequest},
		{"stale timestamp", registerBody(t, "devbox", "correct horse", "", now-600), testutil.ServerKey, http.StatusUnauthorized},
		{"future timestamp", registerBody(t, "devbox", "correct horse", "", now+600), testutil.ServerKey, http.StatusUnauthorized},
		{"bad signature", string(badSigBody), testutil.ServerKey, http.StatusUnauthorized},
		{"unknown field", `{"id":"devbox","extra":1}`, testutil.ServerKey, http.StatusBadRequest},
		{"broken json", `{`, testutil.ServerKey, http.StatusBadRequest},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", tc.body, tc.key)
			if status != tc.status {
				t.Fatalf("status %d, want %d: %s", status, tc.status, body)
			}
		})
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", valid, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("initial register: %d %s", status, body)
	}
	renew := registerBody(t, "devbox", "correct horse", "", now)
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", renew, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("renew with the same key: %d %s", status, body)
	}
	other := registerBody(t, "devbox", "another horse", "", now)
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", other, testutil.ServerKey)
	if status != http.StatusForbidden {
		t.Fatalf("renew with a different key: %d %s", status, body)
	}
}

func TestHealthServerInfoAndStats(t *testing.T) {
	s := testutil.Start(t)
	if status, _ := request(t, http.MethodGet, s.URL+"/healthz", "", ""); status != http.StatusOK {
		t.Fatalf("health: %d", status)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	if status != http.StatusOK {
		t.Fatalf("server info: %d %s", status, body)
	}
	var info struct {
		PublicKey       string `json:"public_key"`
		Version         string `json:"version"`
		Carrier         string `json:"carrier"`
		PunchReadBuffer *int   `json:"punch_udp_read_buffer"`
		RelayReadBuffer *int   `json:"relay_udp_read_buffer"`
	}
	json.Unmarshal(body, &info)
	if info.PublicKey != s.Identity.PublicKeyText() || info.Version == "" {
		t.Fatalf("server info: %+v", info)
	}
	if info.Carrier != s.Carrier {
		t.Fatalf("server info carrier = %q, want %q", info.Carrier, s.Carrier)
	}
	if info.PunchReadBuffer == nil || info.RelayReadBuffer == nil {
		t.Fatalf("server info omits UDP receive buffers: %s", body)
	}

	if status, _ := request(t, http.MethodGet, s.URL+"/v1/stats", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("stats without key: %d", status)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("stats: %d %s", status, body)
	}
	var stats struct {
		Hosts int `json:"hosts"`
		Relay struct {
			Ports []struct {
				Port int `json:"port"`
			} `json:"ports"`
		} `json:"relay"`
	}
	if err := json.Unmarshal(body, &stats); err != nil {
		t.Fatal(err)
	}
	if len(stats.Relay.Ports) != 2 {
		t.Fatalf("relay ports in stats: %+v", stats)
	}
}

func itoa(v int64) string {
	return strconv.FormatInt(v, 10)
}
