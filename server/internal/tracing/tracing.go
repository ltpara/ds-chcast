// Package tracing exports HTTP and data-plane spans to an OpenTelemetry
// collector over OTLP/HTTP JSON. It keeps the dependency list empty: the
// payload is encoded by hand and delivery is best effort, so a slow or missing
// collector never blocks application requests.
package tracing

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	randv2 "math/rand/v2"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	flushInterval = 2 * time.Second
	flushBatch    = 128
	queueSize     = 512
	exportTimeout = 5 * time.Second

	// Span kinds as defined by OTLP.
	kindInternal = 1
	kindServer   = 2
)

// Config describes the collector and sampling policy. Endpoint is the
// OTLP/HTTP base URL; "/v1/traces" is appended. An empty endpoint disables
// tracing entirely and New returns nil. Sample is read at startup; changing it
// requires a restart.
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
	dropped  atomic.Uint64
}

type span struct {
	traceID  string
	spanID   string
	parentID string
	name     string
	kind     int
	attrs    []otlpAttribute
	start    time.Time
	end      time.Time
	status   int
}

// Span is the live state returned by Begin, Start or StartLinked and handed
// back to End or EndSpan. Callers must not reuse a Span after finishing it.
type Span struct {
	traceID  string
	spanID   string
	parentID string
	name     string
	kind     int
	attrs    []otlpAttribute
	start    time.Time
	status   int
}

// Attribute is one span attribute. Values stay strings on the wire except for
// Int, which is exported as an OTLP intValue.
type Attribute struct {
	Key   string
	Value string
	int   bool
}

// String returns a string attribute.
func String(key, value string) Attribute { return Attribute{Key: key, Value: value} }

