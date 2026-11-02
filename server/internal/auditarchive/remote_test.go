package auditarchive

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestNewRejectsBadRemoteURL(t *testing.T) {
	store := openStore(t)
	for _, raw := range []string{
		"ftp://example.com/audit",
		"https://",
		"https://user:pass@example.com/audit",
		"https://example.com/audit?token=1",
		"https://example.com/audit#frag",
	} {
		if _, err := New(t.TempDir(), store, nil, Options{RemoteURL: raw}); err == nil {
			t.Fatalf("remote URL %q was accepted", raw)
		}
	}
	if _, err := New(t.TempDir(), store, nil, Options{RemoteToken: "token"}); err == nil {
		t.Fatal("a remote token without a URL was accepted")
	}
	archiver, err := New(t.TempDir(), store, nil, Options{RemoteURL: "https://example.com/audit/"})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	if got := archiver.Remote(); got != "https://example.com/audit" {
		t.Fatalf("remote = %q", got)
	}
}

type receivedUpload struct {
	method      string
	contentType string
	auth        string
	digest      string
	idempotency string
	body        []byte
}

type uploadRecorder struct {
	mu       sync.Mutex
	uploads  map[string]receivedUpload
	attempts int
	fail     int
}

func (u *uploadRecorder) handler(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(r.Body)
	u.mu.Lock()
	defer u.mu.Unlock()
	u.attempts++
	if u.fail > 0 {
		u.fail--
		w.WriteHeader(http.StatusInternalServerError)
		return
	}
	name := path.Base(r.URL.Path)
	u.uploads[name] = receivedUpload{
		method:      r.Method,
		contentType: r.Header.Get("Content-Type"),
		auth:        r.Header.Get("Authorization"),
		digest:      r.Header.Get("X-Chast-Audit-Sha256"),
		idempotency: r.Header.Get("Idempotency-Key"),
		body:        body,
	}
	w.WriteHeader(http.StatusNoContent)
}

func (u *uploadRecorder) snapshot() (map[string]receivedUpload, int) {
	u.mu.Lock()
	defer u.mu.Unlock()
	out := make(map[string]receivedUpload, len(u.uploads))
	for name, upload := range u.uploads {
		out[name] = upload
	}
	return out, u.attempts
}

func (u *uploadRecorder) setFail(n int) {
	u.mu.Lock()
	u.fail = n
	u.mu.Unlock()
}

func sha256Hex(body []byte) string {
	sum := sha256.Sum256(body)
	return hex.EncodeToString(sum[:])
}

func gunzipBytes(t *testing.T, body []byte) []byte {
	t.Helper()
	reader, err := gzip.NewReader(bytes.NewReader(body))
	if err != nil {
		t.Fatalf("gzip body: %v", err)
	}
	defer reader.Close()
	out, err := io.ReadAll(reader)
	if err != nil {
		t.Fatalf("read gzip body: %v", err)
	}
	return out
}

