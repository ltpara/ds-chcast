package tracing

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

type collector struct {
	t        *testing.T
	mu       sync.Mutex
	payloads []otlpPayload
	requests int
	fail     bool
}

func newCollector(t *testing.T) (*collector, *httptest.Server) {
	t.Helper()
	c := &collector{t: t}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/traces" {
			http.NotFound(w, r)
			return
		}
		c.mu.Lock()
		defer c.mu.Unlock()
		c.requests++
		if c.fail {
			http.Error(w, "collector unavailable", http.StatusInternalServerError)
			return
		}
		var payload otlpPayload
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			t.Errorf("decode OTLP payload: %v", err)
			http.Error(w, "bad payload", http.StatusBadRequest)
			return
		}
		c.payloads = append(c.payloads, payload)
		w.WriteHeader(http.StatusOK)
	}))
	t.Cleanup(srv.Close)
	return c, srv
}

func (c *collector) spans() []otlpSpan {
	c.mu.Lock()
	defer c.mu.Unlock()
	var out []otlpSpan
	for _, payload := range c.payloads {
		for _, resource := range payload.ResourceSpans {
			for _, scope := range resource.ScopeSpans {
				out = append(out, scope.Spans...)
			}
		}
	}
	return out
}

func (c *collector) serviceName() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, payload := range c.payloads {
		for _, resource := range payload.ResourceSpans {
			for _, attr := range resource.Resource.Attributes {
				if attr.Key == "service.name" && attr.Value.StringValue != nil {
					return *attr.Value.StringValue
				}
			}
		}
	}
	return ""
}

func (c *collector) requestCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.requests
}

func closeTracer(t *testing.T, tr *Tracer) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := tr.Close(ctx); err != nil {
		t.Fatalf("close tracer: %v", err)
	}
}

func attribute(t *testing.T, attrs []otlpAttribute, key string) string {
	t.Helper()
	for _, attr := range attrs {
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
	t.Fatalf("attribute %q missing from %+v", key, attrs)
	return ""
}

func TestTracerExportsHTTPSpans(t *testing.T) {
	col, srv := newCollector(t)
	tr := New(Config{Endpoint: srv.URL, Service: "chast-test", Sample: 1})
	if tr == nil {
		t.Fatal("tracer disabled")
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v1/hosts/{id}", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})
	rec := httptest.NewRecorder()
	tr.Handler(mux).ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/hosts/devbox", nil))
	closeTracer(t, tr)

	spans := col.spans()
	if len(spans) != 1 {
		t.Fatalf("exported %d spans, want 1", len(spans))
	}
	span := spans[0]
	if len(span.TraceID) != 32 || len(span.SpanID) != 16 || span.ParentSpanID != "" {
		t.Fatalf("span ids = trace %q span %q parent %q", span.TraceID, span.SpanID, span.ParentSpanID)
	}
	if span.Name != "GET /v1/hosts/{id}" || span.Kind != 2 {
		t.Fatalf("span name/kind = %q/%d", span.Name, span.Kind)
	}
	if span.Status.Code != 0 {
		t.Fatalf("status code = %d", span.Status.Code)
	}
	if got := attribute(t, span.Attributes, "http.response.status_code"); got != "200" {
		t.Fatalf("status attribute = %q", got)
	}
	if got := attribute(t, span.Attributes, "http.route"); got != "/v1/hosts/{id}" {
		t.Fatalf("route attribute = %q", got)
	}
	if col.serviceName() != "chast-test" {
		t.Fatalf("service name = %q", col.serviceName())
	}
	header := rec.Header().Get("traceparent")
	if !strings.HasPrefix(header, "00-"+span.TraceID+"-"+span.SpanID+"-01") {
		t.Fatalf("response traceparent = %q", header)
	}
}

func TestTraceparentPropagationAndServerError(t *testing.T) {
	col, srv := newCollector(t)
	tr := New(Config{Endpoint: srv.URL, Sample: 1})
	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/hosts", func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "boom", http.StatusServiceUnavailable)
	})
	req := httptest.NewRequest(http.MethodPost, "/v1/hosts", strings.NewReader("{}"))
	req.Header.Set("traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01")
	rec := httptest.NewRecorder()
	tr.Handler(mux).ServeHTTP(rec, req)
	closeTracer(t, tr)

	spans := col.spans()
	if len(spans) != 1 {
		t.Fatalf("exported %d spans, want 1", len(spans))
	}
	span := spans[0]
	if span.TraceID != "4bf92f3577b34da6a3ce929d0e0e4736" {
		t.Fatalf("trace id = %q", span.TraceID)
	}
	if span.ParentSpanID != "00f067aa0ba902b7" {
		t.Fatalf("parent span id = %q", span.ParentSpanID)
	}
	if span.Status.Code != 2 {
		t.Fatalf("error status code = %d", span.Status.Code)
	}
	if got := attribute(t, span.Attributes, "http.response.status_code"); got != "503" {
		t.Fatalf("status attribute = %q", got)
	}
	header := rec.Header().Get("traceparent")
	if !strings.HasPrefix(header, "00-4bf92f3577b34da6a3ce929d0e0e4736-") {
		t.Fatalf("response traceparent = %q", header)
	}
}

