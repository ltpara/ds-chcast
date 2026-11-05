//go:build windows

package updater

import "syscall"

// processAlive reports whether pid is still running. A zero timeout turns
// WaitForSingleObject into a liveness probe: the handle signals only on exit.
func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	handle, err := syscall.OpenProcess(synchronize, false, uint32(pid))
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(handle)
	status, err := syscall.WaitForSingleObject(handle, 0)
	return err == nil && status == waitTimeout
}