func TestRemoteDeliveryUploadsClosedFilesOnce(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	const auditBody = "{\"stream\":\"audit\",\"id\":1}\n{\"stream\":\"audit\",\"id\":2}\n"
	const connectionBody = "{\"stream\":\"connections\"}\n"
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), auditBody)
	writeFile(t, filepath.Join(dir, "connections-20261028.jsonl"), connectionBody)
	if err := compressJSONL(filepath.Join(dir, "connections-20261028.jsonl")); err != nil {
		t.Fatalf("compress: %v", err)
	}
	// Today's file is not closed yet and a foreign file is not ours; neither
	// may reach the endpoint.
	writeFile(t, filepath.Join(dir, "audit-20261029.jsonl"), "{\"stream\":\"audit\",\"id\":3}\n")
	writeFile(t, filepath.Join(dir, "notes.txt"), "keep\n")

	recorder := &uploadRecorder{uploads: map[string]receivedUpload{}}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()

	archiver, err := New(dir, store, nil, Options{
		RemoteURL:   server.URL + "/sink",
		RemoteToken: "s3cret",
	})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.DeliveryError != "" || len(result.Delivered) != 2 {
		t.Fatalf("delivery = %+v", result)
	}

	uploads, attempts := recorder.snapshot()
	if attempts != 2 || len(uploads) != 2 {
		t.Fatalf("uploads = %v after %d attempts", uploads, attempts)
	}
	audit, ok := uploads["audit-20261028.jsonl"]
	if !ok {
		t.Fatalf("audit file was not uploaded: %v", uploads)
	}
	if audit.method != http.MethodPut || audit.auth != "Bearer s3cret" ||
		audit.contentType != "application/x-ndjson" ||
		audit.idempotency != sha256Hex([]byte(auditBody)) ||
		audit.digest != sha256Hex([]byte(auditBody)) || string(audit.body) != auditBody {
		t.Fatalf("audit upload = %+v", audit)
	}
	connections, ok := uploads["connections-20261028.jsonl.gz"]
	if !ok {
		t.Fatalf("gzip file was not uploaded: %v", uploads)
	}
	if connections.contentType != "application/gzip" || connections.digest != sha256Hex(connections.body) ||
		connections.idempotency != connections.digest ||
		string(gunzipBytes(t, connections.body)) != connectionBody {
		t.Fatalf("gzip upload = %+v", connections)
	}

	for _, name := range []string{"audit-20261028.jsonl", "connections-20261028.jsonl.gz"} {
		data, err := os.ReadFile(filepath.Join(dir, name+deliveredSuffix))
		if err != nil {
			t.Fatalf("marker for %s: %v", name, err)
		}
		var marker map[string]any
		if err := json.Unmarshal(data, &marker); err != nil || marker["file"] != name ||
			marker["sha256"] == "" || marker["url"] == "" {
			t.Fatalf("marker for %s = %s, %v", name, data, err)
		}
	}
	for _, name := range []string{"audit-20261029.jsonl", "notes.txt"} {
		if _, err := os.Stat(filepath.Join(dir, name+deliveredSuffix)); err == nil {
			t.Fatalf("%s was marked delivered", name)
		}
	}

	result, err = archiver.Run()
	if err != nil {
		t.Fatalf("second run: %v", err)
	}
	if len(result.Delivered) != 0 || result.DeliveryError != "" {
		t.Fatalf("second run delivered again: %+v", result)
	}
	if _, attempts := recorder.snapshot(); attempts != 2 {
		t.Fatalf("second run uploaded %d files again", attempts-2)
	}
}

func TestRemoteDeliveryRetriesTransientFailures(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), "{\"stream\":\"audit\",\"id\":1}\n")

	recorder := &uploadRecorder{uploads: map[string]receivedUpload{}, fail: 2}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()

	archiver, err := New(dir, store, nil, Options{RemoteURL: server.URL})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.retryDelay = func(int) time.Duration { return 0 }
	archiver.now = func() time.Time { return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.DeliveryError != "" || len(result.Delivered) != 1 {
		t.Fatalf("delivery = %+v", result)
	}
	if uploads, attempts := recorder.snapshot(); attempts != 3 || len(uploads) != 1 {
		t.Fatalf("attempts = %d, uploads = %v", attempts, uploads)
	}
}

func TestRemoteDeliveryGatesPruning(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "audit-20261001.jsonl.gz"), "old")
	writeFile(t, filepath.Join(dir, "connections-20261005.jsonl.gz"), "recent")

	recorder := &uploadRecorder{uploads: map[string]receivedUpload{}, fail: 3}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()

	archiver, err := New(dir, store, nil, Options{
		RemoteURL:     server.URL,
		RetentionDays: 7,
	})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.retryDelay = func(int) time.Duration { return 0 }
	archiver.now = func() time.Time { return time.Date(2026, 10, 10, 8, 0, 0, 0, time.UTC) }

	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.DeliveryError == "" || len(result.Delivered) != 0 {
		t.Fatalf("failing delivery = %+v", result)
	}
	if len(result.Pruned) != 0 {
		t.Fatalf("undelivered evidence was pruned: %v", result.Pruned)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261001.jsonl.gz")); err != nil {
		t.Fatalf("undelivered file is gone: %v", err)
	}

	recorder.setFail(0)
	result, err = archiver.Run()
	if err != nil {
		t.Fatalf("second run: %v", err)
	}
	if result.DeliveryError != "" || len(result.Delivered) != 2 {
		t.Fatalf("second delivery = %+v", result)
	}
	if _, attempts := recorder.snapshot(); attempts != 3+2 {
		t.Fatalf("attempts after retry = %d", attempts)
	}

	// The archive runs before delivery in a pass, so the expired file is
	// pruned only once its marker exists.
	result, err = archiver.Run()
	if err != nil {
		t.Fatalf("third run: %v", err)
	}
	if len(result.Pruned) != 1 || result.Pruned[0] != "audit-20261001.jsonl.gz" {
		t.Fatalf("pruned = %v", result.Pruned)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261001.jsonl.gz")); !os.IsNotExist(err) {
		t.Fatalf("expired archive still exists (err=%v)", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261001.jsonl.gz"+deliveredSuffix)); !os.IsNotExist(err) {
		t.Fatalf("marker of a pruned archive still exists (err=%v)", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "connections-20261005.jsonl.gz")); err != nil {
		t.Fatalf("recent archive was pruned: %v", err)
	}
}