func TestUnsampledParentIsNotRecorded(t *testing.T) {
	col, srv := newCollector(t)
	tr := New(Config{Endpoint: srv.URL, Sample: 1})
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, _ *http.Request) {})
	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	req.Header.Set("traceparent", "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00")
	rec := httptest.NewRecorder()
	tr.Handler(mux).ServeHTTP(rec, req)
	closeTracer(t, tr)

	if spans := col.spans(); len(spans) != 0 {
		t.Fatalf("unsampled parent exported %d spans", len(spans))
	}
	if header := rec.Header().Get("traceparent"); header != "" {
		t.Fatalf("unsampled parent got traceparent %q", header)
	}
}

func TestSamplingDisabled(t *testing.T) {
	col, srv := newCollector(t)
	tr := New(Config{Endpoint: srv.URL, Sample: 0})
	if tr.Enabled() {
		t.Fatal("tracer enabled at zero sample rate")
	}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v1/hosts", func(w http.ResponseWriter, _ *http.Request) {})
	rec := httptest.NewRecorder()
	tr.Handler(mux).ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/hosts", nil))
	closeTracer(t, tr)
	if spans := col.spans(); len(spans) != 0 {
		t.Fatalf("zero sample rate exported %d spans", len(spans))
	}
	if col.requestCount() != 0 {
		t.Fatalf("zero sample rate sent %d requests", col.requestCount())
	}
	if header := rec.Header().Get("traceparent"); header != "" {
		t.Fatalf("zero sample rate set traceparent %q", header)
	}
}

func TestExporterFailureIsBestEffort(t *testing.T) {
	col, srv := newCollector(t)
	col.fail = true
	tr := New(Config{Endpoint: srv.URL, Sample: 1})
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v1/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
	})
	for i := 0; i < 5; i++ {
		rec := httptest.NewRecorder()
		tr.Handler(mux).ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/healthz", nil))
		if rec.Code != http.StatusOK {
			t.Fatalf("request %d status = %d", i, rec.Code)
		}
	}
	closeTracer(t, tr)
	if col.requestCount() == 0 {
		t.Fatal("exporter never attempted delivery")
	}
}

func TestUnreachableCollectorDoesNotBlockClose(t *testing.T) {
	tr := New(Config{Endpoint: "http://127.0.0.1:1", Sample: 1})
	if tr == nil {
		t.Fatal("tracer disabled")
	}
	sp := tr.Begin(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/v1/stats", nil))
	tr.End(sp, httptest.NewRequest(http.MethodGet, "/v1/stats", nil), http.StatusOK)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	if err := tr.Close(ctx); err != nil {
		t.Fatalf("close with unreachable collector: %v", err)
	}
}

func TestNilTracerIsInert(t *testing.T) {
	var tr *Tracer
	if tr.Enabled() {
		t.Fatal("nil tracer reported enabled")
	}
	rec := httptest.NewRecorder()
	if sp := tr.Begin(rec, httptest.NewRequest(http.MethodGet, "/", nil)); sp != nil {
		t.Fatal("nil tracer returned a span")
	}
	tr.End(nil, httptest.NewRequest(http.MethodGet, "/", nil), http.StatusOK)
	if err := tr.Close(context.Background()); err != nil {
		t.Fatalf("nil tracer close: %v", err)
	}
	called := false
	tr.Handler(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		called = true
	})).ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	if !called {
		t.Fatal("nil tracer did not call the wrapped handler")
	}
}

func TestParseTraceparent(t *testing.T) {
	trace := "4bf92f3577b34da6a3ce929d0e0e4736"
	span := "00f067aa0ba902b7"
	cases := []struct {
		in      string
		sampled bool
		ok      bool
	}{
		{"00-" + trace + "-" + span + "-01", true, true},
		{"00-" + trace + "-" + span + "-00", false, true},
		{"00-" + trace + "-" + span + "-0A", false, false},
		{"01-" + trace + "-" + span + "-01", false, false},
		{"00-" + strings.Repeat("0", 32) + "-" + span + "-01", false, false},
		{"00-" + trace + "-" + strings.Repeat("0", 16) + "-01", false, false},
		{"00-" + trace + "-" + span, false, false},
		{"", false, false},
	}
	for _, tc := range cases {
		gotTrace, gotSpan, sampled, ok := parseTraceparent(tc.in)
		if ok != tc.ok || sampled != tc.sampled {
			t.Fatalf("parseTraceparent(%q) ok=%t sampled=%t", tc.in, ok, sampled)
		}
		if ok && (gotTrace != trace || gotSpan != span) {
			t.Fatalf("parseTraceparent(%q) = %q/%q", tc.in, gotTrace, gotSpan)
		}
	}
}
