package setup

import (
	"archive/zip"
	"encoding/binary"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func makeZip(t *testing.T, path string, files map[string]string) {
	t.Helper()
	out, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(out)
	for name, body := range files {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write([]byte(body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := out.Close(); err != nil {
		t.Fatal(err)
	}
}

func openZip(t *testing.T, path string) *Payload {
	t.Helper()
	payload, err := OpenPayload(path)
	if err != nil {
		t.Fatalf("OpenPayload: %v", err)
	}
	t.Cleanup(func() { payload.Close() })
	return payload
}

func hasEntry(entries []string, name string) bool {
	for _, entry := range entries {
		if entry == name {
			return true
		}
	}
	return false
}

func TestOpenPayloadReadsPlainZip(t *testing.T) {
	dir := t.TempDir()
	archive := filepath.Join(dir, "release.zip")
	makeZip(t, archive, map[string]string{
		"chast_viewer.exe": "viewer",
		"data/app.so":      "engine",
	})
	payload := openZip(t, archive)
	if payload.Files != 2 || payload.Bytes != int64(len("viewer")+len("engine")) {
		t.Fatalf("files=%d bytes=%d", payload.Files, payload.Bytes)
	}
	entries := payload.Entries()
	if len(entries) != 2 || !hasEntry(entries, "chast_viewer.exe") || !hasEntry(entries, "data/app.so") {
		t.Fatalf("entries=%v", entries)
	}
}

func TestAppendPayloadRoundtrip(t *testing.T) {
	dir := t.TempDir()
	stub := filepath.Join(dir, "chast-setup.exe")
	if err := os.WriteFile(stub, []byte("stub-bytes"), 0o755); err != nil {
		t.Fatal(err)
	}
	archive := filepath.Join(dir, "release.zip")
	makeZip(t, archive, map[string]string{"a.txt": "alpha", "sub/b.txt": "beta"})
	packed := filepath.Join(dir, "setup-full.exe")
	if err := AppendPayload(stub, archive, packed); err != nil {
		t.Fatalf("AppendPayload: %v", err)
	}
	payload, err := OpenSelf(packed)
	if err != nil {
		t.Fatalf("OpenSelf: %v", err)
	}
	defer payload.Close()
	entries := payload.Entries()
	if len(entries) != 2 || !hasEntry(entries, "a.txt") || !hasEntry(entries, "sub/b.txt") {
		t.Fatalf("entries=%v", entries)
	}
	if payload.Bytes != 9 {
		t.Fatalf("bytes=%d", payload.Bytes)
	}
}

func TestOpenSelfRejectsMissingOrTruncatedTrailer(t *testing.T) {
	dir := t.TempDir()
	plain := filepath.Join(dir, "plain.exe")
	if err := os.WriteFile(plain, []byte("MZ not a payload"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := OpenSelf(plain); err == nil {
		t.Fatal("a stub without a payload was accepted")
	}

	stub := filepath.Join(dir, "stub.exe")
	if err := os.WriteFile(stub, []byte("stub"), 0o755); err != nil {
		t.Fatal(err)
	}
	archive := filepath.Join(dir, "release.zip")
	makeZip(t, archive, map[string]string{"a.txt": "alpha"})
	packed := filepath.Join(dir, "packed.exe")
	if err := AppendPayload(stub, archive, packed); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(packed)
	if err != nil {
		t.Fatal(err)
	}
	binary.LittleEndian.PutUint64(raw[len(raw)-8:], uint64(len(raw)))
	if err := os.WriteFile(packed, raw, 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := OpenSelf(packed); err == nil || !strings.Contains(err.Error(), "truncated") {
		t.Fatalf("truncated payload error = %v", err)
	}
}

func TestExtractWritesAndOverwrites(t *testing.T) {
	dir := t.TempDir()
	archive := filepath.Join(dir, "release.zip")
	makeZip(t, archive, map[string]string{"a.txt": "one", "sub/b.txt": "two"})
	root := filepath.Join(dir, "root")
	payload := openZip(t, archive)
	if err := extract(payload, root, func(string, ...any) {}); err != nil {
		t.Fatalf("extract: %v", err)
	}
	if body, err := os.ReadFile(filepath.Join(root, "sub", "b.txt")); err != nil || string(body) != "two" {
		t.Fatalf("b.txt = %q, %v", body, err)
	}

	second := filepath.Join(dir, "second.zip")
	makeZip(t, second, map[string]string{"a.txt": "updated"})
	payload.Close()
	payload = openZip(t, second)
	if err := extract(payload, root, func(string, ...any) {}); err != nil {
		t.Fatalf("extract again: %v", err)
	}
	if body, err := os.ReadFile(filepath.Join(root, "a.txt")); err != nil || string(body) != "updated" {
		t.Fatalf("a.txt = %q, %v", body, err)
	}
	if _, err := os.Stat(filepath.Join(root, "a.txt.new")); !os.IsNotExist(err) {
		t.Fatalf("staging file left behind: %v", err)
	}
}

func TestExtractRejectsEscapingEntries(t *testing.T) {
	dir := t.TempDir()
	archive := filepath.Join(dir, "evil.zip")
	makeZip(t, archive, map[string]string{"../escaped.txt": "no"})
	payload := openZip(t, archive)
	err := extract(payload, filepath.Join(dir, "root"), func(string, ...any) {})
	if err == nil || !strings.Contains(err.Error(), "escapes") {
		t.Fatalf("traversal error = %v", err)
	}
	if _, statErr := os.Stat(filepath.Join(dir, "escaped.txt")); !os.IsNotExist(statErr) {
		t.Fatalf("escaping entry was written: %v", statErr)
	}
}

func TestExtractRejectsSymlinks(t *testing.T) {
	dir := t.TempDir()
	archive := filepath.Join(dir, "link.zip")
	out, err := os.Create(archive)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(out)
	header := &zip.FileHeader{Name: "link"}
	header.SetMode(os.ModeSymlink | 0o777)
	w, err := zw.CreateHeader(header)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write([]byte("target")); err != nil {
		t.Fatal(err)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := out.Close(); err != nil {
		t.Fatal(err)
	}
	payload := openZip(t, archive)
	err = extract(payload, filepath.Join(dir, "root"), func(string, ...any) {})
	if err == nil || !strings.Contains(err.Error(), "is a link") {
		t.Fatalf("symlink error = %v", err)
	}
}

func TestSafeRoot(t *testing.T) {
	for _, bad := range []string{"", ".", "..", ".." + string(filepath.Separator) + "x"} {
		if _, err := safeRoot(bad); err == nil {
			t.Fatalf("safeRoot(%q) accepted", bad)
		}
	}
	if runtime.GOOS == "windows" {
		if _, err := safeRoot(`C:\`); err == nil {
			t.Fatal("safeRoot accepted a volume root")
		}
	}
	got, err := safeRoot(filepath.Join(t.TempDir(), "chast"))
	if err != nil || got == "" {
		t.Fatalf("safeRoot(root) = %q, %v", got, err)
	}
}

func TestAbsoluteRoot(t *testing.T) {
	if _, err := absoluteRoot("  "); err == nil {
		t.Fatal("an empty root was accepted")
	}
	got, err := absoluteRoot(filepath.Join("build", "chast"))
	if err != nil || !filepath.IsAbs(got) {
		t.Fatalf("relative root = %q, %v", got, err)
	}
	absolute := filepath.Join(t.TempDir(), "chast")
	if got, err := absoluteRoot(absolute); err != nil || got != filepath.Clean(absolute) {
		t.Fatalf("absolute root = %q, %v", got, err)
	}
}

func TestSnapshotServiceBinariesRestoresPreviousRelease(t *testing.T) {
	root := filepath.Join(t.TempDir(), "chast")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	write := func(name, body string) {
		t.Helper()
		if err := os.WriteFile(filepath.Join(root, name), []byte(body), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	write("chast-host.exe", "old host")
	write("chast-service.exe", "old service")
	write("chast_viewer.exe", "viewer")

	backup, err := snapshotServiceBinaries(root)
	if err != nil || backup == nil {
		t.Fatalf("snapshotServiceBinaries = %v, %v", backup, err)
	}
	if _, err := os.Stat(backup.dir); err != nil {
		t.Fatalf("backup directory: %v", err)
	}

	write("chast-host.exe", "new host")
	write("chast-service.exe", "new service")
	if err := backup.restore(); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if body, err := os.ReadFile(filepath.Join(root, "chast-host.exe")); err != nil || string(body) != "old host" {
		t.Fatalf("chast-host.exe = %q, %v", body, err)
	}
	if body, err := os.ReadFile(filepath.Join(root, "chast-service.exe")); err != nil || string(body) != "old service" {
		t.Fatalf("chast-service.exe = %q, %v", body, err)
	}
	if body, err := os.ReadFile(filepath.Join(root, "chast_viewer.exe")); err != nil || string(body) != "viewer" {
		t.Fatalf("chast_viewer.exe = %q, %v", body, err)
	}
	backup.discard()
	if _, err := os.Stat(backup.dir); !os.IsNotExist(err) {
		t.Fatalf("backup directory survived discard: %v", err)
	}
}

func TestSnapshotServiceBinariesWithoutInstalledTree(t *testing.T) {
	backup, err := snapshotServiceBinaries(filepath.Join(t.TempDir(), "missing"))
	if err != nil || backup != nil {
		t.Fatalf("missing tree = %v, %v", backup, err)
	}
}

func TestInstallRestoresBinariesWhenExtractionFails(t *testing.T) {
	dir := t.TempDir()
	root := filepath.Join(dir, "chast")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "chast-host.exe"), []byte("old host"), 0o755); err != nil {
		t.Fatal(err)
	}

	archive := filepath.Join(dir, "bad.zip")
	out, err := os.Create(archive)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(out)
	for _, entry := range []struct{ name, body string }{
		{"chast-host.exe", "new host"},
		{"../escaped.txt", "no"},
	} {
		w, err := zw.Create(entry.name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write([]byte(entry.body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := out.Close(); err != nil {
		t.Fatal(err)
	}

	payload := openZip(t, archive)
	if err := Install(payload, Options{Root: root, Quiet: true}); err == nil {
		t.Fatal("an escaping payload was installed")
	}
	if body, err := os.ReadFile(filepath.Join(root, "chast-host.exe")); err != nil || string(body) != "old host" {
		t.Fatalf("chast-host.exe = %q, %v", body, err)
	}
	if _, err := os.Stat(filepath.Join(dir, "escaped.txt")); !os.IsNotExist(err) {
		t.Fatalf("escaping entry was written: %v", err)
	}
}

func TestRemoveTreeDeletesRoot(t *testing.T) {
	root := filepath.Join(t.TempDir(), "chast")
	if err := os.MkdirAll(filepath.Join(root, "data"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "data", "x"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := RemoveTree(root); err != nil {
		t.Fatalf("RemoveTree: %v", err)
	}
	if _, err := os.Stat(root); !os.IsNotExist(err) {
		t.Fatalf("root still exists: %v", err)
	}
}
