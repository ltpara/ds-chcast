package agent

import (
	"context"
	"errors"
	"net"
	"syscall"
	"time"
)

// tcpReuseControl lets several sockets share one local address: the hosted TCP
// punch binds the port its coordination socket used, and its punches leave from
// that same port. RustDesk sets the same options in new_socket, which is what
// makes the NAT mapping reusable.
func tcpReuseControl(_, _ string, c syscall.RawConn) error {
	var setErr error
	err := c.Control(func(fd uintptr) {
		setErr = setTCPReuse(fd)
	})
	if err != nil {
		return err
	}
	return setErr
}

func tcpListenReuse(addr *net.TCPAddr) (net.Listener, error) {
	lc := net.ListenConfig{Control: tcpReuseControl}
	return lc.Listen(context.Background(), "tcp", addr.String())
}

func tcpDialReuseContext(ctx context.Context, local, remote *net.TCPAddr, timeout time.Duration) (net.Conn, error) {
	d := net.Dialer{Timeout: timeout, Control: tcpReuseControl}
	if local != nil {
		d.LocalAddr = local
	}
	return d.DialContext(ctx, "tcp", remote.String())
}

// reservePunchPort holds an ephemeral TCP port for a WebSocket-coordinated
// punch. The listener keeps the port taken until the coordination reply, then
// the caller closes it and dials the peer from the same local port, matching
// the raw coordination path.
func reservePunchPort() (net.Listener, *net.TCPAddr, error) {
	listener, err := tcpListenReuse(&net.TCPAddr{IP: net.IPv4zero})
	if err != nil {
		return nil, nil, err
	}
	bound, ok := listener.Addr().(*net.TCPAddr)
	if !ok || bound.Port == 0 {
		listener.Close()
		return nil, nil, errors.New("the punch port listener returned no TCP address")
	}
	return listener, &net.TCPAddr{IP: net.IPv4zero, Port: bound.Port}, nil
}
