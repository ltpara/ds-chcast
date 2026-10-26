package carrier

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
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

// WSConn is a server-side WebSocket connection that carries one binary message
// per QUIC datagram. Clients must mask their frames; the server never masks.
type WSConn struct {
	conn net.Conn
	br   *bufio.Reader
	wmu  sync.Mutex
}

// UpgradeServer completes the HTTP upgrade from bytes already buffered in br.
// It returns only after the 101 response has been written.
func UpgradeServer(conn net.Conn, br *bufio.Reader) (*WSConn, error) {
	method, target, version, headers, err := readUpgradeRequest(br)
	if err != nil {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, err
	}
	if method != "GET" || version != "HTTP/1.1" || target != WSPath {
		writeUpgradeError(conn, "404 Not Found")
		return nil, fmt.Errorf("unexpected WebSocket request %s %s %s", method, target, version)
	}
	if !headerHas(headers["upgrade"], "websocket") ||
		!headerHas(headers["connection"], "upgrade") {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, errors.New("the request is not a WebSocket upgrade")
	}
	if strings.TrimSpace(headers["sec-websocket-version"]) != "13" {
		writeUpgradeError(conn, "426 Upgrade Required")
		return nil, fmt.Errorf("unsupported WebSocket version %q", headers["sec-websocket-version"])
	}
	key := strings.TrimSpace(headers["sec-websocket-key"])
	if raw, err := base64.StdEncoding.DecodeString(key); err != nil || len(raw) != 16 {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, errors.New("the WebSocket key is not 16 base64-encoded bytes")
	}
	protocols := headers["sec-websocket-protocol"]
	if !headerHas(protocols, WSSubprotocol) {
		writeUpgradeError(conn, "400 Bad Request")
		return nil, fmt.Errorf("the WebSocket subprotocol must be %s", WSSubprotocol)
	}
	response := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + WebSocketAccept(key) + "\r\n" +
		"Sec-WebSocket-Protocol: " + WSSubprotocol + "\r\n\r\n"
	if err := writeAll(conn, []byte(response)); err != nil {
		return nil, err
	}
	return &WSConn{conn: conn, br: br}, nil
}

func readUpgradeRequest(br *bufio.Reader) (method, target, version string, headers map[string]string, err error) {
	line, err := ReadLine(br, maxWSHandshake)
	if err != nil {
		return "", "", "", nil, err
	}
	parts := strings.Fields(line)
	if len(parts) != 3 {
		return "", "", "", nil, fmt.Errorf("malformed WebSocket request line %q", line)
	}
	method, target, version = parts[0], parts[1], parts[2]
	headers = make(map[string]string)
	total := len(line)
	for count := 0; ; count++ {
		if count > maxWSHeaders {
			return "", "", "", nil, errors.New("too many WebSocket headers")
		}
		line, err = ReadLine(br, maxWSHandshake)
		if err != nil {
			return "", "", "", nil, err
		}
		total += len(line)
		if total > maxWSHandshake {
			return "", "", "", nil, errors.New("the WebSocket handshake is too large")
		}
		if line == "" {
			return method, target, version, headers, nil
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
		if !masked {
			return nil, errors.New("a WebSocket carrier client sent an unmasked frame")
		}
		if opcode >= 0x8 && (length > 125 || !fin) {
			return nil, errors.New("malformed WebSocket control frame")
		}
		var mask [4]byte
		if _, err := io.ReadFull(c.br, mask[:]); err != nil {
			return nil, err
		}
		body := make([]byte, length)
		if length > 0 {
			if _, err := io.ReadFull(c.br, body); err != nil {
				return nil, err
			}
			for i := range body {
				body[i] ^= mask[i%4]
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
	header := make([]byte, 2, 4+len(payload))
	header[0] = 0x80 | opcode
	if len(payload) <= 125 {
		header[1] = byte(len(payload))
	} else {
		header[1] = 126
		header = append(header, byte(len(payload)>>8), byte(len(payload)))
	}
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if err := writeAll(c.conn, header); err != nil {
		return err
	}
	if len(payload) == 0 {
		return nil
	}
	return writeAll(c.conn, payload)
}

// SetReadDeadline bounds the next read. The relay uses it as an idle timeout.
func (c *WSConn) SetReadDeadline(t time.Time) error { return c.conn.SetReadDeadline(t) }

// Close tries to send a close frame, then closes the connection.
func (c *WSConn) Close() error {
	_ = c.conn.SetWriteDeadline(time.Now().Add(time.Second))
	_ = c.writeFrame(0x8, nil)
	return c.conn.Close()
}
