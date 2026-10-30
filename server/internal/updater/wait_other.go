//go:build !windows

package updater

import (
	"errors"
	"time"
)

func waitForPID(int, time.Duration) error {
	return errors.New("waiting for a process is only supported on Windows")
}
