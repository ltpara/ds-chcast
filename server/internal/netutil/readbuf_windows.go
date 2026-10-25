//go:build windows

package netutil

import "syscall"

func readSockopt(fd uintptr) (int, error) {
	return syscall.GetsockoptInt(syscall.Handle(fd), syscall.SOL_SOCKET, syscall.SO_RCVBUF)
}
