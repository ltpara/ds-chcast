// Package auditarchive copies the account history tables to append-only JSONL
// files so operators can keep audit evidence beyond the in-database row caps.
package auditarchive

import (
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// batch is the page size used while walking one history table.
const batch = 500

// Options controls what happens to files whose UTC day is already closed.
// Compressed files keep their rows byte-for-byte inside a gzip container;
// pruning removes them once their day is older than RetentionDays.
type Options struct {
	Compress      bool
	RetentionDays int
	// RemoteURL enables HTTP delivery of closed archive files: every closed
	// file is PUT to <RemoteURL>/<file name>. Delivery is at-least-once; a
	// successful upload writes a <file>.delivered marker next to it so later
	// passes skip it. While delivery is enabled an expired file is kept until
	// its marker exists, so retention never deletes evidence that has not
	// reached the remote sink.
	RemoteURL string
	// RemoteToken is sent as "Authorization: Bearer <token>" with every
	// upload. Empty sends no Authorization header.
	RemoteToken string
	// S3 selects native AWS Signature Version 4 delivery to an S3-compatible
	// endpoint and replaces RemoteURL/RemoteToken: every closed file is PUT to
	// <endpoint>/<bucket>/<file> with a signed Authorization header. Delivery
	// stays at-least-once and the local copy is kept until the marker exists.
	S3 *S3Options
}

// deliveredSuffix marks an archive file the remote endpoint accepted. The
// suffix keeps the marker out of splitArchiveName, so markers are never
// mistaken for archives or uploaded.
const deliveredSuffix = ".delivered"

// Archiver appends new history rows to one JSONL file per stream and UTC day.
// Checkpoints live in the account database and advance only after the file
// data is fsynced, so a crash between the two replays a page on the next run
// but never loses rows. Consumers can dedupe on (stream, id).
type Archiver struct {
	dir    string
	store  *account.Store
	logger *slog.Logger
	opts   Options
	now    func() time.Time
	mu     sync.Mutex

	// Remote delivery. remote is empty when disabled; client and retryDelay
	// are fields so tests can run without real waits. s3 is non-nil when the
	// target is signed with SigV4; remote then holds the unsigned bucket URL
	// for operators and logs.
	remote      string
	remoteToken string
	s3          *s3Signer
	client      *http.Client
	retryDelay  func(attempt int) time.Duration
}

// Result reports how many rows one pass copied and which closed files it
// compressed, removed or uploaded.
type Result struct {
	Audit         int
	Connections   int
	FileTransfers int
	Files         []string
	Compressed    []string
	Pruned        []string
	Delivered     []string
	// DeliveryError is the first upload failure of this pass. The file stays
	// pending and the next pass retries it.
	DeliveryError string
}

func New(dir string, store *account.Store, logger *slog.Logger, opts Options) (*Archiver, error) {
	dir = strings.TrimSpace(dir)
	if dir == "" {
		return nil, fmt.Errorf("audit archive directory is empty")
	}
	if store == nil {
		return nil, fmt.Errorf("audit archive needs an account store")
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, fmt.Errorf("audit archive directory %s: %w", dir, err)
	}
	if err := os.MkdirAll(abs, 0o700); err != nil {
		return nil, fmt.Errorf("audit archive directory %s: %w", abs, err)
	}
	if logger == nil {
		logger = slog.Default()
	}
	if opts.RetentionDays < 0 {
		return nil, fmt.Errorf("audit archive retention days must not be negative")
	}
	remote, err := normalizeRemote(opts.RemoteURL)
	if err != nil {
		return nil, err
	}
	if opts.RemoteToken != "" && remote == "" {
		return nil, fmt.Errorf("audit archive remote token needs a remote URL")
	}
	if opts.S3 != nil && (remote != "" || opts.RemoteToken != "") {
		return nil, fmt.Errorf("audit archive remote URL and S3 delivery are mutually exclusive")
	}
	archiver := &Archiver{dir: abs, store: store, logger: logger, opts: opts, now: time.Now}
	if opts.S3 != nil {
		signer, signErr := newS3Signer(*opts.S3)
		if signErr != nil {
			return nil, signErr
		}
		archiver.s3 = signer
		archiver.remote = signer.bucketURL()
	}
	if remote != "" {
		archiver.remote = remote
		archiver.remoteToken = opts.RemoteToken
	}
	if archiver.remote != "" {
		archiver.client = &http.Client{
			Timeout: 2 * time.Minute,
			// Every upload carries a one-shot body (and, for S3, a signature
			// bound to the original host and path), so a redirect can never be
			// replayed safely. Surface 3xx as a delivery error instead of
			// letting net/http try.
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return http.ErrUseLastResponse
			},
		}
		archiver.retryDelay = func(attempt int) time.Duration {
			return time.Duration(attempt) * time.Second
		}
	}
	return archiver, nil
}

