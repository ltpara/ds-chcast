package relaynode

import (
	"bufio"
	"bytes"
	"crypto/ed25519"
	"encoding/json"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const testNodeKey = "node-secret"

var testLog = slog.New(slog.NewTextHandler(io.Discard, nil))

type testNode struct {
	server *Server
	relay  *relay.Manager
	auth   *Authenticator
	http   *httptest.Server
}

func startTestNode(t *testing.T, ports []int) *testNode {
	t.Helper()
	auth := NewAuthenticator()
	m, err := relay.NewManager("127.0.0.1", ports, auth, testLog)
	if err != nil {
		t.Fatalf("relay manager: %v", err)
	}
	s, err := New(Config{
		NodeID:    "relay-1",
		Advertise: "127.0.0.1",
		Key:       testNodeKey,
		Logger:    testLog,
	}, m, auth)
	if err != nil {
		m.Close()
		t.Fatalf("relay node: %v", err)
	}
	ts := httptest.NewServer(s.Handler())
	t.Cleanup(func() {
		ts.Close()
		s.Close()
		m.Close()
	})
	return &testNode{server: s, relay: m, auth: auth, http: ts}
}

func call(t *testing.T, ts *httptest.Server, method, path, key string, body any) (int, []byte) {
	t.Helper()
	var payload []byte
	switch v := body.(type) {
	case nil:
	case []byte:
		payload = v
	default:
		var err error
		payload, err = json.Marshal(v)
		if err != nil {
			t.Fatalf("marshal request: %v", err)
		}
	}
	req, err := http.NewRequest(method, ts.URL+path, bytes.NewReader(payload))
	if err != nil {
		t.Fatalf("request: %v", err)
	}
	if key != "" {
		req.Header.Set("Authorization", "Bearer "+key)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("%s %s: %v", method, path, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read response: %v", err)
	}
	return resp.StatusCode, data
}

func provisionLease(t *testing.T, ts *httptest.Server, id, pub, token string, ttl int) Lease {
	t.Helper()
	status, body := call(t, ts, http.MethodPost, "/v1/leases", testNodeKey, leaseRequest{
		ID:           id,
		PublicKey:    pub,
		CarrierToken: token,
		TTLSeconds:   int64(ttl),
	})
	if status != http.StatusOK {
		t.Fatalf("provision %s: status %d: %s", id, status, body)
	}
	var lease Lease
	if err := json.Unmarshal(body, &lease); err != nil {
		t.Fatalf("decode lease: %v", err)
	}
	return lease
}

func (n *testNode) health(t *testing.T) Health {
	t.Helper()
	status, body := call(t, n.http, http.MethodGet, "/healthz", "", nil)
	if status != http.StatusOK {
		t.Fatalf("health: status %d: %s", status, body)
	}
	var health Health
	if err := json.Unmarshal(body, &health); err != nil {
		t.Fatalf("decode health: %v", err)
	}
	return health
}

func waitHealth(t *testing.T, n *testNode, check func(Health) bool) Health {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		health := n.health(t)
		if check(health) {
			return health
		}
		if time.Now().After(deadline) {
			t.Fatalf("health did not settle: %+v", health)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func TestNewValidatesConfig(t *testing.T) {
	auth := NewAuthenticator()
	m, err := relay.NewManager("127.0.0.1", []int{0}, auth, testLog)
	if err != nil {
		t.Fatalf("relay manager: %v", err)
	}
	defer m.Close()

	cases := []struct {
		name string
		cfg  Config
		auth *Authenticator
		mgr  *relay.Manager
	}{
		{name: "no manager", cfg: Config{Advertise: "127.0.0.1", Key: "k"}, auth: auth},
		{name: "no auth", cfg: Config{Advertise: "127.0.0.1", Key: "k"}, mgr: m},
		{name: "no key", cfg: Config{Advertise: "127.0.0.1"}, auth: auth, mgr: m},
		{name: "no advertise", cfg: Config{Key: "k"}, auth: auth, mgr: m},
		{name: "port in advertise", cfg: Config{Advertise: "127.0.0.1:41000", Key: "k"}, auth: auth, mgr: m},
		{name: "long node id", cfg: Config{Advertise: "127.0.0.1", Key: "k", NodeID: string(make([]byte, 65))}, auth: auth, mgr: m},
	}
	for _, tc := range cases {
		if _, err := New(tc.cfg, tc.mgr, tc.auth); err == nil {
			t.Fatalf("%s: New accepted an invalid config", tc.name)
		}
	}
}

func TestNormalizeAdvertise(t *testing.T) {
	cases := []struct{ in, want string }{
		{"127.0.0.1", "127.0.0.1"},
		{" 127.0.0.1 ", "127.0.0.1"},
		{"[::1]", "::1"},
		{"Relay.Example.COM", "relay.example.com"},
	}
	for _, tc := range cases {
		got, err := normalizeAdvertise(tc.in)
		if err != nil || got != tc.want {
			t.Fatalf("normalizeAdvertise(%q) = %q, %v", tc.in, got, err)
		}
	}
}

func TestControlAPIRefusesMissingKey(t *testing.T) {
	n := startTestNode(t, []int{0})
	_, pub := testKey("host-a")
	body, _ := json.Marshal(leaseRequest{ID: "devbox", PublicKey: pub, CarrierToken: "t", TTLSeconds: 60})

	for _, method := range []string{http.MethodPost, http.MethodGet} {
		path := "/v1/leases"
		if method == http.MethodGet {
			path = "/v1/stats"
		}
		if status, _ := call(t, n.http, method, path, "", body); status != http.StatusUnauthorized {
			t.Fatalf("%s %s without key = %d", method, path, status)
		}
		if status, _ := call(t, n.http, method, path, "wrong-key", body); status != http.StatusUnauthorized {
			t.Fatalf("%s %s with wrong key = %d", method, path, status)
		}
	}

	status, data := call(t, n.http, http.MethodGet, "/healthz", "", nil)
	if status != http.StatusOK {
		t.Fatalf("public health = %d", status)
	}
	var health Health
	if err := json.Unmarshal(data, &health); err != nil || !health.OK || health.Node != "relay-1" {
		t.Fatalf("health = %+v, %v", health, err)
	}

	status, data = call(t, n.http, http.MethodGet, "/v1/stats", testNodeKey, nil)
	if status != http.StatusOK {
		t.Fatalf("stats = %d: %s", status, data)
	}
	var stats Stats
	if err := json.Unmarshal(data, &stats); err != nil || stats.Node != "relay-1" || stats.Leases != 0 {
		t.Fatalf("stats = %+v, %v", stats, err)
	}
}

func TestProvisionRenewsAndReleases(t *testing.T) {
	n := startTestNode(t, []int{0, 0})
	_, pub := testKey("host-a")

	first := provisionLease(t, n.http, "devbox", pub, "carrier-a", 60)
	if first.Port == 0 || first.Relay != net.JoinHostPort("127.0.0.1", strconv.Itoa(first.Port)) {
		t.Fatalf("lease = %+v", first)
	}
	if first.ExpiresInSec <= 0 || first.ExpiresInSec > 60 {
		t.Fatalf("expires_in_sec = %d", first.ExpiresInSec)
	}
	if first.Carrier != "" {
		t.Fatalf("carrier = %q without a shared carrier", first.Carrier)
	}

	renewed := provisionLease(t, n.http, "devbox", pub, "carrier-b", 60)
	if renewed.Port != first.Port {
		t.Fatalf("renewal moved the lease from %d to %d", first.Port, renewed.Port)
	}
	if health := n.health(t); health.Leases != 1 || !n.auth.VerifyCarrier("devbox", "carrier-b") {
		t.Fatalf("renewal did not replace the token: %+v", health)
	}

	_, otherPub := testKey("host-b")
	status, body := call(t, n.http, http.MethodPost, "/v1/leases", testNodeKey, leaseRequest{
		ID: "devbox", PublicKey: otherPub, CarrierToken: "carrier-c", TTLSeconds: 60,
	})
	if status != http.StatusConflict {
		t.Fatalf("key conflict = %d: %s", status, body)
	}
	if !n.auth.VerifyCarrier("devbox", "carrier-b") {
		t.Fatal("rejected renewal changed the live lease")
	}

	second := provisionLease(t, n.http, "devbox2", otherPub, "carrier-d", 60)
	if second.Port == first.Port {
		t.Fatalf("two hosts share port %d", first.Port)
	}

	status, body = call(t, n.http, http.MethodDelete, "/v1/leases/devbox", testNodeKey, nil)
	if status != http.StatusNoContent {
		t.Fatalf("release = %d: %s", status, body)
	}
	if health := n.health(t); health.Leases != 1 {
		t.Fatalf("lease count after release = %d", health.Leases)
	}
	if n.auth.HostOnline("devbox") {
		t.Fatal("released lease is still authenticated")
	}
	if status, _ := call(t, n.http, http.MethodDelete, "/v1/leases/devbox", testNodeKey, nil); status != http.StatusNoContent {
		t.Fatalf("second release = %d", status)
	}

	again := provisionLease(t, n.http, "devbox", pub, "carrier-e", 60)
	if again.Port != first.Port {
		t.Fatalf("released port %d was not reused, got %d", first.Port, again.Port)
	}
}

func TestProvisionValidation(t *testing.T) {
	n := startTestNode(t, []int{0, 0})
	_, pub := testKey("host-a")

	cases := []struct {
		name string
		body any
		want int
	}{
		{"bad id", leaseRequest{ID: "bad id!", PublicKey: pub, CarrierToken: "t", TTLSeconds: 60}, http.StatusBadRequest},
		{"empty id", leaseRequest{PublicKey: pub, CarrierToken: "t", TTLSeconds: 60}, http.StatusBadRequest},
		{"bad key", leaseRequest{ID: "devbox", PublicKey: "nope", CarrierToken: "t", TTLSeconds: 60}, http.StatusBadRequest},
		{"empty token", leaseRequest{ID: "devbox", PublicKey: pub, TTLSeconds: 60}, http.StatusBadRequest},
		{"long token", leaseRequest{ID: "devbox", PublicKey: pub, CarrierToken: string(make([]byte, 129)), TTLSeconds: 60}, http.StatusBadRequest},
		{"unknown field", []byte(`{"id":"devbox","public_key":"` + pub + `","carrier_token":"t","ttl_sec":60,"x":1}`), http.StatusBadRequest},
		{"trailing data", []byte(`{"id":"devbox","public_key":"` + pub + `","carrier_token":"t","ttl_sec":60}{}`), http.StatusBadRequest},
		{"not json", []byte(`hello`), http.StatusBadRequest},
	}
	for _, tc := range cases {
		status, body := call(t, n.http, http.MethodPost, "/v1/leases", testNodeKey, tc.body)
		if status != tc.want {
			t.Fatalf("%s = %d: %s", tc.name, status, body)
		}
	}
	if n.auth.Count() != 0 {
		t.Fatalf("invalid requests created %d leases", n.auth.Count())
	}

	status, body := call(t, n.http, http.MethodDelete, "/v1/leases/bad%20id", testNodeKey, nil)
	if status != http.StatusBadRequest {
		t.Fatalf("invalid release = %d: %s", status, body)
	}

	// A zero TTL is clamped to the minimum lease instead of expiring instantly.
	lease := provisionLease(t, n.http, "devbox", pub, "t", 0)
	if lease.ExpiresInSec < int64(minLeaseTTL/time.Second)-1 {
		t.Fatalf("clamped ttl = %d", lease.ExpiresInSec)
	}
}

func TestProvisionWithoutFreePort(t *testing.T) {
	n := startTestNode(t, []int{0})
	_, pubA := testKey("host-a")
	_, pubB := testKey("host-b")
	provisionLease(t, n.http, "devbox", pubA, "t", 60)

	status, body := call(t, n.http, http.MethodPost, "/v1/leases", testNodeKey, leaseRequest{
		ID: "devbox2", PublicKey: pubB, CarrierToken: "t", TTLSeconds: 60,
	})
	if status != http.StatusServiceUnavailable {
		t.Fatalf("full node = %d: %s", status, body)
	}
	if n.auth.HostOnline("devbox2") {
		t.Fatal("refused host got a lease")
	}
}

func listenTestUDP(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func recvUDP(t *testing.T, conn *net.UDPConn) []byte {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 4096)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("receive: %v", err)
	}
	return append([]byte(nil), buf[:n]...)
}

func relayHandshake(t *testing.T, conn *net.UDPConn, relayAddr *net.UDPAddr, id string, priv ed25519.PrivateKey) wire.Message {
	t.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: id}), relayAddr)
	challenge, err := wire.Decode(recvUDP(t, conn))
	if err != nil || challenge.Type != wire.TypeRelayChallenge || challenge.Nonce == "" {
		t.Fatalf("relay challenge: %v %+v", err, challenge)
	}
	message := identity.RelayMessage(id, relayAddr.Port, challenge.Candidate, challenge.Nonce)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeRelayAuth,
		HostID:    id,
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, message),
	}), relayAddr)
	ack, err := wire.Decode(recvUDP(t, conn))
	if err != nil {
		t.Fatalf("relay ack: %v", err)
	}
	return ack
}

