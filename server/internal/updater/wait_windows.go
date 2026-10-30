//go:build windows

package updater

import (
	"fmt"
	"syscall"
	"time"
)

const (
	synchronize = 0x00100000
	waitTimeout = 0x00000102
)

func waitForPID(pid int, timeout time.Duration) error {
	handle, err := syscall.OpenProcess(synchronize, false, uint32(pid))
	if err != nil {
		// The process is gone or inaccessible; either way there is nothing to
		// wait for.
		return nil
	}
	defer syscall.CloseHandle(handle)
	status, err := syscall.WaitForSingleObject(handle, uint32(timeout/time.Millisecond))
	if err != nil {
		return err
	}
	if status == waitTimeout {
		return fmt.Errorf("process %d did not exit within %s", pid, timeout)
	}
	return nil
}
