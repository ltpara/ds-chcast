package relay

import (
	"errors"
	"net"
	"sync"
	"testing"
)

type scriptedListener struct {
	mu   sync.Mutex
	errs []error
	conn net.Conn
}

func (l *scriptedListener) Accept() (net.Conn, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if len(l.errs) > 0 {
		err := l.errs[0]
		l.errs = l.errs[1:]
		return nil, err
	}
	if l.conn != nil {
		conn := l.conn
		l.conn = nil
		return conn, nil
	}
	return nil, net.ErrClosed
}

func (l *scriptedListener) Close() error   { return nil }
func (l *scriptedListener) Addr() net.Addr { return &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 1} }

// A transient accept failure must not end the listener for good.
func TestAcceptWithRetry(t *testing.T) {
	accepted, peer := net.Pipe()
	defer peer.Close()
	accepted.Close()
	ln := &scriptedListener{
		errs: []error{
			errors.New("accept tcp: too many open files"),
			errors.New("accept tcp: connection reset by peer"),
		},
		conn: accepted,
	}
	if got := acceptWithRetry(ln, testLog, "test accept"); got == nil {
		t.Fatal("transient accept failures ended the loop")
	}
	if got := acceptWithRetry(&scriptedListener{}, testLog, "test accept"); got != nil {
		t.Fatalf("closed listener returned a connection: %v", got)
	}
}