func TestNodeRelaysUDPWithProvisionedLease(t *testing.T) {
	n := startTestNode(t, []int{0})
	priv, pub := testKey("host-a")
	lease := provisionLease(t, n.http, "devbox", pub, "carrier", 60)

	relayAddr, err := net.ResolveUDPAddr("udp", lease.Relay)
	if err != nil {
		t.Fatalf("lease address: %v", err)
	}
	host := listenTestUDP(t)
	viewer := listenTestUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("node relay refused the host: %+v", ack)
	}

	viewer.WriteToUDP([]byte("quic-from-viewer"), relayAddr)
	if got := string(recvUDP(t, host)); got != "quic-from-viewer" {
		t.Fatalf("host received %q", got)
	}
	host.WriteToUDP([]byte("quic-from-host"), relayAddr)
	if got := string(recvUDP(t, viewer)); got != "quic-from-host" {
		t.Fatalf("viewer received %q", got)
	}

	health := n.health(t)
	if health.Leases != 1 {
		t.Fatalf("health leases = %d", health.Leases)
	}
	if len(health.Linked) != 0 {
		t.Fatalf("UDP-only host reported as carrier-linked: %v", health.Linked)
	}
	if stats := n.server.Stats(); stats.Relay.Ports[0].ToHost != uint64(len("quic-from-viewer")) {
		t.Fatalf("relay stats: %+v", stats.Relay.Ports[0])
	}
}

