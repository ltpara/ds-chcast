// Package tracing exports HTTP request spans to an OpenTelemetry collector
// over OTLP/HTTP JSON. It keeps the dependency list empty: the payload is
// encoded by hand and delivery is best effort, so a slow or missing collector
// never blocks application requests.
package tracing

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	randv2 "math/rand/v2"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	flushInterval = 2 * time.Second
	flushBatch    = 128
	queueSize     = 512
	exportTimeout = 5 * time.Second
)

// Config describes the collector and sampling policy. Endpoint is the
// OTLP/HTTP base URL; "/v1/traces" is appended. An empty endpoint disables
// tracing entirely and New returns nil.
type Config struct {
	Endpoint string
	Service  string
	Sample   float64
}

// Tracer records spans and ships them in batches. All methods are safe on a
// nil receiver so callers can stay branch-free when tracing is disabled.
type Tracer struct {
	endpoint string
	service  string
	sample   float64
	queue    chan span
	client   *http.Client
	done     chan struct{}
	once     sync.Once
	wg       sync.WaitGroup
}

type span struct {
	traceID  string
	spanID   string
	parentID string
	name     string
	method   string
	route    string
	status   int
	start    time.Time
	end      time.Time
	scheme   string
}

// Span is the live state returned by Begin and handed back to End. Callers
// must not reuse a Span after End.
type Span struct {
	traceID  string
	spanID   string
	parentID string
	start    time.Time
}

// New starts the exporter goroutine. A configuration without an endpoint
// returns nil, which disables every call site.
func New(cfg Config) *Tracer {
	endpoint := strings.TrimRight(strings.TrimSpace(cfg.Endpoint), "/")
	if endpoint == "" {
		return nil
	}
	service := strings.TrimSpace(cfg.Service)
	if service == "" {
		service = "chast"
	}
	sample := cfg.Sample
	if sample < 0 {
		sample = 0
	}
	if sample > 1 {
		sample = 1
	}
	t := &Tracer{
		endpoint: endpoint,
		service:  service,
		sample:   sample,
		queue:    make(chan span, queueSize),
		client:   &http.Client{Timeout: exportTimeout},
		done:     make(chan struct{}),
	}
	t.wg.Add(1)
	go t.run()
	return t
}

// Enabled reports whether spans are collected.
func (t *Tracer) Enabled() bool { return t != nil && t.sample > 0 }

// Begin starts a span for an incoming request. A valid inbound traceparent
// keeps its trace ID and sampled bit; otherwise a new trace is started when
// the local sampler selects it. The response carries a traceparent header so
// clients can correlate logs with the exported span.
func (t *Tracer) Begin(w http.ResponseWriter, r *http.Request) *Span {
	if !t.Enabled() {
		return nil
	}
	traceID, parentID, sampled, inherited := parseTraceparent(r.Header.Get("traceparent"))
	if inherited {
		if !sampled {
			return nil
		}
	} else {
		if t.sample < 1 && randv2.Float64() >= t.sample {
			return nil
		}
		traceID = newID(16)
		parentID = ""
	}
	spanID := newID(8)
	w.Header().Set("traceparent", "00-"+traceID+"-"+spanID+"-01")
	return &Span{traceID: traceID, spanID: spanID, parentID: parentID, start: time.Now()}
}

// End finishes a span. The route comes from the served mux pattern, which
// keeps span names bounded even for parameterised paths.
func (t *Tracer) End(sp *Span, r *http.Request, status int) {
	if t == nil || sp == nil {
		return
	}
	route := r.Pattern
	name := route
	if route == "" {
		route = r.URL.Path
		name = r.Method + " " + route
	} else if rest, ok := strings.CutPrefix(route, r.Method+" "); ok {
		route = rest
	}
	scheme := "http"
	if r.TLS != nil {
		scheme = "https"
	}
	out := span{
		traceID:  sp.traceID,
		spanID:   sp.spanID,
		parentID: sp.parentID,
		name:     name,
		method:   r.Method,
		route:    route,
		status:   status,
		start:    sp.start,
		end:      time.Now(),
		scheme:   scheme,
	}
	select {
	case t.queue <- out:
	default:
	}
}

// Handler wraps next with Begin/End. Use it where the served handler does not
// already integrate tracing, such as the relay control plane.
func (t *Tracer) Handler(next http.Handler) http.Handler {
	if t == nil {
		return next
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		sp := t.Begin(w, r)
		rec := &recorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		t.End(sp, r, rec.status)
	})
}

type recorder struct {
	http.ResponseWriter
	status      int
	wroteHeader bool
}

func (r *recorder) WriteHeader(status int) {
	if r.wroteHeader {
		return
	}
	r.wroteHeader = true
	r.status = status
	r.ResponseWriter.WriteHeader(status)
}

func (r *recorder) Write(p []byte) (int, error) {
	if !r.wroteHeader {
		r.wroteHeader = true
	}
	return r.ResponseWriter.Write(p)
}

// Flush keeps wrapped streaming handlers streaming.
func (r *recorder) Flush() {
	if f, ok := r.ResponseWriter.(http.Flusher); ok {
		f.Flush()
	}
}

