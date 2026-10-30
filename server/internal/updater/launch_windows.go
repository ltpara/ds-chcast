//go:build windows

package updater

import (
	"os/exec"
	"syscall"
)

// Launch starts the staged installer and detaches it so the agent can exit or
// be terminated while the installer replaces the running binaries. The caller
// is responsible for verifying the digest first (see Verify and Fetch).
func Launch(path string) error {
	cmd := exec.Command(path)
	// DETACHED_PROCESS keeps the installer off the agent's console;
	// CREATE_NEW_PROCESS_GROUP lets it outlive a Ctrl+Break aimed at us.
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: 0x00000008 | 0x00000200,
	}
	if err := cmd.Start(); err != nil {
		return err
	}
	return cmd.Process.Release()
}