func openHostCarrier(t *testing.T, addr, id string, priv ed25519.PrivateKey) (*carrier.FrameWriter, *carrier.FrameReader) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial carrier: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	hello, _ := json.Marshal(carrier.HostHello{HostID: id})
	if _, err := io.WriteString(conn, carrier.MagicHost+string(hello)+"\n"); err != nil {
		t.Fatalf("write host hello: %v", err)
	}
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil {
		t.Fatalf("read challenge: %v", err)
	}
	text, ok := cutMagic(line, carrier.MagicChallenge)
	if !ok {
		t.Fatalf("host link refused: %q", line)
	}
	var challenge carrier.HostChallenge
	if err := json.Unmarshal([]byte(text), &challenge); err != nil {
		t.Fatalf("challenge: %v", err)
	}
	proof, _ := json.Marshal(carrier.HostAuth{
		HostID:    id,
		Nonce:     challenge.Nonce,
		Signature: identity.Sign(priv, identity.CarrierHostMessage(id, "", challenge.Nonce)),
	})
	if _, err := io.WriteString(conn, carrier.MagicAuth+string(proof)+"\n"); err != nil {
		t.Fatalf("write host proof: %v", err)
	}
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil || line+"\n" != carrier.MagicOK {
		t.Fatalf("host ack = %q, %v", line, err)
	}
	return carrier.NewFrameWriter(conn), carrier.NewFrameReader(br)
}