// Close stops accepting spans, flushes what is queued and waits for the
// exporter until ctx expires.
func (t *Tracer) Close(ctx context.Context) error {
	if t == nil {
		return nil
	}
	t.once.Do(func() { close(t.done) })
	finished := make(chan struct{})
	go func() {
		t.wg.Wait()
		close(finished)
	}()
	select {
	case <-finished:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (t *Tracer) run() {
	defer t.wg.Done()
	ticker := time.NewTicker(flushInterval)
	defer ticker.Stop()
	batch := make([]span, 0, flushBatch)
	flush := func() {
		if len(batch) == 0 {
			return
		}
		t.export(batch)
		batch = batch[:0]
	}
	for {
		select {
		case s := <-t.queue:
			batch = append(batch, s)
			if len(batch) >= flushBatch {
				flush()
			}
		case <-ticker.C:
			flush()
		case <-t.done:
			for {
				select {
				case s := <-t.queue:
					batch = append(batch, s)
				default:
					flush()
					return
				}
			}
		}
	}
}

type otlpValue struct {
	StringValue *string `json:"stringValue,omitempty"`
	IntValue    *string `json:"intValue,omitempty"`
}

type otlpAttribute struct {
	Key   string    `json:"key"`
	Value otlpValue `json:"value"`
}

type otlpStatus struct {
	Code int `json:"code,omitempty"`
}

type otlpSpan struct {
	TraceID           string          `json:"traceId"`
	SpanID            string          `json:"spanId"`
	ParentSpanID      string          `json:"parentSpanId,omitempty"`
	Name              string          `json:"name"`
	Kind              int             `json:"kind"`
	StartTimeUnixNano string          `json:"startTimeUnixNano"`
	EndTimeUnixNano   string          `json:"endTimeUnixNano"`
	Attributes        []otlpAttribute `json:"attributes,omitempty"`
	Status            otlpStatus      `json:"status"`
}

type otlpScope struct {
	Name string `json:"name"`
}

type otlpScopeSpans struct {
	Scope otlpScope  `json:"scope"`
	Spans []otlpSpan `json:"spans"`
}

type otlpResource struct {
	Attributes []otlpAttribute `json:"attributes"`
}

type otlpResourceSpans struct {
	Resource   otlpResource     `json:"resource"`
	ScopeSpans []otlpScopeSpans `json:"scopeSpans"`
}

type otlpPayload struct {
	ResourceSpans []otlpResourceSpans `json:"resourceSpans"`
}

func stringValue(v string) otlpValue { return otlpValue{StringValue: &v} }

func intValue(v int) otlpValue {
	s := strconv.Itoa(v)
	return otlpValue{IntValue: &s}
}

func (t *Tracer) export(batch []span) {
	spans := make([]otlpSpan, 0, len(batch))
	for _, s := range batch {
		code := 0
		if s.status >= 500 {
			code = 2
		}
		attrs := []otlpAttribute{
			{Key: "http.request.method", Value: stringValue(s.method)},
			{Key: "http.route", Value: stringValue(s.route)},
			{Key: "url.scheme", Value: stringValue(s.scheme)},
			{Key: "http.response.status_code", Value: intValue(s.status)},
		}
		spans = append(spans, otlpSpan{
			TraceID:           s.traceID,
			SpanID:            s.spanID,
			ParentSpanID:      s.parentID,
			Name:              s.name,
			Kind:              2,
			StartTimeUnixNano: strconv.FormatInt(s.start.UnixNano(), 10),
			EndTimeUnixNano:   strconv.FormatInt(s.end.UnixNano(), 10),
			Attributes:        attrs,
			Status:            otlpStatus{Code: code},
		})
	}
	payload := otlpPayload{ResourceSpans: []otlpResourceSpans{{
		Resource: otlpResource{Attributes: []otlpAttribute{
			{Key: "service.name", Value: stringValue(t.service)},
		}},
		ScopeSpans: []otlpScopeSpans{{
			Scope: otlpScope{Name: "chast"},
			Spans: spans,
		}},
	}}}
	body, err := json.Marshal(payload)
	if err != nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), exportTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, t.endpoint+"/v1/traces", bytes.NewReader(body))
	if err != nil {
		return
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := t.client.Do(req)
	if err != nil {
		return
	}
	resp.Body.Close()
}

// parseTraceparent accepts the W3C version-00 layout and rejects all-zero
// identifiers, which the specification defines as invalid.
func parseTraceparent(value string) (traceID, parentID string, sampled, ok bool) {
	parts := strings.Split(strings.TrimSpace(value), "-")
	if len(parts) != 4 || parts[0] != "00" {
		return "", "", false, false
	}
	if len(parts[1]) != 32 || len(parts[2]) != 16 || len(parts[3]) != 2 {
		return "", "", false, false
	}
	if !lowerHex(parts[1]) || !lowerHex(parts[2]) || !lowerHex(parts[3]) {
		return "", "", false, false
	}
	if parts[1] == strings.Repeat("0", 32) || parts[2] == strings.Repeat("0", 16) {
		return "", "", false, false
	}
	flags, err := strconv.ParseUint(parts[3], 16, 8)
	if err != nil {
		return "", "", false, false
	}
	return parts[1], parts[2], flags&1 == 1, true
}

func lowerHex(s string) bool {
	for _, c := range s {
		if !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func newID(size int) string {
	buf := make([]byte, size)
	if _, err := rand.Read(buf); err != nil {
		now := time.Now().UnixNano()
		for i := range buf {
			buf[i] = byte(now >> (8 * (i % 8)))
		}
	}
	return hex.EncodeToString(buf)
}
