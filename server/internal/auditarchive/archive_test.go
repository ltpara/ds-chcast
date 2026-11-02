package auditarchive

import (
	"compress/gzip"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

func openStore(t *testing.T) *account.Store {
	t.Helper()
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	return store
}

func readLines(t *testing.T, path string) []map[string]any {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	out := make([]map[string]any, 0)
	for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if line == "" {
			continue
		}
		var record map[string]any
		if err := json.Unmarshal([]byte(line), &record); err != nil {
			t.Fatalf("parse %s line %q: %v", path, line, err)
		}
		out = append(out, record)
	}
	return out
}

func TestNewRejectsMissingInput(t *testing.T) {
	store := openStore(t)
	if _, err := New("  ", store, nil, Options{}); err == nil {
		t.Fatal("empty directory was accepted")
	}
	if _, err := New(filepath.Join(t.TempDir(), "archive"), nil, nil, Options{}); err == nil {
		t.Fatal("nil store was accepted")
	}
	if _, err := New(filepath.Join(t.TempDir(), "archive"), store, nil,
		Options{RetentionDays: -1}); err == nil {
		t.Fatal("negative retention was accepted")
	}
}

func TestRunWritesJSONLAndResumes(t *testing.T) {
	store := openStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	start := time.Date(2026, 10, 28, 9, 30, 0, 0, time.UTC)
	for _, detail := range []string{"first", "second"} {
		if err := store.AppendAudit(account.AuditEvent{
			Username: "alice", Action: account.AuditLogin, Detail: detail,
			ClientIP: "203.0.113.9", Location: "Testland", CreatedAt: start, Success: true,
		}); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	if _, err := store.RecordConnection(alice.ID, "alice", "devbox", "viewer",
		"198.51.100.4", "", "", "", start, start.Add(90*time.Second), true, "viewer side"); err != nil {
		t.Fatalf("record connection: %v", err)
	}
	if _, err := store.RecordFileTransfer(alice.ID, "alice", "devbox", "upload",
		"notes.txt", "198.51.100.4", "", "", "", 7, 1, start, true, "viewer file"); err != nil {
		t.Fatalf("record transfer: %v", err)
	}

	dir := filepath.Join(t.TempDir(), "archive")
	archiver, err := New(dir, store, nil, Options{})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return start }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.Audit != 2 || result.Connections != 1 || result.FileTransfers != 1 {
		t.Fatalf("first run = %+v", result)
	}
	if len(result.Files) != 3 {
		t.Fatalf("first run files = %v", result.Files)
	}
	auditPath := filepath.Join(dir, "audit-20261028.jsonl")
	connectionPath := filepath.Join(dir, "connections-20261028.jsonl")
	transferPath := filepath.Join(dir, "file-transfers-20261028.jsonl")
	auditLines := readLines(t, auditPath)
	if len(auditLines) != 2 {
		t.Fatalf("audit lines = %d, want 2", len(auditLines))
	}
	if auditLines[0]["stream"] != "audit" || auditLines[0]["detail"] != "first" ||
		auditLines[1]["detail"] != "second" {
		t.Fatalf("audit lines = %v", auditLines)
	}
	if got := auditLines[0]["created_at"]; got != "2026-10-28T09:30:00Z" {
		t.Fatalf("audit timestamp = %v", got)
	}
	connectionLines := readLines(t, connectionPath)
	if len(connectionLines) != 1 || connectionLines[0]["detail"] != "viewer side" ||
		connectionLines[0]["duration_ms"] != float64(90000) ||
		connectionLines[0]["stream"] != "connections" {
		t.Fatalf("connection lines = %v", connectionLines)
	}
	transferLines := readLines(t, transferPath)
	if len(transferLines) != 1 || transferLines[0]["name"] != "notes.txt" ||
		transferLines[0]["size"] != float64(7) ||
		transferLines[0]["stream"] != "file-transfers" {
		t.Fatalf("transfer lines = %v", transferLines)
	}

	checkpoints, err := archiver.Checkpoints()
	if err != nil {
		t.Fatalf("checkpoints: %v", err)
	}
	if checkpoints[account.HistoryAudit] != 2 ||
		checkpoints[account.HistoryConnections] != 1 ||
		checkpoints[account.HistoryFileTransfers] != 1 {
		t.Fatalf("checkpoints = %v", checkpoints)
	}

	// A second pass with no new rows must not touch the files.
	result, err = archiver.Run()
	if err != nil || result.Audit+result.Connections+result.FileTransfers != 0 ||
		len(result.Files) != 0 {
		t.Fatalf("idle run = %+v, %v", result, err)
	}
	if lines := readLines(t, auditPath); len(lines) != 2 {
		t.Fatalf("idle run duplicated rows: %d", len(lines))
	}

	// New rows append to the same day file instead of rewriting it.
	if err := store.AppendAudit(account.AuditEvent{
		Username: "alice", Action: account.AuditLogout, Detail: "third", CreatedAt: start, Success: true,
	}); err != nil {
		t.Fatalf("append logout: %v", err)
	}
	if _, err := store.RecordFileTransfer(alice.ID, "alice", "devbox", "download",
		"dump.bin", "198.51.100.4", "", "", "", 9, 1, start, true, "second file"); err != nil {
		t.Fatalf("record second transfer: %v", err)
	}
	result, err = archiver.Run()
	if err != nil || result.Audit != 1 || result.Connections != 0 || result.FileTransfers != 1 {
		t.Fatalf("incremental run = %+v, %v", result, err)
	}
	if lines := readLines(t, auditPath); len(lines) != 3 || lines[2]["detail"] != "third" {
		t.Fatalf("audit after append = %v", lines)
	}
	if lines := readLines(t, transferPath); len(lines) != 2 || lines[1]["detail"] != "second file" {
		t.Fatalf("transfers after append = %v", lines)
	}

	// The next UTC day starts a new checkpoint but a backdated row still
	// belongs to its own (now closed) event day, so it is merged there.
	archiver.now = func() time.Time { return start.Add(24 * time.Hour) }
	if err := store.AppendAudit(account.AuditEvent{
		Username: "alice", Action: account.AuditLogin, Detail: "next day", CreatedAt: start, Success: true,
	}); err != nil {
		t.Fatalf("append next day: %v", err)
	}
	result, err = archiver.Run()
	if err != nil {
		t.Fatalf("next day run: %v", err)
	}
	if result.Merged != 1 {
		t.Fatalf("next day merged = %d, want 1", result.Merged)
	}
	if lines := readLines(t, auditPath); len(lines) != 4 || lines[3]["detail"] != "next day" {
		t.Fatalf("late row was not merged into its event day: %v", lines)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261029.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("a late row created today's file instead (err=%v)", err)
	}
}

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