func cutMagic(s, prefix string) (string, bool) {
	if len(s) >= len(prefix) && s[:len(prefix)] == prefix {
		return s[len(prefix):], true
	}
	return "", false
}

func TestNodeSharedCarrierFollowsLease(t *testing.T) {
	n := startTestNode(t, []int{0})
	if err := n.relay.EnableSharedCarrier("127.0.0.1:0"); err != nil {
		t.Fatalf("shared carrier: %v", err)
	}
	priv, pub := testKey("host-a")
	lease := provisionLease(t, n.http, "devbox", pub, "viewer-token", 60)
	if lease.Carrier == "" {
		t.Fatal("lease did not advertise the shared carrier")
	}

	hostSend, hostRecv := openHostCarrier(t, lease.Carrier, "devbox", priv)
	waitHealth(t, n, func(h Health) bool { return len(h.Linked) == 1 && h.Linked[0] == "devbox" })

	conn, err := net.DialTimeout("tcp", lease.Carrier, 2*time.Second)
	if err != nil {
		t.Fatalf("dial carrier: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: "viewer-token"})
	if _, err := io.WriteString(conn, carrier.MagicViewer+string(hello)+"\n"); err != nil {
		t.Fatalf("write viewer hello: %v", err)
	}
	viewerSend, viewerRecv := carrier.NewFrameWriter(conn), carrier.NewFrameReader(conn)
	if err := viewerSend.WriteFrame([]byte("carrier-uplink")); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err := hostRecv.ReadFrame()
	if err != nil || string(payload) != "carrier-uplink" {
		t.Fatalf("host received %q, %v", payload, err)
	}
	if err := hostSend.WriteFrame([]byte("carrier-downlink")); err != nil {
		t.Fatalf("host send: %v", err)
	}
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	payload, err = viewerRecv.ReadFrame()
	if err != nil || string(payload) != "carrier-downlink" {
		t.Fatalf("viewer received %q, %v", payload, err)
	}

	// A viewer with a stale token cannot attach.
	bad, err := net.DialTimeout("tcp", lease.Carrier, 2*time.Second)
	if err != nil {
		t.Fatalf("dial carrier: %v", err)
	}
	defer bad.Close()
	hello, _ = json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: "stale"})
	if _, err := io.WriteString(bad, carrier.MagicViewer+string(hello)+"\n"); err != nil {
		t.Fatalf("write viewer hello: %v", err)
	}
	bad.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := bad.Read(make([]byte, 1)); err == nil {
		t.Fatal("shared carrier accepted a stale token")
	}
}
