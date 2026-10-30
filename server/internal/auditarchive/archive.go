// Package auditarchive copies the account history tables to append-only JSONL
// files so operators can keep audit evidence beyond the in-database row caps.
package auditarchive

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
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
}

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
}

// Result reports how many rows one pass copied and which closed files it
// compressed or removed.
type Result struct {
	Audit         int
	Connections   int
	FileTransfers int
	Files         []string
	Compressed    []string
	Pruned        []string
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
	return &Archiver{dir: abs, store: store, logger: logger, opts: opts, now: time.Now}, nil
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
	return result, nil
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
			if err := os.Remove(path); err != nil {
				a.logger.Warn("audit archive: cannot prune", "file", entry.Name(), "error", err)
				continue
			}
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
