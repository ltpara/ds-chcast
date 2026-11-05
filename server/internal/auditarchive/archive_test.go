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
		"198.51.100.4", "Testland", "punch", "",
		start, start.Add(90*time.Second), true, "viewer side"); err != nil {
		t.Fatalf("record connection: %v", err)
	}
	if _, err := store.RecordFileTransfer(alice.ID, "alice", "devbox", "upload",
		"notes.txt", "198.51.100.4", "Testland", "punch", "",
		7, 1, start, true, "viewer file"); err != nil {
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
		"dump.bin", "198.51.100.4", "Testland", "punch", "",
		9, 1, start, true, "second file"); err != nil {
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

	// A row dated on an earlier day is merged into that closed file instead of
	// today's target, and the file it replaces is reported as merged.
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
	if len(result.Files) != 0 || len(result.Merged) != 1 || result.Merged[0] != auditPath {
		t.Fatalf("late run files=%v merged=%v", result.Files, result.Merged)
	}
	if _, err := os.Stat(filepath.Join(dir, "audit-20261029.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("a late row created today's file (err=%v)", err)
	}
	lines := readLines(t, auditPath)
	if len(lines) != 4 || lines[3]["detail"] != "next day" {
		t.Fatalf("late merge = %v", lines)
	}
}

func TestRunMergesLateHostAuditIntoItsOwnDay(t *testing.T) {
	store := openStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	day28 := time.Date(2026, 10, 28, 9, 30, 0, 0, time.UTC)
	if _, err := store.RecordConnection(alice.ID, "alice", "devbox", "viewer",
		"198.51.100.4", "Testland", "punch", "abcd1234",
		day28, day28.Add(time.Minute), true, "viewer side"); err != nil {
		t.Fatalf("record viewer connection: %v", err)
	}

	dir := filepath.Join(t.TempDir(), "archive")
	archiver, err := New(dir, store, nil, Options{})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	archiver.now = func() time.Time { return day28 }
	if _, err := archiver.Run(); err != nil {
		t.Fatalf("first run: %v", err)
	}

	// A host that was offline spools its reports and forwards them a day late.
	// The session ended after midnight, the file finished two days earlier.
	if _, err := store.RecordHostConnection("host-a", "",
		time.Date(2026, 10, 27, 23, 50, 0, 0, time.UTC),
		time.Date(2026, 10, 28, 0, 10, 0, 0, time.UTC), true, "late session"); err != nil {
		t.Fatalf("record late connection: %v", err)
	}
	if _, err := store.RecordHostFileTransfer("host-a", "", "upload", "late.txt", 5,
		time.Date(2026, 10, 26, 23, 30, 0, 0, time.UTC), true, "late file"); err != nil {
		t.Fatalf("record late transfer: %v", err)
	}
	archiver.now = func() time.Time { return day28.Add(24 * time.Hour) }
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("late run: %v", err)
	}
	if result.Connections != 1 || result.FileTransfers != 1 || len(result.Files) != 0 {
		t.Fatalf("late run = %+v", result)
	}
	if len(result.Merged) != 2 {
		t.Fatalf("merged = %v", result.Merged)
	}

	connectionLines := readLines(t, filepath.Join(dir, "connections-20261028.jsonl"))
	if len(connectionLines) != 2 || connectionLines[1]["detail"] != "late session" ||
		connectionLines[1]["direction"] != "host" {
		t.Fatalf("connection day file = %v", connectionLines)
	}
	transferLines := readLines(t, filepath.Join(dir, "file-transfers-20261026.jsonl"))
	if len(transferLines) != 1 || transferLines[0]["name"] != "late.txt" {
		t.Fatalf("transfer day file = %v", transferLines)
	}
	if _, err := os.Stat(filepath.Join(dir, "connections-20261029.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("a late session created today's file (err=%v)", err)
	}
}

func TestMergeClosedDayRewritesGzipAndSkipsReplayedRows(t *testing.T) {
	store := openStore(t)
	dir := filepath.Join(t.TempDir(), "archive")
	archiver, err := New(dir, store, nil, Options{})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	gzPath := filepath.Join(dir, "connections-20261028.jsonl.gz")
	if err := writeGzipAtomic(gzPath, []byte("{\"stream\":\"connections\",\"id\":7}\n")); err != nil {
		t.Fatalf("seed gzip: %v", err)
	}
	// A marker from the previous delivery must not survive a content change.
	writeFile(t, gzPath+deliveredSuffix, `{"file":"connections-20261028.jsonl.gz"}`)

	records := []record{
		{id: 7, line: []byte(`{"stream":"connections","id":7}`)},
		{id: 8, line: []byte(`{"stream":"connections","id":8}`)},
	}
	target, err := archiver.mergeClosedDay("connections", "20261028", records)
	if err != nil || target != gzPath {
		t.Fatalf("merge = %q, %v", target, err)
	}
	// Replaying the same page after a crash before the checkpoint must not
	// duplicate lines.
	if _, err := archiver.mergeClosedDay("connections", "20261028", records); err != nil {
		t.Fatalf("replayed merge: %v", err)
	}
	body := readGzipText(t, gzPath)
	if want := "{\"stream\":\"connections\",\"id\":7}\n{\"stream\":\"connections\",\"id\":8}\n"; body != want {
		t.Fatalf("merged gzip = %q, want %q", body, want)
	}
	if _, err := os.Stat(gzPath + deliveredSuffix); !os.IsNotExist(err) {
		t.Fatalf("a rewritten file kept its delivered marker (err=%v)", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "connections-20261028.jsonl")); !os.IsNotExist(err) {
		t.Fatalf("a plain file was left behind (err=%v)", err)
	}
}

func TestLateRecordsClampToRetentionAndToday(t *testing.T) {
	store := openStore(t)
	dir := filepath.Join(t.TempDir(), "archive")
	archiver, err := New(dir, store, nil, Options{RetentionDays: 3})
	if err != nil {
		t.Fatalf("new archiver: %v", err)
	}
	now := time.Date(2026, 10, 29, 8, 0, 0, 0, time.UTC)
	archiver.now = func() time.Time { return now }
	if err := store.AppendAudit(account.AuditEvent{
		Username: "alice", Action: account.AuditLogin, Detail: "too old",
		CreatedAt: time.Date(2026, 10, 1, 12, 0, 0, 0, time.UTC), Success: true,
	}); err != nil {
		t.Fatalf("append old audit: %v", err)
	}
	if err := store.AppendAudit(account.AuditEvent{
		Username: "alice", Action: account.AuditLogin, Detail: "from the future",
		CreatedAt: now.Add(48 * time.Hour), Success: true,
	}); err != nil {
		t.Fatalf("append future audit: %v", err)
	}
	result, err := archiver.Run()
	if err != nil {
		t.Fatalf("run: %v", err)
	}
	if len(result.Merged) != 1 {
		t.Fatalf("merged = %v", result.Merged)
	}
	old := readLines(t, filepath.Join(dir, "audit-20261026.jsonl"))
	if len(old) != 1 || old[0]["detail"] != "too old" {
		t.Fatalf("clamped file = %v", old)
	}
	for _, absent := range []string{"audit-20261001.jsonl", "audit-20261031.jsonl"} {
		if _, err := os.Stat(filepath.Join(dir, absent)); !os.IsNotExist(err) {
			t.Fatalf("%s was created (err=%v)", absent, err)
		}
	}
	today := readLines(t, filepath.Join(dir, "audit-20261029.jsonl"))
	if len(today) != 1 || today[0]["detail"] != "from the future" {
		t.Fatalf("today file = %v", today)
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
