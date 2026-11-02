package relay

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/tracing"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

type relaySpan struct {
	TraceID      string `json:"traceId"`
	SpanID       string `json:"spanId"`
	ParentSpanID string `json:"parentSpanId"`
	Name         string `json:"name"`
	Kind         int    `json:"kind"`
	Attributes   []struct {
		Key   string `json:"key"`
		Value struct {
			StringValue *string `json:"stringValue"`
			IntValue    *string `json:"intValue"`
		} `json:"value"`
	} `json:"attributes"`
}

type relaySpanSink struct {
	mu    sync.Mutex
	spans []relaySpan
}

func newRelaySpanSink(t *testing.T) (*relaySpanSink, *httptest.Server) {
	t.Helper()
	sink := &relaySpanSink{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var payload struct {
			ResourceSpans []struct {
				ScopeSpans []struct {
					Spans []relaySpan `json:"spans"`
				} `json:"scopeSpans"`
			} `json:"resourceSpans"`
		}
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			t.Errorf("decode payload: %v", err)
			http.Error(w, "bad payload", http.StatusBadRequest)
			return
		}
		sink.mu.Lock()
		for _, resource := range payload.ResourceSpans {
			for _, scope := range resource.ScopeSpans {
				sink.spans = append(sink.spans, scope.Spans...)
			}
		}
		sink.mu.Unlock()
		w.WriteHeader(http.StatusOK)
	}))
	t.Cleanup(srv.Close)
	return sink, srv
}

func (s *relaySpanSink) byName(t *testing.T, name string) relaySpan {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, span := range s.spans {
		if span.Name == name {
			return span
		}
	}
	t.Fatalf("span %q missing from %+v", name, s.spans)
	return relaySpan{}
}

func relayAttr(t *testing.T, span relaySpan, key string) string {
	t.Helper()
	for _, attr := range span.Attributes {
		if attr.Key != key {
			continue
		}
		if attr.Value.StringValue != nil {
			return *attr.Value.StringValue
		}
		if attr.Value.IntValue != nil {
			return *attr.Value.IntValue
		}
	}
	t.Fatalf("attribute %q missing from %q", key, span.Name)
	return ""
}

func TestRelayTraceJoinsViewerContext(t *testing.T) {
	sink, srv := newRelaySpanSink(t)
	tr := tracing.New(tracing.Config{Endpoint: srv.URL, Sample: 1})
	if tr == nil {
		t.Fatal("tracer disabled")
	}
	m, reg, relayAddr, priv := startRelayWith(t, "devbox", "host-secret",
		WithViewerTokenRequired(true), WithTracer(tr))
	record, ok := reg.Lookup("devbox")
	if !ok || record.CarrierToken == "" {
		t.Fatal("registry did not issue a carrier token")
	}
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}

	const traceID = "4bf92f3577b34da6a3ce929d0e0e4736"
	const lookupSpan = "00f067aa0ba902b7"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type:        wire.TypeRelayViewer,
		HostID:      "devbox",
		Token:       record.CarrierToken,
		Traceparent: "00-" + traceID + "-" + lookupSpan + "-01",
	}), relayAddr)
	ack, err := wire.Decode(recv(t, viewer, 2*time.Second))
	if err != nil || !ack.OK {
		t.Fatalf("viewer token: %v %+v", err, ack)
	}
	viewer.WriteToUDP([]byte("quic-from-viewer"), relayAddr)
	if got := string(recv(t, host, 2*time.Second)); got != "quic-from-viewer" {
		t.Fatalf("host received %q", got)
	}

	m.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := tr.Close(ctx); err != nil {
		t.Fatalf("close tracer: %v", err)
	}

	auth := sink.byName(t, "relay.viewer_auth")
	if auth.TraceID != traceID || auth.ParentSpanID != lookupSpan || auth.Kind != 1 {
		t.Fatalf("viewer auth trace=%q parent=%q kind=%d", auth.TraceID, auth.ParentSpanID, auth.Kind)
	}
	if got := relayAttr(t, auth, "relay.transport"); got != "udp" {
		t.Fatalf("viewer auth transport = %q", got)
	}
	session := sink.byName(t, "relay.udp_session")
	if session.TraceID != traceID || session.ParentSpanID != lookupSpan {
		t.Fatalf("session trace=%q parent=%q, want lookup parent %q", session.TraceID, session.ParentSpanID, lookupSpan)
	}
	if got := relayAttr(t, session, "relay.close_reason"); got != "shutdown" {
		t.Fatalf("session close reason = %q", got)
	}
	if got := relayAttr(t, session, "relay.port"); got == "" {
		t.Fatal("session port attribute is empty")
	}
}
