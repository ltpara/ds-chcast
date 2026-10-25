package relay

import (
	"os"
	"path/filepath"
	"testing"
)

func TestDefaultListFile(t *testing.T) {
	dir := t.TempDir()
	missing := filepath.Join(dir, "missing.txt")
	empty := filepath.Join(dir, "empty.txt")
	if err := os.WriteFile(empty, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	blacklist := filepath.Join(dir, "blacklist.txt")
	if err := os.WriteFile(blacklist, []byte("127.0.0.1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	blocklist := filepath.Join(dir, "blocklist.txt")
	if err := os.WriteFile(blocklist, []byte("127.0.0.2\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	dirCandidate := filepath.Join(dir, "blacklist-dir")
	if err := os.Mkdir(dirCandidate, 0o755); err != nil {
		t.Fatal(err)
	}

	if got := DefaultListFile("custom.txt", missing); got != "custom.txt" {
		t.Fatalf("explicit path = %q", got)
	}
	if got := DefaultListFile("", missing, dirCandidate); got != "" {
		t.Fatalf("missing candidates = %q", got)
	}
	if got := DefaultListFile("", dirCandidate, blacklist, blocklist); got != blacklist {
		t.Fatalf("first regular file = %q, want %q", got, blacklist)
	}
}

func TestManagerReadBuffer(t *testing.T) {
	m, err := NewManager("127.0.0.1", []int{0}, nil, testLog, WithReadBuffer(1<<20))
	if err != nil {
		t.Fatalf("manager: %v", err)
	}
	defer m.Close()
	size := m.ReadBuffer()
	if size <= 0 {
		t.Fatalf("effective read buffer = %d", size)
	}
	if stats := m.Stats(); stats.ReadBuffer != size {
		t.Fatalf("stats read buffer = %d, want %d", stats.ReadBuffer, size)
	}

	port := m.Ports()[0]
	if err := m.BindHost(port, "127.0.0.1"); err != nil {
		t.Fatalf("rebind: %v", err)
	}
	if got := m.ReadBuffer(); got <= 0 {
		t.Fatalf("read buffer after rebind = %d", got)
	}
}
