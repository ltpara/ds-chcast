package logging

import (
	"bytes"
	"compress/gzip"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func rotated(t *testing.T, path string) []string {
	t.Helper()
	entries, err := os.ReadDir(filepath.Dir(path))
	if err != nil {
		t.Fatal(err)
	}
	base := filepath.Base(path) + "."
	var names []string
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), base) {
			names = append(names, entry.Name())
		}
	}
	return names
}

func TestOpenWithoutPathIsDisabled(t *testing.T) {
	writer, err := Open(Config{})
	if err != nil || writer != nil {
		t.Fatalf("Open(empty) = %v, %v", writer, err)
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("nil Close: %v", err)
	}
}

func TestOpenValidatesConfig(t *testing.T) {
	cases := []Config{
		{Path: "x.log", MaxSize: -1},
		{Path: "x.log", MaxBackups: -1},
		{Path: "x.log", MaxAge: -time.Second},
	}
	for _, cfg := range cases {
		if _, err := Open(cfg); err == nil {
			t.Fatalf("Open(%+v) accepted an invalid config", cfg)
		}
	}
}

func TestWriterRotatesAndPrunesByCount(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server.log")
	writer, err := Open(Config{Path: path, MaxSize: 100, MaxBackups: 2})
	if err != nil {
		t.Fatal(err)
	}
	defer writer.Close()
	line := bytes.Repeat([]byte("a"), 60)
	for i := 0; i < 10; i++ {
		if _, err := writer.Write(line); err != nil {
			t.Fatal(err)
		}
		time.Sleep(time.Millisecond)
	}
	if backups := rotated(t, path); len(backups) != 2 {
		t.Fatalf("backups = %v, want 2", backups)
	}
	active, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(active) != len(line) {
		t.Fatalf("active size = %d, want %d", len(active), len(line))
	}
}

func TestWriterCompressesRotatedFiles(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server.log")
	writer, err := Open(Config{Path: path, MaxSize: 32, MaxBackups: 5, Compress: true})
	if err != nil {
		t.Fatal(err)
	}
	first := bytes.Repeat([]byte("b"), 40)
	if _, err := writer.Write(first); err != nil {
		t.Fatal(err)
	}
	time.Sleep(time.Millisecond)
	if _, err := writer.Write([]byte("second\n")); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	backups := rotated(t, path)
	if len(backups) != 1 || !strings.HasSuffix(backups[0], ".gz") {
		t.Fatalf("backups = %v, want one gzip archive", backups)
	}
	archive, err := os.Open(filepath.Join(filepath.Dir(path), backups[0]))
	if err != nil {
		t.Fatal(err)
	}
	defer archive.Close()
	unzipped, err := gzip.NewReader(archive)
	if err != nil {
		t.Fatal(err)
	}
	data, err := io.ReadAll(unzipped)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data, first) {
		t.Fatalf("archive content = %q, want %q", data, first)
	}
}

func TestPruneByAgeAndCount(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "node.log")
	writer, err := Open(Config{Path: path, MaxSize: 100, MaxBackups: 3, MaxAge: time.Hour})
	if err != nil {
		t.Fatal(err)
	}
	defer writer.Close()
	now := time.Date(2026, 10, 27, 12, 0, 0, 0, time.UTC)
	for _, age := range []time.Duration{3 * time.Hour, 90 * time.Minute, 30 * time.Minute, 10 * time.Minute} {
		name := path + "." + now.Add(-age).Format(backupLayout)
		if err := os.WriteFile(name, []byte("old\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	writer.mu.Lock()
	writer.prune(now)
	writer.mu.Unlock()
	backups := rotated(t, path)
	if len(backups) != 2 {
		t.Fatalf("backups after prune = %v, want the two youngest", backups)
	}
}

func TestWriterIsSafeForConcurrentSlogWrites(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server.log")
	writer, err := Open(Config{Path: path, MaxSize: 512, MaxBackups: 100})
	if err != nil {
		t.Fatal(err)
	}
	logger := slog.New(slog.NewTextHandler(writer, nil))
	var group sync.WaitGroup
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			for j := 0; j < 40; j++ {
				logger.Info("concurrent write", "payload", strings.Repeat("x", 40))
			}
		}()
	}
	group.Wait()
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}

	lines := 0
	files := append([]string{path}, prefixFiles(t, path)...)
	for _, name := range files {
		data, err := os.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		lines += strings.Count(string(data), "concurrent write")
	}
	if lines != 8*40 {
		t.Fatalf("logged %d lines across %d files, want %d", lines, len(files), 8*40)
	}
	if len(files) < 2 {
		t.Fatal("the concurrency test never rotated")
	}
}

func prefixFiles(t *testing.T, path string) []string {
	t.Helper()
	names := rotated(t, path)
	files := make([]string, 0, len(names))
	for _, name := range names {
		files = append(files, filepath.Join(filepath.Dir(path), name))
	}
	return files
}
