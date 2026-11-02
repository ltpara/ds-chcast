package rendezvous

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

type punchSpan struct {
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

type punchSpanSink struct {
	mu    sync.Mutex
	spans []punchSpan
}

func newPunchSpanSink(t *testing.T) (*punchSpanSink, *httptest.Server) {
	t.Helper()
	sink := &punchSpanSink{}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var payload struct {
			ResourceSpans []struct {
				ScopeSpans []struct {
					Spans []punchSpan `json:"spans"`
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

func (s *punchSpanSink) find(t *testing.T, name string) punchSpan {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, span := range s.spans {
		if span.Name == name {
			return span
		}
	}
	t.Fatalf("span %q missing from %+v", name, s.spans)
	return punchSpan{}
}

func punchAttr(t *testing.T, span punchSpan, key string) string {
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

func TestPunchSpanJoinsViewerContext(t *testing.T) {
	sink, srv := newPunchSpanSink(t)
	tr := tracing.New(tracing.Config{Endpoint: srv.URL, Sample: 1})
	if tr == nil {
		t.Fatal("tracer disabled")
	}
	_, _, meetAddr, priv := startMeet(t, WithTracer(tr))
	host := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}
	viewer := listenUDP(t)

	const traceID = "4bf92f3577b34da6a3ce929d0e0e4736"
	const lookupSpan = "00f067aa0ba902b7"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type:        wire.TypePunchRequest,
		HostID:      "devbox",
		Nonce:       "0123456789abcdef",
		Traceparent: "00-" + traceID + "-" + lookupSpan + "-01",
	}), meetAddr)
	if reply := recv(t, viewer); !reply.OK || reply.Type != wire.TypePunchPeer {
		t.Fatalf("punch reply: %+v", reply)
	}
	if notify := recv(t, host); notify.Type != wire.TypePunchPeer {
		t.Fatalf("host notification: %+v", notify)
	}
	// The reply is written before the serve goroutine finishes the span.
	time.Sleep(50 * time.Millisecond)

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := tr.Close(ctx); err != nil {
		t.Fatalf("close tracer: %v", err)
	}

	span := sink.find(t, "punch.request")
	if span.TraceID != traceID || span.ParentSpanID != lookupSpan {
		t.Fatalf("punch trace=%q parent=%q", span.TraceID, span.ParentSpanID)
	}
	if span.Kind != 1 {
		t.Fatalf("punch kind = %d, want internal", span.Kind)
	}
	if got := punchAttr(t, span, "punch.host_id"); got != "devbox" {
		t.Fatalf("punch.host_id = %q", got)
	}
	if got := punchAttr(t, span, "punch.viewer_ip"); got != "127.0.0.1" {
		t.Fatalf("punch.viewer_ip = %q", got)
	}
	if got := punchAttr(t, span, "punch.host_candidates"); got == "" {
		t.Fatal("punch.host_candidates is empty")
	}
}
