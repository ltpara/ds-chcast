// Package netproxy dials outbound TCP through an operator-configured HTTP(S)
// CONNECT or SOCKS5 proxy. UDP is not proxied: the tunnel agent switches to the
// TCP carrier when a proxy is set instead of silently connecting around it.
package netproxy

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const handshakeTimeout = 10 * time.Second

// Proxy is a parsed outbound proxy URL. It is immutable after Parse.
type Proxy struct {
	url *url.URL
}

// Parse accepts http://, https://, socks5:// and socks5h:// URLs with an
// explicit port. Credentials are allowed in the userinfo part. socks5 and
// socks5h both leave name resolution to the proxy.
func Parse(raw string) (*Proxy, error) {
	text := strings.TrimSpace(raw)
	if text == "" {
		return nil, errors.New("proxy URL is empty")
	}
	u, err := url.Parse(text)
	if err != nil {
		return nil, errors.New("proxy URL is not a valid URL")
	}
	switch strings.ToLower(u.Scheme) {
	case "http", "https", "socks5", "socks5h":
	default:
		return nil, fmt.Errorf("unsupported proxy scheme %q (use http, https, socks5 or socks5h)", u.Scheme)
	}
	u.Scheme = strings.ToLower(u.Scheme)
	if u.Opaque != "" || u.Hostname() == "" {
		return nil, errors.New("proxy URL needs a host")
	}
	if u.Path != "" && u.Path != "/" {
		return nil, errors.New("proxy URL must not contain a path")
	}
	if u.RawQuery != "" || u.Fragment != "" {
		return nil, errors.New("proxy URL must not contain a query or fragment")
	}
	port := u.Port()
	if port == "" {
		return nil, errors.New("proxy URL needs an explicit port")
	}
	number, err := strconv.Atoi(port)
	if err != nil || number < 1 || number > 65535 {
		return nil, errors.New("proxy URL has an invalid port")
	}
	u.Path = ""
	return &Proxy{url: u}, nil
}

// URL returns the parsed proxy URL. The caller must not mutate it.
func (p *Proxy) URL() *url.URL {
	if p == nil {
		return nil
	}
	return p.url
}

// String omits credentials so the value is safe for logs and process events.
func (p *Proxy) String() string {
	if p == nil {
		return ""
	}
	return p.url.Scheme + "://" + p.url.Host
}

// Redacted is an alias for String; it exists to make log call sites explicit.
func (p *Proxy) Redacted() string { return p.String() }

func (p *Proxy) authHeader() string {
	if p.url.User == nil {
		return ""
	}
	username := p.url.User.Username()
	password, _ := p.url.User.Password()
	return "Basic " + base64.StdEncoding.EncodeToString([]byte(username+":"+password))
}

// DialContext opens a TCP stream to addr through the proxy. A nil Proxy dials
// directly, so callers can use it unconditionally.
func (p *Proxy) DialContext(ctx context.Context, network, addr string) (net.Conn, error) {
	if network != "" && network != "tcp" && network != "tcp4" && network != "tcp6" {
		return nil, fmt.Errorf("proxy only supports TCP, not %q", network)
	}
	if p == nil {
		dialer := &net.Dialer{Timeout: handshakeTimeout}
		return dialer.DialContext(ctx, network, addr)
	}
	if p.url.Scheme == "socks5" || p.url.Scheme == "socks5h" {
		return p.dialSOCKS5(ctx, addr)
	}
	return p.dialHTTP(ctx, addr)
}

// HTTPTransport returns a transport that sends HTTP requests through the
// proxy. base, when non-nil, supplies TLS roots and client certificates for
// the origin server. A nil Proxy returns a transport with only base applied.
func (p *Proxy) HTTPTransport(base *tls.Config) *http.Transport {
	transport := &http.Transport{}
	if base != nil {
		transport.TLSClientConfig = base.Clone()
	}
	if p != nil {
		transport.Proxy = http.ProxyURL(p.url)
	}
	return transport
}

// validHost rejects control characters and separators so a server-provided
// target cannot inject bytes into a CONNECT request or a SOCKS5 frame.
func validHost(host string) error {
	if host == "" {
		return errors.New("host is empty")
	}
	if net.ParseIP(host) != nil {
		return nil
	}
	if len(host) > 255 {
		return errors.New("host name is too long")
	}
	for _, c := range host {
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		case c == '.', c == '-', c == '_':
		default:
			return fmt.Errorf("host %q contains an invalid character", host)
		}
	}
	return nil
}

