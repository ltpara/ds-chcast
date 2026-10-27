package api_test

import (
	"crypto/ed25519"
	"encoding/json"
	"io"
	"net"
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
	Relays          []string `json:"relays"`
	Punch           string   `json:"punch"`
	Carrier         string   `json:"carrier"`
	CarrierToken    string   `json:"carrier_token"`
	Fingerprint     string   `json:"fingerprint"`
	PublicKey       string   `json:"public_key"`
	ServerPublicKey string   `json:"server_public_key"`
	Binding         string   `json:"binding"`
	LocalAddrs      []string `json:"local_addrs"`
	Hostname        string   `json:"hostname"`
	Platform        string   `json:"platform"`
	Version         string   `json:"version"`
	Alias           string   `json:"alias"`
	Tags            []string `json:"tags"`
	LastSeenMS      int64    `json:"last_seen_ms"`
	ExpiresInMS     int64    `json:"expires_in_ms"`
	NATType         string   `json:"nat_type"`
	UpnpPort        int      `json:"upnp_port"`
	RelayOnly       bool     `json:"relay_only"`
	Transport       string   `json:"transport"`
}

func hostKey(secret string) (ed25519.PrivateKey, string) {
	priv := identity.PrivateKeyFromSecret(secret)
	return priv, identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
}

func registerBody(t *testing.T, id, secret, fingerprint string, timestamp int64) string {
	t.Helper()
	return registerBodyWithInfo(t, id, secret, fingerprint, "", "", "", timestamp)
}

func registerBodyWithInfo(t *testing.T, id, secret, fingerprint, hostname, platform, version string, timestamp int64) string {
	t.Helper()
	priv, pub := hostKey(secret)
	body, err := json.Marshal(map[string]any{
		"id":          id,
		"public_key":  pub,
		"fingerprint": fingerprint,
		"hostname":    hostname,
		"platform":    platform,
		"version":     version,
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

	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("lookup without a server key = %d, want 401", status)
	}
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "wrong-key"); status != http.StatusUnauthorized {
		t.Fatalf("lookup with a wrong server key = %d, want 401", status)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
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
	_, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
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

	// A gateway mapping reported through the authenticated keepalive is part
	// of the lookup so viewers can probe the stable external port.
	priv, _ := hostKey("correct horse")
	nonce, err := s.Registry.IssueChallengeScoped("devbox", identity.DomainKeepalive, "v4")
	if err != nil {
		t.Fatal(err)
	}
	message := identity.KeepaliveMessage("devbox", "198.51.100.9:5000", nonce)
	if err := s.Registry.TouchChallenge("devbox", nonce, "198.51.100.9:5000",
		identity.Sign(priv, message), string(message), nil, "", 50000); err != nil {
		t.Fatal(err)
	}
	_, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	json.Unmarshal(body, &host)
	if host.UpnpPort != 50000 {
		t.Fatalf("upnp port = %d, want 50000", host.UpnpPort)
	}

	status, body = request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", status, body)
	}
	// The live lease is gone but the ID -> key binding outlives it.
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
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
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("lookup after identity reset: %d", status)
	}
}

func TestPeerMetadataInLookup(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()

	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBodyWithInfo(t, "office-pc", "office host key", "AB12",
			"Office PC", "windows/amd64", "1.2.3", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	lookup := func() hostReply {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/office-pc", "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("lookup: %d %s", status, body)
		}
		var host hostReply
		if err := json.Unmarshal(body, &host); err != nil {
			t.Fatal(err)
		}
		return host
	}
	host := lookup()
	if host.Hostname != "Office PC" || host.Platform != "windows/amd64" || host.Version != "1.2.3" {
		t.Fatalf("lookup metadata: %+v", host)
	}

	// A renewal without metadata keeps the earlier report.
	status, body = request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "office-pc", "office host key", "AB12", now+1), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("renew: %d %s", status, body)
	}
	host = lookup()
	if host.Hostname != "Office PC" || host.Platform != "windows/amd64" || host.Version != "1.2.3" {
		t.Fatalf("metadata after renewal: %+v", host)
	}

	// The durable record still answers after the lease is gone, like hbbs's
	// peer list for offline peers.
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/office-pc", "", testutil.ServerKey); status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", status, body)
	}
	host = lookup()
	if host.Online || host.Hostname != "Office PC" || host.Version != "1.2.3" {
		t.Fatalf("offline peer metadata: %+v", host)
	}
}

