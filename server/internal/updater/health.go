package updater

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// HealthEnv names the environment variable that tells a freshly updated
// process where to write its startup health marker. The update helper sets it
// for the process it launches and only retires the previous image after the
// marker appears.
const HealthEnv = "CHAST_UPDATE_HEALTH_FILE"

// HealthContent is what a healthy process writes into the marker.
const HealthContent = "healthy\n"

// WriteHealthMarker publishes the startup health marker. It is called by the
// updated program once its window is up; a program that crashes during startup
// never writes it, which is what lets the helper roll back.
func WriteHealthMarker(path string) error {
	path = strings.TrimSpace(path)
	if path == "" {
		return errors.New("health marker path is empty")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(HealthContent), 0o644)
}

// WaitForHealth polls for the marker while the launched process is alive. It
// returns healthy=true as soon as the marker exists. When the process exits
// before writing the marker it returns (false, false); when the timeout
// expires with the process still running it returns (false, true), so the
// caller can leave a slow but live program alone instead of killing it.
func WaitForHealth(marker string, pid int, timeout, poll time.Duration) (healthy, alive bool) {
	if timeout <= 0 {
		timeout = 60 * time.Second
	}
	if poll <= 0 {
		poll = 200 * time.Millisecond
	}
	deadline := time.Now().Add(timeout)
	for {
		if info, err := os.Stat(marker); err == nil && info.Mode().IsRegular() {
			return true, processAlive(pid)
		}
		alive = processAlive(pid)
		if !alive {
			return false, false
		}
		if !time.Now().Before(deadline) {
			return false, true
		}
		time.Sleep(poll)
	}
}
