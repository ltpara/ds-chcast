//go:build windows

package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/updater"
)

// fakeAppSource is compiled into small stand-ins for the viewer. A healthy
// build reads the pending-update nonce and writes the health marker; the
// unhealthy build exits without one. Both append to <exe>.starts.log so the
// test can see which image ran.
const fakeAppSource = `package main

import (
	"encoding/json"
	"os"
	"strconv"
	"time"
)

const mode = %q

func main() {
	exe, err := os.Executable()
	if err != nil {
		os.Exit(2)
	}
	log, err := os.OpenFile(exe+".starts.log", os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		os.Exit(3)
	}
	log.WriteString(mode + "\n")
	log.Close()
	if mode == "hung" {
		os.WriteFile(exe+".pid", []byte(strconv.Itoa(os.Getpid())), 0o644)
		time.Sleep(5 * time.Minute)
		return
	}
	if mode != "healthy" {
		return
	}
	raw, err := os.ReadFile(exe + ".chast-update.json")
	if err != nil {
		os.Exit(4)
	}
	var pending struct {
		Nonce string ` + "`json:\"nonce\"`" + `
	}
	if json.Unmarshal(raw, &pending) != nil || pending.Nonce == "" {
		os.Exit(5)
	}
	marker, err := json.Marshal(map[string]any{"nonce": pending.Nonce, "confirmed_at": 0})
	if err != nil {
		os.Exit(6)
	}
	if err := os.WriteFile(exe+".chast-update.ok", marker, 0o644); err != nil {
		os.Exit(7)
	}
}
`