func TestRemoteDeliveryDisabledWithoutURL(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "audit-20261001.jsonl.gz"), "old")
	archiver, err := New(dir, store, nil, Options{RetentionDays: 7})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return time.Date(2026, 10, 10, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if archiver.Remote() != "" || len(result.Delivered) != 0 || result.DeliveryError != "" {
		t.Fatalf("delivery ran without a URL: %+v", result)
	}
	if len(result.Pruned) != 1 || !strings.Contains(result.Pruned[0], "audit-20261001") {
		t.Fatalf("local-only retention = %v", result.Pruned)
	}
}

// A marker may be lost (manual cleanup, a rewritten day file that kept the
// same bytes); the persisted ledger must still stop a second upload.
func TestRemoteDeliveryLedgerDeduplicatesContent(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	path := filepath.Join(dir, "audit-20261028.jsonl")
	const first = "{\"stream\":\"audit\",\"id\":1}\n"
	writeFile(t, path, first)

	recorder := &uploadRecorder{uploads: map[string]receivedUpload{}}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()
	newArchiver := func() *Archiver {
		t.Helper()
		archiver, err := New(dir, store, nil, Options{RemoteURL: server.URL})
		if err != nil {
			t.Fatalf("new archiver: %v", err)
		}
		archiver.now = func() time.Time { return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC) }
		return archiver
	}

	archiver := newArchiver()
	result, err := archiver.Run()
	if err != nil || len(result.Delivered) != 1 {
		t.Fatalf("first delivery = %+v, %v", result, err)
	}
	if _, err := os.Stat(filepath.Join(dir, ledgerName)); err != nil {
		t.Fatalf("ledger was not written: %v", err)
	}

	if err := os.Remove(path + deliveredSuffix); err != nil {
		t.Fatalf("remove marker: %v", err)
	}
	result, err = archiver.Run()
	if err != nil || len(result.Delivered) != 0 {
		t.Fatalf("ledger hit delivered again: %+v, %v", result, err)
	}
	if _, attempts := recorder.snapshot(); attempts != 1 {
		t.Fatalf("ledger hit uploaded %d times", attempts)
	}
	if !deliveredMarkerExists(path) {
		t.Fatal("ledger hit did not restore the marker")
	}

	// New content gets a new ledger key and is uploaded once.
	merged := first + "{\"stream\":\"audit\",\"id\":2}\n"
	writeFile(t, path, merged)
	if err := os.Remove(path + deliveredSuffix); err != nil {
		t.Fatalf("remove marker: %v", err)
	}
	result, err = archiver.Run()
	if err != nil || len(result.Delivered) != 1 {
		t.Fatalf("changed delivery = %+v, %v", result, err)
	}
	uploads, attempts := recorder.snapshot()
	if attempts != 2 {
		t.Fatalf("changed content attempts = %d", attempts)
	}
	digest := sha256Hex([]byte(merged))
	if upload := uploads["audit-20261028.jsonl"]; upload.digest != digest || upload.idempotency != digest {
		t.Fatalf("changed upload = %+v", upload)
	}

	// A fresh archiver loads the ledger from disk.
	if err := os.Remove(path + deliveredSuffix); err != nil {
		t.Fatalf("remove marker: %v", err)
	}
	restarted := newArchiver()
	result, err = restarted.Run()
	if err != nil || len(result.Delivered) != 0 {
		t.Fatalf("restarted archiver delivered again: %+v, %v", result, err)
	}
	if _, attempts := recorder.snapshot(); attempts != 2 {
		t.Fatalf("restarted archiver uploaded %d times", attempts)
	}
	if !deliveredMarkerExists(path) {
		t.Fatal("restarted archiver did not restore the marker")
	}
}