// Int returns an integer attribute.
func Int(key string, value int64) Attribute {
	return Attribute{Key: key, Value: strconv.FormatInt(value, 10), int: true}
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

// QueueDepth is the number of spans waiting for the exporter.
func (t *Tracer) QueueDepth() int {
	if t == nil {
		return 0
	}
	return len(t.queue)
}

// QueueCapacity is the exporter queue size.
func (t *Tracer) QueueCapacity() int {
	if t == nil {
		return 0
	}
	return cap(t.queue)
}

func (t *Tracer) keep() bool {
	if !t.Enabled() {
		return false
	}
	return t.sample >= 1 || randv2.Float64() < t.sample
}

// Start begins a new root trace for a non-HTTP operation such as a punch or a
// relay session. The span is dropped when the local sampler declines it.
func (t *Tracer) Start(name string, attrs ...Attribute) *Span {
	if !t.keep() {
		return nil
	}
	return &Span{
		traceID: newID(16),
		spanID:  newID(8),
		name:    name,
		kind:    kindInternal,
		attrs:   attributes(attrs),
		start:   time.Now(),
	}
}

// StartLinked joins an existing trace. A malformed or all-zero trace ID falls
// back to Start. Linked spans are exported whenever tracing is enabled because
// the sampling decision was already made upstream.
func (t *Tracer) StartLinked(traceID, parentSpanID, name string, attrs ...Attribute) *Span {
	if !t.Enabled() {
		return nil
	}
	if !validTraceID(traceID) {
		return t.Start(name, attrs...)
	}
	if parentSpanID != "" && (!lowerHex(parentSpanID) || parentSpanID == strings.Repeat("0", 16)) {
		parentSpanID = ""
	}
	return &Span{
		traceID:  traceID,
		spanID:   newID(8),
		parentID: parentSpanID,
		name:     name,
		kind:     kindInternal,
		attrs:    attributes(attrs),
		start:    time.Now(),
	}
}

// StartDerived starts a span on a deterministic trace while still honouring
// the local sampler. Operations without an inbound traceparent use it so a
// stable key, such as a host ID, makes their spans searchable and joins them
// into one trace across processes that derive the same ID.
func (t *Tracer) StartDerived(traceID, name string, attrs ...Attribute) *Span {
	if !t.keep() {
		return nil
	}
	if !validTraceID(traceID) {
		traceID = newID(16)
	}
	return &Span{
		traceID: traceID,
		spanID:  newID(8),
		name:    name,
		kind:    kindInternal,
		attrs:   attributes(attrs),
		start:   time.Now(),
	}
}

// TraceID is the trace the span belongs to, or "" for a nil span.
func (s *Span) TraceID() string {
	if s == nil {
		return ""
	}
	return s.traceID
}

// Add appends attributes to a span.
func (s *Span) Add(attrs ...Attribute) {
	if s == nil {
		return
	}
	s.attrs = append(s.attrs, attributes(attrs)...)
}

// SetStatus marks the span as failed when code is non-zero. OTLP uses 2 for an
// error and 0 for unset.
func (s *Span) SetStatus(code int) {
	if s == nil {
		return
	}
	s.status = code
}

// EndSpan finishes a non-HTTP span.
func (t *Tracer) EndSpan(s *Span) {
	if t == nil || s == nil {
		return
	}
	t.enqueue(span{
		traceID:  s.traceID,
		spanID:   s.spanID,
		parentID: s.parentID,
		name:     s.name,
		kind:     s.kind,
		attrs:    s.attrs,
		start:    s.start,
		end:      time.Now(),
		status:   s.status,
	})
}

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
		if !t.keep() {
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
	code := 0
	if status >= 500 {
		code = 2
	}
	t.enqueue(span{
		traceID:  sp.traceID,
		spanID:   sp.spanID,
		parentID: sp.parentID,
		name:     name,
		kind:     kindServer,
		attrs: []otlpAttribute{
			{Key: "http.request.method", Value: stringValue(r.Method)},
			{Key: "http.route", Value: stringValue(route)},
			{Key: "url.scheme", Value: stringValue(scheme)},
			{Key: "http.response.status_code", Value: intValue(status)},
		},
		start:  sp.start,
		end:    time.Now(),
		status: code,
	})
}

func (t *Tracer) enqueue(out span) {
	select {
	case t.queue <- out:
	default:
		t.dropped.Add(1)
	}
}

// DroppedSpans counts spans discarded because the exporter queue was full.
func (t *Tracer) DroppedSpans() uint64 {
	if t == nil {
		return 0
	}
	return t.dropped.Load()
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

func attributes(in []Attribute) []otlpAttribute {
	if len(in) == 0 {
		return nil
	}
	out := make([]otlpAttribute, 0, len(in))
	for _, attr := range in {
		if attr.Key == "" {
			continue
		}
		value := stringValue(attr.Value)
		if attr.int {
			v := attr.Value
			value = otlpValue{IntValue: &v}
		}
		out = append(out, otlpAttribute{Key: attr.Key, Value: value})
	}
	return out
}

func (t *Tracer) export(batch []span) {
	spans := make([]otlpSpan, 0, len(batch))
	for _, s := range batch {
		spans = append(spans, otlpSpan{
			TraceID:           s.traceID,
			SpanID:            s.spanID,
			ParentSpanID:      s.parentID,
			Name:              s.name,
			Kind:              s.kind,
			StartTimeUnixNano: strconv.FormatInt(s.start.UnixNano(), 10),
			EndTimeUnixNano:   strconv.FormatInt(s.end.UnixNano(), 10),
			Attributes:        s.attrs,
			Status:            otlpStatus{Code: s.status},
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

// DeriveTraceID builds a stable 16-byte trace ID from a namespace and a
// session identifier. Both ends of a punch or relay handshake know the same
// identifier (nonce or carrier token), so their spans land in one trace even
// though the protocol carries no trace headers. The first byte is forced
// non-zero so the result is a valid OTLP trace ID.
func DeriveTraceID(namespace, value string) string {
	sum := sha256.Sum256([]byte(namespace + "\x00" + value))
	sum[0] |= 1
	return hex.EncodeToString(sum[:16])
}

// NewSpanID returns a fresh 8-byte span ID as lowercase hex.
func NewSpanID() string { return newID(8) }

// Traceparent formats a W3C traceparent with the sampled bit set. A malformed
// trace ID yields "", which callers should treat as "do not propagate".
func Traceparent(traceID string) string {
	if !validTraceID(traceID) {
		return ""
	}
	return "00-" + traceID + "-" + newID(8) + "-01"
}

func validTraceID(value string) bool {
	return len(value) == 32 && lowerHex(value) && value != strings.Repeat("0", 32)
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
