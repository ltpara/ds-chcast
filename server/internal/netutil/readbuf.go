package netutil

import "net"

// SetUDPReadBuffer asks the kernel for size bytes of receive buffer and returns
// the value that is actually in effect. The kernel may round the value up,
// double it (Linux) or clamp it to a system maximum, so the result is not
// necessarily the request. A non-positive size leaves the socket untouched and
// reports zero.
func SetUDPReadBuffer(conn *net.UDPConn, size int) (int, error) {
	if conn == nil || size <= 0 {
		return 0, nil
	}
	if err := conn.SetReadBuffer(size); err != nil {
		return 0, err
	}
	raw, err := conn.SyscallConn()
	if err != nil {
		return 0, err
	}
	var value int
	var optErr error
	if err := raw.Control(func(fd uintptr) {
		value, optErr = readSockopt(fd)
	}); err != nil {
		return 0, err
	}
	if optErr != nil {
		return 0, optErr
	}
	return value, nil
}