func TestApplyUpdateWatchdog(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the tunnel and fake images")
	}
	tunnel := buildTunnel(t)
	work := t.TempDir()
	oldApp := buildFakeApp(t, filepath.Join(work, "old"), "old")
	healthyApp := buildFakeApp(t, filepath.Join(work, "healthy"), "healthy")
	unhealthyApp := buildFakeApp(t, filepath.Join(work, "unhealthy"), "unhealthy")
	hungApp := buildFakeApp(t, filepath.Join(work, "hung"), "hung")

	server, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	publisherPub, publisherPriv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	publisherText := identity.PublicKeyText(publisherPub)
	publisherID := identity.PublisherKeyID(publisherText)

	t.Run("healthy image retires the backup", func(t *testing.T) {
		dir := t.TempDir()
		target := installOld(t, dir, oldApp)
		url := serveBinary(t, healthyApp)
		digest, size := fileDigest(t, healthyApp)
		serverSig, err := server.SignSoftware("2.2.0", url, digest, size)
		if err != nil {
			t.Fatal(err)
		}
		logPath := filepath.Join(dir, "apply.log")
		output, err := runApplyUpdateCmd(t, tunnel,
			"--version", "2.2.0",
			"--url", url,
			"--sha256", digest,
			"--size", strconv.FormatInt(size, 10),
			"--signature", serverSig,
			"--publisher-key-id", publisherID,
			"--publisher-signature", identity.Sign(publisherPriv, identity.PublisherMessage("2.2.0", url, digest, size)),
			"--publisher-pubkey", publisherText,
			"--server-pubkey", server.PublicKeyText(),
			"--target", target,
			"--dir", filepath.Join(dir, "staging"),
			"--health-timeout", "30s",
			"--log", logPath,
		)
		if err != nil {
			t.Fatalf("apply-update: %v\n%s\n%s", err, output, readFile(logPath))
		}
		if got := fileDigestHex(t, target); got != digest {
			t.Fatalf("target digest %s, want %s", got, digest)
		}
		if backups := backupsOf(t, target); len(backups) != 0 {
			t.Fatalf("backups left behind: %v", backups)
		}
		if _, err := os.Stat(target + ".chast-update.json"); !os.IsNotExist(err) {
			t.Fatalf("pending state should be gone: %v", err)
		}
		if _, err := os.Stat(target + ".chast-update.ok"); !os.IsNotExist(err) {
			t.Fatalf("health marker should be gone: %v", err)
		}
		if starts := readFile(target + ".starts.log"); strings.TrimSpace(starts) != "healthy" {
			t.Fatalf("started images: %q", starts)
		}
	})

	t.Run("image that exits early is rolled back", func(t *testing.T) {
		dir := t.TempDir()
		target := installOld(t, dir, oldApp)
		oldDigest := fileDigestHex(t, target)
		url := serveBinary(t, unhealthyApp)
		digest, size := fileDigest(t, unhealthyApp)
		serverSig, err := server.SignSoftware("2.2.1", url, digest, size)
		if err != nil {
			t.Fatal(err)
		}
		logPath := filepath.Join(dir, "apply.log")
		output, err := runApplyUpdateCmd(t, tunnel,
			"--version", "2.2.1",
			"--url", url,
			"--sha256", digest,
			"--size", strconv.FormatInt(size, 10),
			"--signature", serverSig,
			"--publisher-key-id", publisherID,
			"--publisher-signature", identity.Sign(publisherPriv, identity.PublisherMessage("2.2.1", url, digest, size)),
			"--publisher-pubkey", publisherText,
			"--server-pubkey", server.PublicKeyText(),
			"--target", target,
			"--dir", filepath.Join(dir, "staging"),
			"--health-timeout", "30s",
			"--log", logPath,
		)
		if err == nil {
			t.Fatalf("apply-update should fail when the new image exits\n%s", output)
		}
		if got := fileDigestHex(t, target); got != oldDigest {
			t.Fatalf("target was not restored: digest %s, want %s", got, oldDigest)
		}
		if backups := backupsOf(t, target); len(backups) != 0 {
			t.Fatalf("backups left behind: %v", backups)
		}
		if _, err := os.Stat(target + ".chast-update.json"); !os.IsNotExist(err) {
			t.Fatalf("pending state should be gone: %v", err)
		}
		waitFor(t, 10*time.Second, func() bool {
			return strings.Contains(readFile(target+".starts.log"), "old")
		}, "the previous image was not restarted")
		starts := readFile(target + ".starts.log")
		if !strings.Contains(starts, "unhealthy") {
			t.Fatalf("rejected image did not run: %q", starts)
		}
		if !strings.Contains(readFile(logPath), "rolling back") {
			t.Fatalf("log does not mention the rollback:\n%s", readFile(logPath))
		}
	})

	t.Run("image that hangs is killed and rolled back", func(t *testing.T) {
		dir := t.TempDir()
		target := installOld(t, dir, oldApp)
		oldDigest := fileDigestHex(t, target)
		url := serveBinary(t, hungApp)
		digest, size := fileDigest(t, hungApp)
		serverSig, err := server.SignSoftware("2.2.4", url, digest, size)
		if err != nil {
			t.Fatal(err)
		}
		logPath := filepath.Join(dir, "apply.log")
		output, err := runApplyUpdateCmd(t, tunnel,
			"--version", "2.2.4",
			"--url", url,
			"--sha256", digest,
			"--size", strconv.FormatInt(size, 10),
			"--signature", serverSig,
			"--publisher-key-id", publisherID,
			"--publisher-signature", identity.Sign(publisherPriv, identity.PublisherMessage("2.2.4", url, digest, size)),
			"--publisher-pubkey", publisherText,
			"--server-pubkey", server.PublicKeyText(),
			"--target", target,
			"--dir", filepath.Join(dir, "staging"),
			"--health-timeout", "2s",
			"--log", logPath,
		)
		if err == nil {
			t.Fatalf("apply-update should give up on an image that never confirms\n%s", output)
		}
		if got := fileDigestHex(t, target); got != oldDigest {
			t.Fatalf("target was not restored: digest %s, want %s", got, oldDigest)
		}
		if backups := backupsOf(t, target); len(backups) != 0 {
			t.Fatalf("backups left behind: %v", backups)
		}
		pid := strings.TrimSpace(readFile(target + ".pid"))
		if pid == "" {
			t.Fatal("the hung image did not report its pid")
		}
		waitFor(t, 15*time.Second, func() bool {
			out, _ := exec.Command("tasklist", "/FI", "PID eq "+pid, "/NH").Output()
			return !strings.Contains(string(out), pid)
		}, "the hung image was not terminated")
	})

	t.Run("confirm-update writes the pending nonce", func(t *testing.T) {
		dir := t.TempDir()
		target := installOld(t, dir, oldApp)
		backup := target + ".old-1"
		if err := os.WriteFile(backup, []byte("previous"), 0o755); err != nil {
			t.Fatal(err)
		}
		pending, err := updater.Begin(target, backup, "2.2.3", time.Minute)
		if err != nil {
			t.Fatal(err)
		}
		output, err := runTunnel(t, tunnel, "confirm-update", "--target", target)
		if err != nil {
			t.Fatalf("confirm-update: %v\n%s", err, output)
		}
		if !pending.Healthy() {
			t.Fatalf("health marker was not written\n%s", output)
		}
		pending.Cancel()

		output, err = runTunnel(t, tunnel, "confirm-update", "--target", target)
		if err != nil {
			t.Fatalf("confirm-update without a pending update: %v\n%s", err, output)
		}
	})

	t.Run("a different publisher pin is rejected", func(t *testing.T) {
		dir := t.TempDir()
		target := installOld(t, dir, oldApp)
		oldDigest := fileDigestHex(t, target)
		url := serveBinary(t, healthyApp)
		digest, size := fileDigest(t, healthyApp)
		serverSig, err := server.SignSoftware("2.2.2", url, digest, size)
		if err != nil {
			t.Fatal(err)
		}
		otherPub, _, err := ed25519.GenerateKey(rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		output, err := runApplyUpdateCmd(t, tunnel,
			"--version", "2.2.2",
			"--url", url,
			"--sha256", digest,
			"--size", strconv.FormatInt(size, 10),
			"--signature", serverSig,
			"--publisher-key-id", publisherID,
			"--publisher-signature", identity.Sign(publisherPriv, identity.PublisherMessage("2.2.2", url, digest, size)),
			"--publisher-pubkey", identity.PublicKeyText(otherPub),
			"--server-pubkey", server.PublicKeyText(),
			"--target", target,
			"--dir", filepath.Join(dir, "staging"),
		)
		if err == nil || !strings.Contains(string(output), "publisher") {
			t.Fatalf("apply-update should reject the publisher pin: %v\n%s", err, output)
		}
		if got := fileDigestHex(t, target); got != oldDigest {
			t.Fatalf("target changed: digest %s, want %s", got, oldDigest)
		}
	})
}