func proxyTarget(addr string) (string, error) {
	host, portText, err := net.SplitHostPort(addr)
	if err != nil || host == "" {
		return "", fmt.Errorf("proxy target %q is not host:port", addr)
	}
	if err := validHost(host); err != nil {
		return "", fmt.Errorf("proxy target %q: %w", addr, err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return "", fmt.Errorf("proxy target %q has an invalid port", addr)
	}
	return net.JoinHostPort(host, portText), nil
}

func (p *Proxy) dialSOCKS5(ctx context.Context, addr string) (net.Conn, error) {
	host, portText, err := net.SplitHostPort(addr)
	if err != nil || host == "" {
		return nil, fmt.Errorf("socks5 target %q is not host:port", addr)
	}
	if err := validHost(host); err != nil {
		return nil, fmt.Errorf("socks5 target %q: %w", addr, err)
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return nil, fmt.Errorf("socks5 target %q has an invalid port", addr)
	}

	dialer := &net.Dialer{Timeout: handshakeTimeout}
	conn, err := dialer.DialContext(ctx, "tcp", p.url.Host)
	if err != nil {
		return nil, fmt.Errorf("socks5 proxy %s: %w", p.String(), err)
	}
	ok := false
	defer func() {
		if !ok {
			conn.Close()
		}
	}()
	if deadline, has := ctx.Deadline(); has {
		_ = conn.SetDeadline(deadline)
	} else {
		_ = conn.SetDeadline(time.Now().Add(handshakeTimeout))
	}

	username := ""
	password := ""
	if p.url.User != nil {
		username = p.url.User.Username()
		password, _ = p.url.User.Password()
		if len(username) > 255 || len(password) > 255 {
			return nil, errors.New("socks5 username and password must be at most 255 bytes")
		}
	}
	methods := []byte{0x00}
	if username != "" {
		methods = []byte{0x00, 0x02}
	}
	greeting := append([]byte{0x05, byte(len(methods))}, methods...)
	if _, err := conn.Write(greeting); err != nil {
		return nil, fmt.Errorf("socks5 greeting: %w", err)
	}
	reply := make([]byte, 2)
	if _, err := io.ReadFull(conn, reply); err != nil {
		return nil, fmt.Errorf("socks5 greeting reply: %w", err)
	}
	if reply[0] != 0x05 {
		return nil, errors.New("socks5 proxy answered with an unexpected version")
	}
	switch reply[1] {
	case 0x00:
	case 0x02:
		if username == "" {
			return nil, errors.New("socks5 proxy requires username/password authentication")
		}
		auth := []byte{0x01, byte(len(username))}
		auth = append(auth, username...)
		auth = append(auth, byte(len(password)))
		auth = append(auth, password...)
		if _, err := conn.Write(auth); err != nil {
			return nil, fmt.Errorf("socks5 authentication: %w", err)
		}
		if _, err := io.ReadFull(conn, reply); err != nil {
			return nil, fmt.Errorf("socks5 authentication reply: %w", err)
		}
		if reply[1] != 0x00 {
			return nil, errors.New("socks5 proxy rejected the username/password")
		}
	default:
		return nil, fmt.Errorf("socks5 proxy offered no acceptable authentication method (0x%02x)", reply[1])
	}

	request := []byte{0x05, 0x01, 0x00}
	if ip := net.ParseIP(host); ip != nil {
		if ip4 := ip.To4(); ip4 != nil {
			request = append(request, 0x01)
			request = append(request, ip4...)
		} else {
			request = append(request, 0x04)
			request = append(request, ip.To16()...)
		}
	} else {
		if len(host) > 255 {
			return nil, errors.New("socks5 target host name is too long")
		}
		request = append(request, 0x03, byte(len(host)))
		request = append(request, host...)
	}
	request = append(request, byte(port>>8), byte(port))
	if _, err := conn.Write(request); err != nil {
		return nil, fmt.Errorf("socks5 connect: %w", err)
	}
	header := make([]byte, 4)
	if _, err := io.ReadFull(conn, header); err != nil {
		return nil, fmt.Errorf("socks5 connect reply: %w", err)
	}
	if header[0] != 0x05 {
		return nil, errors.New("socks5 proxy answered with an unexpected version")
	}
	if header[1] != 0x00 {
		return nil, fmt.Errorf("socks5 proxy refused the connection: %s", socksReplyText(header[1]))
	}
	if err := discardSOCKSAddress(conn, header[3]); err != nil {
		return nil, err
	}
	_ = conn.SetDeadline(time.Time{})
	ok = true
	return conn, nil
}

func discardSOCKSAddress(conn net.Conn, atyp byte) error {
	var length int
	switch atyp {
	case 0x01:
		length = net.IPv4len
	case 0x04:
		length = net.IPv6len
	case 0x03:
		size := make([]byte, 1)
		if _, err := io.ReadFull(conn, size); err != nil {
			return fmt.Errorf("socks5 bound address: %w", err)
		}
		length = int(size[0])
	default:
		return fmt.Errorf("socks5 proxy answered with address type 0x%02x", atyp)
	}
	// The bound address and port are informational; skip them.
	if _, err := io.CopyN(io.Discard, conn, int64(length+2)); err != nil {
		return fmt.Errorf("socks5 bound address: %w", err)
	}
	return nil
}

func socksReplyText(code byte) string {
	switch code {
	case 0x01:
		return "general failure"
	case 0x02:
		return "connection not allowed"
	case 0x03:
		return "network unreachable"
	case 0x04:
		return "host unreachable"
	case 0x05:
		return "connection refused"
	case 0x06:
		return "TTL expired"
	case 0x07:
		return "command not supported"
	case 0x08:
		return "address type not supported"
	default:
		return fmt.Sprintf("error 0x%02x", code)
	}
}

func (p *Proxy) dialHTTP(ctx context.Context, addr string) (net.Conn, error) {
	target, err := proxyTarget(addr)
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{Timeout: handshakeTimeout}
	conn, err := dialer.DialContext(ctx, "tcp", p.url.Host)
	if err != nil {
		return nil, fmt.Errorf("proxy %s: %w", p.String(), err)
	}
	ok := false
	defer func() {
		if !ok {
			conn.Close()
		}
	}()
	if deadline, has := ctx.Deadline(); has {
		_ = conn.SetDeadline(deadline)
	} else {
		_ = conn.SetDeadline(time.Now().Add(handshakeTimeout))
	}
	if p.url.Scheme == "https" {
		tlsConn := tls.Client(conn, &tls.Config{ServerName: p.url.Hostname()})
		if err := tlsConn.HandshakeContext(ctx); err != nil {
			return nil, fmt.Errorf("TLS to proxy %s: %w", p.String(), err)
		}
		conn = tlsConn
	}

	var request strings.Builder
	request.WriteString("CONNECT ")
	request.WriteString(target)
	request.WriteString(" HTTP/1.1\r\nHost: ")
	request.WriteString(target)
	request.WriteString("\r\n")
	if auth := p.authHeader(); auth != "" {
		request.WriteString("Proxy-Authorization: ")
		request.WriteString(auth)
		request.WriteString("\r\n")
	}
	request.WriteString("\r\n")
	if _, err := io.WriteString(conn, request.String()); err != nil {
		return nil, fmt.Errorf("proxy CONNECT: %w", err)
	}

	reader := bufio.NewReader(conn)
	statusLine, err := readHTTPLine(reader)
	if err != nil {
		return nil, fmt.Errorf("proxy CONNECT reply: %w", err)
	}
	fields := strings.Fields(statusLine)
	if len(fields) < 2 || !strings.HasPrefix(fields[0], "HTTP/") {
		return nil, errors.New("proxy CONNECT reply is not HTTP")
	}
	status, err := strconv.Atoi(fields[1])
	if err != nil {
		return nil, errors.New("proxy CONNECT reply has no status code")
	}
	total := len(statusLine)
	for {
		line, err := readHTTPLine(reader)
		if err != nil {
			return nil, fmt.Errorf("proxy CONNECT headers: %w", err)
		}
		total += len(line)
		if total > 32<<10 {
			return nil, errors.New("proxy CONNECT reply headers are too large")
		}
		if line == "\r\n" || line == "\n" || line == "" {
			break
		}
	}
	if status != http.StatusOK {
		return nil, fmt.Errorf("proxy CONNECT refused with HTTP %d", status)
	}
	_ = conn.SetDeadline(time.Time{})
	ok = true
	return &bufferedConn{Conn: conn, reader: reader}, nil
}

func readHTTPLine(reader *bufio.Reader) (string, error) {
	line := make([]byte, 0, 128)
	for {
		b, err := reader.ReadByte()
		if err != nil {
			return "", err
		}
		line = append(line, b)
		if b == '\n' {
			return string(line), nil
		}
		if len(line) > 8<<10 {
			return "", errors.New("line is too long")
		}
	}
}

// bufferedConn preserves bytes the CONNECT reply reader already consumed from
// the tunnel.
type bufferedConn struct {
	net.Conn
	reader *bufio.Reader
}

func (c *bufferedConn) Read(p []byte) (int, error) { return c.reader.Read(p) }

func (c *bufferedConn) CloseWrite() error {
	if closer, ok := c.Conn.(interface{ CloseWrite() error }); ok {
		return closer.CloseWrite()
	}
	return nil
}
