package api_test

// Opt-in acceptance against the actual Windows tray and tunnel processes.
// Default QUIC viewers use files-only mode. Opt-in Flutter modes use isolated
// profiles; input runs only with the explicit dry-run acceptance flag, and no
// service is installed.

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/http/httputil"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
	"golang.org/x/sys/windows"
)

type nativeProcess struct {
	cmd  *exec.Cmd
	done chan struct{}
	err  error
}

func startNativeProcess(t *testing.T, dir, name, binary, input string, args ...string) *nativeProcess {
	return startNativeProcessWithEnv(t, dir, name, binary, input, nil, false, args...)
}

func startNativeProcessWithEnv(t *testing.T, dir, name, binary, input string, extraEnv []string, showWindow bool, args ...string) *nativeProcess {
	t.Helper()
	log, err := os.Create(filepath.Join(dir, name+".log"))
	if err != nil {
		t.Fatal(err)
	}
	p := &nativeProcess{cmd: exec.Command(binary, args...), done: make(chan struct{})}
	p.cmd.Dir = dir
	p.cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: !showWindow}
	if extraEnv != nil {
		p.cmd.Env = extraEnv
	}
	p.cmd.Stdin = strings.NewReader(input)
	p.cmd.Stdout, p.cmd.Stderr = log, log
	if err := p.cmd.Start(); err != nil {
		log.Close()
		t.Fatal(err)
	}
	go func() { p.err = p.cmd.Wait(); log.Close(); close(p.done) }()
	t.Cleanup(func() { p.stop(t) })
	t.Logf("started %s pid=%d", name, p.cmd.Process.Pid)
	return p
}

func (p *nativeProcess) stop(t *testing.T) {
	t.Helper()
	select {
	case <-p.done:
		return
	default:
	}
	// Only the exact owned process tree; do not match executable names.
	kill := exec.Command("taskkill", "/PID", strconv.Itoa(p.cmd.Process.Pid), "/T", "/F")
	kill.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := kill.Run(); err != nil {
		_ = p.cmd.Process.Kill()
	}
	select {
	case <-p.done:
	case <-time.After(10 * time.Second):
		t.Errorf("owned process %d did not exit", p.cmd.Process.Pid)
	}
}

func (p *nativeProcess) exitStatus(wait time.Duration) string {
	select {
	case <-p.done:
	case <-time.After(wait):
	}
	select {
	case <-p.done:
		if p.cmd.ProcessState == nil {
			return fmt.Sprintf("exited: wait=%v", p.err)
		}
		exitCode := p.cmd.ProcessState.ExitCode()
		return fmt.Sprintf("exited: exit_code=%d (0x%08X), wait=%v",
			exitCode, uint32(exitCode), p.err)
	default:
		return "still running"
	}
}

func TestNativeProcessExitStatusIncludesCode(t *testing.T) {
	process := startNativeProcess(t, t.TempDir(), "exit-code", "cmd.exe", "",
		"/D", "/C", "exit 37")
	status := process.exitStatus(2 * time.Second)
	if !strings.Contains(status, "exit_code=37") || !strings.Contains(status, "0x00000025") {
		t.Fatalf("native process status = %q, want decimal and hexadecimal exit code", status)
	}
}

