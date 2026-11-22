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
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

const (
	hostRecordingUploadInterval  = 2 * time.Second
	hostRecordingChunkSize       = 4 << 20
	maxHostRecordingTaskBytes    = 16 << 10
	hostRecordingTimestampHeader = "X-Host-Timestamp"
	hostRecordingSignatureHeader = "X-Host-Signature"
)

var hostRecordingTaskID = regexp.MustCompile(`^[0-9a-f]{32}$`)

// hostRecordingUploadTask is an immutable manifest written atomically by the
// native host. Credentials and server offsets are deliberately not persisted.
type hostRecordingUploadTask struct {
	ID          string `json:"id"`
	Path        string `json:"path"`
	Name        string `json:"name"`
	Size        int64  `json:"size"`
	StartedAtMS int64  `json:"started_at_ms"`
	DurationMS  int64  `json:"duration_ms"`
}

type hostRecordingUploadWorker struct {
	queue  string
	hostID string
	priv   ed25519.PrivateKey
	api    *apiClient
	log    *slog.Logger
	failed string
}

func (h *HostAgent) hostRecordingUploadLoop(queue string) {
	w := &hostRecordingUploadWorker{
		queue: queue, hostID: h.cfg.ID, priv: h.priv, api: h.api,
		log: h.log.With("component", "host-recording-upload"),
	}
	ticker := time.NewTicker(hostRecordingUploadInterval)
	defer ticker.Stop()
	for {
		w.drain(h.ctx)
		select {
		case <-h.ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (w *hostRecordingUploadWorker) drain(ctx context.Context) {
	if err := os.MkdirAll(w.queue, 0700); err != nil {
		w.report(fmt.Errorf("create host recording queue: %w", err))
		return
	}
	release, acquired, err := acquireHostRecordingUploadLock(w.queue)
	if err != nil {
		w.report(err)
		return
	}
	if !acquired {
		return
	}
	defer release()
	entries, err := os.ReadDir(w.queue)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			w.report(err)
		}
		return
	}
	paths := make([]string, 0, len(entries))
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".json") &&
			!strings.HasSuffix(entry.Name(), ".tmp.json") {
			paths = append(paths, filepath.Join(w.queue, entry.Name()))
		}
	}
	sort.Strings(paths)
	for _, path := range paths {
		if ctx.Err() != nil {
			return
		}
		if err := w.processTask(ctx, path); err != nil {
			w.report(err)
			continue
		}
		w.failed = ""
	}
}

func (w *hostRecordingUploadWorker) processTask(ctx context.Context, manifest string) error {
	file, err := os.Open(manifest)
	if err != nil {
		return fmt.Errorf("open task %s: %w", filepath.Base(manifest), err)
	}
	data, readErr := io.ReadAll(io.LimitReader(file, maxHostRecordingTaskBytes+1))
	closeErr := file.Close()
	if readErr != nil {
		return fmt.Errorf("read task %s: %w", filepath.Base(manifest), readErr)
	}
	if closeErr != nil {
		return fmt.Errorf("close task %s: %w", filepath.Base(manifest), closeErr)
	}
	if len(data) > maxHostRecordingTaskBytes {
		return fmt.Errorf("task %s exceeds the manifest size limit", filepath.Base(manifest))
	}
	var task hostRecordingUploadTask
	if err := json.Unmarshal(data, &task); err != nil {
		return fmt.Errorf("decode task %s: %w", filepath.Base(manifest), err)
	}
	if err := validateHostRecordingTask(task, manifest); err != nil {
		return err
	}
	info, err := os.Lstat(task.Path)
	if err != nil {
		return fmt.Errorf("recording %s: %w", task.Name, err)
	}
	if !info.Mode().IsRegular() || info.Size() != task.Size {
		return fmt.Errorf("recording %s no longer matches its sealed task", task.Name)
	}
	complete, err := w.upload(ctx, task)
	if err != nil {
		return err
	}
	if !complete {
		return nil
	}
	if err := os.Remove(manifest); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("remove completed task %s: %w", task.ID, err)
	}
	w.log.Info("host recording archived", "task", task.ID, "name", task.Name, "size", task.Size)
	return nil
}

func validateHostRecordingTask(task hostRecordingUploadTask, manifest string) error {
	if !hostRecordingTaskID.MatchString(task.ID) || filepath.Base(manifest) != task.ID+".json" {
		return fmt.Errorf("invalid host recording task identity in %s", filepath.Base(manifest))
	}
	if task.Path == "" || !filepath.IsAbs(task.Path) || task.Name == "" ||
		task.Name != filepath.Base(task.Path) || task.Size <= 0 ||
		task.StartedAtMS < 0 || task.DurationMS < 0 ||
		!strings.EqualFold(filepath.Ext(task.Name), ".mp4") {
		return fmt.Errorf("invalid host recording task metadata in %s", filepath.Base(manifest))
	}
	return nil
}

