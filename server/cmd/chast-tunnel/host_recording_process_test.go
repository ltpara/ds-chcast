package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"io"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"net/http/httputil"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

const hostRecordingProcessTaskID = "f0123456789abcdef0123456789abcde"
const hostRecordingProcessChunkSize = 4 << 20

type processRecordingTask struct {
	ID          string `json:"id"`
	Path        string `json:"path"`
	Name        string `json:"name"`
	Size        int64  `json:"size"`
	StartedAtMS int64  `json:"started_at_ms"`
	DurationMS  int64  `json:"duration_ms"`
}

func TestHostRecordingUploadProcessResumesAfterKill(t *testing.T) {
	runHostRecordingUploadProcessResumesAfterKill(t, false, "")
}

func TestHostRecordingUploadThroughCppParentResumesAfterKill(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("the C++ TunnelProcess parent harness is Windows-only")
	}
	harness := os.Getenv("CHAST_CPP_TUNNEL_PROCESS_TEST")
	if harness == "" {
		t.Skip("set CHAST_CPP_TUNNEL_PROCESS_TEST to the built host tunnel test executable")
	}
	if os.Getenv("CHAST_CPP_RECORDING_PROCESS_QUEUE") == "" {
		t.Skip("set CHAST_CPP_RECORDING_PROCESS_QUEUE to a C++-produced low-integrity fixture queue")
	}
	runHostRecordingUploadProcessResumesAfterKill(t, true, harness)
}

