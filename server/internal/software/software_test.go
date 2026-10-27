package software

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestValidName(t *testing.T) {
	for _, name := range []string{"chast-2.1.0.exe", "a", "A_b-1.2.tar.gz"} {
		if !ValidName(name) {
			t.Fatalf("ValidName(%q) = false", name)
		}
	}
	for _, name := range []string{"", ".", "..", ".hidden", "-flag", "a/b", `a\b`, "a b", "a:b", strings.Repeat("a", 129)} {
		if ValidName(name) {
			t.Fatalf("ValidName(%q) = true", name)
		}
	}
}

func TestHostedName(t *testing.T) {
	name, err := HostedName("https://downloads.example.com/software/chast-2.1.0.exe")
	if err != nil || name != "chast-2.1.0.exe" {
		t.Fatalf("hosted = %q, %v", name, err)
	}
	name, err = HostedName("https://downloads.example.com/files/chast-2.1.0.exe")
	if err != nil || name != "" {
		t.Fatalf("external = %q, %v", name, err)
	}
	for _, raw := range []string{
		"https://downloads.example.com/software/",
		"https://downloads.example.com/software/sub/chast.exe",
		"https://downloads.example.com/software/..",
		"https://downloads.example.com/software/.hidden",
	} {
		if _, err := HostedName(raw); err == nil {
			t.Fatalf("HostedName(%q) accepted a bad hosted path", raw)
		}
	}
	if _, err := HostedName("://bad"); err == nil {
		t.Fatal("HostedName accepted an unparsable URL")
	}
}

func TestLoadComputesDigest(t *testing.T) {
	dir := t.TempDir()
	body := []byte("chast release payload")
	if err := os.WriteFile(filepath.Join(dir, "chast.exe"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	artifact, err := Load(dir, "chast.exe")
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	if artifact.Name != "chast.exe" || artifact.SHA256 != hex.EncodeToString(sum[:]) || artifact.Size != int64(len(body)) {
		t.Fatalf("artifact = %+v", artifact)
	}
	if _, err := Load(dir, "missing.exe"); err == nil {
		t.Fatal("missing file was loaded")
	}
	if _, err := Load(dir, "../chast.exe"); err == nil {
		t.Fatal("unsafe name was loaded")
	}
	if err := os.Mkdir(filepath.Join(dir, "sub"), 0o700); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(dir, "sub"); err == nil {
		t.Fatal("directory was loaded as an artifact")
	}
}

func TestHandlerServesArtifact(t *testing.T) {
	dir := t.TempDir()
	body := []byte("0123456789")
	if err := os.WriteFile(filepath.Join(dir, "chast.exe"), body, 0o600); err != nil {
		t.Fatal(err)
	}
	mux := http.NewServeMux()
	mux.Handle("GET "+RoutePrefix+"{name}", Handler(dir))
	srv := httptest.NewServer(mux)
	defer srv.Close()

	resp, err := http.Get(srv.URL + RoutePrefix + "chast.exe")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	got, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK || string(got) != string(body) {
		t.Fatalf("download = %d %q", resp.StatusCode, got)
	}
	if resp.Header.Get("Content-Type") != "application/octet-stream" ||
		!strings.Contains(resp.Header.Get("Content-Disposition"), "chast.exe") {
		t.Fatalf("headers = %v", resp.Header)
	}

	head, err := http.Head(srv.URL + RoutePrefix + "chast.exe")
	if err != nil {
		t.Fatal(err)
	}
	head.Body.Close()
	if head.StatusCode != http.StatusOK || head.ContentLength != int64(len(body)) {
		t.Fatalf("HEAD = %d len=%d", head.StatusCode, head.ContentLength)
	}

	req, _ := http.NewRequest(http.MethodGet, srv.URL+RoutePrefix+"chast.exe", nil)
	req.Header.Set("Range", "bytes=2-5")
	partial, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer partial.Body.Close()
	got, _ = io.ReadAll(partial.Body)
	if partial.StatusCode != http.StatusPartialContent || string(got) != "2345" {
		t.Fatalf("range = %d %q", partial.StatusCode, got)
	}

	for _, path := range []string{
		RoutePrefix + "missing.exe",
		RoutePrefix + "bad%20name.exe",
		RoutePrefix + "sub/chast.exe",
	} {
		resp, err := http.Get(srv.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusNotFound {
			t.Fatalf("%s = %d", path, resp.StatusCode)
		}
	}
}
