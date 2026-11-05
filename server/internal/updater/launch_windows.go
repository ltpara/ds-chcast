//go:build windows

package updater

import (
	"os"
	"os/exec"
	"syscall"
)

// Launch starts the staged installer and detaches it so the agent can exit or
// be terminated while the installer replaces the running binaries. The caller
// is responsible for verifying the digest first (see Verify and Fetch).
func Launch(path string) error {
	_, err := LaunchEnv(path, nil)
	return err
}

// LaunchEnv starts path with extra environment variables appended to the
// current process environment and returns its pid. The update helper uses it to
// pass the health marker path to the new release.
func LaunchEnv(path string, env []string) (int, error) {
	cmd := exec.Command(path)
	if len(env) > 0 {
		cmd.Env = append(os.Environ(), env...)
	}
	// DETACHED_PROCESS keeps the installer off the agent's console;
	// CREATE_NEW_PROCESS_GROUP lets it outlive a Ctrl+Break aimed at us.
	cmd.SysProcAttr = &syscall.SysProcAttr{
		CreationFlags: 0x00000008 | 0x00000200,
	}
	if err := cmd.Start(); err != nil {
		return 0, err
	}
	pid := cmd.Process.Pid
	return pid, cmd.Process.Release()
}
