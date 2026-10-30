package updater

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestInstallSwapsAndRestores(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "chast_viewer.exe")
	staged := filepath.Join(dir, "chast-2.2.0.exe")
	if err := os.WriteFile(target, []byte("old viewer"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(staged, []byte("new viewer"), 0o755); err != nil {
		t.Fatal(err)
	}
	backup, err := Install(staged, target)
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	if !strings.HasPrefix(backup, target+".old-") {
		t.Fatalf("backup %q is not next to the target", backup)
	}
	raw, err := os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(raw, []byte("new viewer")) {
		t.Fatalf("target holds %q, want the staged release", raw)
	}
	if _, err := os.Stat(staged); !os.IsNotExist(err) {
		t.Fatalf("staged file still exists after the swap")
	}
	old, err := os.ReadFile(backup)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(old, []byte("old viewer")) {
		t.Fatalf("backup holds %q, want the previous image", old)
	}
	if err := Restore(target, backup); err != nil {
		t.Fatalf("Restore: %v", err)
	}
	raw, err = os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(raw, []byte("old viewer")) {
		t.Fatalf("restore left %q in place", raw)
	}
}

func TestInstallWithoutExistingTarget(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "chast_viewer.exe")
	staged := filepath.Join(dir, "chast-2.2.0.exe")
	if err := os.WriteFile(staged, []byte("first install"), 0o755); err != nil {
		t.Fatal(err)
	}
	backup, err := Install(staged, target)
	if err != nil {
		t.Fatalf("Install: %v", err)
	}
	if backup != "" {
		t.Fatalf("backup %q exists without a previous image", backup)
	}
	raw, err := os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(raw, []byte("first install")) {
		t.Fatalf("target holds %q", raw)
	}
}

func TestInstallRejectsMissingOrSameFile(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "chast_viewer.exe")
	if _, err := Install(filepath.Join(dir, "missing.exe"), target); err == nil {
		t.Fatal("Install accepted a missing staged file")
	}
	if err := os.WriteFile(target, []byte("viewer"), 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := Install(target, target); err == nil || !strings.Contains(err.Error(), "same file") {
		t.Fatalf("Install on the same file = %v", err)
	}
}

func TestRetireRemovesBackup(t *testing.T) {
	backup := filepath.Join(t.TempDir(), "chast_viewer.exe.old-1")
	if err := os.WriteFile(backup, []byte("old"), 0o755); err != nil {
		t.Fatal(err)
	}
	Retire(backup, 50*time.Millisecond)
	if _, err := os.Stat(backup); !os.IsNotExist(err) {
		t.Fatalf("Retire left %s in place", backup)
	}
}

func TestWaitForPIDHandlesGoneProcesses(t *testing.T) {
	if err := WaitForPID(0, time.Second); err != nil {
		t.Fatalf("WaitForPID(0): %v", err)
	}
	if err := WaitForPID(1<<30, time.Second); err != nil {
		t.Fatalf("WaitForPID on a missing pid: %v", err)
	}
	if runtime.GOOS != "windows" {
		return
	}
	if err := WaitForPID(os.Getpid(), 200*time.Millisecond); err == nil {
		t.Fatal("WaitForPID returned before the process exited")
	}
}
