package auditarchive

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// TestS3SignatureMatchesPublishedPutExample pins the signer to the AWS
// documentation "Example: PUT Object" vector. The date header is part of the
// signed set there, which is why sign() also signs an explicit Date.
func TestS3SignatureMatchesPublishedPutExample(t *testing.T) {
	signer, err := newS3Signer(S3Options{
		Endpoint:  "https://examplebucket.s3.amazonaws.com",
		Region:    "us-east-1",
		Bucket:    "examplebucket",
		AccessKey: "AKIAIOSFODNN7EXAMPLE",
		SecretKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
	})
	if err != nil {
		t.Fatalf("signer: %v", err)
	}
	const payload = "Welcome to Amazon S3."
	request, err := http.NewRequest(http.MethodPut,
		"https://examplebucket.s3.amazonaws.com/test%24file.text", strings.NewReader(payload))
	if err != nil {
		t.Fatalf("request: %v", err)
	}
	request.Header.Set("Date", "Fri, 24 May 2013 00:00:00 GMT")
	request.Header.Set("X-Amz-Storage-Class", "REDUCED_REDUNDANCY")
	signer.sign(request, hashHex([]byte(payload)),
		time.Date(2013, 5, 24, 0, 0, 0, 0, time.UTC))

	const expected = "AWS4-HMAC-SHA256 " +
		"Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, " +
		"SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class, " +
		"Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"
	if got := request.Header.Get("Authorization"); got != expected {
		t.Fatalf("authorization = %q, want %q", got, expected)
	}
	if got := request.Header.Get("X-Amz-Content-Sha256"); got !=
		"44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072" {
		t.Fatalf("payload hash = %q", got)
	}
}

type s3Upload struct {
	method      string
	host        string
	path        string
	auth        string
	amzDate     string
	contentHash string
	session     string
	contentType string
	idempotency string
	chastDigest string
	body        []byte
}

type s3Recorder struct {
	mu      sync.Mutex
	uploads []s3Upload
}

func (r *s3Recorder) handler(w http.ResponseWriter, request *http.Request) {
	body, _ := io.ReadAll(request.Body)
	r.mu.Lock()
	defer r.mu.Unlock()
	r.uploads = append(r.uploads, s3Upload{
		method:      request.Method,
		host:        request.Host,
		path:        request.URL.Path,
		auth:        request.Header.Get("Authorization"),
		amzDate:     request.Header.Get("X-Amz-Date"),
		contentHash: request.Header.Get("X-Amz-Content-Sha256"),
		session:     request.Header.Get("X-Amz-Security-Token"),
		contentType: request.Header.Get("Content-Type"),
		idempotency: request.Header.Get("Idempotency-Key"),
		chastDigest: request.Header.Get("X-Chast-Audit-Sha256"),
		body:        body,
	})
	w.WriteHeader(http.StatusNoContent)
}

