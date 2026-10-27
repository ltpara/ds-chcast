package api_test

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
)

type traceAttr struct {
	Key   string `json:"key"`
	Value struct {
		StringValue string `json:"stringValue"`
		IntValue    string `json:"intValue"`
	} `json:"value"`
}

type traceSpan struct {
	TraceID    string      `json:"traceId"`
	SpanID     string      `json:"spanId"`
	Name       string      `json:"name"`
	Kind       int         `json:"kind"`
	Attributes []traceAttr `json:"attributes"`
	Status     struct {
		Code int `json:"code"`
	} `json:"status"`
}

type tracePayload struct {
	ResourceSpans []struct {
		Resource struct {
			Attributes []traceAttr `json:"attributes"`
		} `json:"resource"`
		ScopeSpans []struct {
			Spans []traceSpan `json:"spans"`
		} `json:"scopeSpans"`
	} `json:"resourceSpans"`
}

func traceAttrValue(attrs []traceAttr, key string) string {
	for _, a := range attrs {
		if a.Key != key {
			continue
		}
		if a.Value.StringValue != "" {
			return a.Value.StringValue
		}
		return a.Value.IntValue
	}
	return ""
}

func TestAPIExportsRequestSpans(t *testing.T) {
	payloads := make(chan tracePayload, 8)
	collector := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/traces" {
			t.Errorf("collector path = %q, want /v1/traces", r.URL.Path)
		}
		var p tracePayload
		if err := json.NewDecoder(r.Body).Decode(&p); err == nil {
			payloads <- p
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer collector.Close()

	tracer := tracing.New(tracing.Config{
		Endpoint: collector.URL,
		Service:  "chast-test",
		Sample:   1,
	})
	if tracer == nil {
		t.Fatal("tracer is nil with an endpoint configured")
	}
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		if err := tracer.Close(ctx); err != nil {
			t.Errorf("close tracer: %v", err)
		}
	}()
	srv := testutil.StartWith(t, testutil.Options{Tracer: tracer})

	req, err := http.NewRequest(http.MethodGet, srv.URL+"/v1/hosts?ids=missing", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+testutil.ServerKey)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("lookup status = %d", resp.StatusCode)
	}
	traceparent := resp.Header.Get("traceparent")
	parts := strings.Split(traceparent, "-")
	if len(parts) != 4 || parts[0] != "00" || len(parts[1]) != 32 || len(parts[2]) != 16 || parts[3] != "01" {
		t.Fatalf("traceparent = %q", traceparent)
	}

	var got *traceSpan
	var service string
	timeout := time.After(5 * time.Second)
	for got == nil {
		select {
		case p := <-payloads:
			if len(p.ResourceSpans) > 0 {
				service = traceAttrValue(p.ResourceSpans[0].Resource.Attributes, "service.name")
			}
			for _, rs := range p.ResourceSpans {
				for _, ss := range rs.ScopeSpans {
					for i := range ss.Spans {
						if ss.Spans[i].Name == "GET /v1/hosts" {
							span := ss.Spans[i]
							got = &span
						}
					}
				}
			}
		case <-timeout:
			t.Fatal("no span for GET /v1/hosts was exported")
		}
	}

	if got.TraceID != parts[1] || got.SpanID != parts[2] {
		t.Fatalf("span ids %s/%s do not match response traceparent %q", got.TraceID, got.SpanID, traceparent)
	}
	if got.Kind != 2 {
		t.Fatalf("span kind = %d, want 2 (server)", got.Kind)
	}
	if got.Status.Code != 0 {
		t.Fatalf("span status = %d, want 0", got.Status.Code)
	}
	if v := traceAttrValue(got.Attributes, "http.route"); v != "/v1/hosts" {
		t.Fatalf("http.route = %q", v)
	}
	if v := traceAttrValue(got.Attributes, "http.request.method"); v != "GET" {
		t.Fatalf("http.request.method = %q", v)
	}
	if v := traceAttrValue(got.Attributes, "http.response.status_code"); v != "200" {
		t.Fatalf("http.response.status_code = %q", v)
	}
	if service != "chast-test" {
		t.Fatalf("service.name = %q", service)
	}
}

func TestAPITracesRejectedRequests(t *testing.T) {
	payloads := make(chan tracePayload, 8)
	collector := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var p tracePayload
		if err := json.NewDecoder(r.Body).Decode(&p); err == nil {
			payloads <- p
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer collector.Close()

	tracer := tracing.New(tracing.Config{Endpoint: collector.URL, Service: "chast-test", Sample: 1})
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = tracer.Close(ctx)
	}()
	// RateLimit 0 disables the limiter, so the 401 below comes from the mux
	// handler and must appear with its route name, not as an unrouted span.
	srv := testutil.StartWith(t, testutil.Options{Tracer: tracer})

	resp, err := http.Get(srv.URL + "/v1/hosts")
	if err != nil {
		t.Fatal(err)
	}
	io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", resp.StatusCode)
	}

	timeout := time.After(5 * time.Second)
	for {
		select {
		case p := <-payloads:
			for _, rs := range p.ResourceSpans {
				for _, ss := range rs.ScopeSpans {
					for _, span := range ss.Spans {
						if span.Name != "GET /v1/hosts" {
							continue
						}
						if v := traceAttrValue(span.Attributes, "http.response.status_code"); v != "401" {
							t.Fatalf("http.response.status_code = %q", v)
						}
						if v := traceAttrValue(span.Attributes, "http.route"); v != "/v1/hosts" {
							t.Fatalf("http.route = %q", v)
						}
						return
					}
				}
			}
		case <-timeout:
			t.Fatal("no span for the unauthorized request was exported")
		}
	}
}
