package testutil

import (
	"bufio"
	"encoding/binary"
	"io"
	"net"
	"net/url"
	"strconv"
	"sync"
	"testing"
	"time"
)

// Socks5Server is a minimal RFC 1928/1929 proxy for tests. It records the
// destination of every CONNECT so tests can prove traffic really took the
// proxy, then relays bytes to the real target.
type Socks5Server struct {
	listener net.Listener
	username string
	password string

	mu      sync.Mutex
	conns   map[net.Conn]struct{}
	targets []string
}

// StartSocks5 starts a loopback proxy. Empty credentials accept the no-auth
// method; otherwise username/password authentication is required.
func StartSocks5(t *testing.T, username, password string) *Socks5Server {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("start SOCKS5 proxy: %v", err)
	}
	server := &Socks5Server{
		listener: listener,
		username: username,
		password: password,
		conns:    make(map[net.Conn]struct{}),
	}
	t.Cleanup(server.Close)
	go server.acceptLoop()
	return server
}

func (s *Socks5Server) Addr() string { return s.listener.Addr().String() }

// URL returns the socks5:// form with credentials when configured.
func (s *Socks5Server) URL() string {
	if s.username == "" {
		return "socks5://" + s.Addr()
	}
	return "socks5://" + url.UserPassword(s.username, s.password).String() + "@" + s.Addr()
}

// Targets returns the host:port destinations requested through the proxy.
// Domain names stay as sent by the client.
func (s *Socks5Server) Targets() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.targets...)
}

func (s *Socks5Server) Close() {
	s.listener.Close()
	s.mu.Lock()
	for conn := range s.conns {
		conn.Close()
	}
	s.mu.Unlock()
}

func (s *Socks5Server) acceptLoop() {
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			return
		}
		s.mu.Lock()
		s.conns[conn] = struct{}{}
		s.mu.Unlock()
		go func() {
			defer func() {
				conn.Close()
				s.mu.Lock()
				delete(s.conns, conn)
				s.mu.Unlock()
			}()
			s.handle(conn)
		}()
	}
}

func (s *Socks5Server) record(target string) {
	s.mu.Lock()
	s.targets = append(s.targets, target)
	s.mu.Unlock()
}

func (s *Socks5Server) handle(conn net.Conn) {
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))
	reader := bufio.NewReader(conn)
	header := make([]byte, 2)
	if _, err := io.ReadFull(reader, header); err != nil || header[0] != 5 {
		return
	}
	methods := make([]byte, header[1])
	if _, err := io.ReadFull(reader, methods); err != nil {
		return
	}
	method := byte(0xFF)
	want := byte(0)
	if s.username != "" {
		want = 2
	}
	for _, offered := range methods {
		if offered == want {
			method = want
			break
		}
	}
	conn.Write([]byte{5, method})
	if method == 0xFF {
		return
	}
	if method == 2 {
		version := make([]byte, 2)
		if _, err := io.ReadFull(reader, version); err != nil || version[0] != 1 {
			return
		}
		user := make([]byte, version[1])
		if _, err := io.ReadFull(reader, user); err != nil {
			return
		}
		var passLength [1]byte
		if _, err := io.ReadFull(reader, passLength[:]); err != nil {
			return
		}
		pass := make([]byte, passLength[0])
		if _, err := io.ReadFull(reader, pass); err != nil {
			return
		}
		if string(user) != s.username || string(pass) != s.password {
			conn.Write([]byte{1, 1})
			return
		}
		conn.Write([]byte{1, 0})
	}
	request := make([]byte, 4)
	if _, err := io.ReadFull(reader, request); err != nil {
		return
	}
	if request[0] != 5 || request[1] != 1 {
		conn.Write([]byte{5, 7, 0, 1, 0, 0, 0, 0, 0, 0})
		return
	}
	var host string
	switch request[3] {
	case 1:
		address := make([]byte, 4)
		if _, err := io.ReadFull(reader, address); err != nil {
			return
		}
		host = net.IP(address).String()
	case 3:
		var length [1]byte
		if _, err := io.ReadFull(reader, length[:]); err != nil {
			return
		}
		address := make([]byte, length[0])
		if _, err := io.ReadFull(reader, address); err != nil {
			return
		}
		host = string(address)
	case 4:
		address := make([]byte, 16)
		if _, err := io.ReadFull(reader, address); err != nil {
			return
		}
		host = net.IP(address).String()
	default:
		conn.Write([]byte{5, 8, 0, 1, 0, 0, 0, 0, 0, 0})
		return
	}
	var portBytes [2]byte
	if _, err := io.ReadFull(reader, portBytes[:]); err != nil {
		return
	}
	target := net.JoinHostPort(host, strconv.Itoa(int(binary.BigEndian.Uint16(portBytes[:]))))
	s.record(target)
	upstream, err := net.DialTimeout("tcp", target, 5*time.Second)
	if err != nil {
		conn.Write([]byte{5, 5, 0, 1, 0, 0, 0, 0, 0, 0})
		return
	}
	defer upstream.Close()
	if _, err := conn.Write([]byte{5, 0, 0, 1, 0, 0, 0, 0, 0, 0}); err != nil {
		return
	}
	_ = conn.SetDeadline(time.Time{})
	go func() {
		defer upstream.Close()
		io.Copy(upstream, reader)
	}()
	io.Copy(conn, upstream)
}
