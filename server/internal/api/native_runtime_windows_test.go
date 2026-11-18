package api_test

// Opt-in acceptance against the actual Windows tray and tunnel processes.
// Default QUIC viewers use files-only mode. Opt-in Flutter modes use isolated
// profiles; the host still disables input and no service is installed.

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
	peer, fingerprint, username, accountToken string, isolatedDownloads bool) *nativeProcess {
	t.Helper()
	profile := filepath.Join(dir, name+"-profile")
	downloadDir := filepath.Join(profile, "downloads")
	chastDir := filepath.Join(profile, "chast")
	if err := os.MkdirAll(chastDir, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(downloadDir, 0700); err != nil {
		t.Fatal(err)
	}
	// Remove only this test-owned profile, after the viewer process has stopped.
	t.Cleanup(func() { _ = os.RemoveAll(profile) })
	publicKey, err := base64.RawURLEncoding.DecodeString(serverPublicKey)
	if err != nil {
		t.Fatalf("decode test server identity: %v", err)
	}
	serverFingerprint := fmt.Sprintf("%X", sha256.Sum256(publicKey))
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
				fmt.Sprintf("server:v1:%x/%s", sha256.Sum256([]byte(serverURL)), peer): map[string]any{
					"view_stats": true,
				},
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
		if strings.EqualFold(key, "APPDATA") || strings.EqualFold(key, "CHAST_NATIVE_TESTING") ||
			strings.EqualFold(key, "CHAST_NATIVE_TEST_DOWNLOAD_DIR") ||
			strings.EqualFold(key, "CHAST_NATIVE_TEST_UPLOAD_DIR") {
			continue
		}
		filtered = append(filtered, entry)
	}
	viewerEnv := append(filtered, "APPDATA="+profile)
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
	runNativeFlutterHomeConnect(t, proc.cmd.Process.Pid, peer, "native-runtime-viewer-token", dir)
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

func runNativeFlutterHomeConnect(t *testing.T, pid int, peer, token, artifacts string) {
	t.Helper()
	script := filepath.Join("..", "..", "..", "tools", "native-flutter-home-connect.ps1")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive",
		"-ExecutionPolicy", "Bypass", "-File", script, strconv.Itoa(pid), peer, artifacts)
	cmd.Stdin = strings.NewReader(token + "\n")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("could not start Flutter viewer session: %v\n%s", err, output)
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

func runNativeFlutterInputUI(t *testing.T, pid int, artifacts, tracePath string) {
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
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("Flutter keyboard/mouse input acceptance failed: %v\n%s", err, output)
	}
	t.Log(strings.TrimSpace(string(output)))
}