func readGzipText(t *testing.T, path string) string {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("open %s: %v", path, err)
	}
	defer file.Close()
	reader, err := gzip.NewReader(file)
	if err != nil {
		t.Fatalf("gzip %s: %v", path, err)
	}
	defer reader.Close()
	data, err := io.ReadAll(reader)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	return string(data)
}

func TestRunCompressesClosedDays(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	const auditBody = "{\"stream\":\"audit\",\"id\":1}\n{\"stream\":\"audit\",\"id\":2}\n"
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl"), auditBody)
	writeFile(t, filepath.Join(dir, "connections-20261028.jsonl"), "{\"stream\":\"connections\"}\n")
	writeFile(t, filepath.Join(dir, "audit-20261029.jsonl"), "{\"stream\":\"audit\",\"id\":3}\n")
	writeFile(t, filepath.Join(dir, "notes.txt"), "keep\n")
	// A gzip interrupted before its rename leaves this half-written temp; the
	// next compression rebuilds it from the source and renames it into place.
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl.gz.tmp"), "half a gzip")
	writeFile(t, filepath.Join(dir, "audit-2026.jsonl"), "bad day\n")

	archiver, err := New(dir, store, nil, Options{Compress: true})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time {
		return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC)
	}
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if len(result.Compressed) != 2 {
		t.Fatalf("compressed = %v, want two closed files", result.Compressed)
	}
	if got := readGzipText(t, filepath.Join(dir, "audit-20261028.jsonl.gz")); got != auditBody {
		t.Fatalf("audit gzip = %q", got)
	}
	if got := readGzipText(t, filepath.Join(dir, "connections-20261028.jsonl.gz")); got != "{\"stream\":\"connections\"}\n" {
		t.Fatalf("connections gzip = %q", got)
	}
	for _, gone := range []string{"audit-20261028.jsonl", "connections-20261028.jsonl"} {
		if _, err := os.Stat(filepath.Join(dir, gone)); !os.IsNotExist(err) {
			t.Fatalf("%s still exists (err=%v)", gone, err)
		}
	}
	for _, kept := range []string{"audit-20261029.jsonl", "notes.txt", "audit-2026.jsonl"} {
		if _, err := os.Stat(filepath.Join(dir, kept)); err != nil {
			t.Fatalf("%s was touched: %v", kept, err)
		}
	}
	if data, err := os.ReadFile(filepath.Join(dir, "audit-20261029.jsonl")); err != nil ||
		string(data) != "{\"stream\":\"audit\",\"id\":3}\n" {
		t.Fatalf("today's file changed: %q, %v", data, err)
	}
	result, err = archiver.Run()
	if err != nil || len(result.Compressed)+len(result.Pruned) != 0 {
		t.Fatalf("second run compacted again: %+v, %v", result, err)
	}
}

