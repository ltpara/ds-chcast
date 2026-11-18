package api_test

// Opt-in acceptance against the actual Windows tray and tunnel processes.
// Separate QUIC viewers use files-only mode: no screen capture, clipboard,
// input injection, installed service or system configuration is needed.

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
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

func startNativeFlutterViewer(t *testing.T, dir, binary, serverURL, serverPublicKey,
	peer, fingerprint, username, accountToken string) *nativeProcess {
	t.Helper()
	profile := filepath.Join(dir, "flutter-profile")
	chastDir := filepath.Join(profile, "chast")
	if err := os.MkdirAll(chastDir, 0700); err != nil {
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
		if !strings.EqualFold(strings.SplitN(entry, "=", 2)[0], "APPDATA") {
			filtered = append(filtered, entry)
		}
	}
	proc := startNativeProcessWithEnv(t, dir, "flutter-viewer", binary, "",
		append(filtered, "APPDATA="+profile), true)
	nativeWait(t, "Flutter account and server credentials are DPAPI-protected", 20*time.Second, func() bool {
		select {
		case <-proc.done:
			t.Fatalf("Flutter viewer exited during startup: %v (see flutter-viewer.log)", proc.err)
		default:
		}
		return protectedNativeProfileFile(filepath.Join(chastDir, "server.json")) &&
			protectedNativeProfileFile(filepath.Join(chastDir, "account.json"))
	})
	runNativeFlutterHomeConnect(t, proc.cmd.Process.Pid, peer, "native-runtime-viewer-token", dir)
	return proc
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
	if flutterUI && !browserUI {
		t.Fatal("Flutter UI acceptance requires CHAST_NATIVE_BROWSER_UI=1")
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
	udp.Close()
	const peer = "nativebox"
	const deviceKey = "native-runtime-device-key"
	const token = "native-runtime-viewer-token"
	const accountPassword = "native acceptance password"
	if _, err := s.Accounts.CreateUser("native-acceptance", accountPassword); err != nil {
		t.Fatal(err)
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
	config := fmt.Sprintf("bind=127.0.0.1\nport=%d\ncarrier=false\nlan_discovery=false\nmax_viewers=2\nkeep_awake=false\nlock_after_session_end=false\nauto_disconnect_minutes=0\nallow_input=false\nallow_clipboard=false\nallow_audio=false\nallow_file_transfer=true\nallow_terminal=false\nallow_remote_config=false\nallow_restart=false\nallow_block_input=false\nallow_privacy_mode=false\nallow_microphone=false\nallow_camera=false\nrecord_incoming=false\nautostart=false\nauth_mode=password\ntoken_auth=true\ntoken_mode=reusable\ntoken=%s\ntunnel=true\nserver_url=%s\nhost_key=%s\ntunnel_id=%s\ntunnel_key=%s\ntunnel_listen=127.0.0.1:0\naccess_issuer=%s\naccess_key=%s:%s\nfile_receive_directory=%s\n",
		port, token, front.URL, testutil.ServerKey, peer, deviceKey, issuer.Name(), issuer.KeyID(), issuer.PublicKeyText(), filepath.Join(dir, "receive"))
	if err := os.WriteFile(configPath, []byte(config), 0600); err != nil {
		t.Fatal(err)
	}
	startHost := func(name string) *nativeProcess {
		return startNativeProcess(t, dir, name, filepath.Join(bin, "chast-host.exe"), "", "--portable", "--tray", "--config", configPath)
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
		flutterViewer = startNativeFlutterViewer(t, dir, flutterBin, front.URL, serverPublicKey,
			peer, fingerprint, "native-acceptance", accountToken)
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
	_, secondDir := startViewer("viewer-b")
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
	var command liveconn.Command
	if browserUI {
		runNativeBrowserDisconnect(t, front.URL+"/admin/", first.ConnectionID, "native-acceptance", accountPassword, dir)
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
	if nativeFile(filepath.Join(secondDir, "disconnected")) || detail(second.ConnectionID).State != "active" {
		t.Fatal("non-target viewer disconnected")
	}
	if retry := disconnect(first.ConnectionID, command.RequestKey); retry.CommandID != command.CommandID {
		t.Fatal("retry created another command")
	}
	t.Logf("exact target runtime=%s connection=%s command=%s; other=%s remains active", first.RuntimeID, first.ConnectionID, command.CommandID, second.ConnectionID)

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