// normalizeRemote validates the upload base URL. Credentials, queries and
// fragments are refused: appending a file name to them would produce the
// wrong target, and credentials belong in the token option so they never end
// up in a log line.
func normalizeRemote(raw string) (string, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "", nil
	}
	parsed, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("audit archive remote URL: %w", err)
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return "", fmt.Errorf("audit archive remote URL must be http or https")
	}
	if parsed.Host == "" {
		return "", fmt.Errorf("audit archive remote URL has no host")
	}
	if parsed.User != nil {
		return "", fmt.Errorf("audit archive remote URL must not carry credentials; use the remote token option")
	}
	if parsed.RawQuery != "" || parsed.Fragment != "" {
		return "", fmt.Errorf("audit archive remote URL must not carry a query or fragment")
	}
	return strings.TrimRight(raw, "/"), nil
}

func (a *Archiver) Dir() string { return a.dir }

// Checkpoints returns the last archived row id per stream.
func (a *Archiver) Checkpoints() (map[string]int64, error) {
	streams := []string{
		account.HistoryAudit,
		account.HistoryConnections,
		account.HistoryFileTransfers,
	}
	out := make(map[string]int64, len(streams))
	for _, stream := range streams {
		last, err := a.store.ArchiveCheckpoint(stream)
		if err != nil {
			return nil, err
		}
		out[stream] = last
	}
	return out, nil
}

// Run copies every row appended since the last checkpoint. A failure in one
// stream leaves the others untouched until the next pass.
func (a *Archiver) Run() (Result, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	now := a.now().UTC()
	day := now.Format("20060102")
	var result Result
	// A closed UTC-day file never receives rows again: runStream only appends
	// to today's file. Compacting before the copy keeps today's target out of
	// the scan, and a compaction failure is logged instead of stopping rows.
	a.compact(now, day, &result)
	steps := []struct {
		stream string
		count  *int
		read   func(afterID int64, limit int) ([]record, error)
	}{
		{account.HistoryAudit, &result.Audit, a.readAudit},
		{account.HistoryConnections, &result.Connections, a.readConnections},
		{account.HistoryFileTransfers, &result.FileTransfers, a.readFileTransfers},
	}
	for _, step := range steps {
		count, files, err := a.runStream(step.stream, day, step.read)
		*step.count = count
		result.Files = append(result.Files, files...)
		if err != nil {
			return result, fmt.Errorf("archive %s: %w", step.stream, err)
		}
	}
	if a.remote != "" {
		a.deliver(day, &result)
	}
	return result, nil
}

// Remote reports the upload base URL, empty when delivery is disabled.
func (a *Archiver) Remote() string { return a.remote }

// deliver uploads every closed-day archive that the remote endpoint has not
// accepted yet. Files are independent: the first failure is recorded and the
// rest stay pending, so a broken endpoint cannot stall the archive pass or
// lose the local copy.
func (a *Archiver) deliver(today string, result *Result) {
	entries, err := os.ReadDir(a.dir)
	if err != nil {
		result.DeliveryError = "scan: " + err.Error()
		a.logger.Warn("audit archive: cannot scan the directory for delivery", "error", err)
		return
	}
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		_, day, _, ok := splitArchiveName(entry.Name())
		if !ok || day >= today {
			continue
		}
		path := filepath.Join(a.dir, entry.Name())
		if deliveredMarkerExists(path) {
			continue
		}
		if err := a.upload(path); err != nil {
			result.DeliveryError = entry.Name() + ": " + err.Error()
			a.logger.Warn("audit archive: remote delivery failed", "file", entry.Name(),
				"remote", a.remote, "error", err)
			return
		}
		result.Delivered = append(result.Delivered, entry.Name())
	}
}

// upload PUTs one file and writes the marker only after a 2xx answer. A
// failed upload leaves the file pending for the next pass; retries within the
// pass cover a transient receiver error.
func (a *Archiver) upload(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("not a regular file")
	}
	digest, err := fileSHA256(path)
	if err != nil {
		return err
	}
	name := filepath.Base(path)
	target := a.targetFor(name)
	var lastErr error
	const attempts = 3
	for attempt := 1; attempt <= attempts; attempt++ {
		lastErr = a.putFile(target, path, name, info.Size(), digest)
		if lastErr == nil {
			return writeDeliveredMarker(path, deliveredMarker{
				File:        name,
				Bytes:       info.Size(),
				SHA256:      digest,
				DeliveredAt: a.now().UTC().Format(time.RFC3339),
				URL:         target,
			})
		}
		if attempt < attempts {
			time.Sleep(a.retryDelay(attempt))
		}
	}
	return lastErr
}

