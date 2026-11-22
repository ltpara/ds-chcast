package agent

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func TestHostRecordingUploadResumesFromServerOffset(t *testing.T) {
	root := t.TempDir()
	queue := filepath.Join(root, "recordings")
	if err := os.MkdirAll(queue, 0700); err != nil {
		t.Fatal(err)
	}
	const taskID = "0123456789abcdef0123456789abcdef"
	data := bytes.Repeat([]byte("sealed-mp4-segment-"), hostRecordingChunkSize/19+3)
	recordingPath := filepath.Join(root, "session one.mp4")
	if err := os.WriteFile(recordingPath, data, 0600); err != nil {
		t.Fatal(err)
	}
	task := hostRecordingUploadTask{
		ID: taskID, Path: recordingPath, Name: filepath.Base(recordingPath), Size: int64(len(data)),
		StartedAtMS: 1784822400000, DurationMS: 61000,
	}
	manifest, err := json.Marshal(task)
	if err != nil {
		t.Fatal(err)
	}
	manifestPath := filepath.Join(queue, taskID+".json")
	if err := os.WriteFile(manifestPath, manifest, 0600); err != nil {
		t.Fatal(err)
	}
	if bytes.Contains(manifest, []byte("host-secret")) || bytes.Contains(manifest, []byte("device-secret")) {
		t.Fatal("task manifest contains credentials")
	}

	const hostID = "host-device-1"
	priv := identity.PrivateKeyFromSecret("device-secret-123")
	var mu sync.Mutex
	var uploaded []byte
	var completed bool
	var failFirstAppend = true
	var beginCount, appendCount int
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer host-secret" {
			http.Error(w, "missing host credential", http.StatusUnauthorized)
			return
		}
		const route = "/v1/hosts/host-device-1/recordings/uploads"
		if r.URL.Path != route && !strings.HasPrefix(r.URL.Path, route+"/") {
			http.NotFound(w, r)
			return
		}
		resource := strings.TrimPrefix(r.URL.Path, route)
		if resource == "" && r.Method == http.MethodPost {
			var req struct {
				Name           string `json:"name"`
				Size           int64  `json:"size"`
				StartedAtMS    int64  `json:"started_at_ms"`
				DurationMS     int64  `json:"duration_ms"`
				IdempotencyKey string `json:"idempotency_key"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			if !verifyHostRecordingTestSignature(t, r, priv, hostID, "", 0, req.Size,
				req.Name, strconv.FormatInt(req.StartedAtMS, 10),
				strconv.FormatInt(req.DurationMS, 10), req.IdempotencyKey) {
				http.Error(w, "bad signature", http.StatusUnauthorized)
				return
			}
			mu.Lock()
			beginCount++
			offset := len(uploaded)
			isCompleted := completed
			mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			if isCompleted {
				fmt.Fprint(w, `{"recording":{"id":"recording-1"}}`)
				return
			}
			fmt.Fprintf(w, `{"upload":{"id":"%s","offset":%d}}`, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", offset)
			return
		}
		if resource == "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" && r.Method == http.MethodPatch {
			offset, err := strconv.ParseInt(r.Header.Get("X-Upload-Offset"), 10, 64)
			if err != nil {
				http.Error(w, "bad offset", http.StatusBadRequest)
				return
			}
			chunk, err := io.ReadAll(r.Body)
			if err != nil {
				http.Error(w, "bad chunk", http.StatusBadRequest)
				return
			}
			digest := sha256.Sum256(chunk)
			digestText := hex.EncodeToString(digest[:])
			if r.Header.Get("X-Chunk-SHA256") != digestText ||
				!verifyHostRecordingTestSignature(t, r, priv, hostID,
					"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", offset, int64(len(chunk)), digestText) {
				http.Error(w, "bad chunk proof", http.StatusUnauthorized)
				return
			}
			mu.Lock()
			if int(offset) != len(uploaded) {
				mu.Unlock()
				http.Error(w, "stale offset", http.StatusConflict)
				return
			}
			uploaded = append(uploaded, chunk...)
			appendCount++
			fail := failFirstAppend
			failFirstAppend = false
			newOffset := len(uploaded)
			mu.Unlock()
			if fail {
				http.Error(w, "simulated lost response", http.StatusBadGateway)
				return
			}
			fmt.Fprintf(w, `{"upload":{"id":"%s","offset":%d}}`, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", newOffset)
			return
		}
		if resource == "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/complete" && r.Method == http.MethodPost {
			if !verifyHostRecordingTestSignature(t, r, priv, hostID,
				"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, 0) {
				http.Error(w, "bad completion proof", http.StatusUnauthorized)
				return
			}
			mu.Lock()
			if !bytes.Equal(uploaded, data) {
				mu.Unlock()
				http.Error(w, "size or content mismatch", http.StatusConflict)
				return
			}
			completed = true
			mu.Unlock()
			fmt.Fprint(w, `{"recording":{"id":"recording-1"}}`)
			return
		}
		http.NotFound(w, r)
	}))
	defer server.Close()

	worker := &hostRecordingUploadWorker{
		queue: queue, hostID: hostID, priv: priv,
		api: newAPIClient(server.URL, "host-secret", nil, nil),
		log: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	if err := worker.processTask(context.Background(), manifestPath); err == nil {
		t.Fatal("the simulated lost append response should leave the task pending")
	}
	if _, err := os.Stat(manifestPath); err != nil {
		t.Fatalf("task was removed before archive completion: %v", err)
	}
	if err := worker.processTask(context.Background(), manifestPath); err != nil {
		t.Fatalf("resume upload: %v", err)
	}
	if _, err := os.Stat(manifestPath); !os.IsNotExist(err) {
		t.Fatalf("completed task manifest remains: %v", err)
	}
	if _, err := os.Stat(recordingPath); err != nil {
		t.Fatalf("local source recording was removed: %v", err)
	}
	mu.Lock()
	defer mu.Unlock()
	if !completed || beginCount != 2 || appendCount != 2 || !bytes.Equal(uploaded, data) {
		t.Fatalf("resume state: complete=%v begin=%d append=%d bytes=%d want=%d",
			completed, beginCount, appendCount, len(uploaded), len(data))
	}
}

func TestHostRecordingUploadConsumesCppManifest(t *testing.T) {
	queue := os.Getenv("CHAST_CPP_RECORDING_QUEUE")
	if queue == "" {
		t.Skip("set CHAST_CPP_RECORDING_QUEUE to a manifest emitted by the C++ producer")
	}
	entries, err := os.ReadDir(queue)
	if err != nil {
		t.Fatal(err)
	}
	var manifestPath string
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".json") &&
			!strings.HasSuffix(entry.Name(), ".tmp.json") {
			if manifestPath != "" {
				t.Fatal("expected exactly one C++ task manifest")
			}
			manifestPath = filepath.Join(queue, entry.Name())
		}
	}
	if manifestPath == "" {
		t.Fatal("C++ producer did not leave a task manifest")
	}
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		t.Fatal(err)
	}
	var task hostRecordingUploadTask
	if err := json.Unmarshal(data, &task); err != nil {
		t.Fatalf("decode C++ manifest: %v", err)
	}
	if err := validateHostRecordingTask(task, manifestPath); err != nil {
		t.Fatalf("C++ manifest contract: %v", err)
	}
	if task.StartedAtMS <= 0 || task.DurationMS <= 0 {
		t.Fatalf("C++ manifest timing is invalid: started=%d duration=%d",
			task.StartedAtMS, task.DurationMS)
	}
	recording, err := os.ReadFile(task.Path)
	if err != nil {
		t.Fatal(err)
	}
	if int64(len(recording)) != task.Size {
		t.Fatalf("C++ manifest size=%d, file size=%d", task.Size, len(recording))
	}

	const hostID = "cpp-host"
	priv := identity.PrivateKeyFromSecret("cpp-device-secret-123")
	const uploadID = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	var uploaded []byte
	completed := false
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer cpp-host-secret" {
			http.Error(w, "missing host credential", http.StatusUnauthorized)
			return
		}
		const route = "/v1/hosts/cpp-host/recordings/uploads"
		if r.URL.Path != route && !strings.HasPrefix(r.URL.Path, route+"/") {
			http.NotFound(w, r)
			return
		}
		resource := strings.TrimPrefix(r.URL.Path, route)
		if resource == "" && r.Method == http.MethodPost {
			var req struct {
				Name           string `json:"name"`
				Size           int64  `json:"size"`
				StartedAtMS    int64  `json:"started_at_ms"`
				DurationMS     int64  `json:"duration_ms"`
				IdempotencyKey string `json:"idempotency_key"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			if req.Name != task.Name || req.Size != task.Size ||
				req.StartedAtMS != task.StartedAtMS || req.DurationMS != task.DurationMS ||
				req.IdempotencyKey != task.ID ||
				!verifyHostRecordingTestSignature(t, r, priv, hostID, "", 0, req.Size,
					req.Name, strconv.FormatInt(req.StartedAtMS, 10),
					strconv.FormatInt(req.DurationMS, 10), req.IdempotencyKey) {
				http.Error(w, "C++ manifest metadata or begin signature mismatch", http.StatusUnauthorized)
				return
			}
			fmt.Fprintf(w, `{"upload":{"id":"%s","offset":0}}`, uploadID)
			return
		}
		if resource == "/"+uploadID && r.Method == http.MethodPatch {
			offset, err := strconv.ParseInt(r.Header.Get("X-Upload-Offset"), 10, 64)
			if err != nil || offset != 0 {
				http.Error(w, "bad upload offset", http.StatusConflict)
				return
			}
			chunk, err := io.ReadAll(r.Body)
			if err != nil {
				http.Error(w, "bad chunk", http.StatusBadRequest)
				return
			}
			digest := sha256.Sum256(chunk)
			digestText := hex.EncodeToString(digest[:])
			if r.Header.Get("X-Chunk-SHA256") != digestText ||
				!verifyHostRecordingTestSignature(t, r, priv, hostID, uploadID,
					offset, int64(len(chunk)), digestText) {
				http.Error(w, "bad signed chunk", http.StatusUnauthorized)
				return
			}
			uploaded = append(uploaded, chunk...)
			fmt.Fprintf(w, `{"upload":{"id":"%s","offset":%d}}`, uploadID, len(uploaded))
			return
		}
		if resource == "/"+uploadID+"/complete" && r.Method == http.MethodPost {
			if !verifyHostRecordingTestSignature(t, r, priv, hostID, uploadID, 0, 0) ||
				!bytes.Equal(uploaded, recording) {
				http.Error(w, "completion proof or bytes mismatch", http.StatusConflict)
				return
			}
			completed = true
			fmt.Fprint(w, `{"recording":{"id":"cpp-recording"}}`)
			return
		}
		http.NotFound(w, r)
	}))
	defer server.Close()

	worker := &hostRecordingUploadWorker{
		queue: queue, hostID: hostID, priv: priv,
		api: newAPIClient(server.URL, "cpp-host-secret", nil, nil),
		log: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	worker.drain(context.Background())
	if !completed || !bytes.Equal(uploaded, recording) {
		t.Fatalf("loopback archive did not complete: completed=%v uploaded=%d want=%d",
			completed, len(uploaded), len(recording))
	}
	if _, err := os.Stat(manifestPath); !os.IsNotExist(err) {
		t.Fatalf("completed C++ task was not removed: %v", err)
	}
	if _, err := os.Stat(task.Path); err != nil {
		t.Fatalf("C++ source recording was not retained: %v", err)
	}
}

