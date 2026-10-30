//go:build !windows

package updater

import "errors"

// Launch is Windows-only: the host and viewer are Windows binaries and the
// installer is a Windows executable.
func Launch(string) error {
	return errors.New("automatic update is only supported on Windows")
}