func TestS3DeliveryUploadsSignedObjects(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	const body = "{\"stream\":\"audit\",\"id\":1}\n"
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), body)

	recorder := &s3Recorder{}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()

	archiver, err := New(dir, store, nil, Options{S3: &S3Options{
		Endpoint:     server.URL + "/proxy/",
		Region:       "us-east-1",
		Bucket:       "audit-bucket",
		AccessKey:    "AKIDEXAMPLE",
		SecretKey:    "secret",
		SessionToken: "session-token-1",
	}})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	now := time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC)
	archiver.now = func() time.Time { return now }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.DeliveryError != "" || len(result.Delivered) != 1 {
		t.Fatalf("delivery = %+v", result)
	}
	if got, want := archiver.Remote(), server.URL+"/proxy/audit-bucket"; got != want {
		t.Fatalf("remote = %q, want %q", got, want)
	}

	recorder.mu.Lock()
	uploads := append([]s3Upload(nil), recorder.uploads...)
	recorder.mu.Unlock()
	if len(uploads) != 1 {
		t.Fatalf("uploads = %+v", uploads)
	}
	upload := uploads[0]
	name := "audit-20261028.jsonl"
	sum := sha256.Sum256([]byte(body))
	digest := hex.EncodeToString(sum[:])
	if upload.method != http.MethodPut || upload.path != "/proxy/audit-bucket/"+name {
		t.Fatalf("upload = %+v", upload)
	}
	if !strings.HasPrefix(upload.auth,
		"AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20261029/us-east-1/s3/aws4_request") {
		t.Fatalf("authorization = %q", upload.auth)
	}
	if !strings.Contains(upload.auth, "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-security-token") ||
		!strings.Contains(upload.auth, "Signature=") {
		t.Fatalf("signed headers = %q", upload.auth)
	}
	if upload.amzDate != "20261029T080000Z" || upload.session != "session-token-1" {
		t.Fatalf("amz date/token = %+v", upload)
	}
	if upload.contentHash != digest || string(upload.body) != body {
		t.Fatalf("payload = %+v", upload)
	}
	if upload.contentType != "application/x-ndjson" ||
		upload.idempotency != "" || upload.chastDigest != "" {
		t.Fatalf("non-S3 headers leaked into an S3 upload: %+v", upload)
	}

	data, err := os.ReadFile(filepath.Join(dir, name+deliveredSuffix))
	if err != nil {
		t.Fatalf("marker: %v", err)
	}
	var marker deliveredMarker
	if err := json.Unmarshal(data, &marker); err != nil {
		t.Fatalf("marker json: %v", err)
	}
	if marker.File != name || marker.SHA256 != digest ||
		marker.URL != server.URL+"/proxy/audit-bucket/"+name ||
		strings.Contains(marker.URL, "session-token-1") || strings.Contains(marker.URL, "secret") {
		t.Fatalf("marker = %+v", marker)
	}

	// The marker skips a second upload.
	recorder.mu.Lock()
	recorder.uploads = nil
	recorder.mu.Unlock()
	result, err = archiver.Run()
	if err != nil {
		t.Fatalf("second run: %v", err)
	}
	recorder.mu.Lock()
	uploads = append([]s3Upload(nil), recorder.uploads...)
	recorder.mu.Unlock()
	if len(result.Delivered) != 0 || result.DeliveryError != "" || len(uploads) != 0 {
		t.Fatalf("second run re-uploaded: %+v / %+v", result, uploads)
	}
}

func TestS3OptionsValidation(t *testing.T) {
	valid := S3Options{
		Endpoint:  "https://s3.example.com",
		Region:    "us-east-1",
		Bucket:    "bucket",
		AccessKey: "key",
		SecretKey: "secret",
	}
	if _, err := newS3Signer(valid); err != nil {
		t.Fatalf("valid options rejected: %v", err)
	}
	cases := []struct {
		name string
		edit func(*S3Options)
	}{
		{"empty endpoint", func(o *S3Options) { o.Endpoint = "" }},
		{"ftp endpoint", func(o *S3Options) { o.Endpoint = "ftp://s3.example.com" }},
		{"credentials in endpoint", func(o *S3Options) { o.Endpoint = "https://u:p@s3.example.com" }},
		{"query in endpoint", func(o *S3Options) { o.Endpoint = "https://s3.example.com?x=1" }},
		{"fragment in endpoint", func(o *S3Options) { o.Endpoint = "https://s3.example.com#f" }},
		{"empty region", func(o *S3Options) { o.Region = "" }},
		{"empty bucket", func(o *S3Options) { o.Bucket = "" }},
		{"slash in bucket", func(o *S3Options) { o.Bucket = "a/b" }},
		{"empty access key", func(o *S3Options) { o.AccessKey = "" }},
		{"empty secret", func(o *S3Options) { o.SecretKey = "" }},
		{"unknown addressing", func(o *S3Options) { o.Addressing = "dns" }},
		{"virtual over an IP endpoint", func(o *S3Options) {
			o.Addressing = "virtual"
			o.Endpoint = "https://10.0.0.5:9000"
		}},
	}
	for _, tc := range cases {
		opts := valid
		tc.edit(&opts)
		if _, err := newS3Signer(opts); err == nil {
			t.Fatalf("%s was accepted", tc.name)
		}
	}

	store := openStore(t)
	if _, err := New(t.TempDir(), store, nil, Options{
		RemoteURL: "https://example.com/sink",
		S3:        &valid,
	}); err == nil {
		t.Fatal("HTTP and S3 delivery were accepted together")
	}
	archiver, err := New(t.TempDir(), store, nil, Options{S3: &valid})
	if err != nil {
		t.Fatalf("valid S3 archiver: %v", err)
	}
	if got := archiver.Remote(); got != "https://s3.example.com/bucket" {
		t.Fatalf("remote = %q", got)
	}
}

