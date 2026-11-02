package api_test

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func loopbackUDP(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func loopbackRecv(t *testing.T, conn *net.UDPConn) []byte {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	buf := make([]byte, 4096)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("receive: %v", err)
	}
	return append([]byte(nil), buf[:n]...)
}

func loopbackRecvMessage(t *testing.T, conn *net.UDPConn) wire.Message {
	t.Helper()
	msg, err := wire.Decode(loopbackRecv(t, conn))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return msg
}

// loopbackKeepalive registers the observed address of conn with the rendezvous
// server, which is what makes the host's candidate and carrier token available
// to a lookup.
func loopbackKeepalive(t *testing.T, conn *net.UDPConn, punchAddr *net.UDPAddr, id, secret string) {
	t.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: id}), punchAddr)
	challenge := loopbackRecvMessage(t, conn)
	if challenge.Type != wire.TypeKeepaliveChallenge || challenge.Nonce == "" {
		t.Fatalf("keepalive challenge: %+v", challenge)
	}
	priv := identity.PrivateKeyFromSecret(secret)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:       wire.TypeKeepaliveAuth,
		HostID:     id,
		Nonce:      challenge.Nonce,
		Candidate:  challenge.Candidate,
		Signature:  identity.Sign(priv, identity.KeepaliveMessage(id, challenge.Candidate, challenge.Nonce)),
		LocalAddrs: []string{conn.LocalAddr().String()},
		NATType:    "port-restricted",
	}), punchAddr)
	if ack := loopbackRecvMessage(t, conn); !ack.OK {
		t.Fatalf("keepalive ack: %+v", ack)
	}
}

// loopbackRelayHandshake performs hello -> challenge -> signed proof so the
// relay forwards viewer traffic to conn.
func loopbackRelayHandshake(t *testing.T, conn *net.UDPConn, relayAddr *net.UDPAddr, id, secret string) wire.Message {
	t.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: id}), relayAddr)
	challenge := loopbackRecvMessage(t, conn)
	if challenge.Type != wire.TypeRelayChallenge || challenge.Nonce == "" {
		t.Fatalf("relay challenge: %+v", challenge)
	}
	priv := identity.PrivateKeyFromSecret(secret)
	message := identity.RelayMessage(id, relayAddr.Port, challenge.Candidate, challenge.Nonce)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeRelayAuth,
		HostID:    id,
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, message),
	}), relayAddr)
	return loopbackRecvMessage(t, conn)
}

