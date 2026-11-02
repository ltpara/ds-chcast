//go:build windows

package updater

import (
	"os"
	"os/exec"
	"strconv"
	"syscall"
)

func startDetached(path string) (*exec.Cmd, error) {
	cmd := exec.Command(path)
	// DETACHED_PROCESS keeps the installer off the agent's console;
	// CREATE_NEW_PROCESS_GROUP lets it outlive a Ctrl+Break aimed at us.
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: 0x00000008 | 0x00000200,
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return cmd, nil
}

// Launch starts the staged installer and detaches it so the agent can exit or
// be terminated while the installer replaces the running binaries. The caller
// is responsible for verifying the digest first (see Verify and Fetch).
func Launch(path string) error {
	cmd, err := startDetached(path)
	if err != nil {
		return err
	}
	return cmd.Process.Release()
}

// Start begins path detached and returns a handle that can be watched for an
// early exit or stopped during a rollback.
func Start(path string) (*Running, error) {
	cmd, err := startDetached(path)
	if err != nil {
		return nil, err
	}
	return newRunning(cmd), nil
}

// Terminate stops a process and its children. A process that has already
// exited is reported by taskkill; the fallback kill handles that case.
func Terminate(pid int) error {
	if pid <= 0 {
		return nil
	}
	if err := exec.Command("taskkill", "/PID", strconv.Itoa(pid), "/T", "/F").Run(); err == nil {
		return nil
	}
	process, err := os.FindProcess(pid)
	if err != nil {
		return err
	}
	return process.Kill()
}