func TestRunPrunesExpiredArchives(t *testing.T) {
	store := openStore(t)
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "audit-20261001.jsonl.gz"), "old gzip")
	writeFile(t, filepath.Join(dir, "connections-20261020.jsonl"), "{\"old\":true}\n")
	writeFile(t, filepath.Join(dir, "file-transfers-20261022.jsonl.gz"), "boundary")
	writeFile(t, filepath.Join(dir, "audit-20261028.jsonl.gz"), "recent")
	writeFile(t, filepath.Join(dir, "audit-20261029.jsonl"), "{\"today\":true}\n")
	writeFile(t, filepath.Join(dir, "audit-20261001.log"), "foreign\n")

	archiver, err := New(dir, store, nil, Options{RetentionDays: 7})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time {
		return time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC)
	}
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	pruned := make(map[string]bool, len(result.Pruned))
	for _, name := range result.Pruned {
		pruned[name] = true
	}
	if len(pruned) != 2 || !pruned["audit-20261001.jsonl.gz"] ||
		!pruned["connections-20261020.jsonl"] {
		t.Fatalf("pruned = %v", result.Pruned)
	}
	if len(result.Compressed) != 0 {
		t.Fatalf("compression ran while disabled: %v", result.Compressed)
	}
	for _, kept := range []string{
		"file-transfers-20261022.jsonl.gz", "audit-20261028.jsonl.gz",
		"audit-20261029.jsonl", "audit-20261001.log",
	} {
		if _, err := os.Stat(filepath.Join(dir, kept)); err != nil {
			t.Fatalf("%s was pruned: %v", kept, err)
		}
	}
}