// targetFor returns the object URL for one archive file: a signed S3 object
// when S3 delivery is configured, otherwise the plain HTTP sink URL.
func (a *Archiver) targetFor(name string) string {
	if a.s3 != nil {
		return a.s3.target(name)
	}
	return a.remote + "/" + url.PathEscape(name)
}

func (a *Archiver) putFile(target, path, name string, size int64, digest string) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()
	request, err := http.NewRequest(http.MethodPut, target, file)
	if err != nil {
		return err
	}
	// A file body has no automatic length; without it the receiver cannot
	// distinguish a truncated upload from a complete one.
	request.ContentLength = size
	request.Header.Set("Content-Type", contentTypeFor(name))
	if a.s3 != nil {
		// The digest is the payload hash, so SigV4 never re-reads the file.
		a.s3.sign(request, digest, a.now())
	} else {
		request.Header.Set("X-Chast-Audit-Sha256", digest)
		request.Header.Set("Idempotency-Key", name)
		if a.remoteToken != "" {
			request.Header.Set("Authorization", "Bearer "+a.remoteToken)
		}
	}
	response, err := a.client.Do(request)
	if err != nil {
		return err
	}
	io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
	response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode > 299 {
		return fmt.Errorf("receiver answered %s", response.Status)
	}
	return nil
}

func contentTypeFor(name string) string {
	if strings.HasSuffix(name, ".gz") {
		return "application/gzip"
	}
	return "application/x-ndjson"
}

func fileSHA256(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	hasher := sha256.New()
	if _, err := io.Copy(hasher, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(hasher.Sum(nil)), nil
}

type deliveredMarker struct {
	File        string `json:"file"`
	Bytes       int64  `json:"bytes"`
	SHA256      string `json:"sha256"`
	DeliveredAt string `json:"delivered_at"`
	URL         string `json:"url"`
}

// writeDeliveredMarker publishes the marker atomically: a half-written marker
// would be read as "already delivered" and skip the upload forever.
func writeDeliveredMarker(path string, marker deliveredMarker) error {
	data, err := json.Marshal(marker)
	if err != nil {
		return err
	}
	markerPath := path + deliveredSuffix
	tmp := markerPath + ".tmp"
	if err := os.WriteFile(tmp, data, 0o600); err != nil {
		return err
	}
	if err := os.Rename(tmp, markerPath); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

func deliveredMarkerExists(path string) bool {
	_, err := os.Stat(path + deliveredSuffix)
	return err == nil
}

// compact gzips every closed-day JSONL file and removes files outside the
// retention window. Only names produced by runStream are touched, so foreign
// files in the directory (notes, exports, another tool's JSONL) stay alone.
func (a *Archiver) compact(now time.Time, today string, result *Result) {
	if !a.opts.Compress && a.opts.RetentionDays <= 0 {
		return
	}
	entries, err := os.ReadDir(a.dir)
	if err != nil {
		a.logger.Warn("audit archive: cannot scan the directory", "error", err)
		return
	}
	cutoff := ""
	if a.opts.RetentionDays > 0 {
		cutoff = now.AddDate(0, 0, -a.opts.RetentionDays).Format("20060102")
	}
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		_, day, gzipped, ok := splitArchiveName(entry.Name())
		// Future-dated names can only come from a clock that moved backwards;
		// leaving them alone is safer than compressing a file today appends to.
		if !ok || day >= today {
			continue
		}
		path := filepath.Join(a.dir, entry.Name())
		if cutoff != "" && day < cutoff {
			// With remote delivery enabled, evidence leaves the disk only after
			// the receiver accepted it. An undelivered file is retried by the
			// delivery step of this pass and pruned on a later one.
			if a.remote != "" && !deliveredMarkerExists(path) {
				continue
			}
			if err := os.Remove(path); err != nil {
				a.logger.Warn("audit archive: cannot prune", "file", entry.Name(), "error", err)
				continue
			}
			// Best effort: an orphaned marker is ignored by the uploader and
			// harmless, but leaving it would hide a later file of the same name.
			os.Remove(path + deliveredSuffix)
			result.Pruned = append(result.Pruned, entry.Name())
			continue
		}
		if !a.opts.Compress || gzipped {
			continue
		}
		if err := compressJSONL(path); err != nil {
			a.logger.Warn("audit archive: cannot compress", "file", entry.Name(), "error", err)
			continue
		}
		result.Compressed = append(result.Compressed, entry.Name()+".gz")
		// The .gz is a new object for the uploader. A marker left over from a
		// plain file delivered before compression was enabled must not hide it.
		os.Remove(path + deliveredSuffix)
	}
}

// splitArchiveName accepts exactly `<stream>-<YYYYMMDD>.jsonl` optionally
// followed by `.gz`. It rejects temp files so a half-written gzip from a
// crash is never mistaken for an archive.
func splitArchiveName(name string) (stream, day string, gzipped, ok bool) {
	base := name
	if strings.HasSuffix(base, ".gz") {
		gzipped = true
		base = strings.TrimSuffix(base, ".gz")
	}
	if !strings.HasSuffix(base, ".jsonl") {
		return "", "", false, false
	}
	base = strings.TrimSuffix(base, ".jsonl")
	for _, candidate := range []string{
		account.HistoryAudit,
		account.HistoryConnections,
		account.HistoryFileTransfers,
	} {
		prefix := candidate + "-"
		if !strings.HasPrefix(base, prefix) {
			continue
		}
		day = strings.TrimPrefix(base, prefix)
		if len(day) != 8 {
			return "", "", false, false
		}
		for i := 0; i < len(day); i++ {
			if day[i] < '0' || day[i] > '9' {
				return "", "", false, false
			}
		}
		return candidate, day, gzipped, true
	}
	return "", "", false, false
}

// compressJSONL writes a gzip container to a temp file, fsyncs it and renames
// it over `<name>.gz` before removing the original. A crash at any point
// leaves either the original or a complete gzip, never both halves missing.
func compressJSONL(path string) error {
	src, err := os.Open(path)
	if err != nil {
		return err
	}
	tmp := path + ".gz.tmp"
	out, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o600)
	if err != nil {
		src.Close()
		return err
	}
	zw := gzip.NewWriter(out)
	_, copyErr := io.Copy(zw, src)
	// Windows refuses to delete a file with a live handle, so the source is
	// closed before the original is removed below.
	srcCloseErr := src.Close()
	closeErr := zw.Close()
	syncErr := out.Sync()
	outErr := out.Close()
	if copyErr != nil || srcCloseErr != nil || closeErr != nil || syncErr != nil || outErr != nil {
		os.Remove(tmp)
		if copyErr != nil {
			return copyErr
		}
		if srcCloseErr != nil {
			return srcCloseErr
		}
		if closeErr != nil {
			return closeErr
		}
		if syncErr != nil {
			return syncErr
		}
		return outErr
	}
	if err := os.Rename(tmp, path+".gz"); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Remove(path); err != nil {
		return fmt.Errorf("remove %s after compression: %w", path, err)
	}
	return nil
}

