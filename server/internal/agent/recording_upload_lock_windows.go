//go:build windows

package agent

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"
)

var (
	recordingKernel32      = syscall.NewLazyDLL("kernel32.dll")
	recordingCreateMutexW  = recordingKernel32.NewProc("CreateMutexW")
	recordingWaitForSingle = recordingKernel32.NewProc("WaitForSingleObject")
	recordingReleaseMutex  = recordingKernel32.NewProc("ReleaseMutex")
	recordingCloseHandle   = recordingKernel32.NewProc("CloseHandle")
)

func acquireHostRecordingUploadLock(queue string) (func(), bool, error) {
	runtime.LockOSThread()
	absolute, err := filepath.Abs(queue)
	if err != nil {
		runtime.UnlockOSThread()
		return nil, false, err
	}
	digest := sha256.Sum256([]byte(filepath.Clean(absolute)))
	name, err := syscall.UTF16PtrFromString(`Global\chast-host-recording-upload-` + hex.EncodeToString(digest[:16]))
	if err != nil {
		runtime.UnlockOSThread()
		return nil, false, err
	}
	handle, _, callErr := recordingCreateMutexW.Call(0, 0, uintptr(unsafe.Pointer(name)))
	if handle == 0 {
		runtime.UnlockOSThread()
		return nil, false, fmt.Errorf("create host recording worker mutex: %w", callErr)
	}
	wait, _, waitErr := recordingWaitForSingle.Call(handle, 0)
	if wait == 0 || wait == 0x80 { // WAIT_OBJECT_0 or WAIT_ABANDONED
		return func() {
			recordingReleaseMutex.Call(handle)
			recordingCloseHandle.Call(handle)
			runtime.UnlockOSThread()
		}, true, nil
	}
	recordingCloseHandle.Call(handle)
	runtime.UnlockOSThread()
	if wait == 0x102 { // WAIT_TIMEOUT
		return nil, false, nil
	}
	return nil, false, fmt.Errorf("wait for host recording worker mutex: %w", waitErr)
}