func TestHostRecordingUploadWorkerLockIsExclusive(t *testing.T) {
	if os.Getenv("CHAST_RECORDING_LOCK_CHILD") == "1" {
		release, acquired, err := acquireHostRecordingUploadLock(os.Getenv("CHAST_RECORDING_LOCK_QUEUE"))
		if err != nil {
			t.Errorf("child lock: %v", err)
			os.Exit(2)
		}
		if acquired {
			release()
			os.Exit(0)
		}
		os.Exit(10)
	}
	queue := filepath.Join(t.TempDir(), "queue")
	if err := os.MkdirAll(queue, 0700); err != nil {
		t.Fatal(err)
	}
	release, acquired, err := acquireHostRecordingUploadLock(queue)
	if err != nil || !acquired {
		t.Fatalf("first worker lock: acquired=%v error=%v", acquired, err)
	}
	child := func() error {
		command := exec.Command(os.Args[0], "-test.run=^TestHostRecordingUploadWorkerLockIsExclusive$")
		command.Env = append(os.Environ(), "CHAST_RECORDING_LOCK_CHILD=1", "CHAST_RECORDING_LOCK_QUEUE="+queue)
		return command.Run()
	}
	err = child()
	var exitErr *exec.ExitError
	if !errors.As(err, &exitErr) || exitErr.ExitCode() != 10 {
		release()
		t.Fatalf("second process did not observe the held lock: %v", err)
	}
	release()
	if err := child(); err != nil {
		t.Fatalf("lock was not released after its owner exited: %v", err)
	}
}

func verifyHostRecordingTestSignature(t *testing.T, r *http.Request, priv []byte,
	hostID, uploadID string, offset, size int64, metadata ...string) bool {
	t.Helper()
	timestamp, err := strconv.ParseInt(r.Header.Get(hostRecordingTimestampHeader), 10, 64)
	if err != nil {
		return false
	}
	publicKey := ed25519.PublicKey(priv[32:])
	message := identity.HostRecordingUploadMessage(hostID, r.Method, uploadID,
		timestamp, offset, size, metadata...)
	return identity.Verify(publicKey, message, r.Header.Get(hostRecordingSignatureHeader))
}