// TestLoopbackTraceCoversLookupPunchAndRelay drives one viewer through the real
// HTTP lookup, UDP punch and UDP relay paths against a single testutil server,
// then asserts the OTLP export joins them into one trace.
func TestLoopbackTraceCoversLookupPunchAndRelay(t *testing.T) {
	payloads := make(chan tracePayload, 16)
	collector := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var p tracePayload
		if err := json.NewDecoder(r.Body).Decode(&p); err == nil {
			payloads <- p
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer collector.Close()

	tracer := tracing.New(tracing.Config{Endpoint: collector.URL, Service: "chast-loopback", Sample: 1})
	if tracer == nil {
		t.Fatal("tracer is nil with an endpoint configured")
	}
	srv := testutil.StartWith(t, testutil.Options{Tracer: tracer})

	const (
		hostID     = "devbox"
		hostSecret = "host-secret"
	)
	status, body := request(t, http.MethodPost, srv.URL+"/v1/hosts",
		registerBody(t, hostID, hostSecret, "AB12", time.Now().Unix()), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}

	hostConn := loopbackUDP(t)
	viewerConn := loopbackUDP(t)
	punchAddr, err := net.ResolveUDPAddr("udp", srv.Punch)
	if err != nil {
		t.Fatal(err)
	}
	loopbackKeepalive(t, hostConn, punchAddr, hostID, hostSecret)

	lookupReq, err := http.NewRequest(http.MethodGet, srv.URL+"/v1/hosts/"+hostID, nil)
	if err != nil {
		t.Fatal(err)
	}
	lookupReq.Header.Set("Authorization", "Bearer "+testutil.ServerKey)
	lookupResp, err := http.DefaultClient.Do(lookupReq)
	if err != nil {
		t.Fatal(err)
	}
	var lookedUp struct {
		CarrierToken string `json:"carrier_token"`
		Relay        string `json:"relay"`
	}
	err = json.NewDecoder(lookupResp.Body).Decode(&lookedUp)
	lookupResp.Body.Close()
	if err != nil {
		t.Fatalf("decode lookup: %v", err)
	}
	if lookupResp.StatusCode != http.StatusOK {
		t.Fatalf("lookup status = %d", lookupResp.StatusCode)
	}
	traceparent := lookupResp.Header.Get("traceparent")
	parts := strings.Split(traceparent, "-")
	if len(parts) != 4 || parts[0] != "00" || parts[3] != "01" {
		t.Fatalf("lookup traceparent = %q", traceparent)
	}
	traceID, lookupSpanID := parts[1], parts[2]
	if lookedUp.CarrierToken == "" || lookedUp.Relay == "" {
		t.Fatalf("lookup is missing carrier_token/relay: %+v", lookedUp)
	}

	nonce := "punch-nonce-0001"
	timestamp := time.Now().Unix()
	viewerConn.WriteToUDP(wire.Encode(wire.Message{
		Type:        wire.TypePunchRequest,
		HostID:      hostID,
		Nonce:       nonce,
		Timestamp:   timestamp,
		Auth:        identity.PunchAuth(testutil.ServerKey, hostID, nonce, timestamp),
		Traceparent: traceparent,
	}), punchAddr)
	if reply := loopbackRecvMessage(t, viewerConn); !reply.OK || reply.Type != wire.TypePunchPeer {
		t.Fatalf("punch reply: %+v", reply)
	}
	if notify := loopbackRecvMessage(t, hostConn); notify.Type != wire.TypePunchPeer {
		t.Fatalf("punch notification: %+v", notify)
	}

	relayAddr, err := net.ResolveUDPAddr("udp", lookedUp.Relay)
	if err != nil {
		t.Fatalf("relay address %q: %v", lookedUp.Relay, err)
	}
	if ack := loopbackRelayHandshake(t, hostConn, relayAddr, hostID, hostSecret); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}
	viewerConn.WriteToUDP(wire.Encode(wire.Message{
		Type:        wire.TypeRelayViewer,
		HostID:      hostID,
		Token:       lookedUp.CarrierToken,
		Traceparent: traceparent,
	}), relayAddr)
	if ack := loopbackRecvMessage(t, viewerConn); !ack.OK {
		t.Fatalf("relay viewer auth: %+v", ack)
	}
	viewerConn.WriteToUDP([]byte("quic-datagram"), relayAddr)
	if got := string(loopbackRecv(t, hostConn)); got != "quic-datagram" {
		t.Fatalf("host received %q, want the relayed datagram", got)
	}

	srv.Relay.Close()
	srv.Rendezvous.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := tracer.Close(ctx); err != nil {
		t.Fatalf("close tracer: %v", err)
	}

	spans := make(map[string]traceSpan)
	var lookup traceSpan
	deadline := time.After(5 * time.Second)
	for {
		select {
		case p := <-payloads:
			for _, rs := range p.ResourceSpans {
				for _, ss := range rs.ScopeSpans {
					for _, span := range ss.Spans {
						spans[span.Name] = span
						if span.SpanID == lookupSpanID {
							lookup = span
						}
					}
				}
			}
		case <-deadline:
			t.Fatalf("exported spans are incomplete: %+v", spans)
		}
		if lookup.SpanID != "" && len(spans["punch.request"].SpanID) > 0 &&
			len(spans["relay.viewer_auth"].SpanID) > 0 && len(spans["relay.udp_session"].SpanID) > 0 {
			break
		}
	}

	if lookup.TraceID != traceID || lookup.ParentSpanID != "" || lookup.Kind != 2 {
		t.Fatalf("lookup span trace=%q parent=%q kind=%d", lookup.TraceID, lookup.ParentSpanID, lookup.Kind)
	}
	if !strings.HasPrefix(lookup.Name, "GET /v1/hosts") {
		t.Fatalf("lookup span name = %q", lookup.Name)
	}
	for _, name := range []string{"punch.request", "relay.viewer_auth", "relay.udp_session"} {
		span := spans[name]
		if span.TraceID != traceID {
			t.Fatalf("%s trace = %q, want lookup trace %q", name, span.TraceID, traceID)
		}
		if span.ParentSpanID != lookupSpanID {
			t.Fatalf("%s parent = %q, want lookup span %q", name, span.ParentSpanID, lookupSpanID)
		}
		if span.Kind != 1 {
			t.Fatalf("%s kind = %d, want 1", name, span.Kind)
		}
	}
	if got := traceAttrValue(spans["punch.request"].Attributes, "punch.host_id"); got != hostID {
		t.Fatalf("punch.host_id = %q", got)
	}
	if got := traceAttrValue(spans["punch.request"].Attributes, "punch.host_candidates"); got == "" {
		t.Fatal("punch.host_candidates attribute is empty")
	}
	if got := traceAttrValue(spans["relay.viewer_auth"].Attributes, "relay.transport"); got != "udp" {
		t.Fatalf("relay.transport = %q", got)
	}
	if got := traceAttrValue(spans["relay.udp_session"].Attributes, "relay.close_reason"); got != "shutdown" {
		t.Fatalf("relay.close_reason = %q", got)
	}
	if got := traceAttrValue(spans["relay.udp_session"].Attributes, "relay.port"); got == "" {
		t.Fatal("relay.port attribute is empty")
	}
}
