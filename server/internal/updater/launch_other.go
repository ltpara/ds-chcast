//go:build !windows

package updater

import "errors"

// Launch is Windows-only: the host and viewer are Windows binaries and the
// installer is a Windows executable.
func Launch(string) error {
	_, err := LaunchEnv("", nil)
	return err
}

func LaunchEnv(string, []string) (int, error) {
	return 0, errors.New("automatic update is only supported on Windows")
}