func (w *hostRecordingUploadWorker) upload(ctx context.Context, task hostRecordingUploadTask) (bool, error) {
	if !secureHostRecordingEndpoint(w.api.endpoint()) {
		return false, errors.New("host recording uploads require HTTPS except for loopback tests")
	}
	base := "/v1/hosts/" + url.PathEscape(w.hostID) + "/recordings/uploads"
	begin := struct {
		Name           string `json:"name"`
		Size           int64  `json:"size"`
		StartedAtMS    int64  `json:"started_at_ms"`
		DurationMS     int64  `json:"duration_ms"`
		IdempotencyKey string `json:"idempotency_key"`
	}{task.Name, task.Size, task.StartedAtMS, task.DurationMS, task.ID}
	body, err := json.Marshal(begin)
	if err != nil {
		return false, err
	}
	var view hostRecordingUploadView
	if err := w.request(ctx, http.MethodPost, base, "", 0, task.Size,
		[]string{task.Name, strconv.FormatInt(task.StartedAtMS, 10),
			strconv.FormatInt(task.DurationMS, 10), task.ID}, body, "", &view); err != nil {
		return false, err
	}
	if view.Recording != nil {
		return true, nil
	}
	if view.Upload == nil || !hostRecordingTaskID.MatchString(view.Upload.ID) ||
		view.Upload.Offset < 0 || view.Upload.Offset > task.Size {
		return false, errors.New("host recording begin returned an invalid upload offset")
	}
	uploadID := view.Upload.ID
	uploadPath := base + "/" + url.PathEscape(uploadID)
	offset := view.Upload.Offset
	file, err := os.Open(task.Path)
	if err != nil {
		return false, fmt.Errorf("open recording %s: %w", task.Name, err)
	}
	defer file.Close()
	for offset < task.Size {
		length := int64(hostRecordingChunkSize)
		if remaining := task.Size - offset; remaining < length {
			length = remaining
		}
		chunk := make([]byte, int(length))
		read, readErr := file.ReadAt(chunk, offset)
		if readErr != nil && !errors.Is(readErr, io.EOF) {
			return false, fmt.Errorf("read recording %s at offset %d: %w", task.Name, offset, readErr)
		}
		if int64(read) != length {
			return false, fmt.Errorf("recording %s ended before its sealed size", task.Name)
		}
		digest := sha256.Sum256(chunk)
		digestText := hex.EncodeToString(digest[:])
		var appended hostRecordingUploadView
		if err := w.request(ctx, http.MethodPatch, uploadPath, uploadID, offset, length,
			[]string{digestText}, chunk, digestText, &appended); err != nil {
			return false, err
		}
		if appended.Upload == nil || appended.Upload.Offset != offset+length {
			return false, errors.New("host recording append returned an unexpected offset")
		}
		offset = appended.Upload.Offset
	}
	var completed json.RawMessage
	if err := w.request(ctx, http.MethodPost, uploadPath+"/complete", uploadID, 0, 0,
		nil, nil, "", &completed); err != nil {
		return false, err
	}
	return true, nil
}

func secureHostRecordingEndpoint(raw string) bool {
	u, err := url.Parse(raw)
	if err != nil {
		return false
	}
	if strings.EqualFold(u.Scheme, "https") {
		return true
	}
	if !strings.EqualFold(u.Scheme, "http") {
		return false
	}
	host := strings.TrimSuffix(strings.ToLower(u.Hostname()), ".")
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

type hostRecordingUploadView struct {
	Upload *struct {
		ID     string `json:"id"`
		Offset int64  `json:"offset"`
	} `json:"upload,omitempty"`
	Recording json.RawMessage `json:"recording,omitempty"`
}

func (w *hostRecordingUploadWorker) request(ctx context.Context, method, path, uploadID string,
	offset, signedSize int64, metadata []string, body []byte, chunkDigest string, out any) error {
	timestamp := time.Now().Unix()
	message := identity.HostRecordingUploadMessage(w.hostID, method, uploadID,
		timestamp, offset, signedSize, metadata...)
	reqCtx, cancel := context.WithTimeout(ctx, 12*time.Minute)
	defer cancel()
	var reader io.Reader
	if body != nil {
		reader = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(reqCtx, method, w.api.endpoint()+path, reader)
	if err != nil {
		return err
	}
	if body != nil {
		req.ContentLength = int64(len(body))
	}
	if body != nil && method != http.MethodPatch {
		req.Header.Set("Content-Type", "application/json")
	}
	if method == http.MethodPatch {
		req.Header.Set("X-Upload-Offset", strconv.FormatInt(offset, 10))
		req.Header.Set("X-Chunk-SHA256", chunkDigest)
	}
	req.Header.Set(hostRecordingTimestampHeader, strconv.FormatInt(timestamp, 10))
	req.Header.Set(hostRecordingSignatureHeader, identity.Sign(w.priv, message))
	if w.api.serverKey != "" {
		req.Header.Set("Authorization", "Bearer "+w.api.serverKey)
	}
	resp, err := w.api.poll.Do(req)
	if err != nil {
		return fmt.Errorf("%s %s: %w", method, path, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return fmt.Errorf("%s %s: read reply: %w", method, path, err)
	}
	if resp.StatusCode >= 300 {
		var apiErr apiError
		detail := fmt.Sprintf("HTTP %d", resp.StatusCode)
		if json.Unmarshal(data, &apiErr) == nil && apiErr.Error != "" {
			detail = apiErr.Error
		}
		return &apiStatusError{method: method, path: path, detail: detail, status: resp.StatusCode}
	}
	if out != nil {
		if err := json.Unmarshal(data, out); err != nil {
			return fmt.Errorf("%s %s: decode reply: %w", method, path, err)
		}
	}
	return nil
}

func (w *hostRecordingUploadWorker) report(err error) {
	text := err.Error()
	if text == w.failed {
		w.log.Debug("host recording upload retry", "error", err)
		return
	}
	w.failed = text
	w.log.Warn("host recording upload", "error", err)
}