func buildTunnel(t *testing.T) string {
	t.Helper()
	out := filepath.Join(t.TempDir(), "chast-tunnel.exe")
	cmd := exec.Command("go", "build", "-o", out, ".")
	if raw, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build chast-tunnel: %v\n%s", err, raw)
	}
	return out
}

func buildFakeApp(t *testing.T, dir, mode string) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "go.mod"), []byte("module fakeapp\n\ngo 1.24.0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	source := fmt.Sprintf(fakeAppSource, mode)
	if err := os.WriteFile(filepath.Join(dir, "main.go"), []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(dir, "fake-"+mode+".exe")
	cmd := exec.Command("go", "build", "-o", out, ".")
	cmd.Dir = dir
	if raw, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build fake image %s: %v\n%s", mode, err, raw)
	}
	return out
}

// installOld places the old image at target and returns target.
func installOld(t *testing.T, dir, oldApp string) string {
	t.Helper()
	target := filepath.Join(dir, "target.exe")
	raw, err := os.ReadFile(oldApp)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(target, raw, 0o755); err != nil {
		t.Fatal(err)
	}
	return target
}

func serveBinary(t *testing.T, path string) string {
	t.Helper()
	server := httptest.NewServer(http.FileServer(http.Dir(filepath.Dir(path))))
	t.Cleanup(server.Close)
	return server.URL + "/" + filepath.Base(path)
}

func runApplyUpdateCmd(t *testing.T, tunnel string, args ...string) (string, error) {
	t.Helper()
	return runTunnel(t, tunnel, append([]string{"apply-update"}, args...)...)
}

func runTunnel(t *testing.T, tunnel string, args ...string) (string, error) {
	t.Helper()
	cmd := exec.Command(tunnel, args...)
	done := make(chan struct{})
	var output []byte
	var err error
	go func() {
		output, err = cmd.CombinedOutput()
		close(done)
	}()
	select {
	case <-done:
		return string(output), err
	case <-time.After(2 * time.Minute):
		_ = cmd.Process.Kill()
		<-done
		t.Fatalf("apply-update did not finish\n%s", output)
		return "", nil
	}
}

func fileDigest(t *testing.T, path string) (string, int64) {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(raw)
	return hex.EncodeToString(sum[:]), int64(len(raw))
}

func fileDigestHex(t *testing.T, path string) string {
	t.Helper()
	digest, _ := fileDigest(t, path)
	return digest
}

func backupsOf(t *testing.T, target string) []string {
	t.Helper()
	matches, err := filepath.Glob(target + ".old-*")
	if err != nil {
		t.Fatal(err)
	}
	return matches
}

func readFile(path string) string {
	raw, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return string(raw)
}

func waitFor(t *testing.T, timeout time.Duration, done func() bool, message string) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if done() {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatal(message)
}
