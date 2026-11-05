package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func loopbackNotice(t *testing.T, body []byte, url string) []string {
	t.Helper()
	publisher, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	digest := hex.EncodeToString(sum[:])
	sig, err := publisher.SignSoftware("9.9.9", url, digest, int64(len(body)))
	if err != nil {
		t.Fatal(err)
	}
	return []string{
		"--version", "9.9.9",
		"--url", url,
		"--sha256", digest,
		"--size", strconv.FormatInt(int64(len(body)), 10),
		"--signature", sig,
		"--server-pubkey", publisher.PublicKeyText(),
	}
}

func startReleaseServer(t *testing.T, body []byte) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/software/chast-9.9.9.exe" {
			http.NotFound(w, r)
			return
		}
		_, _ = w.Write(body)
	}))
	t.Cleanup(srv.Close)
	return srv
}

// TestUpdateLoopbackHealthyAndRollback exercises apply-update with a real
// download, a real executable swap and a real child process. The healthy case
// uses a pre-written marker (the --health-file seam); the crash case launches a
// system program that exits without writing one. It is opt-in because it starts
// detached processes.
func TestUpdateLoopbackHealthyAndRollback(t *testing.T) {
	if os.Getenv("CHAST_UPDATE_LOOPBACK") != "1" {
		t.Skip("set CHAST_UPDATE_LOOPBACK=1 to run the process loopback")
	}
	launcher := filepath.Join(os.Getenv("SystemRoot"), "System32", "where.exe")
	if _, err := os.Stat(launcher); err != nil {
		t.Skipf("system launcher unavailable: %v", err)
	}

	t.Run("healthy", func(t *testing.T) {
		release := []byte("new viewer image")
		srv := startReleaseServer(t, release)
		notice := loopbackNotice(t, release, srv.URL+"/software/chast-9.9.9.exe")
		dir := t.TempDir()
		target := filepath.Join(dir, "viewer.exe")
		if err := os.WriteFile(target, []byte("old viewer"), 0o755); err != nil {
			t.Fatal(err)
		}
		marker := filepath.Join(dir, "health")
		if err := os.WriteFile(marker, []byte("healthy\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		args := append(notice,
			"--target", target,
			"--launch", launcher,
			"--dir", filepath.Join(dir, "staging"),
			"--health-file", marker,
			"--health-timeout", "5s",
			"--retire-timeout", "2s",
		)
		if err := runApplyUpdate(args); err != nil {
			t.Fatalf("runApplyUpdate: %v", err)
		}
		raw, err := os.ReadFile(target)
		if err != nil {
			t.Fatal(err)
		}
		if string(raw) != string(release) {
			t.Fatalf("target holds %q, want the release", raw)
		}
		if matches, _ := filepath.Glob(target + ".old-*"); len(matches) != 0 {
			t.Fatalf("healthy update kept backups: %v", matches)
		}
	})

	t.Run("crash", func(t *testing.T) {
		old, err := os.ReadFile(launcher)
		if err != nil {
			t.Fatal(err)
		}
		release := []byte("broken viewer image")
		srv := startReleaseServer(t, release)
		notice := loopbackNotice(t, release, srv.URL+"/software/chast-9.9.9.exe")
		dir := t.TempDir()
		target := filepath.Join(dir, "viewer.exe")
		if err := os.WriteFile(target, old, 0o755); err != nil {
			t.Fatal(err)
		}
		args := append(notice,
			"--target", target,
			"--launch", launcher,
			"--dir", filepath.Join(dir, "staging"),
			"--health-file", filepath.Join(dir, "health"),
			"--health-timeout", "5s",
			"--retire-timeout", "2s",
		)
		started := time.Now()
		if err := runApplyUpdate(args); err == nil {
			t.Fatal("runApplyUpdate accepted a viewer that exited before reporting healthy")
		}
		if time.Since(started) > 4*time.Second {
			t.Fatal("the rollback waited for the whole health timeout after the viewer exited")
		}
		raw, err := os.ReadFile(target)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(raw, old) {
			t.Fatal("the previous binary was not restored")
		}
	})
}