func TestBatchLookup(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	peers := []struct {
		id, secret, hostname, platform, version string
	}{
		{"alpha", "alpha host key", "Alpha", "windows/amd64", "1.0.0"},
		{"beta", "beta host key", "Beta", "linux/amd64", "2.0.0"},
	}
	for _, peer := range peers {
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBodyWithInfo(t, peer.id, peer.secret, "", peer.hostname, peer.platform, peer.version, now), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("register %s: %d %s", peer.id, status, body)
		}
	}
	if err := s.Registry.Touch("alpha", "198.51.100.7:5000", nil, "cone"); err != nil {
		t.Fatal(err)
	}

	status, body := request(t, http.MethodGet, s.URL+"/v1/hosts?ids=alpha,beta,ghost,alpha", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("batch lookup: %d %s", status, body)
	}
	var batch struct {
		Hosts   []hostReply `json:"hosts"`
		Missing []string    `json:"missing"`
	}
	if err := json.Unmarshal(body, &batch); err != nil {
		t.Fatal(err)
	}
	if len(batch.Hosts) != 2 || batch.Hosts[0].ID != "alpha" || batch.Hosts[1].ID != "beta" {
		t.Fatalf("batch hosts: %+v", batch.Hosts)
	}
	if len(batch.Missing) != 1 || batch.Missing[0] != "ghost" {
		t.Fatalf("batch missing: %+v", batch.Missing)
	}
	if !batch.Hosts[0].Online || batch.Hosts[0].Hostname != "Alpha" || batch.Hosts[0].CarrierToken == "" {
		t.Fatalf("online peer: %+v", batch.Hosts[0])
	}
	if batch.Hosts[1].Online || batch.Hosts[1].Hostname != "Beta" || batch.Hosts[1].CarrierToken != "" {
		t.Fatalf("offline peer: %+v", batch.Hosts[1])
	}
	for _, host := range batch.Hosts {
		if _, err := identity.VerifyHostBinding(host.Binding, s.Identity.PublicKeyText()); err != nil {
			t.Fatalf("binding for %s: %v", host.ID, err)
		}
	}

	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts?ids=alpha", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("batch lookup without a server key = %d, want 401", status)
	}
	for _, query := range []string{"", "?ids=", "?ids=alpha,,beta", "?ids=bad%20id", "?ids=" + strings.Repeat("a", 65)} {
		if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts"+query, "", testutil.ServerKey); status != http.StatusBadRequest {
			t.Fatalf("batch lookup %q = %d, want 400", query, status)
		}
	}

	ids := make([]string, 200)
	for i := range ids {
		ids[i] = "peer-" + strconv.Itoa(i)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts?ids="+strings.Join(ids, ","), "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("batch at the limit: %d %s", status, body)
	}
	json.Unmarshal(body, &batch)
	if len(batch.Hosts) != 0 || len(batch.Missing) != len(ids) {
		t.Fatalf("batch at the limit: %d hosts, %d missing", len(batch.Hosts), len(batch.Missing))
	}
	ids = append(ids, "peer-over")
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts?ids="+strings.Join(ids, ","), "", testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("batch over the limit = %d, want 400", status)
	}
}