func runHostRecordingUploadProcessResumesAfterKill(t *testing.T, throughCppParent bool, harness string) {
	root := t.TempDir()
	binaryName := "chast-tunnel"
	if runtime.GOOS == "windows" {
		binaryName += ".exe"
	}
	binary := filepath.Join(root, binaryName)
	packageDir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	build := exec.Command("go", "build", "-o", binary, ".")
	build.Dir = packageDir
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build chast-tunnel process fixture: %v\n%s", err, output)
	}
	archiveDir := filepath.Join(root, "archive")
	server := testutil.StartWith(t, testutil.Options{RecordingsDir: archiveDir})
	owner, err := server.Accounts.CreateUser("host-archive-owner", "test-password-123")
	if err != nil {
		t.Fatalf("create archive owner: %v", err)
	}

	queue := filepath.Join(root, "queue")
	var task processRecordingTask
	var manifestPath string
	var recordingPath string
	var recording []byte
	var manifest []byte
	if throughCppParent {
		queue = os.Getenv("CHAST_CPP_RECORDING_PROCESS_QUEUE")
		entries, err := os.ReadDir(queue)
		if err != nil {
			t.Fatalf("read C++ fixture queue: %v", err)
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") ||
				strings.HasSuffix(entry.Name(), ".tmp.json") {
				continue
			}
			if manifestPath != "" {
				t.Fatal("expected exactly one C++ recording task")
			}
			manifestPath = filepath.Join(queue, entry.Name())
		}
		if manifestPath == "" {
			t.Fatal("C++ producer did not leave a recording task")
		}
		manifest, err = os.ReadFile(manifestPath)
		if err != nil {
			t.Fatalf("read C++ recording task: %v", err)
		}
		if err := json.Unmarshal(manifest, &task); err != nil {
			t.Fatalf("decode C++ recording task: %v", err)
		}
		recordingPath = task.Path
		recording, err = os.ReadFile(recordingPath)
		if err != nil {
			t.Fatalf("read C++ recording fixture: %v", err)
		}
		if int64(len(recording)) != task.Size || task.Size <= hostRecordingProcessChunkSize {
			t.Fatalf("C++ fixture size = %d, want > %d", task.Size, hostRecordingProcessChunkSize)
		}
	} else {
		if err := os.MkdirAll(queue, 0700); err != nil {
			t.Fatal(err)
		}
		seed := []byte("sealed-host-recording-segment-")
		recording = bytes.Repeat(seed, hostRecordingProcessChunkSize/len(seed)+100)
		recordingPath = filepath.Join(root, "captured segment.mp4")
		if err := os.WriteFile(recordingPath, recording, 0600); err != nil {
			t.Fatal(err)
		}
		task = processRecordingTask{
			ID: hostRecordingProcessTaskID, Path: recordingPath,
			Name: filepath.Base(recordingPath), Size: int64(len(recording)),
			StartedAtMS: time.Now().Add(-time.Minute).UnixMilli(), DurationMS: 60000,
		}
		manifestPath = filepath.Join(queue, task.ID+".json")
		manifest, err = json.Marshal(task)
		if err != nil {
			t.Fatal(err)
		}
	}

	var dropFirstPatch atomic.Bool
	dropFirstPatch.Store(true)
	firstPatchApplied := make(chan struct{})
	var stopped atomic.Bool
	var offsetsMu sync.Mutex
	var initialOffsets []int64
	var postRestartOffsets []int64
	hostID := "process-host"
	if throughCppParent {
		hostID = "cpp-parent-host"
	}
	uploadBase := "/v1/hosts/" + hostID + "/recordings/uploads"
	target, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	backend := httputil.NewSingleHostReverseProxy(target)
	backend.ErrorLog = log.New(io.Discard, "", 0)
	backend.Transport = hostRecordingProcessTransport{
		base: &http.Transport{},
		onResponse: func(request *http.Request, response *http.Response) error {
			if response.StatusCode >= 200 && response.StatusCode < 300 &&
				request.Method == http.MethodPost && request.URL.Path == uploadBase {
				body, err := io.ReadAll(response.Body)
				if err != nil {
					return err
				}
				_ = response.Body.Close()
				response.Body = io.NopCloser(bytes.NewReader(body))
				response.ContentLength = int64(len(body))
				response.Header.Set("Content-Length", strconv.Itoa(len(body)))
				var result struct {
					Upload *struct {
						Offset int64 `json:"offset"`
					} `json:"upload"`
				}
				if json.Unmarshal(body, &result) == nil && result.Upload != nil {
					offsetsMu.Lock()
					if stopped.Load() {
						postRestartOffsets = append(postRestartOffsets, result.Upload.Offset)
					} else {
						initialOffsets = append(initialOffsets, result.Upload.Offset)
					}
					offsetsMu.Unlock()
				}
			}
			if response.StatusCode >= 200 && response.StatusCode < 300 &&
				request.Method == http.MethodPatch &&
				strings.HasPrefix(request.URL.Path, uploadBase+"/") &&
				dropFirstPatch.CompareAndSwap(true, false) {
				_ = response.Body.Close()
				body := []byte("simulated lost response after server commit")
				response.StatusCode = http.StatusBadGateway
				response.Status = "502 Bad Gateway"
				response.Header = make(http.Header)
				response.Header.Set("Content-Type", "text/plain")
				response.Header.Set("Content-Length", strconv.Itoa(len(body)))
				response.Body = io.NopCloser(bytes.NewReader(body))
				response.ContentLength = int64(len(body))
				close(firstPatchApplied)
			}
			return nil
		},
	}
	front := httptest.NewTLSServer(backend)
	t.Cleanup(front.Close)
	caPath := filepath.Join(root, "test-server-ca.pem")
	ca := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: front.Certificate().Raw})
	if err := os.WriteFile(caPath, ca, 0600); err != nil {
		t.Fatal(err)
	}

	proxy := startHostRecordingProcessProxy(t)
	carrier := startHostRecordingProcessCarrier(t)
	args := []string{
		"--server", front.URL,
		"--server-key", testutil.ServerKey,
		"--tls-ca", caPath,
		"--proxy", "http://" + proxy.listener.Addr().String(),
		"--id", hostID,
		"--key", "process-host-device-secret",
		"--local", "127.0.0.1:9",
		"--carrier", carrier.Addr().String(),
		"--listen", "127.0.0.1:0",
		"--keepalive", "300ms",
		"--lan-discovery=false",
		"--recording-queue", queue,
		"--upload-recordings",
	}
	startProcess := func() *hostRecordingTunnelProcess {
		if throughCppParent {
			return startHostRecordingCppParent(t, harness, binary, front.URL, caPath,
				"http://"+proxy.listener.Addr().String(), queue,
				"127.0.0.1:"+strconv.Itoa(freeHostRecordingUDPPort(t)))
		}
		return startHostRecordingProcess(t, binary, args)
	}
	first := startProcess()
	first.waitReady(t)
	if _, found, err := server.Registry.SetOwner(hostID, owner.Username); err != nil || !found {
		t.Fatalf("assign recording owner: found=%v error=%v", found, err)
	}
	if !throughCppParent {
		if err := os.WriteFile(manifestPath, manifest, 0600); err != nil {
			t.Fatal(err)
		}
	}

	select {
	case <-firstPatchApplied:
	case <-time.After(15 * time.Second):
		first.stop()
		t.Fatalf("host process did not send the first recording chunk; stderr: %s", first.stderr.String())
	}
	time.Sleep(100 * time.Millisecond)
	if err := first.stop(); err == nil {
		t.Fatal("force-terminated host process exited successfully")
	}
	if throughCppParent {
		assertCppTunnelChildStopped(t, first.stderr.String())
	}
	if _, err := os.Stat(manifestPath); err != nil {
		t.Fatalf("task manifest was removed before completion: %v", err)
	}
	stopped.Store(true)

	second := startProcess()
	second.waitReady(t)
	deadline := time.Now().Add(20 * time.Second)
	var archived []byte
	var archiveRecordID string
	for time.Now().Before(deadline) {
		records, err := server.Accounts.Recordings(owner.ID)
		if err != nil {
			t.Fatalf("list uploaded recordings: %v", err)
		}
		if len(records) == 1 {
			archiveRecordID = records[0].ID
			archived, err = os.ReadFile(filepath.Join(archiveDir, owner.ID, archiveRecordID+".mp4"))
			if err == nil {
				break
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	if !bytes.Equal(archived, recording) {
		t.Fatalf("archived bytes differ: got %d, want %d", len(archived), len(recording))
	}
	digest := sha256.Sum256(recording)
	archive, err := server.Accounts.RecordingByUploadKey(owner.ID, task.ID)
	if err != nil {
		t.Fatalf("find idempotent archive: %v", err)
	}
	if archive.ID != archiveRecordID || archive.Size != int64(len(recording)) ||
		archive.SHA256 != hex.EncodeToString(digest[:]) {
		t.Fatalf("archive metadata mismatch: %+v", archive)
	}
	manifestRemoved := false
	manifestDeadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(manifestDeadline) {
		_, err := os.Stat(manifestPath)
		if os.IsNotExist(err) {
			manifestRemoved = true
			break
		}
		if err != nil {
			t.Fatalf("stat completed task manifest: %v", err)
		}
		time.Sleep(25 * time.Millisecond)
	}
	if !manifestRemoved {
		_ = second.stop()
		info, statErr := os.Stat(manifestPath)
		mode := ""
		if statErr == nil {
			mode = info.Mode().String()
		}
		probePath := manifestPath + ".rename-probe"
		renameErr := os.Rename(manifestPath, probePath)
		if renameErr == nil {
			_ = os.Rename(probePath, manifestPath)
		}
		t.Fatalf("completed task manifest remains (mode=%s stat=%v rename=%v); parent stderr: %s",
			mode, statErr, renameErr, second.stderr.String())
	}
	if _, err := os.Stat(recordingPath); err != nil {
		t.Fatalf("local source recording was removed: %v", err)
	}

	offsetsMu.Lock()
	defer offsetsMu.Unlock()
	if len(initialOffsets) == 0 || initialOffsets[0] != 0 {
		t.Fatalf("initial upload offsets = %v, want first offset 0", initialOffsets)
	}
	resumed := false
	for _, offset := range postRestartOffsets {
		if offset == hostRecordingProcessChunkSize {
			resumed = true
		}
	}
	if !resumed {
		t.Fatalf("second host process did not resume at server offset %d: %v",
			hostRecordingProcessChunkSize, postRestartOffsets)
	}
	if proxy.connects.Load() == 0 {
		t.Fatal("host process did not use the configured HTTP CONNECT proxy")
	}
}

func assertCppTunnelChildStopped(t *testing.T, stderr string) {
	t.Helper()
	const marker = "CHAST_TUNNEL_PID="
	var pid string
	for _, line := range strings.Split(stderr, "\n") {
		if strings.HasPrefix(line, marker) {
			pid = strings.TrimSpace(strings.TrimPrefix(line, marker))
			break
		}
	}
	if _, err := strconv.Atoi(pid); err != nil {
		t.Fatalf("C++ parent did not report a valid tunnel child PID: %q; stderr: %s", pid, stderr)
	}
	output, err := exec.Command("tasklist", "/FO", "CSV", "/NH", "/FI", "PID eq "+pid).CombinedOutput()
	if err != nil {
		t.Fatalf("check tunnel child process %s: %v: %s", pid, err, output)
	}
	if strings.Contains(string(output), `"`+pid+`"`) {
		t.Fatalf("C++ TunnelProcess child %s survived abrupt parent termination: %s", pid, output)
	}
}

func startHostRecordingCppParent(t *testing.T, harness, tunnelBinary, serverURL, caPath,
	proxyURL, queue, listen string) *hostRecordingTunnelProcess {
	t.Helper()
	cmd := exec.Command(harness, "--parent-host")
	cmd.Env = hostRecordingProcessEnvironment(map[string]string{
		"CHAST_PARENT_TUNNEL_EXE": tunnelBinary,
		"CHAST_PARENT_SERVER":     serverURL,
		"CHAST_PARENT_SERVER_KEY": testutil.ServerKey,
		"CHAST_PARENT_CA":         caPath,
		"CHAST_PARENT_PROXY":      proxyURL,
		"CHAST_PARENT_QUEUE":      queue,
		"CHAST_PARENT_LISTEN":     listen,
	})
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	process := &hostRecordingTunnelProcess{
		cmd: cmd, lines: make(chan string, 32), stdoutDone: make(chan struct{}),
	}
	cmd.Stderr = &process.stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	go func() {
		defer close(process.stdoutDone)
		defer close(process.lines)
		scanner := bufio.NewScanner(stdout)
		for scanner.Scan() {
			process.lines <- scanner.Text()
		}
	}()
	t.Cleanup(func() { process.stop() })
	return process
}

func hostRecordingProcessEnvironment(values map[string]string) []string {
	env := os.Environ()
	for key, value := range values {
		prefix := strings.ToUpper(key) + "="
		filtered := make([]string, 0, len(env)+1)
		for _, entry := range env {
			if !strings.HasPrefix(strings.ToUpper(entry), prefix) {
				filtered = append(filtered, entry)
			}
		}
		env = append(filtered, key+"="+value)
	}
	return env
}

func freeHostRecordingUDPPort(t *testing.T) int {
	t.Helper()
	conn, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve temporary tunnel UDP port: %v", err)
	}
	port := conn.LocalAddr().(*net.UDPAddr).Port
	if err := conn.Close(); err != nil {
		t.Fatalf("release temporary tunnel UDP port: %v", err)
	}
	return port
}

type hostRecordingTunnelProcess struct {
	cmd        *exec.Cmd
	lines      chan string
	stdoutDone chan struct{}
	stderr     bytes.Buffer
	waited     bool
	waitErr    error
}

type hostRecordingProcessTransport struct {
	base       http.RoundTripper
	onResponse func(*http.Request, *http.Response) error
}

func (t hostRecordingProcessTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	response, err := t.base.RoundTrip(request)
	if err != nil {
		return nil, err
	}
	if t.onResponse != nil {
		if err := t.onResponse(request, response); err != nil {
			_ = response.Body.Close()
			return nil, err
		}
	}
	return response, nil
}

func startHostRecordingProcess(t *testing.T, binary string, args []string) *hostRecordingTunnelProcess {
	t.Helper()
	cmd := exec.Command(binary, append([]string{"host"}, args...)...)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	process := &hostRecordingTunnelProcess{
		cmd: cmd, lines: make(chan string, 32), stdoutDone: make(chan struct{}),
	}
	cmd.Stderr = &process.stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	go func() {
		defer close(process.stdoutDone)
		defer close(process.lines)
		scanner := bufio.NewScanner(stdout)
		for scanner.Scan() {
			process.lines <- scanner.Text()
		}
	}()
	t.Cleanup(func() { process.stop() })
	return process
}

func (p *hostRecordingTunnelProcess) waitReady(t *testing.T) {
	t.Helper()
	timer := time.NewTimer(10 * time.Second)
	defer timer.Stop()
	for {
		select {
		case line, ok := <-p.lines:
			if !ok {
				p.stop()
				t.Fatalf("host process exited before ready (error %v): %s", p.waitErr, p.stderr.String())
			}
			var event struct {
				Event string `json:"event"`
			}
			if json.Unmarshal([]byte(line), &event) == nil && event.Event == "ready" {
				return
			}
		case <-timer.C:
			p.stop()
			t.Fatalf("host process did not become ready: %s", p.stderr.String())
		}
	}
}

func (p *hostRecordingTunnelProcess) stop() error {
	if p.waited {
		return p.waitErr
	}
	if p.cmd.ProcessState == nil {
		_ = p.cmd.Process.Kill()
	}
	<-p.stdoutDone
	p.waitErr = p.cmd.Wait()
	p.waited = true
	return p.waitErr
}

type hostRecordingProcessProxy struct {
	listener net.Listener
	connects atomic.Int32
}

func startHostRecordingProcessProxy(t *testing.T) *hostRecordingProcessProxy {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("proxy listener: %v", err)
	}
	proxy := &hostRecordingProcessProxy{listener: listener}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go proxy.handle(conn)
		}
	}()
	return proxy
}

func (p *hostRecordingProcessProxy) handle(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	request, err := http.ReadRequest(reader)
	if err != nil || request.Method != http.MethodConnect {
		_, _ = io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
		return
	}
	p.connects.Add(1)
	upstream, err := net.DialTimeout("tcp", request.Host, 5*time.Second)
	if err != nil {
		_, _ = io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
		return
	}
	defer upstream.Close()
	if _, err := io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\n"); err != nil {
		return
	}
	go func() {
		_, _ = io.Copy(upstream, reader)
		_ = upstream.Close()
	}()
	_, _ = io.Copy(conn, upstream)
}

func startHostRecordingProcessCarrier(t *testing.T) net.Listener {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("local carrier listener: %v", err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				reader := bufio.NewReader(conn)
				line, err := reader.ReadString('\n')
				if err == nil && line == "CHASTT1\n" {
					_, _ = io.Copy(io.Discard, reader)
				}
			}(conn)
		}
	}()
	return listener
}