func (a *Archiver) runStream(stream, day string, read func(afterID int64, limit int) ([]record, error)) (int, []string, error) {
	checkpoint, err := a.store.ArchiveCheckpoint(stream)
	if err != nil {
		return 0, nil, err
	}
	path := filepath.Join(a.dir, stream+"-"+day+".jsonl")
	var (
		file    *os.File
		files   []string
		written int
	)
	for {
		records, err := read(checkpoint, batch)
		if err != nil {
			closeFile(file)
			return written, files, err
		}
		if len(records) == 0 {
			break
		}
		if file == nil {
			file, err = os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
			if err != nil {
				return written, files, fmt.Errorf("open %s: %w", path, err)
			}
			files = append(files, path)
		}
		var buf bytes.Buffer
		buf.Grow(len(records) * 160)
		for _, record := range records {
			buf.Write(record.line)
			buf.WriteByte('\n')
		}
		if _, err := file.Write(buf.Bytes()); err != nil {
			closeFile(file)
			return written, files, fmt.Errorf("write %s: %w", path, err)
		}
		if err := file.Sync(); err != nil {
			closeFile(file)
			return written, files, fmt.Errorf("sync %s: %w", path, err)
		}
		last := records[len(records)-1].id
		if err := a.store.AdvanceArchiveCheckpoint(stream, last); err != nil {
			closeFile(file)
			return written, files, err
		}
		checkpoint = last
		written += len(records)
		if len(records) < batch {
			break
		}
	}
	if err := closeFile(file); err != nil {
		return written, files, fmt.Errorf("close %s: %w", path, err)
	}
	return written, files, nil
}

func closeFile(file *os.File) error {
	if file == nil {
		return nil
	}
	return file.Close()
}

type record struct {
	id   int64
	line []byte
}