func migrationBody(t *testing.T, id, oldID, secret, hostname string, timestamp, signedAt int64) string {
	t.Helper()
	priv, pub := hostKey(secret)
	body, err := json.Marshal(map[string]any{
		"id":                id,
		"public_key":        pub,
		"hostname":          hostname,
		"timestamp":         timestamp,
		"signature":         identity.Sign(priv, identity.RegisterMessage(id, "", timestamp)),
		"old_id":            oldID,
		"migrate_signature": identity.Sign(priv, identity.MigrateMessage(oldID, id, signedAt)),
	})
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

func TestHostIDMigration(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBodyWithInfo(t, "devbox", "devbox key", "AB12", "Dev Box", "windows/amd64", "1.0.0", now),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/devbox",
		`{"alias":"Workshop","tags":["lab"]}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("labels: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "taken", "another horse", "", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register taken: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		migrationBody(t, "hijack", "devbox", "another horse", "", now, now),
		testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("migration with a foreign key = %d %s, want 403", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		migrationBody(t, "newbox", "devbox", "devbox key", "", now, now-60),
		testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("migration with a stale proof = %d %s, want 401", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		migrationBody(t, "taken", "devbox", "devbox key", "", now, now),
		testutil.ServerKey); status != http.StatusForbidden {
		t.Fatalf("migration onto a taken id = %d %s, want 403", status, body)
	}

	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		migrationBody(t, "newbox", "devbox", "devbox key", "New Box", now, now),
		testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("migration: %d %s", status, body)
	}
	var reply struct {
		ID           string `json:"id"`
		MigratedFrom string `json:"migrated_from"`
		Relay        string `json:"relay"`
	}
	if err := json.Unmarshal(body, &reply); err != nil {
		t.Fatal(err)
	}
	if reply.ID != "newbox" || reply.MigratedFrom != "devbox" || reply.Relay == "" {
		t.Fatalf("migration reply: %+v", reply)
	}

	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("old id lookup = %d, want 404", status)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/newbox", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("new id lookup: %d %s", status, body)
	}
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if host.Hostname != "New Box" || host.Fingerprint != "AB12" || host.Alias != "Workshop" ||
		len(host.Tags) != 1 || host.Tags[0] != "lab" {
		t.Fatalf("migrated peer = %+v", host)
	}

	// The same request is idempotent after the server moved the record.
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		migrationBody(t, "newbox", "devbox", "devbox key", "", now, now),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("migration retry: %d %s", status, body)
	}
	priv, pub := hostKey("devbox key")
	orphanProof, err := json.Marshal(map[string]any{
		"id":                "no-old",
		"public_key":        pub,
		"timestamp":         now,
		"signature":         identity.Sign(priv, identity.RegisterMessage("no-old", "", now)),
		"migrate_signature": identity.Sign(priv, identity.MigrateMessage("devbox", "no-old", now)),
	})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", string(orphanProof), testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("migrate signature without old_id = %d %s, want 400", status, body)
	}
}

func TestIdentityLabels(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBodyWithInfo(t, "devbox", "devbox key", "AB12", "Dev Box", "windows/amd64", "1.0.0", now),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	patch := func(id, body, key string) (int, []byte) {
		t.Helper()
		return request(t, http.MethodPatch, s.URL+"/v1/identities/"+id, body, key)
	}
	lookup := func() hostReply {
		t.Helper()
		status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("lookup: %d %s", status, body)
		}
		var host hostReply
		if err := json.Unmarshal(body, &host); err != nil {
			t.Fatal(err)
		}
		return host
	}

	if status, _ := patch("devbox", `{"alias":"Studio PC"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("labels without a server key = %d, want 401", status)
	}
	if status, _ := patch("ghost", `{"alias":"Studio PC"}`, testutil.ServerKey); status != http.StatusNotFound {
		t.Fatalf("labels for an unknown peer = %d, want 404", status)
	}
	if status, _ := patch("devbox", `{}`, testutil.ServerKey); status != http.StatusBadRequest {
		t.Fatalf("empty labels = %d, want 400", status)
	}
	status, body := patch("devbox", `{"alias":"Studio PC"}`, testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("set alias: %d %s", status, body)
	}
	var labels struct {
		ID    string   `json:"id"`
		Alias string   `json:"alias"`
		Tags  []string `json:"tags"`
	}
	json.Unmarshal(body, &labels)
	if labels.ID != "devbox" || labels.Alias != "Studio PC" {
		t.Fatalf("alias response: %+v", labels)
	}
	host := lookup()
	if host.Alias != "Studio PC" || len(host.Tags) != 0 || host.Hostname != "Dev Box" {
		t.Fatalf("lookup after alias: %+v", host)
	}

	if status, body := patch("devbox", `{"tags":["office","prod","office",""]}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("set tags: %d %s", status, body)
	}
	host = lookup()
	if host.Alias != "Studio PC" || len(host.Tags) != 2 || host.Tags[0] != "office" || host.Tags[1] != "prod" {
		t.Fatalf("lookup after tags: %+v", host)
	}

	// Host metadata renewal must not wipe operator labels.
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBodyWithInfo(t, "devbox", "devbox key", "AB12", "Renamed", "windows/amd64", "2.0.0", now+1),
		testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("renew: %d %s", status, body)
	}
	host = lookup()
	if host.Alias != "Studio PC" || len(host.Tags) != 2 || host.Hostname != "Renamed" {
		t.Fatalf("labels after renewal: %+v", host)
	}

	_, body = request(t, http.MethodGet, s.URL+"/v1/hosts?ids=devbox", "", testutil.ServerKey)
	var batch struct {
		Hosts []hostReply `json:"hosts"`
	}
	json.Unmarshal(body, &batch)
	if len(batch.Hosts) != 1 || batch.Hosts[0].Alias != "Studio PC" || len(batch.Hosts[0].Tags) != 2 {
		t.Fatalf("labels in batch lookup: %+v", batch.Hosts)
	}

	if status, body := patch("devbox", `{"alias":"","tags":[]}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("clear labels: %d %s", status, body)
	}
	host = lookup()
	if host.Alias != "" || len(host.Tags) != 0 {
		t.Fatalf("labels were not cleared: %+v", host)
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
	_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
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

func TestRelayAdvertisementIncludesExtraEndpoints(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RelayExtra: []string{"::1"}})
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "correct horse", "AB12", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg struct {
		Relay  string   `json:"relay"`
		Relays []string `json:"relays"`
	}
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	if reg.Relay == "" {
		t.Fatalf("register reply has no primary relay: %s", body)
	}
	_, primaryPort, err := net.SplitHostPort(reg.Relay)
	if err != nil {
		t.Fatal(err)
	}
	if len(reg.Relays) != 2 || reg.Relays[0] != reg.Relay {
		t.Fatalf("primary relay must stay first: %+v", reg)
	}
	if reg.Relays[1] != net.JoinHostPort("::1", primaryPort) {
		t.Fatalf("register relays: %+v", reg.Relays)
	}

	_, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	var host hostReply
	if err := json.Unmarshal(body, &host); err != nil {
		t.Fatal(err)
	}
	if host.Relay != reg.Relay || len(host.Relays) != 2 || host.Relays[1] != reg.Relays[1] {
		t.Fatalf("lookup relays: primary %q, relays %+v", host.Relay, host.Relays)
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
		_, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
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

func TestSoftwareUpdateAdvertisement(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{
		SoftwareVersion: "2.1.0",
		SoftwareURL:     "https://downloads.example.com/chast-2.1.0.exe",
	})
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "update-pc", "update secret", "CD34", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	type software struct {
		Version string `json:"version"`
		URL     string `json:"url"`
	}
	var reg struct {
		Software *software `json:"software"`
	}
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	if reg.Software == nil || reg.Software.Version != "2.1.0" ||
		reg.Software.URL != "https://downloads.example.com/chast-2.1.0.exe" {
		t.Fatalf("register software = %+v", reg.Software)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	if status != http.StatusOK {
		t.Fatalf("server info: %d %s", status, body)
	}
	var info struct {
		Software *software `json:"software"`
	}
	if err := json.Unmarshal(body, &info); err != nil || info.Software == nil || info.Software.Version != "2.1.0" {
		t.Fatalf("server info software = %+v (%v)", info.Software, err)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/update-pc", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var single struct {
		ID       string    `json:"id"`
		Software *software `json:"software"`
	}
	if err := json.Unmarshal(body, &single); err != nil || single.Software == nil || single.Software.Version != "2.1.0" {
		t.Fatalf("lookup software = %+v (%v)", single.Software, err)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts?ids=update-pc", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("batch: %d %s", status, body)
	}
	var batch struct {
		Hosts    []json.RawMessage `json:"hosts"`
		Software *software         `json:"software"`
	}
	if err := json.Unmarshal(body, &batch); err != nil || batch.Software == nil || batch.Software.Version != "2.1.0" {
		t.Fatalf("batch software = %+v (%v)", batch.Software, err)
	}
	if len(batch.Hosts) != 1 {
		t.Fatalf("batch hosts = %d", len(batch.Hosts))
	}
}

func TestSoftwareUpdateAbsentByDefault(t *testing.T) {
	s := testutil.Start(t)
	_, body := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	var info map[string]any
	if err := json.Unmarshal(body, &info); err != nil {
		t.Fatal(err)
	}
	if _, ok := info["software"]; ok {
		t.Fatalf("server without --software-url advertised software: %s", body)
	}
}

func itoa(v int64) string {
	return strconv.FormatInt(v, 10)
}