func TestRunMergesLateRowsIntoEventDay(t *testing.T) {
	store := openStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	day28 := time.Date(2026, 10, 28, 9, 0, 0, 0, time.UTC)
	day29 := day28.Add(24 * time.Hour)
	for _, event := range []account.AuditEvent{
		{Action: account.AuditLogin, Detail: "late-a", CreatedAt: day28.Add(time.Hour)},
		{Action: account.AuditLogin, Detail: "on-29", CreatedAt: day29.Add(time.Hour)},
		{Action: account.AuditLogin, Detail: "late-c", CreatedAt: day28.Add(2 * time.Hour)},
	} {
		event.Username = "alice"
		event.Success = true
		if err := store.AppendAudit(event); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	// The session started on the 27th, so that is its archive day.
	if _, err := store.RecordConnection(alice.ID, "alice", "devbox", "viewer",
		"198.51.100.4", "", "", "", day28.Add(-24*time.Hour), day28.Add(time.Hour),
		true, "late connection"); err != nil {
		t.Fatalf("record connection: %v", err)
	}
	if _, err := store.RecordFileTransfer(alice.ID, "alice", "devbox", "upload",
		"late.txt", "198.51.100.4", "", "", "", 3, 1, day28.Add(30*time.Minute),
		true, "late file"); err != nil {
		t.Fatalf("record transfer: %v", err)
	}

	dir := t.TempDir()
	archiver, err := New(dir, store, nil, Options{})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return time.Date(2026, 10, 30, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.Audit != 3 || result.Connections != 1 || result.FileTransfers != 1 {
		t.Fatalf("run = %+v", result)
	}
	if result.Merged != 5 {
		t.Fatalf("merged = %d, want 5", result.Merged)
	}
	if len(result.Files) != 4 {
		t.Fatalf("files = %v, want four day files", result.Files)
	}
	audit28 := readLines(t, filepath.Join(dir, "audit-20261028.jsonl"))
	if len(audit28) != 2 || audit28[0]["detail"] != "late-a" || audit28[1]["detail"] != "late-c" {
		t.Fatalf("late audit lines = %v", audit28)
	}
	audit29 := readLines(t, filepath.Join(dir, "audit-20261029.jsonl"))
	if len(audit29) != 1 || audit29[0]["detail"] != "on-29" {
		t.Fatalf("next-day audit lines = %v", audit29)
	}
	connections := readLines(t, filepath.Join(dir, "connections-20261027.jsonl"))
	if len(connections) != 1 || connections[0]["detail"] != "late connection" {
		t.Fatalf("late connection lines = %v", connections)
	}
	transfers := readLines(t, filepath.Join(dir, "file-transfers-20261028.jsonl"))
	if len(transfers) != 1 || transfers[0]["detail"] != "late file" {
		t.Fatalf("late transfer lines = %v", transfers)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261030.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("a closed-day row created today's file (err=%v)", err)
	}
}

func TestRunMergesLateRowsIntoCompressedDay(t *testing.T) {
	store := openStore(t)
	if err := store.AppendAudit(account.AuditEvent{
		Username: "alice", Action: account.AuditLogin, Detail: "late",
		CreatedAt: time.Date(2026, 10, 28, 10, 0, 0, 0, time.UTC), Success: true,
	}); err != nil {
		t.Fatalf("append audit: %v", err)
	}
	dir := t.TempDir()
	plain := filepath.Join(dir, "audit-20261028.jsonl")
	const existing = "{\"stream\":\"audit\",\"id\":1}\n"
	writeFile(t, plain, existing)
	if err := compressJSONL(plain); err != nil {
		t.Fatalf("compress: %v", err)
	}
	gz := plain + ".gz"
	if err := writeDeliveredMarker(gz, deliveredMarker{
		File: "audit-20261028.jsonl.gz", SHA256: "old", DeliveredAt: "2026-10-29T00:00:00Z",
		URL: "https://sink.example/audit-20261028.jsonl.gz",
	}); err != nil {
		t.Fatalf("write marker: %v", err)
	}

	archiver, err := New(dir, store, nil, Options{Compress: true})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return time.Date(2026, 10, 30, 8, 0, 0, 0, time.UTC) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if result.Merged != 1 {
		t.Fatalf("merged = %d, want 1", result.Merged)
	}
	got := readGzipText(t, gz)
	if !strings.HasPrefix(got, existing) || !strings.Contains(got, `"detail":"late"`) {
		t.Fatalf("merged gzip = %q", got)
	}
	if deliveredMarkerExists(gz) {
		t.Fatal("stale delivered marker survived the merge")
	}
	if _, err := os.Stat(plain); !os.IsNotExist(err) {
		t.Fatalf("merge left an uncompressed file (err=%v)", err)
	}
}