type auditLine struct {
	Stream    string `json:"stream"`
	ID        int64  `json:"id"`
	CreatedAt string `json:"created_at"`
	Username  string `json:"username"`
	Action    string `json:"action"`
	Detail    string `json:"detail"`
	ClientIP  string `json:"client_ip"`
	Location  string `json:"location"`
	Success   bool   `json:"success"`
}

type connectionLine struct {
	Stream     string `json:"stream"`
	ID         int64  `json:"id"`
	StartedAt  string `json:"started_at"`
	EndedAt    string `json:"ended_at"`
	DurationMS int64  `json:"duration_ms"`
	Username   string `json:"username"`
	PeerID     string `json:"peer_id"`
	HostID     string `json:"host_id"`
	AuditRef   string `json:"audit_ref"`
	Source     string `json:"source"`
	Direction  string `json:"direction"`
	ClientIP   string `json:"client_ip"`
	Location   string `json:"location"`
	Route      string `json:"route"`
	Success    bool   `json:"success"`
	Detail     string `json:"detail"`
}

type transferLine struct {
	Stream     string `json:"stream"`
	ID         int64  `json:"id"`
	OccurredAt string `json:"occurred_at"`
	Username   string `json:"username"`
	PeerID     string `json:"peer_id"`
	HostID     string `json:"host_id"`
	AuditRef   string `json:"audit_ref"`
	Source     string `json:"source"`
	Direction  string `json:"direction"`
	Name       string `json:"name"`
	Size       int64  `json:"size"`
	Files      int    `json:"files"`
	ClientIP   string `json:"client_ip"`
	Location   string `json:"location"`
	Route      string `json:"route"`
	Success    bool   `json:"success"`
	Detail     string `json:"detail"`
}

func (a *Archiver) readAudit(afterID int64, limit int) ([]record, error) {
	events, err := a.store.AuditEventsAfter(afterID, limit)
	if err != nil {
		return nil, err
	}
	records := make([]record, 0, len(events))
	for _, event := range events {
		line, err := json.Marshal(auditLine{
			Stream:    account.HistoryAudit,
			ID:        event.ID,
			CreatedAt: event.CreatedAt.UTC().Format(time.RFC3339Nano),
			Username:  event.Username,
			Action:    event.Action,
			Detail:    event.Detail,
			ClientIP:  event.ClientIP,
			Location:  event.Location,
			Success:   event.Success,
		})
		if err != nil {
			return nil, err
		}
		records = append(records, record{id: event.ID, line: line})
	}
	return records, nil
}

func (a *Archiver) readConnections(afterID int64, limit int) ([]record, error) {
	events, err := a.store.ConnectionsAfter(afterID, limit)
	if err != nil {
		return nil, err
	}
	records := make([]record, 0, len(events))
	for _, event := range events {
		line, err := json.Marshal(connectionLine{
			Stream:     account.HistoryConnections,
			ID:         event.ID,
			StartedAt:  event.StartedAt.UTC().Format(time.RFC3339Nano),
			EndedAt:    event.EndedAt.UTC().Format(time.RFC3339Nano),
			DurationMS: event.Duration().Milliseconds(),
			Username:   event.Username,
			PeerID:     event.PeerID,
			HostID:     event.HostID,
			AuditRef:   event.AuditRef,
			Source:     event.Source,
			Direction:  event.Direction,
			ClientIP:   event.ClientIP,
			Location:   event.Location,
			Route:      event.Route,
			Success:    event.Success,
			Detail:     event.Detail,
		})
		if err != nil {
			return nil, err
		}
		records = append(records, record{id: event.ID, line: line})
	}
	return records, nil
}

func (a *Archiver) readFileTransfers(afterID int64, limit int) ([]record, error) {
	events, err := a.store.FileTransfersAfter(afterID, limit)
	if err != nil {
		return nil, err
	}
	records := make([]record, 0, len(events))
	for _, event := range events {
		line, err := json.Marshal(transferLine{
			Stream:     account.HistoryFileTransfers,
			ID:         event.ID,
			OccurredAt: event.OccurredAt.UTC().Format(time.RFC3339Nano),
			Username:   event.Username,
			PeerID:     event.PeerID,
			HostID:     event.HostID,
			AuditRef:   event.AuditRef,
			Source:     event.Source,
			Direction:  event.Direction,
			Name:       event.Name,
			Size:       event.Size,
			Files:      event.Files,
			ClientIP:   event.ClientIP,
			Location:   event.Location,
			Route:      event.Route,
			Success:    event.Success,
			Detail:     event.Detail,
		})
		if err != nil {
			return nil, err
		}
		records = append(records, record{id: event.ID, line: line})
	}
	return records, nil
}
