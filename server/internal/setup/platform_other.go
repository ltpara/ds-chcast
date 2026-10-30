//go:build !windows

package setup

import (
	"errors"
	"os"
	"path/filepath"
)

// DefaultRoot returns the per-user or machine-wide install directory.
func DefaultRoot(machine bool) string {
	if machine {
		return "/opt/chast"
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".local", "share", "chast")
}

// IsElevated reports whether the current process holds an elevated token.
func IsElevated() bool {
	return os.Geteuid() == 0
}

func prepareInstall(opts Options) (func(), error) {
	return nil, nil
}

func platformInstall(opts Options, payloadBytes int64) error {
	return errors.New("the chast installer supports Windows only")
}

func platformUninstall(opts Options) error {
	return errors.New("the chast uninstaller supports Windows only")
}

func finishUninstall(root string) error {
	return RemoveTree(root)
}