func verifyNativeFlutterInputEvidence(t *testing.T, artifacts, tracePath string) {
	t.Helper()
	resultData, err := os.ReadFile(filepath.Join(artifacts, "flutter-input-results.json"))
	if err != nil {
		t.Fatalf("read Flutter input result: %v", err)
	}
	var result struct {
		ControlOnClicked                 bool `json:"control_on_clicked"`
		ControlOnVerified                bool `json:"control_on_verified"`
		ControlOffClicked                bool `json:"control_off_clicked"`
		ControlOffVerified               bool `json:"control_off_verified"`
		RemoteAUpBeforePhysicalCleanup   bool `json:"remote_a_up_before_physical_cleanup"`
		PhysicalCleanupHasNoKeyAEvent    bool `json:"physical_cleanup_has_no_key_a_event"`
		TraceEventsBeforePhysicalCleanup int  `json:"trace_events_before_physical_cleanup"`
		TraceEventsAfterPhysicalCleanup  int  `json:"trace_events_after_physical_cleanup"`
	}
	if err := json.Unmarshal(resultData, &result); err != nil {
		t.Fatalf("parse Flutter input result: %v", err)
	}
	if !result.ControlOnClicked || !result.ControlOnVerified ||
		!result.ControlOffClicked || !result.ControlOffVerified ||
		!result.RemoteAUpBeforePhysicalCleanup || !result.PhysicalCleanupHasNoKeyAEvent {
		t.Fatalf("Flutter input UI did not verify the complete release sequence: %+v", result)
	}
	traceData, err := os.ReadFile(tracePath)
	if err != nil {
		t.Fatalf("read native input trace: %v", err)
	}
	var sawMouse bool
	aDownIndex, aUpIndex := -1, -1
	traceEvents := make([]struct {
		Kind     string `json:"kind"`
		Action   string `json:"action"`
		HIDUsage int    `json:"hid_usage"`
	}, 0)
	for _, line := range strings.Split(string(traceData), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		var event struct {
			Kind     string `json:"kind"`
			Action   string `json:"action"`
			HIDUsage int    `json:"hid_usage"`
		}
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
		if event.Kind == "key" && event.HIDUsage == 0x04 && event.Action == "up" && aDownIndex >= 0 && aUpIndex == -1 {
			aUpIndex = index
		}
	}
	if !sawMouse || aDownIndex < 0 || aUpIndex < 0 {
		t.Fatalf("native input trace missing mouse/A down/A up evidence: mouse=%t down=%d up=%d (%s)", sawMouse, aDownIndex, aUpIndex, tracePath)
	}
	if aDownIndex >= aUpIndex {
		t.Fatalf("native input trace has A up before A down: down=%d up=%d (%s)", aDownIndex, aUpIndex, tracePath)
	}
	if result.TraceEventsBeforePhysicalCleanup <= 0 || result.TraceEventsBeforePhysicalCleanup > len(traceEvents) {
		t.Fatalf("invalid trace event count before physical cleanup: before=%d total=%d", result.TraceEventsBeforePhysicalCleanup, len(traceEvents))
	}
	if result.TraceEventsAfterPhysicalCleanup < result.TraceEventsBeforePhysicalCleanup || result.TraceEventsAfterPhysicalCleanup > len(traceEvents) {
		t.Fatalf("invalid trace event count after physical cleanup: before=%d after=%d total=%d", result.TraceEventsBeforePhysicalCleanup, result.TraceEventsAfterPhysicalCleanup, len(traceEvents))
	}
	if aUpIndex >= result.TraceEventsBeforePhysicalCleanup {
		t.Fatalf("A up was not emitted before the physical cleanup boundary: up=%d boundary=%d", aUpIndex, result.TraceEventsBeforePhysicalCleanup)
	}
	for _, event := range traceEvents[result.TraceEventsBeforePhysicalCleanup:] {
		if event.Kind == "key" && event.HIDUsage == 0x04 {
			t.Fatalf("physical A cleanup emitted an unexpected trace event after the release boundary: %+v", event)
		}
	}
	t.Logf("PASS: real Flutter control toggle forwarded mouse and A down/up; A up index=%d preceded physical cleanup boundary=%d and cleanup added no A event (%s)", aUpIndex, result.TraceEventsBeforePhysicalCleanup, tracePath)
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
	fileTransferUI := os.Getenv("CHAST_NATIVE_FILE_TRANSFER") == "1"
	viewerStrategyUI := os.Getenv("CHAST_NATIVE_VIEWER_STRATEGY") == "1"
	inputInjectionUI := os.Getenv("CHAST_NATIVE_INPUT_INJECTION") == "1"
	if flutterUI && !browserUI && !fileTransferUI && !viewerStrategyUI && !inputInjectionUI {
		t.Fatal("Flutter UI acceptance requires browser, file-transfer, viewer-strategy, or input-injection mode")
	}
	if dualFlutter && (!flutterUI || !browserUI) {
		t.Fatal("dual Flutter acceptance requires CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_BROWSER_UI=1")
	}
	if fileTransferUI && !flutterUI {
		t.Fatal("Flutter file transfer acceptance requires CHAST_NATIVE_FLUTTER_UI=1")
	}
	if viewerStrategyUI && (!flutterUI || browserUI || dualFlutter || fileTransferUI) {
		t.Fatal("viewer strategy acceptance requires only CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_VIEWER_STRATEGY=1")
	}
	if inputInjectionUI && (!flutterUI || browserUI || dualFlutter || fileTransferUI || viewerStrategyUI) {
		t.Fatal("input injection acceptance requires only CHAST_NATIVE_FLUTTER_UI=1 and CHAST_NATIVE_INPUT_INJECTION=1")
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
	config := fmt.Sprintf("bind=127.0.0.1\nport=%d\ncarrier=false\nlan_discovery=false\nmax_viewers=2\nkeep_awake=false\nlock_after_session_end=false\nauto_disconnect_minutes=0\ninput=false\nclipboard=false\naudio=false\nfile_transfer=true\nterminal=false\nallow_remote_config=false\nrestart=false\nblock_input=false\nprivacy_mode=false\nmicrophone=false\ncamera=false\nrecord_incoming=false\nautostart=false\nauth_mode=password\ntoken_auth=true\ntoken_mode=reusable\ntoken=%s\ntunnel=true\nserver_url=%s\nhost_key=%s\ntunnel_id=%s\ntunnel_key=%s\ntunnel_listen=%s\naccess_issuer=%s\naccess_key=%s:%s\nfile_receive_dir=%s\n",
		port, token, front.URL, testutil.ServerKey, peer, deviceKey, tunnelListen, issuer.Name(), issuer.KeyID(), issuer.PublicKeyText(), receiveDirectory)
	if inputInjectionUI {
		config = strings.Replace(config, "input=false", "input=true", 1)
	}
	if err := os.WriteFile(configPath, []byte(config), 0600); err != nil {
		t.Fatal(err)
	}
	startHost := func(name string) *nativeProcess {
		if inputInjectionUI {
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
			peer, fingerprint, "native-acceptance", accountToken, fileTransferUI)
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
	if inputInjectionUI {
		runNativeFlutterInputUI(t, flutterViewer.cmd.Process.Pid, dir, inputTracePath)
		verifyNativeFlutterInputEvidence(t, dir, inputTracePath)
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
			peer, fingerprint, "native-acceptance", accountToken, false)
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
			peer, fingerprint, "native-acceptance", accountToken, false)
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
			peer, fingerprint, "native-acceptance", accountToken, false)
	} else {
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
	if (!dualFlutter && nativeFile(filepath.Join(secondDir, "disconnected"))) || detail(second.ConnectionID).State != "active" {
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
