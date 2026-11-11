package carrier

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	WSPath        = "/chast"
	WSSubprotocol = "chast-carrier.v1"

	wsGUID         = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
	maxWSHandshake = 8192
	maxWSHeaders   = 64
)

var (
	errWSText     = errors.New("the carrier only accepts WebSocket binary messages")
	errWSFragment = errors.New("fragmented WebSocket messages are not supported")
)

// WebSocketAccept returns the RFC 6455 Sec-WebSocket-Accept value for a client
// key. Exposed for tests and handshake diagnostics.
func WebSocketAccept(key string) string {
	sum := sha1.Sum([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

// WSConn is one WebSocket connection that carries one binary message per
// write. A server never masks its frames and requires clients to mask theirs;
// a client does the opposite, as RFC 6455 requires.
type WSConn struct {
	conn    net.Conn
	br      *bufio.Reader
	headers map[string]string
	client  bool
	wmu     sync.Mutex
}

// UpgradeRequest is one parsed HTTP upgrade request. net/http hands a hijacked
// request to its handler already parsed, so the signalling endpoint builds this
// value from *http.Request instead of reading the socket again.
type UpgradeRequest struct {
	Method  string
	Target  string
	Version string
	Headers map[string]string
}

// Header returns a request header matched case-insensitively. Repeated headers
// are joined with commas; an unknown header returns "".
func (r UpgradeRequest) Header(name string) string {
	return r.Headers[strings.ToLower(strings.TrimSpace(name))]
}

// UpgradeRequestFromHTTP converts a request net/http has already parsed. A
// handler that hijacks the connection uses it with CompleteUpgrade instead of
// reading the request off the socket a second time.
func UpgradeRequestFromHTTP(r *http.Request) UpgradeRequest {
	headers := make(map[string]string, len(r.Header))
	for name, values := range r.Header {
		headers[strings.ToLower(name)] = strings.Join(values, ",")
	}
	return UpgradeRequest{Method: r.Method, Target: r.URL.RequestURI(), Version: r.Proto, Headers: headers}
}

// UpgradeServer completes the HTTP upgrade for the carrier path from bytes
// already buffered in br. It returns only after the 101 response is written.
func UpgradeServer(conn net.Conn, br *bufio.Reader) (*WSConn, error) {
	return UpgradeServerAt(conn, br, WSPath, WSSubprotocol)
}

// UpgradeServerAt is UpgradeServer for a different endpoint path and
// subprotocol. The path is matched exactly and the client must offer the
// configured subprotocol.
func UpgradeServerAt(conn net.Conn, br *bufio.Reader, path, protocol string) (*WSConn, error) {
	req, err := ReadUpgradeRequest(br)
	if err != nil {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, err
	}
	return CompleteUpgrade(conn, br, req, path, protocol)
}

// ReadUpgradeRequest parses a request line and header block from the wire.
func ReadUpgradeRequest(br *bufio.Reader) (UpgradeRequest, error) {
	line, err := ReadLine(br, maxWSHandshake)
	if err != nil {
		return UpgradeRequest{}, err
	}
	parts := strings.Fields(line)
	if len(parts) != 3 {
		return UpgradeRequest{}, fmt.Errorf("malformed WebSocket request line %q", line)
	}
	headers, err := readHeaders(br)
	if err != nil {
		return UpgradeRequest{}, err
	}
	return UpgradeRequest{Method: parts[0], Target: parts[1], Version: parts[2], Headers: headers}, nil
}

// CompleteUpgrade validates a parsed request and writes the 101 response. It is
// the upgrade step for connections whose request the caller already read.
func CompleteUpgrade(conn net.Conn, br *bufio.Reader, req UpgradeRequest, path, protocol string) (*WSConn, error) {
	if req.Method != "GET" || req.Version != "HTTP/1.1" || req.Target != path {
		writeUpgradeError(conn, "404 Not Found")
		return nil, fmt.Errorf("unexpected WebSocket request %s %s %s", req.Method, req.Target, req.Version)
	}
	if !headerHas(req.Header("upgrade"), "websocket") ||
		!headerHas(req.Header("connection"), "upgrade") {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, errors.New("the request is not a WebSocket upgrade")
	}
	if strings.TrimSpace(req.Header("sec-websocket-version")) != "13" {
		writeUpgradeError(conn, "426 Upgrade Required")
		return nil, fmt.Errorf("unsupported WebSocket version %q", req.Header("sec-websocket-version"))
	}
	key := strings.TrimSpace(req.Header("sec-websocket-key"))
	if raw, err := base64.StdEncoding.DecodeString(key); err != nil || len(raw) != 16 {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, errors.New("the WebSocket key is not 16 base64-encoded bytes")
	}
	if !headerHas(req.Header("sec-websocket-protocol"), protocol) {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, fmt.Errorf("the WebSocket subprotocol must be %s", protocol)
	}
	response := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + WebSocketAccept(key) + "\r\n" +
		"Sec-WebSocket-Protocol: " + protocol + "\r\n\r\n"
	if err := writeAll(conn, []byte(response)); err != nil {
		return nil, err
	}
	return &WSConn{conn: conn, br: br, headers: req.Headers}, nil
}

// NewWSClient performs the client half of the upgrade over an established
// connection. The caller may have dialed through a proxy or wrapped conn in
// TLS; this function only speaks HTTP. path must be the exact endpoint the
// server advertises and protocol the subprotocol it requires.
func NewWSClient(conn net.Conn, br *bufio.Reader, host, path, protocol string) (*WSConn, error) {
	var keyBytes [16]byte
	if _, err := rand.Read(keyBytes[:]); err != nil {
		return nil, err
	}
	key := base64.StdEncoding.EncodeToString(keyBytes[:])
	request := "GET " + path + " HTTP/1.1\r\n" +
		"Host: " + host + "\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Key: " + key + "\r\n" +
		"Sec-WebSocket-Version: 13\r\n" +
		"Sec-WebSocket-Protocol: " + protocol + "\r\n\r\n"
	if err := writeAll(conn, []byte(request)); err != nil {
		return nil, err
	}
	line, err := ReadLine(br, maxWSHandshake)
	if err != nil {
		return nil, err
	}
	parts := strings.Fields(line)
	if len(parts) < 2 || parts[0] != "HTTP/1.1" || parts[1] != "101" {
		return nil, fmt.Errorf("the WebSocket handshake was refused: %q", line)
	}
	headers, err := readHeaders(br)
	if err != nil {
		return nil, err
	}
	if !headerHas(headers["upgrade"], "websocket") || !headerHas(headers["connection"], "upgrade") {
		return nil, errors.New("the server did not upgrade the connection")
	}
	if !strings.EqualFold(strings.TrimSpace(headers["sec-websocket-accept"]), WebSocketAccept(key)) {
		return nil, errors.New("the WebSocket accept value did not match the key")
	}
	if !headerHas(headers["sec-websocket-protocol"], protocol) {
		return nil, fmt.Errorf("the server did not select %s", protocol)
	}
	return &WSConn{conn: conn, br: br, headers: headers, client: true}, nil
}

// Header returns the value of a request header from the upgrade handshake,
// matched case-insensitively. Repeated headers are joined with commas. It
// returns an empty string for an unknown header.
func (c *WSConn) Header(name string) string {
	if c == nil {
		return ""
	}
	return c.headers[strings.ToLower(strings.TrimSpace(name))]
}

// readHeaders reads up to the blank line that ends an HTTP header block.
func readHeaders(br *bufio.Reader) (map[string]string, error) {
	headers := make(map[string]string)
	total := 0
	for count := 0; ; count++ {
		if count > maxWSHeaders {
			return nil, errors.New("too many WebSocket headers")
		}
		line, err := ReadLine(br, maxWSHandshake)
		if err != nil {
			return nil, err
		}
		total += len(line)
		if total > maxWSHandshake {
			return nil, errors.New("the WebSocket handshake is too large")
		}
		if line == "" {
			return headers, nil
		}
		colon := strings.IndexByte(line, ':')
		if colon <= 0 {
			continue
		}
		name := strings.ToLower(strings.TrimSpace(line[:colon]))
		value := strings.TrimSpace(line[colon+1:])
		if previous, ok := headers[name]; ok {
			headers[name] = previous + "," + value
		} else {
			headers[name] = value
		}
	}
}

func headerHas(value, wanted string) bool {
	for _, part := range strings.Split(value, ",") {
		if strings.EqualFold(strings.TrimSpace(part), wanted) {
			return true
		}
	}
	return false
}

func writeUpgradeError(conn net.Conn, status string) {
	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_ = writeAll(conn, []byte("HTTP/1.1 "+status+"\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"))
}

// ReadMessage returns the next binary message, answering pings and treating a
// close as io.EOF. Fragmented, text and continuation frames are rejected.
func (c *WSConn) ReadMessage() ([]byte, error) {
	for {
		var head [2]byte
		if _, err := io.ReadFull(c.br, head[:]); err != nil {
			return nil, err
		}
		fin := head[0]&0x80 != 0
		opcode := head[0] & 0x0F
		masked := head[1]&0x80 != 0
		length := uint64(head[1] & 0x7F)
		switch length {
		case 126:
			var extended [2]byte
			if _, err := io.ReadFull(c.br, extended[:]); err != nil {
				return nil, err
			}
			length = uint64(binary.BigEndian.Uint16(extended[:]))
		case 127:
			var extended [8]byte
			if _, err := io.ReadFull(c.br, extended[:]); err != nil {
				return nil, err
			}
			length = binary.BigEndian.Uint64(extended[:])
		}
		if length > MaxPayload {
			return nil, ErrFrameTooLarge
		}
		// RFC 6455: a client masks, a server does not. Rejecting the wrong
		// direction keeps one endpoint from being mistaken for the other.
		if c.client == masked {
			return nil, errors.New("WebSocket frame masked by the wrong side")
		}
		if opcode >= 0x8 && (length > 125 || !fin) {
			return nil, errors.New("malformed WebSocket control frame")
		}
		var mask [4]byte
		if masked {
			if _, err := io.ReadFull(c.br, mask[:]); err != nil {
				return nil, err
			}
		}
		body := make([]byte, length)
		if length > 0 {
			if _, err := io.ReadFull(c.br, body); err != nil {
				return nil, err
			}
			if masked {
				for i := range body {
					body[i] ^= mask[i%4]
				}
			}
		}
		switch opcode {
		case 0x8: // close
			return nil, io.EOF
		case 0x9: // ping
			if err := c.writeFrame(0xA, body); err != nil {
				return nil, err
			}
			continue
		case 0xA: // pong
			continue
		case 0x2: // binary
			if !fin {
				return nil, errWSFragment
			}
			return body, nil
		case 0x0:
			return nil, errWSFragment
		default:
			return nil, errWSText
		}
	}
}

// WriteBinary sends one unmasked binary message.
func (c *WSConn) WriteBinary(payload []byte) error {
	if len(payload) > MaxPayload {
		return ErrFrameTooLarge
	}
	return c.writeFrame(0x2, payload)
}

func (c *WSConn) writeFrame(opcode byte, payload []byte) error {
	header := make([]byte, 2, 8+len(payload))
	header[0] = 0x80 | opcode
	maskBit := byte(0)
	if c.client {
		maskBit = 0x80
	}
	if len(payload) <= 125 {
		header[1] = maskBit | byte(len(payload))
	} else {
		header[1] = maskBit | 126
		header = append(header, byte(len(payload)>>8), byte(len(payload)))
	}
	var mask [4]byte
	if c.client {
		if _, err := rand.Read(mask[:]); err != nil {
			return err
		}
		header = append(header, mask[:]...)
	}
	body := payload
	if c.client {
		body = make([]byte, len(payload))
		for i := range payload {
			body[i] = payload[i] ^ mask[i%4]
		}
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if err := writeAll(c.conn, header); err != nil {
		return err
	}
	if len(body) == 0 {
		return nil
	}
	return writeAll(c.conn, body)
}

// SetReadDeadline bounds the next read. The relay uses it as an idle timeout.
func (c *WSConn) SetReadDeadline(t time.Time) error { return c.conn.SetReadDeadline(t) }

// Close tries to send a close frame, then closes the connection.
func (c *WSConn) Close() error {
	_ = c.conn.SetWriteDeadline(time.Now().Add(time.Second))
	_ = c.writeFrame(0x8, nil)
	return c.conn.Close()
}
