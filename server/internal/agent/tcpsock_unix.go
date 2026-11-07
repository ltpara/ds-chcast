//go:build !windows

package agent

import "golang.org/x/sys/unix"

func setTCPReuse(fd uintptr) error {
	if err := unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_REUSEADDR, 1); err != nil {
		return err
	}
	// Linux and the BSDs refuse a second live bind of one address without this;
	// Windows has no equivalent and SO_REUSEADDR is enough there.
	_ = unix.SetsockoptInt(int(fd), unix.SOL_SOCKET, unix.SO_REUSEPORT, 1)
	return nil
}
