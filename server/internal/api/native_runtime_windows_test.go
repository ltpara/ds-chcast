package api_test

// Opt-in acceptance against the actual Windows tray and tunnel processes.
// Separate QUIC viewers use files-only mode: no screen capture, clipboard,
// input injection, installed service or system configuration is needed.

import (
	"encoding/json"
	"fmt"
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
	t.Helper()
	log, err := os.Create(filepath.Join(dir, name+".log"))
	if err != nil {
		t.Fatal(err)
	}
	p := &nativeProcess{cmd: exec.Command(binary, args...), done: make(chan struct{})}
	p.cmd.Dir = dir
	p.cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
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
	for _, name := range []string{"chast-host.exe", "chast-tunnel.exe", "chast-live-viewer-probe.exe"} {
		if !nativeFile(filepath.Join(bin, name)) {
			t.Fatalf("missing native binary: %s", name)
		}
	}
	dir := t.TempDir()
	if root := os.Getenv("CHAST_NATIVE_ARTIFACT_DIR"); root != "" {
		dir, err = os.MkdirTemp(root, "run-")
		if err != nil {
			t.Fatal(err)
		}
	}
	t.Logf("artifacts: %s", dir)
	s := testutil.Start(t)
	target, _ := url.Parse(s.URL)
	proxy := httputil.NewSingleHostReverseProxy(target)
	var offline atomic.Bool
	var blockReceipts atomic.Bool
	front := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
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
	configPath := filepath.Join(dir, "host.ini")
	policyPath := filepath.Join(dir, "host.policy.json")
	livePath := policyPath + ".live-connections"
	config := fmt.Sprintf("bind=127.0.0.1\nport=%d\ncarrier=false\nlan_discovery=false\nmax_viewers=2\nkeep_awake=false\nlock_after_session_end=false\nauto_disconnect_minutes=0\nallow_input=false\nallow_clipboard=false\nallow_audio=false\nallow_file_transfer=true\nallow_terminal=false\nallow_remote_config=false\nallow_restart=false\nallow_block_input=false\nallow_privacy_mode=false\nallow_microphone=false\nallow_camera=false\nrecord_incoming=false\nautostart=false\nauth_mode=password\ntoken_auth=true\ntoken_mode=reusable\ntoken=%s\ntunnel=true\nserver_url=%s\nhost_key=%s\ntunnel_id=%s\ntunnel_key=%s\ntunnel_listen=127.0.0.1:0\nfile_receive_directory=%s\n",
		port, token, front.URL, testutil.ServerKey, peer, deviceKey, filepath.Join(dir, "receive"))
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
	startViewer := func(name string) (*nativeProcess, string) {
		out := filepath.Join(dir, name)
		if err := os.Mkdir(out, 0700); err != nil {
			t.Fatal(err)
		}
		p := startNativeProcess(t, dir, name, filepath.Join(bin, "chast-live-viewer-probe.exe"), token+"\n", strconv.Itoa(port), fingerprint, out)
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
	disconnect := func(id, key string) liveconn.Command {
		status, body := request(t, http.MethodPost, s.URL+"/v1/live-connections/"+peer+"/"+id+"/disconnect", mustJSON(t, map[string]string{"request_key": key, "reason": "isolated runtime acceptance"}), testutil.ServerKey)
		if status != http.StatusAccepted {
			t.Fatalf("disconnect: %d %s", status, body)
		}
		var command liveconn.Command
		if err := json.Unmarshal(body, &command); err != nil {
			t.Fatal(err)
		}
		return command
	}
	_, firstDir := startViewer("viewer-a")
	var first liveConnectionReply
	nativeWait(t, "first real connection reaches API", 15*time.Second, func() bool {
		rows := list()
		if len(rows) != 1 {
			return false
		}
		first = rows[0]
		return first.State == "active"
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
	command := disconnect(first.ConnectionID, "native-exact-target")
	nativeWait(t, "real target removed and signed absence receipt accepted", 20*time.Second, func() bool {
		d := detail(first.ConnectionID)
		return d.State == "closed" && d.CommandStatus == "applied" && d.ReceiptConfirmedAbsent && nativeFile(filepath.Join(firstDir, "disconnected"))
	})
	if nativeFile(filepath.Join(secondDir, "disconnected")) || detail(second.ConnectionID).State != "active" {
		t.Fatal("non-target viewer disconnected")
	}
	if retry := disconnect(first.ConnectionID, "native-exact-target"); retry.CommandID != command.CommandID {
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
	if err := s.Accounts.DeletePolicyAssignment(assignment.ID); err != nil {
		t.Fatal(err)
	}
	waitStrategy("")
	localConfig, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(localConfig), "max_viewers=2\n") {
		t.Fatal("managed overlay changed the persisted local preference")
	}
	t.Log("PASS: exact QUIC target, other viewer preserved, control-plane outage, agent restart, native process restart, old runtime isolation, strategy apply/revoke")
}
