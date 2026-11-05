package updater

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestHealthMarkerAndWait(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "nested", "health")
	if healthy, _ := WaitForHealth(marker, os.Getpid(), 50*time.Millisecond, 10*time.Millisecond); healthy {
		t.Fatal("WaitForHealth reported a marker that does not exist")
	}
	if err := WriteHealthMarker(marker); err != nil {
		t.Fatalf("WriteHealthMarker: %v", err)
	}
	raw, err := os.ReadFile(marker)
	if err != nil {
		t.Fatal(err)
	}
	if string(raw) != HealthContent {
		t.Fatalf("marker holds %q, want %q", raw, HealthContent)
	}
	healthy, alive := WaitForHealth(marker, os.Getpid(), time.Second, 10*time.Millisecond)
	if !healthy || !alive {
		t.Fatalf("WaitForHealth = (%v, %v), want (true, true)", healthy, alive)
	}
}

func TestWaitForHealthSeesEarlyExit(t *testing.T) {
	marker := filepath.Join(t.TempDir(), "health")
	healthy, alive := WaitForHealth(marker, 1<<30, 50*time.Millisecond, 10*time.Millisecond)
	if healthy || alive {
		t.Fatalf("WaitForHealth on a missing pid = (%v, %v), want (false, false)", healthy, alive)
	}
}

func TestWaitForHealthToleratesSlowStart(t *testing.T) {
	marker := filepath.Join(t.TempDir(), "health")
	go func() {
		time.Sleep(150 * time.Millisecond)
		_ = WriteHealthMarker(marker)
	}()
	healthy, alive := WaitForHealth(marker, os.Getpid(), 5*time.Second, 20*time.Millisecond)
	if !healthy || !alive {
		t.Fatalf("WaitForHealth = (%v, %v), want (true, true)", healthy, alive)
	}
}

func TestWaitForHealthLeavesLiveSlowProcessAlone(t *testing.T) {
	marker := filepath.Join(t.TempDir(), "health")
	healthy, alive := WaitForHealth(marker, os.Getpid(), 100*time.Millisecond, 10*time.Millisecond)
	if healthy || !alive {
		t.Fatalf("WaitForHealth = (%v, %v), want (false, true)", healthy, alive)
	}
}

// TestWaitForHealthNoticesExit runs a short-lived child that never writes the
// marker. The watchdog must report the exit rather than waiting for the full
// timeout.
func TestWaitForHealthNoticesExit(t *testing.T) {
	if _, err := exec.LookPath("cmd"); err != nil {
		t.Skip("cmd.exe is unavailable")
	}
	marker := filepath.Join(t.TempDir(), "health")
	cmd := exec.Command("cmd", "/c", "exit", "0")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	pid := cmd.Process.Pid
	if err := cmd.Wait(); err != nil {
		t.Fatalf("child: %v", err)
	}
	started := time.Now()
	healthy, alive := WaitForHealth(marker, pid, 5*time.Second, 20*time.Millisecond)
	if healthy || alive {
		t.Fatalf("WaitForHealth = (%v, %v), want (false, false)", healthy, alive)
	}
	if time.Since(started) > 2*time.Second {
		t.Fatal("WaitForHealth waited for the timeout after the child exited")
	}
}