// TestS3VirtualHostedDelivery uploads through a virtual host: the bucket moves
// into the Host header and the signature must cover that exact host.
func TestS3VirtualHostedDelivery(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	const body = "{\"stream\":\"audit\",\"id\":1}\n"
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), body)

	recorder := &s3Recorder{}
	server := httptest.NewServer(http.HandlerFunc(recorder.handler))
	defer server.Close()

	archiver, err := New(dir, store, nil, Options{S3: &S3Options{
		Endpoint:   "http://s3.example.com",
		Region:     "eu-west-1",
		Bucket:     "audit-bucket",
		AccessKey:  "AKIDEXAMPLE",
		SecretKey:  "secret",
		Addressing: "virtual",
	}})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time {
		return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC)
	}
	// The virtual host does not resolve here, so send every dial to the test
	// server while leaving the signed Host header untouched.
	dialer := &net.Dialer{}
	archiver.client.Transport = &http.Transport{
		DialContext: func(ctx context.Context, network, _ string) (net.Conn, error) {
			return dialer.DialContext(ctx, network, server.Listener.Addr().String())
		},
	}
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.DeliveryError != "" || len(result.Delivered) != 1 {
		t.Fatalf("delivery = %+v", result)
	}
	if got, want := archiver.Remote(), "http://audit-bucket.s3.example.com"; got != want {
		t.Fatalf("remote = %q, want %q", got, want)
	}

	recorder.mu.Lock()
	uploads := append([]s3Upload(nil), recorder.uploads...)
	recorder.mu.Unlock()
	if len(uploads) != 1 {
		t.Fatalf("uploads = %+v", uploads)
	}
	upload := uploads[0]
	name := "audit-20261028.jsonl"
	if upload.method != http.MethodPut || upload.path != "/"+name {
		t.Fatalf("upload = %+v", upload)
	}
	if upload.host != "audit-bucket.s3.example.com" {
		t.Fatalf("host = %q", upload.host)
	}
	digest := hashHex([]byte(body))
	if upload.contentHash != digest {
		t.Fatalf("payload hash = %q", upload.contentHash)
	}

	// Recompute the signature from what the server saw. The canonical host has
	// to be the virtual host, otherwise the upload would fail at AWS.
	canonical := "PUT\n/" + name + "\n\nhost:" + upload.host +
		"\nx-amz-content-sha256:" + digest + "\nx-amz-date:" + upload.amzDate +
		"\n\nhost;x-amz-content-sha256;x-amz-date\n" + digest
	key := hmacSHA256([]byte("AWS4secret"), "20261029")
	key = hmacSHA256(key, "eu-west-1")
	key = hmacSHA256(key, "s3")
	key = hmacSHA256(key, "aws4_request")
	signature := hex.EncodeToString(hmacSHA256(key,
		"AWS4-HMAC-SHA256\n"+upload.amzDate+"\n20261029/eu-west-1/s3/aws4_request\n"+hashHex([]byte(canonical))))
	wantAuth := "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20261029/eu-west-1/s3/aws4_request, " +
		"SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=" + signature
	if upload.auth != wantAuth {
		t.Fatalf("authorization = %q, want %q", upload.auth, wantAuth)
	}

	data, err := os.ReadFile(filepath.Join(dir, name+deliveredSuffix))
	if err != nil {
		t.Fatalf("marker: %v", err)
	}
	var marker deliveredMarker
	if err := json.Unmarshal(data, &marker); err != nil {
		t.Fatalf("marker json: %v", err)
	}
	if marker.URL != "http://audit-bucket.s3.example.com/"+name {
		t.Fatalf("marker URL = %q", marker.URL)
	}
}