// The ledger belongs to one sink; pointing the archiver elsewhere must send
// the evidence again even when the bytes did not change.
func TestRemoteDeliveryLedgerScopedToSink(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	path := filepath.Join(dir, "audit-20261028.jsonl")
	writeFile(t, path, "{\"stream\":\"audit\",\"id\":1}\n")

	recorderA := &uploadRecorder{uploads: map[string]receivedUpload{}}
	serverA := httptest.NewServer(http.HandlerFunc(recorderA.handler))
	defer serverA.Close()
	first, err := New(dir, store, nil, Options{RemoteURL: serverA.URL})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	first.now = func() time.Time { return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC) }
	if result, err := first.Run(); err != nil || len(result.Delivered) != 1 {
		t.Fatalf("first sink delivery = %+v, %v", result, err)
	}
	if err := os.Remove(path + deliveredSuffix); err != nil {
		t.Fatalf("remove marker: %v", err)
	}

	recorderB := &uploadRecorder{uploads: map[string]receivedUpload{}}
	serverB := httptest.NewServer(http.HandlerFunc(recorderB.handler))
	defer serverB.Close()
	second, err := New(dir, store, nil, Options{RemoteURL: serverB.URL})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	second.now = first.now
	result, err := second.Run()
	if err != nil || len(result.Delivered) != 1 {
		t.Fatalf("second sink delivery = %+v, %v", result, err)
	}
	if _, attempts := recorderB.snapshot(); attempts != 1 {
		t.Fatalf("second sink attempts = %d", attempts)
	}
	if _, attempts := recorderA.snapshot(); attempts != 1 {
		t.Fatalf("first sink attempts changed: %d", attempts)
	}
}

// A corrupt ledger only costs deduplication; delivery must still progress and
// replace it with a valid one.
func TestRemoteDeliveryLedgerToleratesCorruption(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), "{\"stream\":\"audit\",\"id\":1}\n")
	writeFile(t, filepath.Join(dir, ledgerName), "{not json")

	recorder := &uploadRecorder{uploads: map[string]receivedUpload{}}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()
	archiver, err := New(dir, store, nil, Options{RemoteURL: server.URL})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil || len(result.Delivered) != 1 || result.DeliveryError != "" {
		t.Fatalf("delivery = %+v, %v", result, err)
	}
	data, err := os.ReadFile(filepath.Join(dir, ledgerName))
	if err != nil {
		t.Fatalf("ledger: %v", err)
	}
	var ledger deliveryLedger
	if err := json.Unmarshal(data, &ledger); err != nil || len(ledger.Entries) != 1 {
		t.Fatalf("replacement ledger = %s, %v", data, err)
	}
}

// A redirect must fail the upload instead of being followed: the PUT body is a
// one-shot file and, for S3, the signature covers the original host and path.
func TestRemoteDeliveryDoesNotFollowRedirects(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), "{\"stream\":\"audit\",\"id\":1}\n")

	var targetHits, redirectHits int32
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&targetHits, 1)
		w.WriteHeader(http.StatusNoContent)
	}))
	defer target.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&redirectHits, 1)
		http.Redirect(w, r, target.URL+"/audit-20261028.jsonl", http.StatusTemporaryRedirect)
	}))
	defer redirect.Close()

	archiver, err := New(dir, store, nil, Options{RemoteURL: redirect.URL})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.retryDelay = func(int) time.Duration { return 0 }
	archiver.now = func() time.Time { return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.DeliveryError == "" || !strings.Contains(result.DeliveryError, "307") {
		t.Fatalf("delivery error = %q", result.DeliveryError)
	}
	if len(result.Delivered) != 0 {
		t.Fatalf("redirected upload counted as delivered: %+v", result)
	}
	if got := atomic.LoadInt32(&targetHits); got != 0 {
		t.Fatalf("redirect target received %d uploads", got)
	}
	if got := atomic.LoadInt32(&redirectHits); got != 3 {
		t.Fatalf("redirect endpoint attempts = %d, want 3", got)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261028.jsonl"+deliveredSuffix)); !os.IsNotExist(err) {
		t.Fatalf("redirected file got a marker (err=%v)", err)
	}
}
