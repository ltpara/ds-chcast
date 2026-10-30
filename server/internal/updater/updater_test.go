package updater

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func digestOf(body []byte) string {
	sum := sha256.Sum256(body)
	return hex.EncodeToString(sum[:])
}

// signedNotice builds a notice the way chast-server does: the digest and size
// are signed with the server identity, so a test that changes either field
// fails verification.
func signedNotice(t *testing.T, srv *identity.Server, version, downloadURL string, body []byte) Notice {
	t.Helper()
	digest := digestOf(body)
	sig, err := srv.SignSoftware(version, downloadURL, digest, int64(len(body)))
	if err != nil {
		t.Fatalf("sign notice: %v", err)
	}
	return Notice{
		Version:   version,
		URL:       downloadURL,
		SHA256:    digest,
		Size:      int64(len(body)),
		Signature: sig,
	}
}

func TestVerifyAcceptsSignedNotice(t *testing.T) {
	srv, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	body := []byte("installer bytes")
	n := signedNotice(t, srv, "2.1.0", "https://downloads.example.com/software/chast-2.1.0.exe", body)
	if err := Verify(n, srv.PublicKeyText()); err != nil {
		t.Fatalf("Verify: %v", err)
	}
}

func TestVerifyRejectsTamperedFields(t *testing.T) {
	srv, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	body := []byte("installer bytes")
	base := signedNotice(t, srv, "2.1.0", "https://downloads.example.com/software/chast-2.1.0.exe", body)

	cases := []struct {
		name   string
		mutate func(*Notice)
		want   string
	}{
		{"version", func(n *Notice) { n.Version = "9.9.9" }, "signature"},
		{"url", func(n *Notice) { n.URL = "https://evil.example.com/software/chast-2.1.0.exe" }, "signature"},
		{"digest", func(n *Notice) { n.SHA256 = digestOf([]byte("other")) }, "signature"},
		{"size", func(n *Notice) { n.Size++ }, "signature"},
		{"missing signature", func(n *Notice) { n.Signature = "" }, "no digest"},
		{"oversize", func(n *Notice) { n.Size = MaxSize + 1 }, "exceeds"},
		{"bad scheme", func(n *Notice) { n.URL = "ftp://example.com/a.exe" }, "http"},
		{"credentials", func(n *Notice) { n.URL = "https://u:p@example.com/a.exe" }, "credentials"},
		{"odd name", func(n *Notice) { n.URL = "https://example.com/soft%20ware.exe" }, "file name"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			n := base
			tc.mutate(&n)
			err := Verify(n, srv.PublicKeyText())
			if err == nil {
				t.Fatalf("Verify accepted a tampered notice")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error %q does not mention %q", err, tc.want)
			}
		})
	}
}

func TestVerifyRejectsWrongServerKey(t *testing.T) {
	srv, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	other, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	body := []byte("installer bytes")
	n := signedNotice(t, srv, "2.1.0", "https://downloads.example.com/software/chast-2.1.0.exe", body)
	if err := Verify(n, other.PublicKeyText()); err == nil || !strings.Contains(err.Error(), "signature") {
		t.Fatalf("Verify with the wrong key = %v, want a signature error", err)
	}
	if err := Verify(n, ""); err == nil || !strings.Contains(err.Error(), "pinned server key") {
		t.Fatalf("Verify without a pin = %v, want a pinned key error", err)
	}
}

func TestFetchStagesVerifiedFile(t *testing.T) {
	body := []byte("installer payload for the digest check")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/software/chast-2.1.0.exe" {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write(body)
	}))
	defer srv.Close()
	dir := t.TempDir()
	n := Notice{
		Version: "2.1.0",
		URL:     srv.URL + "/software/chast-2.1.0.exe",
		SHA256:  digestOf(body),
		Size:    int64(len(body)),
	}
	got, err := Fetch(context.Background(), srv.Client(), n, dir)
	if err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if filepath.Dir(got) != dir || filepath.Base(got) != "chast-2.1.0.exe" {
		t.Fatalf("staged file %q outside %q", got, dir)
	}
	raw, err := os.ReadFile(got)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(raw, body) {
		t.Fatalf("staged bytes differ from the served body")
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 {
		t.Fatalf("staging directory holds %d entries, want only the release", len(entries))
	}
}

func TestFetchReplacesStaleStagedFile(t *testing.T) {
	body := []byte("fresh installer payload")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(body)
	}))
	defer srv.Close()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "chast-2.1.0.exe"), []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}
	n := Notice{
		Version: "2.1.0",
		URL:     srv.URL + "/software/chast-2.1.0.exe",
		SHA256:  digestOf(body),
		Size:    int64(len(body)),
	}
	got, err := Fetch(context.Background(), srv.Client(), n, dir)
	if err != nil {
		t.Fatalf("Fetch over a stale file: %v", err)
	}
	raw, err := os.ReadFile(got)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(raw, body) {
		t.Fatalf("stale file was not replaced")
	}
}

func TestFetchRejectsBadDigestAndSize(t *testing.T) {
	body := []byte("0123456789")
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(body)
	}))
	defer srv.Close()

	cases := []struct {
		name string
		n    Notice
		want string
	}{
		{"digest", Notice{Version: "2", URL: srv.URL + "/a.exe", SHA256: digestOf([]byte("other")), Size: int64(len(body))}, "digest"},
		{"short body", Notice{Version: "2", URL: srv.URL + "/a.exe", SHA256: digestOf(body), Size: int64(len(body)) + 4}, "size"},
		{"long body", Notice{Version: "2", URL: srv.URL + "/a.exe", SHA256: digestOf(body), Size: int64(len(body)) - 4}, "size"},
		{"no digest", Notice{Version: "2", URL: srv.URL + "/a.exe", Size: int64(len(body))}, "no digest"},
		{"not http", Notice{Version: "2", URL: "file:///tmp/a.exe", SHA256: digestOf(body), Size: int64(len(body))}, "http"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			if _, err := Fetch(context.Background(), srv.Client(), tc.n, dir); err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("Fetch = %v, want an error mentioning %q", err, tc.want)
			}
			entries, err := os.ReadDir(dir)
			if err != nil {
				t.Fatal(err)
			}
			if len(entries) != 0 {
				t.Fatalf("a failed download left %d entries behind", len(entries))
			}
		})
	}
}

func TestFetchRejectsErrorStatus(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "gone", http.StatusNotFound)
	}))
	defer srv.Close()
	n := Notice{Version: "2", URL: srv.URL + "/a.exe", SHA256: digestOf(nil), Size: 1}
	if _, err := Fetch(context.Background(), srv.Client(), n, t.TempDir()); err == nil || !strings.Contains(err.Error(), "404") {
		t.Fatalf("Fetch = %v, want the HTTP status", err)
	}
}

func TestFetchHonoursContext(t *testing.T) {
	block := make(chan struct{})
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.(http.Flusher).Flush()
		<-block
	}))
	defer srv.Close()
	defer close(block)
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	n := Notice{Version: "2", URL: srv.URL + "/a.exe", SHA256: digestOf([]byte("x")), Size: 1}
	if _, err := Fetch(ctx, srv.Client(), n, t.TempDir()); err == nil {
		t.Fatal("Fetch ignored the cancelled context")
	}
}

func TestLaunchMissingFileFails(t *testing.T) {
	if err := Launch(filepath.Join(t.TempDir(), "missing.exe")); err == nil {
		t.Fatal("Launch started a file that does not exist")
	}
}