func nativeWait(t *testing.T, what string, timeout time.Duration, predicate func() bool) {
	t.Helper()
	until := time.Now().Add(timeout)
	for time.Now().Before(until) {
		if predicate() {
			t.Log("verified: " + what)
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("timeout: %s", what)
}

func runNativeBrowserDisconnect(t *testing.T, url, connectionID, username, password, artifacts string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	script := filepath.Join("..", "webadmin", "native_disconnect_browser.cjs")
	cmd := exec.CommandContext(ctx, "node", script, url, connectionID, artifacts)
	cmd.Env = append(os.Environ(),
		"CHAST_BROWSER_USERNAME="+username,
		"CHAST_BROWSER_PASSWORD="+password,
	)
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("management browser disconnect failed: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func protectedNativeProfileFile(path string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	var envelope struct {
		Protected string `json:"protected"`
	}
	return json.Unmarshal(data, &envelope) == nil && envelope.Protected != ""
}

func startNativeFlutterViewer(t *testing.T, dir, name, binary, serverURL, serverPublicKey,
	peer, fingerprint, username, accountToken string, isolatedDownloads bool,
	statusDirectory string, shortcutAcceptance ...bool) *nativeProcess {
	t.Helper()
	profile := filepath.Join(dir, name+"-profile")
	downloadDir := filepath.Join(profile, "downloads")
	recordingDir := filepath.Join(profile, "Videos")
	chastDir := filepath.Join(profile, "chast")
	if err := os.MkdirAll(chastDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(downloadDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(recordingDir, 0700); err != nil {
		t.Fatal(err)
	}
	// Remove only this test-owned profile, after the viewer process has stopped.
	t.Cleanup(func() { _ = os.RemoveAll(profile) })
	publicKey, err := base64.RawURLEncoding.DecodeString(serverPublicKey)
	if err != nil {
		t.Fatalf("decode test server identity: %v", err)
	}
	serverFingerprint := fmt.Sprintf("%X", sha256.Sum256(publicKey))
	peerPreferences := map[string]any{
		"view_stats": statusDirectory == "" &&
			!(len(shortcutAcceptance) > 0 && shortcutAcceptance[0]),
	}
	if len(shortcutAcceptance) > 0 && shortcutAcceptance[0] {
		peerPreferences["view_default_only"] = true
		peerPreferences["view_remote_shortcuts"] = []map[string]any{
			{
				"action":    "toggleControl",
				"keyUsage":  0x40, // F7
				"modifiers": 1,    // Ctrl
				"keyLabel":  "F7",
			},
			{
				"action":    "toggleStats",
				"keyUsage":  0x41, // F8
				"modifiers": 1,    // Ctrl
				"keyLabel":  "F8",
			},
		}
	}
	profileFiles := map[string]any{
		"server.json": map[string]any{
			"url":      serverURL,
			"mode":     "auto",
			"key":      testutil.ServerKey,
			"link_key": testutil.ServerKey,
		},
		"account.json": map[string]any{
			"server":    serverURL,
			"username":  username,
			"token":     accountToken,
			"expiresAt": time.Now().Add(20 * time.Minute).UnixMilli(),
		},
		"server_identities.json": map[string]any{
			serverURL: map[string]any{
				"public_key":   serverPublicKey,
				"fingerprint":  serverFingerprint,
				"pinned_at_ms": time.Now().UnixMilli(),
			},
		},
		"trust.json": map[string]string{
			"server:" + serverURL + "/" + peer: fingerprint,
		},
		"ui.json": map[string]any{
			"peers": map[string]any{
				fmt.Sprintf("server:v1:%x/%s", sha256.Sum256([]byte(serverURL)), peer): peerPreferences,
			},
		},
	}
	for name, value := range profileFiles {
		data, err := json.Marshal(value)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(chastDir, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	env := os.Environ()
	filtered := env[:0]
	for _, entry := range env {
		key := strings.SplitN(entry, "=", 2)[0]
		if strings.EqualFold(key, "APPDATA") || strings.EqualFold(key, "USERPROFILE") || strings.EqualFold(key, "CHAST_NATIVE_TESTING") ||
			strings.EqualFold(key, "CHAST_NATIVE_TEST_TAB_LABEL") ||
			strings.EqualFold(key, "CHAST_NATIVE_TEST_STATUS_DIR") ||
			strings.EqualFold(key, "CHAST_NATIVE_TEST_DOWNLOAD_DIR") ||
			strings.EqualFold(key, "CHAST_NATIVE_TEST_UPLOAD_DIR") {
			continue
		}
		filtered = append(filtered, entry)
	}
	viewerEnv := append(filtered,
		"APPDATA="+profile,
		"USERPROFILE="+profile,
		"CHAST_NATIVE_TESTING=1",
		"CHAST_NATIVE_TEST_TICKET_DIR="+chastDir,
		"CHAST_NATIVE_TEST_TABS_TRACE="+filepath.Join(dir, "session-tabs.tsv"))
	if os.Getenv("CHAST_NATIVE_MULTIWINDOW") == "1" {
		viewerEnv = append(viewerEnv,
			"CHAST_NATIVE_TEST_TAB_LABEL=nativebox session title deliberately long for full tooltip acceptance 0123456789 abcdefghijklmnopqrstuvwxyz")
	}
	if statusDirectory != "" {
		viewerEnv = append(viewerEnv, "CHAST_NATIVE_TEST_STATUS_DIR="+statusDirectory)
	}
	if isolatedDownloads {
		viewerEnv = append(viewerEnv,
			"CHAST_NATIVE_TESTING=1",
			"CHAST_NATIVE_TEST_DOWNLOAD_DIR="+downloadDir,
			"CHAST_NATIVE_TEST_UPLOAD_DIR="+dir)
	}
	proc := startNativeProcessWithEnv(t, dir, name, binary, "", viewerEnv, true)
	nativeWait(t, "Flutter account and server credentials are DPAPI-protected", 20*time.Second, func() bool {
		select {
		case <-proc.done:
			t.Fatalf("%s exited during startup: %v (see %s.log)", name, proc.err, name)
		default:
		}
		return protectedNativeProfileFile(filepath.Join(chastDir, "server.json")) &&
			protectedNativeProfileFile(filepath.Join(chastDir, "account.json"))
	})
	runNativeFlutterHomeConnect(t, proc.cmd.Process.Pid, peer, "native-runtime-viewer-token", dir, 1, false)
	return proc
}

func startNativeFlutterFrameSampler(t *testing.T, pid int, artifacts string) *nativeProcess {
	t.Helper()
	script, err := filepath.Abs(filepath.Join("..", "..", "..", "tools", "native-flutter-frame-sampler.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	return startNativeProcess(t, artifacts, "flutter-frame-sampler", "powershell.exe", "",
		"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
		"-File", script, strconv.Itoa(pid), artifacts,
		filepath.Join(artifacts, "flutter-frame-sampler.stop"))
}

func finishNativeFlutterFrameSampler(t *testing.T, sampler *nativeProcess, artifacts string) []string {
	t.Helper()
	stopPath := filepath.Join(artifacts, "flutter-frame-sampler.stop")
	if err := os.WriteFile(stopPath, nil, 0600); err != nil {
		t.Fatal(err)
	}
	select {
	case <-sampler.done:
		if sampler.err != nil {
			log, _ := os.ReadFile(filepath.Join(artifacts, "flutter-frame-sampler.log"))
			t.Fatalf("Flutter frame sampler failed: %v\n%s", sampler.err, log)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("Flutter frame sampler did not stop after its stop marker")
	}
	paths, err := filepath.Glob(filepath.Join(artifacts, "flutter-secondary-frame-*.png"))
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) < 6 {
		log, _ := os.ReadFile(filepath.Join(artifacts, "flutter-frame-sampler.log"))
		t.Fatalf("Flutter frame sampler captured only %d samples\n%s", len(paths), log)
	}
	return paths
}

func changedFlutterStatsPixels(previousPath, nextPath string) (int, error) {
	previousFile, err := os.Open(previousPath)
	if err != nil {
		return 0, err
	}
	previous, err := png.Decode(previousFile)
	_ = previousFile.Close()
	if err != nil {
		return 0, err
	}
	nextFile, err := os.Open(nextPath)
	if err != nil {
		return 0, err
	}
	next, err := png.Decode(nextFile)
	_ = nextFile.Close()
	if err != nil {
		return 0, err
	}
	if previous.Bounds() != next.Bounds() {
		return 0, fmt.Errorf("Flutter sample dimensions changed: %v != %v", previous.Bounds(), next.Bounds())
	}
	bounds := previous.Bounds()
	region := image.Rect(bounds.Max.X-300, bounds.Min.Y+35, bounds.Max.X-8, bounds.Min.Y+430).Intersect(bounds)
	changed := 0
	for y := region.Min.Y; y < region.Max.Y; y += 4 {
		for x := region.Min.X; x < region.Max.X; x += 4 {
			a := color.RGBAModel.Convert(previous.At(x, y)).(color.RGBA)
			b := color.RGBAModel.Convert(next.At(x, y)).(color.RGBA)
			delta := absInt(int(a.R)-int(b.R)) + absInt(int(a.G)-int(b.G)) + absInt(int(a.B)-int(b.B))
			if delta > 48 {
				changed++
			}
		}
	}
	return changed, nil
}

func absInt(value int) int {
	if value < 0 {
		return -value
	}
	return value
}

func assertFlutterStatsChanged(t *testing.T, paths []string) {
	t.Helper()
	changedPairs := 0
	for i := 1; i < len(paths); i++ {
		changed, err := changedFlutterStatsPixels(paths[i-1], paths[i])
		if err != nil {
			t.Fatalf("compare Flutter frame samples: %v", err)
		}
		if changed >= 3 {
			changedPairs++
		}
	}
	comparisons := len(paths) - 1
	if changedPairs < comparisons*2/3 {
		t.Fatalf("non-target Flutter stats region changed in only %d/%d samples; inspect %s", changedPairs, comparisons, filepath.Dir(paths[0]))
	}
	t.Logf("verified non-target Flutter stats region changed across %d/%d consecutive samples (%d PNGs)", changedPairs, comparisons, len(paths))
}

func runNativeFlutterHomeConnect(t *testing.T, pid int, peer, token, artifacts string,
	expectedSessions int, useExistingCredentials bool) {
	t.Helper()
	script := filepath.Join("..", "..", "..", "tools", "native-flutter-home-connect.ps1")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script, strconv.Itoa(pid), peer, artifacts,
		"-ExpectedSessionCount", strconv.Itoa(expectedSessions))
	if useExistingCredentials {
		cmd.Args = append(cmd.Args, "-UseExistingCredentials")
	}
	if token == "" {
		cmd.Args = append(cmd.Args, "-AllowEmptyToken")
	}
	cmd.Stdin = strings.NewReader(token + "\n")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("could not start Flutter viewer session: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func runNativeFlutterCloseSelectedTab(t *testing.T, viewer *nativeProcess, peer, token, artifacts string,
	expectedSessions int, expectPostFailure ...bool) {
	t.Helper()
	pid := viewer.cmd.Process.Pid
	script := filepath.Join("..", "..", "..", "tools", "native-flutter-home-connect.ps1")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script, strconv.Itoa(pid), peer, artifacts,
		"-ExpectedSessionCount", strconv.Itoa(expectedSessions), "-UseExistingCredentials", "-CloseSelectedTab")
	if len(expectPostFailure) > 0 && expectPostFailure[0] {
		cmd.Args = append(cmd.Args, "-ExpectCloseFailure")
	}
	cmd.Stdin = strings.NewReader(token + "\n")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		processState := viewer.exitStatus(2 * time.Second)
		t.Fatalf("could not close the selected Flutter session tab: %v; viewer %s\n%s", err, processState, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func runNativeFlutterFileTransferUI(t *testing.T, pid int, artifacts, phase, uploadPath string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Second)
	defer cancel()
	script, err := filepath.Abs(filepath.Join("..", "..", "..", "tools", "native-flutter-file-transfer.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", script, "-ProcessId", strconv.Itoa(pid),
		"-ArtifactDirectory", artifacts, "-Phase", phase)
	if uploadPath != "" {
		cmd.Args = append(cmd.Args, "-UploadPath", uploadPath)
	}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Flutter file browser %s failed: %v\n%s", phase, err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func runNativeFlutterInputUI(t *testing.T, pid int, artifacts, tracePath string,
	multiSessionTabs bool, backgroundStatusPath string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	script, err := filepath.Abs(filepath.Join("..", "..", "..", "tools", "native-flutter-input.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script,
		"-ProcessId", strconv.Itoa(pid), "-ArtifactDirectory", artifacts, "-TracePath", tracePath)
	if multiSessionTabs {
		cmd.Args = append(cmd.Args, "-MultiSessionTabs")
	}
	if backgroundStatusPath != "" {
		cmd.Args = append(cmd.Args, "-HoldBackgroundSession", "-BackgroundStatusPath", backgroundStatusPath)
	}
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Flutter keyboard/mouse input acceptance failed: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func runNativeFlutterShortcutUI(t *testing.T, pid int, artifacts, tracePath,
	preferencesPath, peerPreferencesKey string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	script, err := filepath.Abs(filepath.Join("..", "..", "..", "tools", "native-flutter-input.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script,
		"-ProcessId", strconv.Itoa(pid),
		"-ArtifactDirectory", artifacts,
		"-TracePath", tracePath,
		"-VerifyKeyboardShortcuts",
		"-PreferencesPath", preferencesPath,
		"-PeerPreferencesKey", peerPreferencesKey)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Flutter keyboard shortcut acceptance failed: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func verifyNativeFlutterShortcutEvidence(t *testing.T, artifacts, tracePath string) {
	t.Helper()
	resultData, err := os.ReadFile(filepath.Join(artifacts, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter shortcut result: %v", err)
	}
	resultData = bytes.TrimPrefix(resultData, []byte{0xef, 0xbb, 0xbf})
	var result struct {
		ShortcutPreferenceLoaded          bool `json:"shortcut_preference_loaded"`
		ControlEnabledByShortcutVerified  bool `json:"control_enabled_by_shortcut_verified"`
		StatsShortcutWithCaptureVerified  bool `json:"stats_shortcut_with_capture_verified"`
		ControlDisabledByShortcutVerified bool `json:"control_disabled_by_shortcut_verified"`
		RemoteInputBlockedAfterControlOff bool `json:"remote_input_blocked_after_control_disabled"`
		StatsShortcutAfterControlDisabled bool `json:"stats_shortcut_after_control_disabled_verified"`
		FunctionKeysNotForwardedToHost    bool `json:"function_keys_not_forwarded_to_host"`
	}
	if err := json.Unmarshal(resultData, &result); err != nil {
		t.Fatalf("parse Flutter shortcut result: %v", err)
	}
	if !result.ShortcutPreferenceLoaded || !result.ControlEnabledByShortcutVerified ||
		!result.StatsShortcutWithCaptureVerified || !result.ControlDisabledByShortcutVerified ||
		!result.RemoteInputBlockedAfterControlOff || !result.StatsShortcutAfterControlDisabled ||
		!result.FunctionKeysNotForwardedToHost {
		t.Fatalf("Flutter shortcut UI did not verify the expected local action paths: %+v", result)
	}
	traceData, err := os.ReadFile(tracePath)
	if err != nil {
		t.Fatalf("read shortcut host input trace: %v", err)
	}
	for _, line := range strings.Split(string(traceData), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var event struct {
			Kind     string `json:"kind"`
			HIDUsage int    `json:"hid_usage"`
		}
		if err := json.Unmarshal([]byte(line), &event); err != nil {
			t.Fatalf("parse shortcut host input trace line %q: %v", line, err)
		}
		if event.Kind == "key" && (event.HIDUsage == 0x40 || event.HIDUsage == 0x41) {
			t.Fatalf("a configured F7/F8 shortcut reached the loopback host: %s", line)
		}
	}
	t.Log("PASS: Ctrl+F7 toggled remote control, Ctrl+F8 toggled only the local stats preference, and unbound input stopped after control was disabled")
}

func runNativeFlutterAudioIsolationUI(t *testing.T, pid int, artifacts,
	statusPathA, statusPathB string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	script, err := filepath.Abs(filepath.Join("..", "..", "..", "tools", "native-flutter-input.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script,
		"-ProcessId", strconv.Itoa(pid),
		"-ArtifactDirectory", artifacts,
		"-TracePath", filepath.Join(artifacts, "audio-tabs-input-events.jsonl"),
		"-MultiSessionTabs", "-VerifyAudioIsolation",
		"-AudioStatusPathA", statusPathA,
		"-AudioStatusPathB", statusPathB)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Flutter tab audio isolation acceptance failed: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func runNativeFlutterDisplayWindowLifecycleUI(t *testing.T, pid int, artifacts,
	disconnectMarker string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	script, err := filepath.Abs(filepath.Join("..", "..", "..", "tools", "native-flutter-input.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script,
		"-ProcessId", strconv.Itoa(pid),
		"-ArtifactDirectory", artifacts,
		"-TracePath", filepath.Join(artifacts, "display-window-lifecycle-input.jsonl"),
		"-MultiSessionTabs", "-VerifyDisplayWindowLifecycle",
		"-DisplayDisconnectMarkerPath", disconnectMarker)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Flutter display-window lifecycle acceptance failed: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func enableNativeFlutterAutoRecordForNextSession(t *testing.T, dir string) {
	t.Helper()
	path := filepath.Join(dir, "flutter-target-profile", "chast", "ui.json")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read isolated Flutter preferences: %v", err)
	}
	var prefs map[string]any
	if err := json.Unmarshal(data, &prefs); err != nil {
		t.Fatalf("parse isolated Flutter preferences: %v", err)
	}
	prefs["view_auto_record"] = true
	data, err = json.Marshal(prefs)
	if err != nil {
		t.Fatalf("encode isolated Flutter preferences: %v", err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatalf("enable auto-record for the second session only: %v", err)
	}
}

func waitNativeFlutterStatusPaths(t *testing.T, directory string, count int) []string {
	t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		entries, err := os.ReadDir(directory)
		if err != nil && !os.IsNotExist(err) {
			t.Fatalf("read isolated viewer status directory: %v", err)
		}
		files := make([]string, 0, len(entries))
		for _, entry := range entries {
			if !entry.IsDir() && strings.EqualFold(filepath.Ext(entry.Name()), ".tsv") {
				files = append(files, filepath.Join(directory, entry.Name()))
			}
		}
		if len(files) >= count {
			sort.Strings(files)
			return files
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("expected %d per-session status snapshots in %s", count, directory)
	return nil
}

type nativeViewerStatusSnapshot struct {
	auditRef      string
	state         uint64
	received      uint64
	decoded       uint64
	published     uint64
	recording     bool
	recordSamples uint64
	recordPath    string
	audioEnabled  bool
	audioMuted    bool
	audioReceived uint64
	audioDecoded  uint64
}

type nativeAwakeLeaseSnapshot struct {
	Event    string `json:"event"`
	AuditRef string `json:"audit_ref"`
	State    uint64 `json:"state"`
	Held     bool   `json:"held"`
}

func readNativeAwakeLeaseSnapshots(path string) (map[string]nativeAwakeLeaseSnapshot, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, false
	}
	lines := strings.Split(strings.TrimSpace(string(data)), "\n")
	latest := make(map[string]nativeAwakeLeaseSnapshot)
	for _, line := range lines {
		fields := strings.Split(strings.TrimRight(line, "\r"), "\t")
		if len(fields) != 4 || fields[1] == "" {
			return nil, false
		}
		state, err := strconv.ParseUint(fields[2], 10, 64)
		if err != nil || state > 5 {
			return nil, false
		}
		held, err := strconv.ParseUint(fields[3], 10, 64)
		if err != nil || held > 1 {
			return nil, false
		}
		latest[fields[1]] = nativeAwakeLeaseSnapshot{
			Event: fields[0], AuditRef: fields[1], State: state, Held: held == 1,
		}
	}
	return latest, len(latest) > 0
}

func readNativeViewerStatusSnapshot(path string) (nativeViewerStatusSnapshot, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nativeViewerStatusSnapshot{}, false
	}
	fields := strings.SplitN(strings.TrimRight(string(data), "\r\n"), "\t", 12)
	if len(fields) != 8 && len(fields) != 12 {
		return nativeViewerStatusSnapshot{}, false
	}
	values := make([]uint64, 6)
	for i, fieldIndex := range []int{0, 2, 3, 4, 5, 6} {
		value, err := strconv.ParseUint(fields[fieldIndex], 10, 64)
		if err != nil {
			return nativeViewerStatusSnapshot{}, false
		}
		values[i] = value
	}
	if values[4] > 1 {
		return nativeViewerStatusSnapshot{}, false
	}
	snapshot := nativeViewerStatusSnapshot{
		auditRef:      fields[1],
		state:         values[0],
		received:      values[1],
		decoded:       values[2],
		published:     values[3],
		recording:     values[4] == 1,
		recordSamples: values[5],
		recordPath:    fields[7],
	}
	if len(fields) == 12 {
		audioEnabled, err := strconv.ParseUint(fields[8], 10, 64)
		if err != nil || audioEnabled > 1 {
			return nativeViewerStatusSnapshot{}, false
		}
		audioMuted, err := strconv.ParseUint(fields[9], 10, 64)
		if err != nil || audioMuted > 1 {
			return nativeViewerStatusSnapshot{}, false
		}
		snapshot.audioEnabled = audioEnabled == 1
		snapshot.audioMuted = audioMuted == 1
		if snapshot.audioReceived, err = strconv.ParseUint(fields[10], 10, 64); err != nil {
			return nativeViewerStatusSnapshot{}, false
		}
		if snapshot.audioDecoded, err = strconv.ParseUint(fields[11], 10, 64); err != nil {
			return nativeViewerStatusSnapshot{}, false
		}
	}
	return snapshot, true
}

func waitNativeFlutterRecordingStatusPath(t *testing.T, directory, recordingDirectory string) string {
	t.Helper()
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		for _, path := range waitNativeFlutterStatusPaths(t, directory, 2) {
			snapshot, ok := readNativeViewerStatusSnapshot(path)
			if !ok || snapshot.state != 3 || !snapshot.recording || snapshot.recordSamples == 0 || snapshot.recordPath == "" {
				continue
			}
			relative, err := filepath.Rel(recordingDirectory, snapshot.recordPath)
			if err == nil && relative != "." && relative != ".." &&
				!strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
				return path
			}
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("no streaming, recording session snapshot points into %s", recordingDirectory)
	return ""
}

func waitNativeFlutterNonRecordingStatusPath(t *testing.T, directory string) string {
	t.Helper()
	paths := waitNativeFlutterStatusPaths(t, directory, 1)
	path := paths[0]
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		snapshot, ok := readNativeViewerStatusSnapshot(path)
		if ok && snapshot.auditRef != "" && snapshot.state == 3 &&
			!snapshot.recording && snapshot.recordSamples == 0 && snapshot.recordPath == "" {
			return path
		}
		time.Sleep(200 * time.Millisecond)
	}
	snapshot, _ := readNativeViewerStatusSnapshot(path)
	t.Fatalf("initial Flutter session did not reach streaming without recording: %+v", snapshot)
	return ""
}

func verifyNativeFlutterTabRecordingIsolation(t *testing.T, artifacts, firstStatusPath,
	secondStatusPath, recordingDirectory string) {
	t.Helper()
	if firstStatusPath == secondStatusPath {
		t.Fatalf("session A and B share one status snapshot: %s", firstStatusPath)
	}
	data, err := os.ReadFile(filepath.Join(artifacts, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter tab recording result: %v", err)
	}
	data = bytes.TrimPrefix(data, []byte{0xef, 0xbb, 0xbf})
	var result struct {
		BackgroundRecordingBefore     bool   `json:"background_recording_before"`
		BackgroundRecordingAfter      bool   `json:"background_recording_after"`
		BackgroundRecordSamplesBefore uint64 `json:"background_record_samples_before"`
		BackgroundRecordSamplesAfter  uint64 `json:"background_record_samples_after"`
		BackgroundRecordingPath       string `json:"background_recording_path"`
		SessionARecordingButtonOK     bool   `json:"session_a_recording_button_verified"`
		SessionARecordingButtonName   string `json:"session_a_recording_button_name"`
		SessionARecordingButtonPixels int    `json:"session_a_recording_button_accent_pixels"`
		SessionARecordingScreenshot   string `json:"session_a_recording_screenshot"`
		SessionBRecordingButtonOK     bool   `json:"session_b_recording_button_verified"`
		SessionBRecordingButtonName   string `json:"session_b_recording_button_name"`
		SessionBRecordingButtonPixels int    `json:"session_b_recording_button_accent_pixels"`
		SessionBRecordingScreenshot   string `json:"session_b_recording_screenshot"`
	}
	if err := json.Unmarshal(data, &result); err != nil {
		t.Fatalf("parse Flutter tab recording result: %v", err)
	}
	if !result.BackgroundRecordingBefore || !result.BackgroundRecordingAfter ||
		result.BackgroundRecordSamplesAfter <= result.BackgroundRecordSamplesBefore ||
		!result.SessionARecordingButtonOK || result.SessionARecordingButtonPixels > 4 ||
		!result.SessionBRecordingButtonOK || result.SessionBRecordingButtonPixels < 20 ||
		result.BackgroundRecordingPath == "" || result.SessionARecordingScreenshot == "" ||
		result.SessionBRecordingScreenshot == "" {
		t.Fatalf("session B did not keep recording and advance samples: %+v", result)
	}
	for _, screenshot := range []string{result.SessionARecordingScreenshot, result.SessionBRecordingScreenshot} {
		if info, err := os.Stat(screenshot); err != nil || info.Size() == 0 {
			t.Fatalf("recording toolbar screenshot is missing or empty: %s (err=%v)", screenshot, err)
		}
	}
	relative, err := filepath.Rel(recordingDirectory, result.BackgroundRecordingPath)
	if err != nil || relative == "." || relative == ".." ||
		strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		t.Fatalf("session B recording path %q is outside %s", result.BackgroundRecordingPath, recordingDirectory)
	}

	deadline := time.Now().Add(30 * time.Second)
	var first, second nativeViewerStatusSnapshot
	for time.Now().Before(deadline) {
		var firstOK, secondOK bool
		first, firstOK = readNativeViewerStatusSnapshot(firstStatusPath)
		second, secondOK = readNativeViewerStatusSnapshot(secondStatusPath)
		if firstOK && secondOK && first.auditRef != "" && second.auditRef != "" &&
			first.auditRef != second.auditRef && first.state == 3 && !first.recording &&
			first.recordSamples == 0 && first.recordPath == "" && second.state == 3 &&
			second.recording && second.recordSamples >= result.BackgroundRecordSamplesAfter &&
			second.recordPath == result.BackgroundRecordingPath {
			isolation := map[string]any{
				"session_a_audit_ref":                      first.auditRef,
				"session_a_state":                          first.state,
				"session_a_recording":                      first.recording,
				"session_a_record_samples":                 first.recordSamples,
				"session_a_recording_button_verified":      result.SessionARecordingButtonOK,
				"session_a_recording_button_name":          result.SessionARecordingButtonName,
				"session_a_recording_button_accent_pixels": result.SessionARecordingButtonPixels,
				"session_a_recording_screenshot":           result.SessionARecordingScreenshot,
				"session_b_audit_ref":                      second.auditRef,
				"session_b_state":                          second.state,
				"session_b_recording":                      second.recording,
				"session_b_record_samples":                 second.recordSamples,
				"session_b_recording_button_verified":      result.SessionBRecordingButtonOK,
				"session_b_recording_button_name":          result.SessionBRecordingButtonName,
				"session_b_recording_button_accent_pixels": result.SessionBRecordingButtonPixels,
				"session_b_recording_screenshot":           result.SessionBRecordingScreenshot,
				"recording_button_verification":            "toolbar screenshot accent pixels; UI Automation unavailable for Flutter controls",
				"samples_before_hidden_hold":               result.BackgroundRecordSamplesBefore,
				"samples_after_hidden_hold":                result.BackgroundRecordSamplesAfter,
				"recording_path":                           second.recordPath,
			}
			encoded, err := json.MarshalIndent(isolation, "", "  ")
			if err != nil {
				t.Fatalf("encode Flutter tab recording isolation evidence: %v", err)
			}
			if err := os.WriteFile(filepath.Join(artifacts, "flutter-tab-recording-isolation.json"), encoded, 0600); err != nil {
				t.Fatalf("write Flutter tab recording isolation evidence: %v", err)
			}
			t.Logf("PASS: session A remains streaming without recording while session B records; toolbar accent pixels A=%d, B=%d; evidence: %s", result.SessionARecordingButtonPixels, result.SessionBRecordingButtonPixels, filepath.Join(artifacts, "flutter-tab-recording-isolation.json"))
			return
		}
		time.Sleep(200 * time.Millisecond)
	}
	t.Fatalf("per-session recording state was not isolated: A=%+v B=%+v; expected A streaming/not recording and B streaming/recording with samples >= %d", first, second, result.BackgroundRecordSamplesAfter)
}

func verifyNativeFlutterTabInputEvidence(t *testing.T, artifacts, tracePath string) {
	t.Helper()
	resultData, err := os.ReadFile(filepath.Join(artifacts, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter multi-session input result: %v", err)
	}
	resultData = bytes.TrimPrefix(resultData, []byte{0xef, 0xbb, 0xbf})
	var result struct {
		ControlOnVerified                  bool   `json:"control_on_verified"`
		TabSwitchReleasedRemoteA           bool   `json:"tab_switch_released_remote_a"`
		BackgroundPointerInjectionRejected bool   `json:"background_pointer_injection_rejected"`
		InputReleaseReceived               bool   `json:"input_release_received"`
		ReleaseHeldQueued                  bool   `json:"release_held_queued"`
		ReleaseAllStarted                  bool   `json:"release_all_started"`
		ReleaseAllCompleted                bool   `json:"release_all_completed"`
		InjectedAUpBeforePhysicalCleanup   bool   `json:"injected_a_up_before_physical_cleanup"`
		BackgroundMouseEventsBefore        int    `json:"background_mouse_events_before"`
		BackgroundMouseEventsAfter         int    `json:"background_mouse_events_after"`
		TraceEventsBeforeBackgroundInput   int    `json:"trace_events_before_background_input"`
		TraceEventsAfterBackgroundInput    int    `json:"trace_events_after_background_input"`
		BackgroundSessionReturnedToA       bool   `json:"background_session_returned_to_a"`
		BackgroundSessionHoldMS            int    `json:"background_session_hold_ms"`
		BackgroundStreamingBefore          bool   `json:"background_streaming_before"`
		BackgroundStreamingAfter           bool   `json:"background_streaming_after"`
		BackgroundReceivedBefore           uint64 `json:"background_received_before"`
		BackgroundReceivedAfter            uint64 `json:"background_received_after"`
		BackgroundDecodedBefore            uint64 `json:"background_decoded_before"`
		BackgroundDecodedAfter             uint64 `json:"background_decoded_after"`
		BackgroundPublishedBefore          uint64 `json:"background_published_before"`
		BackgroundPublishedAfter           uint64 `json:"background_published_after"`
		BackgroundRecordingBefore          bool   `json:"background_recording_before"`
		BackgroundRecordingAfter           bool   `json:"background_recording_after"`
		BackgroundRecordSamplesBefore      uint64 `json:"background_record_samples_before"`
		BackgroundRecordSamplesAfter       uint64 `json:"background_record_samples_after"`
		BackgroundRecordingPath            string `json:"background_recording_path"`
	}
	if err := json.Unmarshal(resultData, &result); err != nil {
		t.Fatalf("parse Flutter multi-session input result: %v", err)
	}
	if !result.ControlOnVerified || !result.TabSwitchReleasedRemoteA ||
		!result.BackgroundPointerInjectionRejected || !result.InputReleaseReceived ||
		!result.ReleaseHeldQueued || !result.ReleaseAllStarted ||
		!result.ReleaseAllCompleted || !result.InjectedAUpBeforePhysicalCleanup ||
		result.BackgroundMouseEventsBefore != result.BackgroundMouseEventsAfter ||
		result.TraceEventsBeforeBackgroundInput <= 0 ||
		result.TraceEventsAfterBackgroundInput < result.TraceEventsBeforeBackgroundInput ||
		!result.BackgroundSessionReturnedToA || result.BackgroundSessionHoldMS < 1800 ||
		!result.BackgroundStreamingBefore || !result.BackgroundStreamingAfter ||
		result.BackgroundReceivedAfter <= result.BackgroundReceivedBefore ||
		result.BackgroundDecodedAfter <= result.BackgroundDecodedBefore ||
		result.BackgroundPublishedAfter <= result.BackgroundPublishedBefore ||
		!result.BackgroundRecordingBefore || !result.BackgroundRecordingAfter ||
		result.BackgroundRecordSamplesAfter <= result.BackgroundRecordSamplesBefore {
		t.Fatalf("Flutter tab input acceptance did not prove switch release and background rejection: %+v", result)
	}
	traceData, err := os.ReadFile(tracePath)
	if err != nil {
		t.Fatalf("read multi-session native input trace: %v", err)
	}
	type traceEvent struct {
		Kind      string `json:"kind"`
		Action    string `json:"action"`
		HIDUsage  int    `json:"hid_usage"`
		Stage     string `json:"stage"`
		InputType uint32 `json:"input_type"`
		ScanCode  uint32 `json:"scan_code"`
		Flags     uint32 `json:"flags"`
	}
	events := make([]traceEvent, 0)
	for _, line := range strings.Split(string(traceData), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var event traceEvent
		if err := json.Unmarshal([]byte(line), &event); err != nil {
			t.Fatalf("parse multi-session input trace line %q: %v", line, err)
		}
		events = append(events, event)
	}
	if result.TraceEventsBeforeBackgroundInput > len(events) {
		t.Fatalf("background input trace boundary exceeds trace: boundary=%d total=%d", result.TraceEventsBeforeBackgroundInput, len(events))
	}
	for _, event := range events[result.TraceEventsBeforeBackgroundInput:] {
		if event.Kind == "mouse" {
			t.Fatalf("hidden Flutter view emitted a remote mouse event after tab switch: %+v", event)
		}
	}
	aDown, received, queued, releaseBegin, aUp, releaseEnd := -1, -1, -1, -1, -1, -1
	for i, event := range events {
		if event.Kind == "key" && event.HIDUsage == 0x04 && event.Action == "down" && aDown < 0 {
			aDown = i
		}
		if event.Kind == "release_control" && event.Stage == "input_release_received" && aDown >= 0 && received < 0 {
			received = i
		}
		if event.Kind == "release_control" && event.Stage == "release_held_queued" && received >= 0 && queued < 0 {
			queued = i
		}
		if event.Kind == "release_control" && event.Stage == "release_all_begin" && queued >= 0 && releaseBegin < 0 {
			releaseBegin = i
		}
		if event.Kind == "injected" && event.InputType == 1 && event.ScanCode == 0x1e &&
			event.Flags&0x2 != 0 && releaseBegin >= 0 && aUp < 0 {
			aUp = i
		}
		if event.Kind == "release_control" && event.Stage == "release_all_end" && aUp >= 0 && releaseEnd < 0 {
			releaseEnd = i
		}
	}
	if !(aDown >= 0 && aDown < received && received < queued && queued < releaseBegin &&
		releaseBegin < aUp && aUp < releaseEnd && releaseEnd < result.TraceEventsBeforeBackgroundInput) {
		t.Fatalf("tab switch release trace order is incomplete: down=%d received=%d queued=%d begin=%d up=%d end=%d boundary=%d", aDown, received, queued, releaseBegin, aUp, releaseEnd, result.TraceEventsBeforeBackgroundInput)
	}
	t.Logf("PASS: tab switch released held remote A before hidden-view input probe; hidden pointer produced no host input (%s)", tracePath)
}

func verifyNativeFlutterInputEvidence(t *testing.T, artifacts, tracePath string) {
	t.Helper()
	resultData, err := os.ReadFile(filepath.Join(artifacts, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter input result: %v", err)
	}
	resultData = bytes.TrimPrefix(resultData, []byte{0xef, 0xbb, 0xbf})
	var result struct {
		ControlOnClicked                 bool `json:"control_on_clicked"`
		ControlOnVerified                bool `json:"control_on_verified"`
		ControlOffClicked                bool `json:"control_off_clicked"`
		ControlOffVerified               bool `json:"control_off_verified"`
		RemoteAUpBeforePhysicalCleanup   bool `json:"remote_a_up_before_physical_cleanup"`
		PhysicalCleanupHasNoKeyAEvent    bool `json:"physical_cleanup_has_no_key_a_event"`
		InputReleaseReceived             bool `json:"input_release_received"`
		ReleaseHeldQueued                bool `json:"release_held_queued"`
		ReleaseAllStarted                bool `json:"release_all_started"`
		ReleaseAllCompleted              bool `json:"release_all_completed"`
		InjectedAUpBeforePhysicalCleanup bool `json:"injected_a_up_before_physical_cleanup"`
		TraceEventsBeforePhysicalCleanup int  `json:"trace_events_before_physical_cleanup"`
		TraceEventsAfterPhysicalCleanup  int  `json:"trace_events_after_physical_cleanup"`
	}
	if err := json.Unmarshal(resultData, &result); err != nil {
		t.Fatalf("parse Flutter input result: %v", err)
	}
	if !result.ControlOnClicked || !result.ControlOnVerified ||
		!result.ControlOffClicked || !result.ControlOffVerified ||
		!result.RemoteAUpBeforePhysicalCleanup || !result.PhysicalCleanupHasNoKeyAEvent ||
		!result.InputReleaseReceived || !result.ReleaseHeldQueued ||
		!result.ReleaseAllStarted || !result.ReleaseAllCompleted ||
		!result.InjectedAUpBeforePhysicalCleanup {
		t.Fatalf("Flutter input UI did not verify the complete release sequence: %+v", result)
	}
	traceData, err := os.ReadFile(tracePath)
	if err != nil {
		t.Fatalf("read native input trace: %v", err)
	}
	type nativeInputTraceEvent struct {
		Kind      string `json:"kind"`
		Action    string `json:"action"`
		HIDUsage  int    `json:"hid_usage"`
		Stage     string `json:"stage"`
		InputType uint32 `json:"input_type"`
		ScanCode  uint32 `json:"scan_code"`
		Flags     uint32 `json:"flags"`
	}
	var sawMouse bool
	aDownIndex := -1
	releaseReceivedIndex := -1
	releaseQueuedIndex := -1
	releaseAllBeginIndex := -1
	injectedAUpIndex := -1
	releaseAllEndIndex := -1
	traceEvents := make([]nativeInputTraceEvent, 0)
	for _, line := range strings.Split(string(traceData), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var event nativeInputTraceEvent
		if err := json.Unmarshal([]byte(line), &event); err != nil {
			t.Fatalf("parse native input trace line %q: %v", line, err)
		}
		traceEvents = append(traceEvents, event)
		index := len(traceEvents) - 1
		if event.Kind == "mouse" {
			sawMouse = true
		}
		if event.Kind == "key" && event.HIDUsage == 0x04 && event.Action == "down" && aDownIndex == -1 {
			aDownIndex = index
		}
		if event.Kind == "release_control" && event.Stage == "input_release_received" &&
			aDownIndex >= 0 &&
			releaseReceivedIndex == -1 {
			releaseReceivedIndex = index
		}
		if event.Kind == "release_control" && event.Stage == "release_held_queued" &&
			releaseReceivedIndex >= 0 && releaseQueuedIndex == -1 {
			releaseQueuedIndex = index
		}
		if event.Kind == "release_control" && event.Stage == "release_all_begin" &&
			releaseQueuedIndex >= 0 && releaseAllBeginIndex == -1 {
			releaseAllBeginIndex = index
		}
		if event.Kind == "injected" && event.InputType == 1 && event.ScanCode == 0x1e &&
			event.Flags&0x2 != 0 && releaseAllBeginIndex >= 0 && injectedAUpIndex == -1 {
			injectedAUpIndex = index
		}
		if event.Kind == "release_control" && event.Stage == "release_all_end" &&
			injectedAUpIndex >= 0 && releaseAllEndIndex == -1 {
			releaseAllEndIndex = index
		}
	}
	if !sawMouse || aDownIndex < 0 || releaseReceivedIndex < 0 || releaseQueuedIndex < 0 ||
		releaseAllBeginIndex < 0 || injectedAUpIndex < 0 || releaseAllEndIndex < 0 {
		t.Fatalf("native input trace missing ordered release evidence: mouse=%t A-down=%d release-received=%d release-queued=%d release-all-begin=%d injected-A-up=%d release-all-end=%d (%s)",
			sawMouse, aDownIndex, releaseReceivedIndex, releaseQueuedIndex,
			releaseAllBeginIndex, injectedAUpIndex, releaseAllEndIndex, tracePath)
	}
	if !(aDownIndex < releaseReceivedIndex && releaseReceivedIndex < releaseQueuedIndex &&
		releaseQueuedIndex < releaseAllBeginIndex && releaseAllBeginIndex < injectedAUpIndex &&
		injectedAUpIndex < releaseAllEndIndex) {
		t.Fatalf("native input trace has an invalid release order: A-down=%d received=%d queued=%d begin=%d A-up=%d end=%d (%s)",
			aDownIndex, releaseReceivedIndex, releaseQueuedIndex,
			releaseAllBeginIndex, injectedAUpIndex, releaseAllEndIndex, tracePath)
	}
	if result.TraceEventsBeforePhysicalCleanup <= 0 || result.TraceEventsBeforePhysicalCleanup > len(traceEvents) {
		t.Fatalf("invalid trace event count before physical cleanup: before=%d total=%d", result.TraceEventsBeforePhysicalCleanup, len(traceEvents))
	}
	if result.TraceEventsAfterPhysicalCleanup < result.TraceEventsBeforePhysicalCleanup || result.TraceEventsAfterPhysicalCleanup > len(traceEvents) {
		t.Fatalf("invalid trace event count after physical cleanup: before=%d after=%d total=%d", result.TraceEventsBeforePhysicalCleanup, result.TraceEventsAfterPhysicalCleanup, len(traceEvents))
	}
	if releaseAllEndIndex >= result.TraceEventsBeforePhysicalCleanup {
		t.Fatalf("release_all did not complete before the physical cleanup boundary: end=%d boundary=%d", releaseAllEndIndex, result.TraceEventsBeforePhysicalCleanup)
	}
	for _, event := range traceEvents[result.TraceEventsBeforePhysicalCleanup:] {
		if event.Kind == "key" && event.HIDUsage == 0x04 {
			t.Fatalf("physical A cleanup emitted an unexpected trace event after the release boundary: %+v", event)
		}
	}
	t.Logf("PASS: Flutter sent mouse/A down, host received InputRelease, queued release_held, and release_all injected A-up at %d before cleanup boundary %d; physical cleanup added no remote A event (%s)", injectedAUpIndex, result.TraceEventsBeforePhysicalCleanup, tracePath)
}

func nativeFile(path string) bool { _, err := os.Stat(path); return err == nil }

func nativeAgentPID(parent int) (uint32, error) {
	h, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return 0, err
	}
	defer windows.CloseHandle(h)
	entry := windows.ProcessEntry32{}
	entry.Size = uint32(unsafe.Sizeof(entry))
	for err = windows.Process32First(h, &entry); err == nil; err = windows.Process32Next(h, &entry) {
		if entry.ParentProcessID == uint32(parent) && strings.EqualFold(windows.UTF16ToString(entry.ExeFile[:]), "chast-tunnel.exe") {
			return entry.ProcessID, nil
		}
	}
	return 0, fmt.Errorf("owned tray has no tunnel child")
}

func TestWindowsNativeRuntimeAcceptance(t *testing.T) {
	bin := os.Getenv("CHAST_NATIVE_BIN_DIR")
	if bin == "" {
		t.Skip("CHAST_NATIVE_BIN_DIR is not set (opt-in real Windows processes)")
	}
	bin, err := filepath.Abs(bin)
	if err != nil {
		t.Fatal(err)
	}
	browserUI := os.Getenv("CHAST_NATIVE_BROWSER_UI") == "1"
	flutterUI := os.Getenv("CHAST_NATIVE_FLUTTER_UI") == "1"
	dualFlutter := os.Getenv("CHAST_NATIVE_DUAL_FLUTTER") == "1"
	multiWindowUI := os.Getenv("CHAST_NATIVE_MULTIWINDOW") == "1"
	multiWindowInputUI := os.Getenv("CHAST_NATIVE_MULTIWINDOW_INPUT") == "1"
	fileTransferUI := os.Getenv("CHAST_NATIVE_FILE_TRANSFER") == "1"
	viewerStrategyUI := os.Getenv("CHAST_NATIVE_VIEWER_STRATEGY") == "1"
	inputInjectionUI := os.Getenv("CHAST_NATIVE_INPUT_INJECTION") == "1"
	keyboardShortcutsUI := os.Getenv("CHAST_NATIVE_KEYBOARD_SHORTCUTS") == "1"
	if flutterUI && !browserUI && !multiWindowUI && !fileTransferUI && !viewerStrategyUI && !inputInjectionUI && !keyboardShortcutsUI {
		t.Fatal("Flutter UI acceptance requires browser, multi-window, file-transfer, viewer-strategy, input-injection, or keyboard-shortcut mode")
	}
	if dualFlutter && (!flutterUI || !browserUI) {
		t.Fatal("dual Flutter acceptance requires CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_BROWSER_UI=1")
	}
	if multiWindowUI && (!flutterUI || browserUI || dualFlutter || fileTransferUI || viewerStrategyUI || inputInjectionUI || keyboardShortcutsUI) {
		t.Fatal("multi-window acceptance requires only CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_MULTIWINDOW=1")
	}
	if multiWindowInputUI && (!multiWindowUI || !flutterUI || browserUI || dualFlutter || fileTransferUI || viewerStrategyUI || inputInjectionUI || keyboardShortcutsUI) {
		t.Fatal("multi-window input acceptance requires Flutter multi-window mode only")
	}
	if fileTransferUI && !flutterUI {
		t.Fatal("Flutter file transfer acceptance requires CHAST_NATIVE_FLUTTER_UI=1")
	}
	if viewerStrategyUI && (!flutterUI || browserUI || dualFlutter || fileTransferUI) {
		t.Fatal("viewer strategy acceptance requires only CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_VIEWER_STRATEGY=1")
	}
	if inputInjectionUI && (!flutterUI || browserUI || dualFlutter || fileTransferUI || viewerStrategyUI || keyboardShortcutsUI) {
		t.Fatal("input injection acceptance requires only CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_INPUT_INJECTION=1")
	}
	if keyboardShortcutsUI && (!flutterUI || browserUI || dualFlutter || multiWindowUI || fileTransferUI || viewerStrategyUI || inputInjectionUI) {
		t.Fatal("keyboard shortcut acceptance requires only CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_KEYBOARD_SHORTCUTS=1")
	}
	for _, name := range []string{"chast-host.exe", "chast-tunnel.exe", "chast-live-viewer-probe.exe"} {
		if !nativeFile(filepath.Join(bin, name)) {
			t.Fatalf("missing native binary: %s", name)
		}
	}
	flutterBin := os.Getenv("CHAST_NATIVE_FLUTTER_BIN")
	if flutterUI && !nativeFile(flutterBin) {
		t.Fatalf("missing Flutter Release viewer: %s", flutterBin)
	}
	dir := t.TempDir()
	if root := os.Getenv("CHAST_NATIVE_ARTIFACT_DIR"); root != "" {
		dir, err = os.MkdirTemp(root, "run-")
		if err != nil {
			t.Fatal(err)
		}
	}
	t.Logf("artifacts: %s", dir)
	issuer := newTestIssuer(t)
	s := testutil.StartWith(t, testutil.Options{AccessIssuer: issuer})
	target, _ := url.Parse(s.URL)
	proxy := httputil.NewSingleHostReverseProxy(target)
	var capturedViewerDelivery atomic.Pointer[account.ViewerStrategyDelivery]
	proxy.ModifyResponse = func(resp *http.Response) error {
		if !viewerStrategyUI || resp.StatusCode != http.StatusOK || resp.Request == nil ||
			resp.Request.URL.Path != "/v1/hosts/nativebox" {
			return nil
		}
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			return err
		}
		if err := resp.Body.Close(); err != nil {
			return err
		}
		resp.Body = io.NopCloser(bytes.NewReader(body))
		resp.ContentLength = int64(len(body))
		resp.Header.Set("Content-Length", strconv.Itoa(len(body)))
		var lookup struct {
			ViewerStrategy *account.ViewerStrategyDelivery `json:"viewer_strategy"`
		}
		if err := json.Unmarshal(body, &lookup); err == nil && lookup.ViewerStrategy != nil {
			capturedViewerDelivery.Store(lookup.ViewerStrategy)
		}
		return nil
	}
	var offline atomic.Bool
	var blockReceipts atomic.Bool
	var browserDisconnectRequestKey atomic.Value
	browserDisconnectRequestKey.Store("")
	front := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if browserUI && strings.HasSuffix(r.URL.Path, "/disconnect") {
			body, err := io.ReadAll(r.Body)
			if err != nil {
				http.Error(w, "could not read disconnect request", http.StatusBadRequest)
				return
			}
			var request struct {
				RequestKey string `json:"request_key"`
			}
			if json.Unmarshal(body, &request) == nil {
				browserDisconnectRequestKey.Store(request.RequestKey)
			}
			r.Body = io.NopCloser(bytes.NewReader(body))
		}
		if offline.Load() || (blockReceipts.Load() && strings.HasSuffix(r.URL.Path, "/receipts")) {
			http.Error(w, "isolated control-plane outage", http.StatusServiceUnavailable)
			return
		}
		proxy.ServeHTTP(w, r)
	}))
	t.Cleanup(front.Close)
	udp, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := udp.LocalAddr().(*net.UDPAddr).Port
	defer udp.Close()
	// The tray INI rejects port 0 and would fall back to a wildcard listener.
	// Reserve the tunnel port while the host port is still held so they differ.
	tunnelUDP, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer tunnelUDP.Close()
	tunnelListen := tunnelUDP.LocalAddr().String()
	const peer = "nativebox"
	const deviceKey = "native-runtime-device-key"
	const token = "native-runtime-viewer-token"
	const accountPassword = "native acceptance password"
	viewerUser, err := s.Accounts.CreateUser("native-acceptance", accountPassword)
	if err != nil {
		t.Fatal(err)
	}
	var viewerStrategyPolicyID, viewerStrategyAssignmentID string
	var viewerStrategySettings strategy.Settings
	if viewerStrategyUI {
		policy, err := s.Accounts.CreatePolicyObject("native viewer strategy acceptance", account.DevicePolicyOpen, 0, nil, nil, "native-acceptance")
		if err != nil {
			t.Fatalf("create isolated viewer strategy policy: %v", err)
		}
		assignment, err := s.Accounts.CreatePolicyAssignment(policy.ID, account.PolicyScopeAll, "", 0, "native-acceptance")
		if err != nil {
			t.Fatalf("assign isolated viewer strategy policy: %v", err)
		}
		viewerStrategyPolicyID = policy.ID
		viewerStrategyAssignmentID = assignment.ID
		viewerStrategySettings = strategy.Settings{
			"viewer.tunnel_mode":            {Mode: strategy.Locked, Value: json.RawMessage(`"auto"`)},
			"viewer.use_websocket":          {Mode: strategy.Locked, Value: json.RawMessage(`false`)},
			"viewer.upload_recordings":      {Mode: strategy.Locked, Value: json.RawMessage(`false`)},
			"viewer.sync_initial_clipboard": {Mode: strategy.Locked, Value: json.RawMessage(`false`)},
			"viewer.view_fit":               {Mode: strategy.Locked, Value: json.RawMessage(`1`)},
		}
		if _, err := s.Accounts.SetClientStrategy(policy.ID, 0, viewerStrategySettings, "native-acceptance"); err != nil {
			t.Fatalf("configure isolated viewer strategy policy: %v", err)
		}
	}
	if browserUI {
		if _, err := s.Accounts.CreateAdminRole("native-runtime-manager", "Native Runtime Manager", "isolated browser acceptance", account.AllPermissions()); err != nil {
			t.Fatal(err)
		}
		if _, err := s.Accounts.BindRole("native-acceptance", "native-runtime-manager", "global", "", ""); err != nil {
			t.Fatal(err)
		}
	}
	accountToken := login(t, s.URL, "native-acceptance", accountPassword).Token
	var serverPublicKey string
	if flutterUI {
		status, body := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
		if status != http.StatusOK {
			t.Fatalf("read test server identity: %d %s", status, body)
		}
		var info struct {
			PublicKey string `json:"public_key"`
		}
		if err := json.Unmarshal(body, &info); err != nil || info.PublicKey == "" {
			t.Fatalf("invalid test server identity: %v", err)
		}
		serverPublicKey = info.PublicKey
	}
	configPath := filepath.Join(dir, "host.ini")
	policyPath := filepath.Join(dir, "host.policy.json")
	livePath := policyPath + ".live-connections"
	receiveDirectory := filepath.Join(dir, "receive")
	if err := os.MkdirAll(receiveDirectory, 0700); err != nil {
		t.Fatalf("create isolated host receive directory: %v", err)
	}
	inputTracePath := filepath.Join(dir, "input-events.jsonl")
	viewerStatusDirectory := ""
	if multiWindowInputUI {
		viewerStatusDirectory = filepath.Join(dir, "viewer-status")
		if err := os.MkdirAll(viewerStatusDirectory, 0700); err != nil {
			t.Fatalf("create isolated viewer status directory: %v", err)
		}
	}
	config := fmt.Sprintf("bind=127.0.0.1\nport=%d\ncarrier=false\nlan_discovery=false\nmax_viewers=2\nkeep_awake=false\nlock_after_session_end=false\nauto_disconnect_minutes=0\ninput=false\nclipboard=false\naudio=false\nfile_transfer=true\nterminal=false\nallow_remote_config=false\nrestart=false\nblock_input=false\nprivacy_mode=false\nmicrophone=false\ncamera=false\nrecord_incoming=false\nautostart=false\nauth_mode=password\ntoken_auth=true\ntoken_mode=reusable\ntoken=%s\ntunnel=true\nserver_url=%s\nhost_key=%s\ntunnel_id=%s\ntunnel_key=%s\ntunnel_listen=%s\naccess_issuer=%s\naccess_key=%s:%s\nfile_receive_dir=%s\n",
		port, token, front.URL, testutil.ServerKey, peer, deviceKey, tunnelListen, issuer.Name(), issuer.KeyID(), issuer.PublicKeyText(), receiveDirectory)
	if inputInjectionUI || multiWindowInputUI || keyboardShortcutsUI {
		config = strings.Replace(config, "input=false", "input=true", 1)
	}
	if err := os.WriteFile(configPath, []byte(config), 0600); err != nil {
		t.Fatal(err)
	}
	startHost := func(name string) *nativeProcess {
		if inputInjectionUI || multiWindowInputUI || keyboardShortcutsUI {
			env := append(os.Environ(),
				"CHAST_NATIVE_TESTING=1",
				"CHAST_NATIVE_INPUT_DRY_RUN=1",
				"CHAST_NATIVE_INPUT_TRACE="+inputTracePath)
			return startNativeProcessWithEnv(t, dir, name, filepath.Join(bin, "chast-host.exe"), "", env, false,
				"--portable", "--tray", "--config", configPath)
		}
		return startNativeProcess(t, dir, name, filepath.Join(bin, "chast-host.exe"), "", "--portable", "--tray", "--config", configPath)
	}
	if err := udp.Close(); err != nil {
		t.Fatal(err)
	}
	if err := tunnelUDP.Close(); err != nil {
		t.Fatal(err)
	}
	host := startHost("host-1")
	var fingerprint string
	nativeWait(t, "actual tray registers through its tunnel", 30*time.Second, func() bool {
		record, ok := s.Registry.Identity(peer)
		fingerprint = record.Fingerprint
		return ok && fingerprint != "" && nativeFile(livePath)
	})
	startViewerAttempt := func(name string, managed bool) (*nativeProcess, string) {
		out := filepath.Join(dir, name)
		if err := os.Mkdir(out, 0700); err != nil {
			t.Fatal(err)
		}
		input := token + "\n"
		if managed {
			publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
			if err != nil {
				t.Fatal(err)
			}
			status, body := request(t, http.MethodPost, s.URL+"/v1/access-tickets",
				ticketRequestBody(t, peer, publicKey, 0xffffffff), accountToken)
			if status != http.StatusCreated {
				t.Fatalf("issue managed probe ticket: %d %s", status, body)
			}
			ticket := decodeAccessTicket(t, body)
			seed := privateKey.Seed()
			input += ticket.Ticket + "\n" +
				base64.RawURLEncoding.EncodeToString(seed) + "\n"
		}
		p := startNativeProcess(t, dir, name, filepath.Join(bin, "chast-live-viewer-probe.exe"), input, strconv.Itoa(port), fingerprint, out)
		return p, out
	}
	startViewer := func(name string) (*nativeProcess, string) {
		p, out := startViewerAttempt(name, false)
		nativeWait(t, name+" completes pinned QUIC handshake", 20*time.Second, func() bool {
			select {
			case <-p.done:
				t.Fatalf("%s exited before handshake: %v (see log)", name, p.err)
			default:
			}
			return nativeFile(filepath.Join(out, "connected"))
		})
		return p, out
	}
	startManagedViewer := func(name string) (*nativeProcess, string) {
		p, out := startViewerAttempt(name, true)
		nativeWait(t, name+" completes ticket-authenticated pinned QUIC handshake", 20*time.Second, func() bool {
			select {
			case <-p.done:
				t.Fatalf("%s exited before handshake: %v (see log)", name, p.err)
			default:
			}
			return nativeFile(filepath.Join(out, "connected"))
		})
		return p, out
	}
	expectViewerRefused := func(name string, managed bool) {
		p, out := startViewerAttempt(name, managed)
		nativeWait(t, name+" is refused before completing a session", 20*time.Second, func() bool {
			if nativeFile(filepath.Join(out, "connected")) {
				t.Fatalf("%s unexpectedly completed a QUIC session", name)
			}
			select {
			case <-p.done:
				if p.err == nil {
					t.Fatalf("%s exited successfully without a connected marker", name)
				}
				return true
			default:
				return false
			}
		})
	}
	list := func() []liveConnectionReply {
		status, body := request(t, http.MethodGet, s.URL+"/v1/live-connections", "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("list: %d %s", status, body)
		}
		var reply liveConnectionListReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatal(err)
		}
		return reply.Connections
	}
	detail := func(id string) liveConnectionReply {
		status, body := request(t, http.MethodGet, s.URL+"/v1/live-connections/"+peer+"/"+id, "", testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("detail: %d %s", status, body)
		}
		return decodeLiveConnection(t, body)
	}
	disconnect := func(id, key string, reasons ...string) liveconn.Command {
		reason := "isolated runtime acceptance"
		if len(reasons) > 0 {
			reason = reasons[0]
		}
		status, body := request(t, http.MethodPost, s.URL+"/v1/live-connections/"+peer+"/"+id+"/disconnect", mustJSON(t, map[string]string{"request_key": key, "reason": reason}), testutil.ServerKey)
		if status != http.StatusAccepted {
			t.Fatalf("disconnect: %d %s", status, body)
		}
		var command liveconn.Command
		if err := json.Unmarshal(body, &command); err != nil {
			t.Fatal(err)
		}
		return command
	}
	var firstDir string
	var flutterViewer *nativeProcess
	if flutterUI {
		flutterViewer = startNativeFlutterViewer(t, dir, "flutter-target", flutterBin, front.URL, serverPublicKey,
			peer, fingerprint, "native-acceptance", accountToken, fileTransferUI, viewerStatusDirectory,
			keyboardShortcutsUI)
	} else {
		_, firstDir = startViewer("viewer-a")
	}
	var first liveConnectionReply
	firstConnectionTimeout := 15 * time.Second
	if flutterUI {
		firstConnectionTimeout = 90 * time.Second
	}
	nativeWait(t, "first real connection reaches API", firstConnectionTimeout, func() bool {
		if flutterUI {
			select {
			case <-flutterViewer.done:
				t.Fatalf("Flutter viewer exited before reporting its session: %v (see flutter-viewer.log)", flutterViewer.err)
			default:
			}
		}
		rows := list()
		if len(rows) != 1 {
			return false
		}
		first = rows[0]
		return first.HostID == peer && first.RuntimeID != "" && first.State == "active"
	})
	firstViewerStatusPath := ""
	if multiWindowInputUI {
		firstViewerStatusPath = waitNativeFlutterNonRecordingStatusPath(t, viewerStatusDirectory)
	}
	if multiWindowUI {
		if multiWindowInputUI {
			enableNativeFlutterAutoRecordForNextSession(t, dir)
		}
		runNativeFlutterHomeConnect(t, flutterViewer.cmd.Process.Pid, peer,
			"native-runtime-viewer-token", dir, 2, true)
	}
	if inputInjectionUI {
		runNativeFlutterInputUI(t, flutterViewer.cmd.Process.Pid, dir, inputTracePath, false, "")
		verifyNativeFlutterInputEvidence(t, dir, inputTracePath)
		return
	}
	if keyboardShortcutsUI {
		nativeWait(t, "loopback host grants input to the controlled Flutter session", 10*time.Second, func() bool {
			data, err := os.ReadFile(livePath)
			if err != nil {
				return false
			}
			var snapshot struct {
				Connections []struct {
					ConnectionID string `json:"connection_id"`
					Permissions  uint32 `json:"permissions"`
					Controller   bool   `json:"controller"`
				} `json:"connections"`
			}
			if json.Unmarshal(data, &snapshot) != nil {
				return false
			}
			for _, connection := range snapshot.Connections {
				if connection.ConnectionID == first.ConnectionID {
					return connection.Controller && connection.Permissions&0x0001 != 0
				}
			}
			return false
		})
		peerPreferencesKey := fmt.Sprintf("server:v1:%x/%s", sha256.Sum256([]byte(front.URL)), peer)
		preferencesPath := filepath.Join(dir, "flutter-target-profile", "chast", "ui.json")
		runNativeFlutterShortcutUI(t, flutterViewer.cmd.Process.Pid, dir, inputTracePath,
			preferencesPath, peerPreferencesKey)
		verifyNativeFlutterShortcutEvidence(t, dir, inputTracePath)
		return
	}
	if viewerStrategyUI {
		verifyStrategyDelivery := func(label string, expectedRevision int64, expected strategy.Settings) (*account.ViewerStrategyDelivery, *account.ClientStrategyReceipt) {
			t.Helper()
			var delivery *account.ViewerStrategyDelivery
			nativeWait(t, label+" lookup delivered the expected viewer strategy revision", 30*time.Second, func() bool {
				delivery = capturedViewerDelivery.Load()
				return delivery != nil && delivery.Snapshot.Revision == expectedRevision
			})
			if len(delivery.Snapshot.Settings) != len(expected) {
				t.Fatalf("%s delivery settings count = %d, want %d", label, len(delivery.Snapshot.Settings), len(expected))
			}
			for key, want := range expected {
				got, ok := delivery.Snapshot.Settings[key]
				if !ok || got.Mode != want.Mode || !bytes.Equal(bytes.TrimSpace(got.Value), bytes.TrimSpace(want.Value)) {
					t.Fatalf("%s delivery setting %s = %+v (present=%t), want %+v", label, key, got, ok, want)
				}
			}
			var receipt *account.ClientStrategyReceipt
			nativeWait(t, label+" session reported a receipt for its exact delivery", 45*time.Second, func() bool {
				var err error
				receipt, err = s.Accounts.ViewerStrategyReceipt(viewerUser.ID, peer, delivery.Context)
				if err != nil {
					t.Fatalf("read %s viewer strategy receipt: %v", label, err)
				}
				return receipt != nil
			})
			if receipt.Revision != expectedRevision || receipt.Revision != delivery.Snapshot.Revision || receipt.Digest != delivery.Snapshot.Digest {
				t.Fatalf("%s receipt identity does not match delivery: revision=%d/%d digest=%s/%s",
					label, receipt.Revision, delivery.Snapshot.Revision, receipt.Digest, delivery.Snapshot.Digest)
			}
			if receipt.Status != strategy.Applied || len(receipt.Results) != len(expected) {
				t.Fatalf("%s receipt status/results = %s/%d, want applied/%d", label, receipt.Status, len(receipt.Results), len(expected))
			}
			for key := range expected {
				if result, ok := receipt.Results[key]; !ok || result.Status != strategy.Applied {
					t.Fatalf("%s receipt result for %s = %+v (present=%t)", label, key, result, ok)
				}
			}
			t.Logf("verified: %s Flutter strategy revision=%d has %d applied settings", label, receipt.Revision, len(expected))
			return delivery, receipt
		}
		toEvidence := func(label string, receipt *account.ClientStrategyReceipt) map[string]any {
			return map[string]any{
				"label":          label,
				"revision":       receipt.Revision,
				"digest":         receipt.Digest,
				"status":         receipt.Status,
				"results":        receipt.Results,
				"reported_at_ms": receipt.ReportedAtMS,
			}
		}
		initialDelivery, initialReceipt := verifyStrategyDelivery("initial", 1, viewerStrategySettings)
		evidence := []map[string]any{toEvidence("initial", initialReceipt)}

		updatedSettings := make(strategy.Settings, len(viewerStrategySettings))
		for key, setting := range viewerStrategySettings {
			updatedSettings[key] = setting
		}
		updatedSettings["viewer.view_fit"] = strategy.Setting{Mode: strategy.Locked, Value: json.RawMessage(`0`)}
		updatedPolicy, err := s.Accounts.SetClientStrategy(viewerStrategyPolicyID, 1, updatedSettings, "native-acceptance")
		if err != nil || updatedPolicy.Revision != 2 {
			t.Fatalf("update viewer strategy for next session: revision=%d err=%v", updatedPolicy.Revision, err)
		}
		capturedViewerDelivery.Store(nil)
		updatedViewer := startNativeFlutterViewer(t, dir, "flutter-viewer-strategy-updated", flutterBin, front.URL, serverPublicKey,
			peer, fingerprint, "native-acceptance", accountToken, false, "")
		nativeWait(t, "updated Flutter session connects while the first session stays active", 90*time.Second, func() bool {
			select {
			case <-updatedViewer.done:
				t.Fatalf("updated Flutter viewer exited before connecting: %v", updatedViewer.err)
			default:
			}
			rows := list()
			if len(rows) != 2 {
				return false
			}
			for _, row := range rows {
				if row.ConnectionID == first.ConnectionID && row.State == "active" {
					for _, candidate := range rows {
						if candidate.ConnectionID != first.ConnectionID && candidate.State == "active" {
							return true
						}
					}
				}
			}
			return false
		})
		_, updatedReceipt := verifyStrategyDelivery("updated-next-session", 2, updatedSettings)
		oldReceipt, err := s.Accounts.ViewerStrategyReceipt(viewerUser.ID, peer, initialDelivery.Context)
		if err != nil || oldReceipt == nil || oldReceipt.Revision != initialReceipt.Revision || oldReceipt.Digest != initialReceipt.Digest {
			t.Fatalf("updating policy replaced the original session receipt: receipt=%+v err=%v", oldReceipt, err)
		}
		evidence = append(evidence, toEvidence("updated-next-session", updatedReceipt))

		if err := s.Accounts.DeletePolicyAssignment(viewerStrategyAssignmentID); err != nil {
			t.Fatalf("withdraw viewer strategy assignment: %v", err)
		}
		flutterViewer.stop(t)
		updatedViewer.stop(t)
		nativeWait(t, "prior Flutter sessions close before the withdrawal session", 30*time.Second, func() bool {
			for _, row := range list() {
				if row.State == "active" {
					return false
				}
			}
			return true
		})
		capturedViewerDelivery.Store(nil)
		withdrawnViewer := startNativeFlutterViewer(t, dir, "flutter-viewer-strategy-withdrawn", flutterBin, front.URL, serverPublicKey,
			peer, fingerprint, "native-acceptance", accountToken, false, "")
		nativeWait(t, "withdrawn-strategy Flutter session connects", 90*time.Second, func() bool {
			select {
			case <-withdrawnViewer.done:
				t.Fatalf("withdrawn-strategy Flutter viewer exited before connecting: %v", withdrawnViewer.err)
			default:
			}
			rows := list()
			return len(rows) == 1 && rows[0].State == "active"
		})
		_, withdrawnReceipt := verifyStrategyDelivery("withdrawn-next-session", 3, strategy.Settings{})
		evidence = append(evidence, toEvidence("withdrawn-next-session", withdrawnReceipt))
		result, err := json.MarshalIndent(map[string]any{
			"scope":    "Windows single-machine loopback, real Flutter viewer strategy update and withdrawal",
			"sessions": evidence,
		}, "", "  ")
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "flutter-viewer-strategy-results.json"), result, 0600); err != nil {
			t.Fatalf("write viewer strategy evidence: %v", err)
		}
		t.Log("PASS: real Flutter strategy delivery/update/withdrawal applied on revisions 1, 2, and 3")
		return
	}
	var secondDir string
	var secondFlutterViewer *nativeProcess
	if dualFlutter {
		secondFlutterViewer = startNativeFlutterViewer(t, dir, "flutter-nontarget", flutterBin, front.URL, serverPublicKey,
			peer, fingerprint, "native-acceptance", accountToken, false, "")
	} else if !multiWindowUI {
		_, secondDir = startViewer("viewer-b")
	}
	var second liveConnectionReply
	nativeWait(t, "two real viewers reported", 15*time.Second, func() bool {
		rows := list()
		if len(rows) != 2 {
			return false
		}
		for _, row := range rows {
			if row.ConnectionID != first.ConnectionID {
				second = row
			}
		}
		return second.State == "active"
	})
	if multiWindowUI {
		if multiWindowInputUI {
			statusPath := waitNativeFlutterRecordingStatusPath(t, viewerStatusDirectory,
				filepath.Join(dir, "flutter-target-profile", "Videos"))
			if statusPath == firstViewerStatusPath {
				t.Fatalf("new recording session reused the initial session status path: %s", statusPath)
			}
			runNativeFlutterInputUI(t, flutterViewer.cmd.Process.Pid, dir,
				inputTracePath, true, statusPath)
			verifyNativeFlutterTabInputEvidence(t, dir, inputTracePath)
			verifyNativeFlutterTabRecordingIsolation(t, dir, firstViewerStatusPath, statusPath,
				filepath.Join(dir, "flutter-target-profile", "Videos"))
			return
		}
		if os.Getenv("CHAST_NATIVE_TEST_FAIL_TAB_CLOSE_POST_ONCE") == "1" {
			t.Log("injecting one real PostMessageW failure against an invalid HWND; the live child HWND remains valid")
			runNativeFlutterCloseSelectedTab(t, flutterViewer, peer,
				"native-runtime-viewer-token", dir, 2, true)
			select {
			case <-flutterViewer.done:
				t.Fatalf("Flutter viewer exited after the injected close-message failure: %v", flutterViewer.err)
			default:
			}
		}
		closeCycles := 3
		if raw := strings.TrimSpace(os.Getenv("CHAST_NATIVE_MULTIWINDOW_CLOSE_CYCLES")); raw != "" {
			parsed, err := strconv.Atoi(raw)
			if err != nil || parsed < 1 || parsed > 20 {
				t.Fatalf("CHAST_NATIVE_MULTIWINDOW_CLOSE_CYCLES must be between 1 and 20, got %q", raw)
			}
			closeCycles = parsed
		}
		t.Logf("running %d selected-tab close/reconnect cycles", closeCycles)
		for closeCycle := 1; closeCycle <= closeCycles; closeCycle++ {
			oldFirst, oldSecond := first, second
			runNativeFlutterCloseSelectedTab(t, flutterViewer, peer,
				"native-runtime-viewer-token", dir, 2)
			var survivor liveConnectionReply
			nativeWait(t, fmt.Sprintf("close cycle %d leaves the other tab active", closeCycle), 30*time.Second, func() bool {
				rows := list()
				if len(rows) != 1 || rows[0].State != "active" {
					return false
				}
				survivor = rows[0]
				return survivor.ConnectionID == oldFirst.ConnectionID || survivor.ConnectionID == oldSecond.ConnectionID
			})
			closedID := oldFirst.ConnectionID
			if survivor.ConnectionID == oldFirst.ConnectionID {
				closedID = oldSecond.ConnectionID
			}
			nativeWait(t, "closed tab connection reaches a terminal state", 20*time.Second, func() bool {
				return detail(closedID).State == "closed"
			})
			if detail(survivor.ConnectionID).State != "active" {
				t.Fatalf("surviving connection %s stopped while its sibling tab closed", survivor.ConnectionID)
			}

			runNativeFlutterHomeConnect(t, flutterViewer.cmd.Process.Pid, peer,
				"native-runtime-viewer-token", dir, 2, true)
			var reconnected liveConnectionReply
			nativeWait(t, fmt.Sprintf("close cycle %d reconnect preserves the surviving tab", closeCycle), 45*time.Second, func() bool {
				rows := list()
				if len(rows) != 2 {
					return false
				}
				foundSurvivor := false
				foundReconnect := false
				for _, row := range rows {
					if row.ConnectionID == survivor.ConnectionID && row.State == "active" {
						foundSurvivor = true
					} else if row.ConnectionID != closedID && row.State == "active" {
						reconnected = row
						foundReconnect = true
					}
				}
				return foundSurvivor && foundReconnect
			})
			first, second = reconnected, survivor
			select {
			case <-flutterViewer.done:
				t.Fatalf("Flutter viewer exited during close/reconnect cycle %d: %v", closeCycle, flutterViewer.err)
			default:
			}
			t.Logf("verified close/reconnect cycle %d: closed=%s reconnected=%s; sibling=%s remained active",
				closeCycle, closedID, first.ConnectionID, second.ConnectionID)
		}
	}
	if fileTransferUI {
		fixtureName := "native-flutter-file-transfer.bin"
		fixturePath := filepath.Join(dir, fixtureName)
		fixture := make([]byte, 512*1024+37)
		for i := range fixture {
			fixture[i] = byte((i*131 + 17) % 251)
		}
		if err := os.WriteFile(fixturePath, fixture, 0600); err != nil {
			t.Fatalf("write isolated Flutter file-transfer fixture: %v", err)
		}
		expectedHash := sha256.Sum256(fixture)
		verifyFile := func(path string) bool {
			data, err := os.ReadFile(path)
			return err == nil && len(data) == len(fixture) && sha256.Sum256(data) == expectedHash
		}
		runNativeFlutterFileTransferUI(t, flutterViewer.cmd.Process.Pid, dir, "Upload", fixturePath)
		hostCopy := filepath.Join(receiveDirectory, fixtureName)
		nativeWait(t, "Flutter UI upload has the expected size and SHA-256 on the host", 60*time.Second, func() bool {
			return verifyFile(hostCopy)
		})
		runNativeFlutterFileTransferUI(t, flutterViewer.cmd.Process.Pid, dir, "Download", "")
		viewerCopy := filepath.Join(dir, "flutter-target-profile", "downloads", fixtureName)
		nativeWait(t, "Flutter UI download has the expected size and SHA-256 in its isolated directory", 60*time.Second, func() bool {
			return verifyFile(viewerCopy)
		})
		downloaded, err := os.ReadFile(viewerCopy)
		if err != nil {
			t.Fatal(err)
		}
		// Keep the verified bytes outside the temporary profile for independent review.
		downloadArtifact := filepath.Join(dir, "verified-flutter-download.bin")
		if err := os.WriteFile(downloadArtifact, downloaded, 0600); err != nil {
			t.Fatal(err)
		}
		result, err := json.MarshalIndent(map[string]any{
			"scope": "Windows single-machine loopback, real Flutter file browser",
			"size":  len(fixture), "sha256": fmt.Sprintf("%X", expectedHash),
			"source": fixturePath, "host_copy": hostCopy, "viewer_copy": viewerCopy,
			"retained_download": downloadArtifact, "upload_verified": true, "download_verified": true,
		}, "", "  ")
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "flutter-file-transfer-result.json"), result, 0600); err != nil {
			t.Fatal(err)
		}
		t.Logf("PASS: Flutter UI upload and download verified size=%d SHA-256=%X", len(fixture), expectedHash)
		if !browserUI {
			return
		}
	}
	var command liveconn.Command
	if browserUI {
		var frameSampler *nativeProcess
		if dualFlutter {
			frameSampler = startNativeFlutterFrameSampler(t, secondFlutterViewer.cmd.Process.Pid, dir)
		}
		runNativeBrowserDisconnect(t, front.URL+"/admin/", first.ConnectionID, "native-acceptance", accountPassword, dir)
		if dualFlutter {
			assertFlutterStatsChanged(t, finishNativeFlutterFrameSampler(t, frameSampler, dir))
		}
		requestKey, _ := browserDisconnectRequestKey.Load().(string)
		if requestKey == "" {
			t.Fatal("management UI did not submit a disconnect request key")
		}
		command = disconnect(first.ConnectionID, requestKey, "")
	} else {
		command = disconnect(first.ConnectionID, "native-exact-target")
	}
	nativeWait(t, "real target removed and signed absence receipt accepted", 20*time.Second, func() bool {
		d := detail(first.ConnectionID)
		return d.State == "closed" && d.CommandStatus == "applied" && d.ReceiptConfirmedAbsent &&
			(flutterUI || nativeFile(filepath.Join(firstDir, "disconnected")))
	})
	if flutterUI {
		select {
		case <-flutterViewer.done:
			t.Fatalf("Flutter viewer exited after the target session was disconnected: %v", flutterViewer.err)
		default:
		}
	}
	if dualFlutter {
		select {
		case <-secondFlutterViewer.done:
			t.Fatalf("non-target Flutter viewer exited during exact disconnect: %v", secondFlutterViewer.err)
		default:
		}
	}
	if (!dualFlutter && !multiWindowUI && nativeFile(filepath.Join(secondDir, "disconnected"))) || detail(second.ConnectionID).State != "active" {
		t.Fatal("non-target viewer disconnected")
	}
	if retry := disconnect(first.ConnectionID, command.RequestKey); retry.CommandID != command.CommandID {
		t.Fatal("retry created another command")
	}
	t.Logf("exact target runtime=%s connection=%s command=%s; other=%s remains active", first.RuntimeID, first.ConnectionID, command.CommandID, second.ConnectionID)
	if dualFlutter {
		t.Log("PASS: real management page disconnected one Flutter video session; the non-target Flutter viewer stayed active and its stats region continued updating during the operation")
		return
	}
	if multiWindowUI {
		t.Log("PASS: two session tabs in one real Flutter process connected to the loopback host; exact disconnect closed one connection while the other remained active")
		return
	}

	// Lose control-plane HTTP access without dropping real QUIC sessions.
	offline.Store(true)
	queued := disconnect(second.ConnectionID, "native-offline-retry")
	time.Sleep(6 * time.Second)
	if d := detail(second.ConnectionID); d.CommandStatus != "queued" || nativeFile(filepath.Join(secondDir, "disconnected")) {
		t.Fatalf("outage falsely settled connection: %+v", d)
	}
	blockReceipts.Store(true)
	offline.Store(false)
	nativeWait(t, "real disconnect completes while receipt upload is unavailable", 20*time.Second, func() bool {
		data, _ := os.ReadFile(livePath)
		var native struct {
			Receipts []liveconn.Receipt `json:"receipts"`
		}
		if json.Unmarshal(data, &native) != nil {
			return false
		}
		for _, receipt := range native.Receipts {
			if receipt.CommandID == queued.CommandID && receipt.Status == "applied" && receipt.ConfirmedAbsent {
				return nativeFile(filepath.Join(secondDir, "disconnected"))
			}
		}
		return false
	})
	if d := detail(second.ConnectionID); d.CommandStatus != "delivered" || d.ReceiptConfirmedAbsent {
		t.Fatalf("native absence alone must not claim signed command confirmation: %+v", d)
	}
	// Kill only the tray's owned tunnel, then run the same production binary
	// against the same native spool. Its durable state must survive restart.
	pid, err := nativeAgentPID(host.cmd.Process.Pid)
	if err != nil {
		t.Fatal(err)
	}
	child, err := os.FindProcess(int(pid))
	if err != nil {
		t.Fatal(err)
	}
	if err := child.Kill(); err != nil {
		t.Fatal(err)
	}
	blockReceipts.Store(false)
	restartedAgent := startNativeProcess(t, dir, "agent-restarted", filepath.Join(bin, "chast-tunnel.exe"), testutil.ServerKey+"\n"+deviceKey+"\n",
		"host", "--server", front.URL, "--host-key-stdin", "--key-stdin", "--id", peer, "--fingerprint", fingerprint,
		"--local", fmt.Sprintf("127.0.0.1:%d", port), "--listen", "127.0.0.1:0", "--lan-discovery=false",
		"--policy-state", policyPath, "--policy-ack", filepath.Join(dir, "host.policy-ack.json"), "--live-state", livePath)
	nativeWait(t, "agent restart uploads persisted native receipt without another disconnect", 25*time.Second, func() bool {
		d := detail(second.ConnectionID)
		return d.CommandID == queued.CommandID && d.State == "closed" && d.CommandStatus == "applied" && d.ReceiptConfirmedAbsent && nativeFile(filepath.Join(secondDir, "disconnected"))
	})
	restartedAgent.stop(t)
	_, crashDir := startViewer("viewer-before-crash")
	host.stop(t)
	nativeWait(t, "actual viewer observes native process crash", 25*time.Second, func() bool { return nativeFile(filepath.Join(crashDir, "disconnected")) })
	host = startHost("host-2")
	var runtime struct {
		RuntimeID string `json:"runtime_id"`
	}
	nativeWait(t, "crashed tray restart publishes a fresh runtime", 30*time.Second, func() bool {
		data, _ := os.ReadFile(livePath)
		_ = json.Unmarshal(data, &runtime)
		return runtime.RuntimeID != "" && runtime.RuntimeID != first.RuntimeID
	})
	_, thirdDir := startViewer("viewer-c")
	var third liveConnectionReply
	nativeWait(t, "new runtime connection reaches API", 20*time.Second, func() bool {
		rows := list()
		for _, row := range rows {
			if row.RuntimeID == runtime.RuntimeID && row.State == "active" {
				third = row
				return true
			}
		}
		return false
	})
	// Replay the old inbox while the agent cannot overwrite it. The actual
	// tray must ignore another runtime's commands and preserve this viewer.
	offline.Store(true)
	// Deliberately retain an old runtime but name the NEW connection with a
	// fresh deadline. An empty/pruned inbox would not exercise the guard.
	replayed := command
	replayed.ConnectionID = third.ConnectionID
	replayed.DeadlineMS = time.Now().Add(time.Minute).UnixMilli()
	replayed.Status = liveconn.CommandDelivered
	oldInbox, err := json.Marshal(map[string]any{"version": 1, "runtime_id": first.RuntimeID, "commands": []liveconn.Command{replayed}})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "replayed-old-runtime.json"), oldInbox, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(livePath+".commands", oldInbox, 0600); err != nil {
		t.Fatal(err)
	}
	time.Sleep(3 * time.Second)
	if nativeFile(filepath.Join(thirdDir, "disconnected")) {
		t.Fatal("old runtime inbox disconnected the new viewer")
	}
	var replayState struct {
		Connections []liveconn.Connection `json:"connections"`
		Receipts    []liveconn.Receipt    `json:"receipts"`
	}
	replayBytes, err := os.ReadFile(livePath)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(replayBytes, &replayState); err != nil {
		t.Fatal(err)
	}
	if len(replayState.Connections) != 1 || replayState.Connections[0].ConnectionID != third.ConnectionID || len(replayState.Receipts) != 0 {
		t.Fatalf("old runtime cache was acted upon: %+v", replayState)
	}
	offline.Store(false)
	if d := detail(third.ConnectionID); d.CommandID != "" || d.State != "active" {
		t.Fatalf("replayed command changed new runtime: %+v", d)
	}
	t.Logf("old runtime inbox ignored; new runtime=%s connection=%s", third.RuntimeID, third.ConnectionID)

	// Real strategy fetch -> native application -> listener restart -> signed
	// current-revision receipt, using a harmless connection-cap change.
	policy, err := s.Accounts.CreatePolicyObject("native-strategy", "open", 0, nil, nil, "acceptance")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = s.Accounts.SetClientStrategy(policy.ID, 0, strategy.Settings{"host.max_viewers": {Mode: "locked", Value: json.RawMessage(`3`)}}, "acceptance"); err != nil {
		t.Fatal(err)
	}
	assignment, err := s.Accounts.CreatePolicyAssignment(policy.ID, "device", peer, 10, "acceptance")
	if err != nil {
		t.Fatal(err)
	}
	waitStrategy := func(wantPolicy string) {
		nativeWait(t, "real current strategy receipt for "+wantPolicy, 45*time.Second, func() bool {
			snapshot, err := s.Accounts.ResolveClientStrategy(peer, strategy.Host)
			if err != nil || snapshot.PolicyID != wantPolicy {
				return false
			}
			record, _ := s.Registry.Identity(peer)
			receipt, err := s.Accounts.CurrentClientStrategyReceipt(snapshot, record.PublicKey)
			return err == nil && receipt != nil && receipt.Status == strategy.Applied
		})
	}
	waitStrategy(policy.ID)
	data, _ := os.ReadFile(livePath)
	_ = json.Unmarshal(data, &runtime)
	if runtime.RuntimeID == third.RuntimeID || !nativeFile(filepath.Join(thirdDir, "disconnected")) {
		t.Fatal("startup strategy did not restart the real listener")
	}
	activeConnections := func() []liveConnectionReply {
		rows := list()
		active := make([]liveConnectionReply, 0, len(rows))
		for _, row := range rows {
			if row.State == "active" {
				active = append(active, row)
			}
		}
		return active
	}
	managedViewers := make([]*nativeProcess, 0, 3)
	managedViewerDirs := make([]string, 0, 3)
	for i := 1; i <= 3; i++ {
		viewer, out := startManagedViewer(fmt.Sprintf("managed-cap-%d", i))
		managedViewers = append(managedViewers, viewer)
		managedViewerDirs = append(managedViewerDirs, out)
	}
	var managedRows []liveConnectionReply
	managedReported := false
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		managedRows = list()
		active := 0
		for _, row := range managedRows {
			if row.State == "active" {
				active++
			}
		}
		if active == 3 {
			managedReported = true
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !managedReported {
		local, _ := os.ReadFile(livePath)
		t.Fatalf("locked max_viewers=3 sessions not fully reported: API=%+v native=%s", managedRows, local)
	}
	expectViewerRefused("managed-cap-fourth", true)
	if active := activeConnections(); len(active) != 3 {
		t.Fatalf("the rejected fourth viewer changed the three active sessions: %+v", active)
	}
	t.Log("verified managed max_viewers=3: three real QUIC sessions accepted, fourth refused")
	for i, viewer := range managedViewers {
		if err := os.WriteFile(filepath.Join(managedViewerDirs[i], "stop"), nil, 0600); err != nil {
			t.Fatal(err)
		}
		select {
		case <-viewer.done:
			if viewer.err != nil {
				t.Fatalf("managed viewer %d did not close cleanly: %v", i+1, viewer.err)
			}
		case <-time.After(10 * time.Second):
			t.Fatalf("managed viewer %d did not exit after graceful stop", i+1)
		}
	}
	nativeWait(t, "managed viewer sessions drain before policy withdrawal", 20*time.Second, func() bool {
		return len(activeConnections()) == 0
	})

	if err := s.Accounts.DeletePolicyAssignment(assignment.ID); err != nil {
		t.Fatal(err)
	}
	waitStrategy("")
	nativeWait(t, "strategy withdrawal restarts the host and closes managed sessions", 20*time.Second, func() bool {
		return len(activeConnections()) == 0
	})
	localConfig, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(localConfig), "max_viewers=2\n") {
		t.Fatal("managed overlay changed the persisted local preference")
	}
	for i := 1; i <= 2; i++ {
		startViewer(fmt.Sprintf("local-cap-%d", i))
	}
	nativeWait(t, "revoked strategy restores the local max_viewers=2 cap", 20*time.Second, func() bool {
		return len(activeConnections()) == 2
	})
	expectViewerRefused("local-cap-third", false)
	if active := activeConnections(); len(active) != 2 {
		t.Fatalf("the rejected third viewer changed the two local sessions: %+v", active)
	}
	t.Log("PASS: exact QUIC target, outage/recovery, runtime isolation, managed 3-viewer cap and local 2-viewer cap after withdrawal")
}

func TestWindowsNativeFlutterTabAudioAcceptance(t *testing.T) {
	if os.Getenv("CHAST_NATIVE_MULTIWINDOW_AUDIO") != "1" {
		t.Skip("set CHAST_NATIVE_MULTIWINDOW_AUDIO=1 for isolated real Flutter UI acceptance")
	}
	bin := os.Getenv("CHAST_NATIVE_BIN_DIR")
	if bin == "" {
		t.Fatal("CHAST_NATIVE_BIN_DIR is required")
	}
	bin, err := filepath.Abs(bin)
	if err != nil {
		t.Fatal(err)
	}
	flutterBin := os.Getenv("CHAST_NATIVE_FLUTTER_BIN")
	for _, path := range []string{filepath.Join(bin, "chast-viewer-probe.exe"), flutterBin} {
		if !nativeFile(path) {
			t.Fatalf("missing required test executable: %s", path)
		}
	}
	root := os.Getenv("CHAST_NATIVE_ARTIFACT_DIR")
	dir := t.TempDir()
	if root != "" {
		dir, err = os.MkdirTemp(root, "flutter-tab-audio-")
		if err != nil {
			t.Fatal(err)
		}
	}
	t.Logf("artifacts: %s", dir)

	fixtureDir := filepath.Join(dir, "synthetic-audio-host")
	host := startNativeProcess(t, dir, "synthetic-audio-host",
		filepath.Join(bin, "chast-viewer-probe.exe"), "", "--media-window-host", fixtureDir)
	t.Cleanup(func() {
		_ = os.WriteFile(filepath.Join(fixtureDir, "stop"), nil, 0600)
	})
	var endpoint struct {
		Port        uint16
		Fingerprint string
	}
	nativeWait(t, "loopback fixture publishes its synthetic-audio endpoint", 20*time.Second, func() bool {
		select {
		case <-host.done:
			t.Fatalf("synthetic media fixture exited: %v (see synthetic-audio-host.log)", host.err)
		default:
		}
		data, err := os.ReadFile(filepath.Join(fixtureDir, "endpoint.json"))
		if err != nil {
			return false
		}
		return json.Unmarshal(data, &endpoint) == nil &&
			endpoint.Port != 0 && endpoint.Fingerprint != ""
	})
	peer := net.JoinHostPort("127.0.0.1", strconv.Itoa(int(endpoint.Port)))
	statusDir := filepath.Join(dir, "viewer-status")
	if err := os.Mkdir(statusDir, 0700); err != nil {
		t.Fatal(err)
	}
	profile := filepath.Join(dir, "flutter-audio-profile")
	chastDir := filepath.Join(profile, "chast")
	if err := os.MkdirAll(chastDir, 0700); err != nil {
		t.Fatal(err)
	}
	trust, err := json.Marshal(map[string]string{peer: endpoint.Fingerprint})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(chastDir, "trust.json"), trust, 0600); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(profile) })

	awakeTracePath := filepath.Join(dir, "awake-leases.tsv")
	env := make([]string, 0, len(os.Environ())+7)
	for _, entry := range os.Environ() {
		key := strings.SplitN(entry, "=", 2)[0]
		switch strings.ToUpper(key) {
		case "APPDATA", "USERPROFILE", "CHAST_NATIVE_TESTING",
			"CHAST_NATIVE_TEST_STATUS_DIR", "CHAST_NATIVE_TEST_AUDIO_SINK",
			"CHAST_NATIVE_TEST_TABS_TRACE", "CHAST_NATIVE_TEST_AWAKE_TRACE":
			continue
		}
		env = append(env, entry)
	}
	env = append(env,
		"APPDATA="+profile,
		"USERPROFILE="+profile,
		"CHAST_NATIVE_TESTING=1",
		"CHAST_NATIVE_TEST_AUDIO_SINK=1",
		"CHAST_NATIVE_TEST_STATUS_DIR="+statusDir,
		"CHAST_NATIVE_TEST_TABS_TRACE="+filepath.Join(dir, "session-tabs.tsv"),
		"CHAST_NATIVE_TEST_AWAKE_TRACE="+awakeTracePath)
	viewer := startNativeProcessWithEnv(t, dir, "flutter-audio-viewer",
		flutterBin, "", env, true)
	runNativeFlutterHomeConnect(t, viewer.cmd.Process.Pid, peer, "", dir, 1, false)
	paths := waitNativeFlutterStatusPaths(t, statusDir, 1)
	statusAPath := paths[0]
	waitAudioReady := func(path, label string) nativeViewerStatusSnapshot {
		t.Helper()
		var snapshot nativeViewerStatusSnapshot
		nativeWait(t, label+" streams and decodes synthetic audio", 30*time.Second, func() bool {
			var ok bool
			snapshot, ok = readNativeViewerStatusSnapshot(path)
			return ok && snapshot.state == 3 && snapshot.audioEnabled &&
				!snapshot.audioMuted && snapshot.audioReceived >= 30 &&
				snapshot.audioDecoded >= 30
		})
		return snapshot
	}
	beforeA := waitAudioReady(statusAPath, "session A")

	runNativeFlutterHomeConnect(t, viewer.cmd.Process.Pid, peer, "", dir, 2, true)
	paths = waitNativeFlutterStatusPaths(t, statusDir, 2)
	var statusBPath string
	for _, path := range paths {
		if path != statusAPath {
			statusBPath = path
			break
		}
	}
	if statusBPath == "" {
		t.Fatal("second Flutter tab did not create an independent native status file")
	}
	beforeB := waitAudioReady(statusBPath, "session B")
	runNativeFlutterAudioIsolationUI(t, viewer.cmd.Process.Pid, dir, statusAPath, statusBPath)

	resultData, err := os.ReadFile(filepath.Join(dir, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter audio UI evidence: %v", err)
	}
	var uiResult map[string]json.RawMessage
	if err := json.Unmarshal(bytes.TrimPrefix(resultData, []byte{0xef, 0xbb, 0xbf}), &uiResult); err != nil {
		t.Fatalf("parse Flutter audio UI evidence: %v", err)
	}
	result := make(map[string]bool, 7)
	for _, key := range []string{
		"session_a_audio_muted_after_toggle",
		"session_b_audio_unchanged_after_a_toggle",
		"session_b_audio_muted_after_toggle",
		"session_a_audio_unchanged_after_b_toggle",
		"session_a_audio_unmuted_after_toggle",
		"session_b_audio_state_preserved_after_switch",
		"audio_isolation_verified",
	} {
		var passed bool
		if err := json.Unmarshal(uiResult[key], &passed); err != nil || !passed {
			t.Fatalf("Flutter toolbar audio routing assertion %s failed: %v", key, result)
		}
		result[key] = passed
	}
	afterA, okA := readNativeViewerStatusSnapshot(statusAPath)
	afterB, okB := readNativeViewerStatusSnapshot(statusBPath)
	if !okA || !okB || afterA.state != 3 || afterB.state != 3 ||
		!afterA.audioEnabled || !afterB.audioEnabled || afterA.audioMuted ||
		!afterB.audioMuted || afterA.audioDecoded <= beforeA.audioDecoded ||
		afterB.audioDecoded <= beforeB.audioDecoded {
		t.Fatalf("final per-tab audio state/counters are invalid: A=%+v B=%+v", afterA, afterB)
	}
	evidence, err := json.MarshalIndent(map[string]any{
		"scope": "same-process Windows Release Flutter tabs; loopback-only synthetic silence; test player discards output",
		"session_a": map[string]any{
			"audit_ref": afterA.auditRef, "muted_final": afterA.audioMuted,
			"decoded_before": beforeA.audioDecoded, "decoded_after": afterA.audioDecoded,
		},
		"session_b": map[string]any{
			"audit_ref": afterB.auditRef, "muted_final": afterB.audioMuted,
			"decoded_before": beforeB.audioDecoded, "decoded_after": afterB.audioDecoded,
		},
		"transitions": result,
	}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "flutter-tab-audio-isolation.json"), evidence, 0600); err != nil {
		t.Fatalf("write Flutter audio isolation evidence: %v", err)
	}

	readAwakeStates := func() (map[string]nativeAwakeLeaseSnapshot, bool) {
		return readNativeAwakeLeaseSnapshots(awakeTracePath)
	}
	var awakeInitial map[string]nativeAwakeLeaseSnapshot
	nativeWait(t, "both Flutter sessions hold an awake lease", 10*time.Second, func() bool {
		states, ok := readAwakeStates()
		if !ok || len(states) != 2 {
			return false
		}
		for _, status := range states {
			if !status.Held {
				return false
			}
		}
		awakeInitial = states
		return true
	})
	if awakeInitial[afterA.auditRef].AuditRef == "" || awakeInitial[afterB.auditRef].AuditRef == "" {
		t.Fatalf("awake leases do not match the two Flutter session identities: %+v", awakeInitial)
	}
	disconnectOne := func() {
		t.Helper()
		if err := os.WriteFile(filepath.Join(fixtureDir, "disconnect-one"), nil, 0600); err != nil {
			t.Fatalf("request one isolated viewer disconnect: %v", err)
		}
	}
	disconnectOne()
	var awakeAfterOne map[string]nativeAwakeLeaseSnapshot
	nativeWait(t, "one ended Flutter session releases while its sibling remains awake", 20*time.Second, func() bool {
		select {
		case <-viewer.done:
			t.Fatal("Flutter viewer exited after disconnecting only one session")
		default:
		}
		states, ok := readAwakeStates()
		if !ok || len(states) != 2 {
			return false
		}
		heldCount, endedCount := 0, 0
		for _, status := range states {
			if status.Held {
				heldCount++
			} else if status.State == 4 || status.State == 5 {
				endedCount++
			}
		}
		if heldCount != 1 || endedCount != 1 {
			return false
		}
		awakeAfterOne = states
		return true
	})
	disconnectOne()
	var awakeAfterAll map[string]nativeAwakeLeaseSnapshot
	nativeWait(t, "the last ended Flutter session releases the final awake lease", 20*time.Second, func() bool {
		states, ok := readAwakeStates()
		if !ok || len(states) != 2 {
			return false
		}
		for _, status := range states {
			if status.Held || (status.State != 4 && status.State != 5) {
				return false
			}
		}
		awakeAfterAll = states
		return true
	})
	awakeEvidence, err := json.MarshalIndent(map[string]any{
		"scope":   "two real Flutter Release session tabs; loopback synthetic host; transient process-scoped Windows power request only",
		"initial": awakeInitial, "after_first_disconnect": awakeAfterOne,
		"after_last_disconnect": awakeAfterAll,
	}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "flutter-tab-awake-lease-isolation.json"), awakeEvidence, 0600); err != nil {
		t.Fatalf("write Flutter awake lease evidence: %v", err)
	}
	t.Logf("PASS: Flutter tab mute actions remained session-scoped; evidence: %s",
		filepath.Join(dir, "flutter-tab-audio-isolation.json"))
	t.Logf("PASS: first viewer disconnect retained its sibling awake lease and the final disconnect released both; evidence: %s",
		filepath.Join(dir, "flutter-tab-awake-lease-isolation.json"))
}

func TestWindowsNativeFlutterDisplayWindowLifecycleAcceptance(t *testing.T) {
	if os.Getenv("CHAST_NATIVE_MULTIWINDOW_DISPLAY") != "1" {
		t.Skip("set CHAST_NATIVE_MULTIWINDOW_DISPLAY=1 for isolated display-window lifecycle acceptance")
	}
	bin := os.Getenv("CHAST_NATIVE_BIN_DIR")
	if bin == "" {
		t.Fatal("CHAST_NATIVE_BIN_DIR is required")
	}
	bin, err := filepath.Abs(bin)
	if err != nil {
		t.Fatal(err)
	}
	flutterBin := os.Getenv("CHAST_NATIVE_FLUTTER_BIN")
	for _, path := range []string{filepath.Join(bin, "chast-viewer-probe.exe"), flutterBin} {
		if !nativeFile(path) {
			t.Fatalf("missing required test executable: %s", path)
		}
	}
	root := os.Getenv("CHAST_NATIVE_ARTIFACT_DIR")
	dir := t.TempDir()
	if root != "" {
		dir, err = os.MkdirTemp(root, "flutter-display-window-")
		if err != nil {
			t.Fatal(err)
		}
	}
	t.Logf("artifacts: %s", dir)

	fixtureDir := filepath.Join(dir, "synthetic-display-host")
	host := startNativeProcess(t, dir, "synthetic-display-host",
		filepath.Join(bin, "chast-viewer-probe.exe"), "",
		"--multi-display-window-host", fixtureDir)
	t.Cleanup(func() { _ = os.WriteFile(filepath.Join(fixtureDir, "stop"), nil, 0600) })
	var endpoint struct {
		Port        uint16 `json:"port"`
		Fingerprint string `json:"fingerprint"`
	}
	nativeWait(t, "loopback fixture publishes its two-display endpoint", 20*time.Second, func() bool {
		select {
		case <-host.done:
			log, _ := os.ReadFile(filepath.Join(dir, "synthetic-display-host.log"))
			t.Fatalf("synthetic display fixture exited: %v\n%s", host.err, log)
		default:
		}
		data, err := os.ReadFile(filepath.Join(fixtureDir, "endpoint.json"))
		return err == nil && json.Unmarshal(data, &endpoint) == nil &&
			endpoint.Port != 0 && endpoint.Fingerprint != ""
	})
	peer := net.JoinHostPort("127.0.0.1", strconv.Itoa(int(endpoint.Port)))
	statusDir := filepath.Join(dir, "viewer-status")
	if err := os.Mkdir(statusDir, 0700); err != nil {
		t.Fatal(err)
	}
	profile := filepath.Join(dir, "flutter-display-profile")
	chastDir := filepath.Join(profile, "chast")
	if err := os.MkdirAll(chastDir, 0700); err != nil {
		t.Fatal(err)
	}
	trust, err := json.Marshal(map[string]string{peer: endpoint.Fingerprint})
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(chastDir, "trust.json"), trust, 0600); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(profile) })

	env := make([]string, 0, len(os.Environ())+5)
	for _, entry := range os.Environ() {
		key := strings.SplitN(entry, "=", 2)[0]
		switch strings.ToUpper(key) {
		case "APPDATA", "USERPROFILE", "CHAST_NATIVE_TESTING",
			"CHAST_NATIVE_TEST_STATUS_DIR", "CHAST_NATIVE_TEST_TABS_TRACE":
			continue
		}
		env = append(env, entry)
	}
	env = append(env,
		"APPDATA="+profile,
		"USERPROFILE="+profile,
		"CHAST_NATIVE_TESTING=1",
		"CHAST_NATIVE_TEST_STATUS_DIR="+statusDir,
		"CHAST_NATIVE_TEST_TABS_TRACE="+filepath.Join(dir, "session-tabs.tsv"))
	viewer := startNativeProcessWithEnv(t, dir, "flutter-display-viewer",
		flutterBin, "", env, true)
	runNativeFlutterHomeConnect(t, viewer.cmd.Process.Pid, peer, "", dir, 1, false)
	paths := waitNativeFlutterStatusPaths(t, statusDir, 1)
	statusAPath := paths[0]
	var beforeA nativeViewerStatusSnapshot
	nativeWait(t, "display session A streams and decodes video", 40*time.Second, func() bool {
		var ok bool
		beforeA, ok = readNativeViewerStatusSnapshot(statusAPath)
		return ok && beforeA.state == 3 && beforeA.received > 0 && beforeA.decoded > 0
	})

	runNativeFlutterHomeConnect(t, viewer.cmd.Process.Pid, peer, "", dir, 2, true)
	paths = waitNativeFlutterStatusPaths(t, statusDir, 2)
	var statusBPath string
	for _, path := range paths {
		if path != statusAPath {
			statusBPath = path
			break
		}
	}
	if statusBPath == "" {
		t.Fatal("second Flutter tab did not create an independent native status file")
	}
	var beforeB nativeViewerStatusSnapshot
	nativeWait(t, "display session B streams and decodes video", 40*time.Second, func() bool {
		var ok bool
		beforeB, ok = readNativeViewerStatusSnapshot(statusBPath)
		return ok && beforeB.state == 3 && beforeB.received > 0 && beforeB.decoded > 0
	})

	disconnectMarker := filepath.Join(fixtureDir, "disconnect-first")
	runNativeFlutterDisplayWindowLifecycleUI(t, viewer.cmd.Process.Pid, dir, disconnectMarker)
	resultData, err := os.ReadFile(filepath.Join(dir, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter display-window evidence: %v", err)
	}
	var uiResult struct {
		DisplayAVisibleWhenASelected        bool  `json:"display_a_visible_when_a_selected"`
		DisplayBVisibleWhenBSelected        bool  `json:"display_b_visible_when_b_selected"`
		DisplayWindowsFollowParentTabs      bool  `json:"display_windows_follow_parent_tabs"`
		DisplayAClosedAfterParentDisconnect bool  `json:"display_a_closed_after_parent_disconnect"`
		DisplayBSurvivedParentDisconnect    bool  `json:"display_b_survived_parent_disconnect"`
		SiblingTabSurvivedParentDisconnect  bool  `json:"sibling_tab_survived_parent_disconnect"`
		LifecycleVerified                   bool  `json:"display_window_lifecycle_verified"`
		DisplayAHandle                      int64 `json:"display_a_window_handle"`
		DisplayBHandle                      int64 `json:"display_b_window_handle"`
	}
	if err := json.Unmarshal(bytes.TrimPrefix(resultData, []byte{0xef, 0xbb, 0xbf}), &uiResult); err != nil {
		t.Fatalf("parse Flutter display-window evidence: %v", err)
	}
	if !uiResult.LifecycleVerified || !uiResult.DisplayAVisibleWhenASelected ||
		!uiResult.DisplayBVisibleWhenBSelected || !uiResult.DisplayWindowsFollowParentTabs ||
		!uiResult.DisplayAClosedAfterParentDisconnect ||
		!uiResult.DisplayBSurvivedParentDisconnect ||
		!uiResult.SiblingTabSurvivedParentDisconnect ||
		uiResult.DisplayAHandle == 0 || uiResult.DisplayBHandle == 0 {
		t.Fatalf("Flutter display-window lifecycle assertions failed: %+v", uiResult)
	}

	var afterA, afterB nativeViewerStatusSnapshot
	nativeWait(t, "parent A ends while sibling B remains streaming", 15*time.Second, func() bool {
		var okA, okB bool
		afterA, okA = readNativeViewerStatusSnapshot(statusAPath)
		afterB, okB = readNativeViewerStatusSnapshot(statusBPath)
		return okA && okB && (afterA.state == 4 || afterA.state == 5) &&
			afterB.state == 3 && afterB.decoded > beforeB.decoded
	})
	evidence, err := json.MarshalIndent(map[string]any{
		"scope":                            "same-process Windows Release Flutter tabs; loopback synthetic two-display topology; both logical displays alias one read-only local capture",
		"session_a_audit_ref":              beforeA.auditRef,
		"session_b_audit_ref":              beforeB.auditRef,
		"session_a_state_after_disconnect": afterA.state,
		"session_b_state_after_disconnect": afterB.state,
		"session_b_decoded_before":         beforeB.decoded,
		"session_b_decoded_after":          afterB.decoded,
		"ui":                               uiResult,
	}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "flutter-display-window-lifecycle.json"), evidence, 0600); err != nil {
		t.Fatalf("write display-window lifecycle evidence: %v", err)
	}
	t.Logf("PASS: display windows followed their tabs; disconnecting A closed only A's window while B remained visible and streaming; evidence: %s",
		filepath.Join(dir, "flutter-display-window-lifecycle.json"))
}
