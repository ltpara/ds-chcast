package updater

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func preparePending(t *testing.T, timeout time.Duration) (*PendingUpdate, string, string) {
	t.Helper()
	dir := t.TempDir()
	target := filepath.Join(dir, "chast_viewer.exe")
	backup := target + ".old-123"
	if err := os.WriteFile(target, []byte("new image"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(backup, []byte("old image"), 0o644); err != nil {
		t.Fatal(err)
	}
	pending, err := Begin(target, backup, "2.1.0", timeout)
	if err != nil {
		t.Fatal(err)
	}
	return pending, target, backup
}

func TestPendingUpdateLifecycle(t *testing.T) {
	_, target, backup := preparePending(t, time.Minute)
	loaded, err := ReadPending(target)
	if err != nil {
		t.Fatalf("ReadPending: %v", err)
	}
	if loaded.Version != "2.1.0" || loaded.Nonce == "" || loaded.Backup != backup {
		t.Fatalf("pending = %+v", loaded)
	}
	if loaded.Healthy() {
		t.Fatal("a fresh update is already healthy")
	}
	confirmed, err := Confirm(target)
	if err != nil {
		t.Fatalf("Confirm: %v", err)
	}
	if !confirmed {
		t.Fatal("Confirm did not find the pending update")
	}
	if !loaded.Healthy() {
		t.Fatal("health marker was not accepted")
	}
	if err := loaded.AwaitHealth(make(chan struct{}), 5*time.Millisecond); err != nil {
		t.Fatalf("AwaitHealth with a marker: %v", err)
	}
	if err := loaded.Rollback(); err != nil {
		t.Fatalf("Rollback: %v", err)
	}
	raw, err := os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != "old image" {
		t.Fatalf("target holds %q after rollback", raw)
	}
	if _, err := os.Stat(backup); !os.IsNotExist(err) {
		t.Fatalf("backup still exists after rollback: %v", err)
	}
	loaded.Cancel()
	if _, err := os.Stat(StatePath(target)); !os.IsNotExist(err) {
		t.Fatal("Cancel left the state file")
	}
	if _, err := os.Stat(HealthPath(target)); !os.IsNotExist(err) {
		t.Fatal("Cancel left the health marker")
	}
	confirmed, err = Confirm(target)
	if err != nil || confirmed {
		t.Fatalf("Confirm without a pending update = %v, %v", confirmed, err)
	}
}

func TestPendingUpdateRejectsUnsafeBackup(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "chast_viewer.exe")
	if err := os.WriteFile(target, []byte("new"), 0o644); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name   string
		backup string
	}{
		{"empty", ""},
		{"other directory", filepath.Join(t.TempDir(), "chast_viewer.exe.old-1")},
		{"wrong name", filepath.Join(dir, "unrelated.exe")},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := Begin(target, tc.backup, "2.1.0", time.Minute); err == nil {
				t.Fatal("Begin accepted an unsafe backup")
			}
		})
	}
	tampered := &PendingUpdate{
		Version: "2.1.0",
		Target:  target,
		Backup:  filepath.Join(t.TempDir(), "chast_viewer.exe.old-1"),
		Nonce:   "00112233445566778899aabbccddeeff",
	}
	if err := tampered.Rollback(); err == nil {
		t.Fatal("Rollback accepted a backup outside the target directory")
	}
}

func TestAwaitHealthFailures(t *testing.T) {
	unhealthy, _, _ := preparePending(t, 50*time.Millisecond)
	err := unhealthy.AwaitHealth(make(chan struct{}), 5*time.Millisecond)
	if !errors.Is(err, ErrUpdateUnhealthy) {
		t.Fatalf("AwaitHealth timeout = %v", err)
	}

	exited, target, _ := preparePending(t, time.Minute)
	closed := make(chan struct{})
	close(closed)
	err = exited.AwaitHealth(closed, 5*time.Millisecond)
	if !errors.Is(err, ErrUpdateExited) {
		t.Fatalf("AwaitHealth early exit = %v", err)
	}

	// A marker with another nonce belongs to a different update.
	if err := os.WriteFile(HealthPath(target), []byte(`{"nonce":"deadbeef"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if exited.Healthy() {
		t.Fatal("a marker from another update was accepted")
	}
}

func TestConfirmRejectsMismatchedState(t *testing.T) {
	_, target, _ := preparePending(t, time.Minute)
	other := filepath.Join(t.TempDir(), "other.exe")
	if err := os.WriteFile(other, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(StatePath(target))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(StatePath(other), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Confirm(other); err == nil || !strings.Contains(err.Error(), "names target") {
		t.Fatalf("Confirm with a copied state = %v", err)
	}
}

func TestReadPendingRejectsOversizedState(t *testing.T) {
	dir := t.TempDir()
	target := filepath.Join(dir, "chast_viewer.exe")
	if err := os.WriteFile(StatePath(target), make([]byte, maxStateBytes+1), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := ReadPending(target); err == nil || !strings.Contains(err.Error(), "exceeds") {
		t.Fatalf("ReadPending oversize = %v", err)
	}
}
