package carrier

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"
)

func TestFrameRoundTrip(t *testing.T) {
	var stream bytes.Buffer
	writer := NewFrameWriter(&stream)
	payloads := [][]byte{[]byte("initial"), nil, bytes.Repeat([]byte{0xA5}, MaxPayload)}
	for _, payload := range payloads {
		if err := writer.WriteFrame(payload); err != nil {
			t.Fatalf("write %d bytes: %v", len(payload), err)
		}
	}
	reader := NewFrameReader(&stream)
	for i, want := range payloads {
		got, err := reader.ReadFrame()
		if err != nil {
			t.Fatalf("frame %d: %v", i, err)
		}
		if !bytes.Equal(got, want) {
			t.Fatalf("frame %d = %d bytes, want %d", i, len(got), len(want))
		}
	}
}

func TestFrameRejectsOversizedLengthAndPayload(t *testing.T) {
	var header [4]byte
	binary.LittleEndian.PutUint32(header[:], MaxPayload+1)
	if _, err := NewFrameReader(bytes.NewReader(header[:])).ReadFrame(); !errors.Is(err, ErrFrameTooLarge) {
		t.Fatalf("oversized header error = %v", err)
	}
	var stream bytes.Buffer
	if err := NewFrameWriter(&stream).WriteFrame(make([]byte, MaxPayload+1)); !errors.Is(err, ErrFrameTooLarge) {
		t.Fatalf("oversized write error = %v", err)
	}
}

func TestReadLineLimits(t *testing.T) {
	br := bufio.NewReader(strings.NewReader("CHASTT-OK1\r\nnext\n" + strings.Repeat("x", 300) + "\n"))
	line, err := ReadLine(br, 64)
	if err != nil || line != "CHASTT-OK1" {
		t.Fatalf("line = %q, %v", line, err)
	}
	if line, err = ReadLine(br, 64); err != nil || line != "next" {
		t.Fatalf("line = %q, %v", line, err)
	}
	if _, err = ReadLine(br, 64); err == nil {
		t.Fatal("overlong line was accepted")
	}
}

func TestWebSocketAcceptVector(t *testing.T) {
	// RFC 6455 section 1.3.
	if got := WebSocketAccept("dGhlIHNhbXBsZSBub25jZQ=="); got != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" {
		t.Fatalf("accept = %q", got)
	}
}

const testWSRequest = "GET /chast HTTP/1.1\r\n" +
	"Host: 127.0.0.1\r\n" +
	"Upgrade: websocket\r\n" +
	"Connection: Upgrade\r\n" +
	"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
	"Sec-WebSocket-Version: 13\r\n" +
	"Sec-WebSocket-Protocol: chast-carrier.v1\r\n\r\n"

func openTestWS(t *testing.T) (*WSConn, *bufio.Reader, net.Conn) {
	t.Helper()
	server, client := net.Pipe()
	t.Cleanup(func() {
		server.Close()
		client.Close()
	})
	br := bufio.NewReader(server)
	type result struct {
		ws  *WSConn
		err error
	}
	done := make(chan result, 1)
	go func() {
		ws, err := UpgradeServer(server, br)
		done <- result{ws, err}
	}()
	if _, err := io.WriteString(client, testWSRequest); err != nil {
		t.Fatalf("write request: %v", err)
	}
	cr := bufio.NewReader(client)
	var response strings.Builder
	for {
		line, err := cr.ReadString('\n')
		if err != nil {
			t.Fatalf("read status: %v", err)
		}
		response.WriteString(line)
		if line == "\r\n" {
			break
		}
	}
	if !strings.Contains(response.String(), "101 Switching Protocols") ||
		!strings.Contains(response.String(), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") {
		t.Fatalf("response = %q", response.String())
	}
	r := <-done
	if r.err != nil {
		t.Fatalf("upgrade: %v", r.err)
	}
	return r.ws, cr, client
}

func clientFrame(fin bool, opcode byte, payload []byte) []byte {
	frame := make([]byte, 0, len(payload)+14)
	first := opcode & 0x0F
	if fin {
		first |= 0x80
	}
	frame = append(frame, first)
	switch {
	case len(payload) <= 125:
		frame = append(frame, 0x80|byte(len(payload)))
	case len(payload) <= 65535:
		frame = append(frame, 0x80|126, byte(len(payload)>>8), byte(len(payload)))
	default:
		frame = append(frame, 0x80|127)
		var size [8]byte
		binary.BigEndian.PutUint64(size[:], uint64(len(payload)))
		frame = append(frame, size[:]...)
	}
	mask := [4]byte{0x11, 0x22, 0x33, 0x44}
	frame = append(frame, mask[:]...)
	for i, b := range payload {
		frame = append(frame, b^mask[i%4])
	}
	return frame
}

func readServerFrame(t *testing.T, br *bufio.Reader) (byte, []byte) {
	t.Helper()
	var head [2]byte
	if _, err := io.ReadFull(br, head[:]); err != nil {
		t.Fatalf("read frame header: %v", err)
	}
	opcode := head[0] & 0x0F
	if head[1]&0x80 != 0 {
		t.Fatal("the server masked a frame")
	}
	length := uint64(head[1] & 0x7F)
	switch length {
	case 126:
		var extended [2]byte
		if _, err := io.ReadFull(br, extended[:]); err != nil {
			t.Fatalf("read length: %v", err)
		}
		length = uint64(binary.BigEndian.Uint16(extended[:]))
	case 127:
		var extended [8]byte
		if _, err := io.ReadFull(br, extended[:]); err != nil {
			t.Fatalf("read length: %v", err)
		}
		length = binary.BigEndian.Uint64(extended[:])
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(br, payload); err != nil {
		t.Fatalf("read payload: %v", err)
	}
	return opcode, payload
}

func TestWebSocketBinaryRoundTrip(t *testing.T) {
	ws, br, client := openTestWS(t)
	payload := bytes.Repeat([]byte("datagram"), 400)
	go func() {
		client.Write(clientFrame(true, 0x2, payload))
	}()
	got, err := ws.ReadMessage()
	if err != nil {
		t.Fatalf("read message: %v", err)
	}
	if !bytes.Equal(got, payload) {
		t.Fatalf("message = %d bytes, want %d", len(got), len(payload))
	}
	go func() {
		if err := ws.WriteBinary(payload); err != nil {
			t.Errorf("write binary: %v", err)
		}
	}()
	opcode, body := readServerFrame(t, br)
	if opcode != 0x2 || !bytes.Equal(body, payload) {
		t.Fatalf("downlink = opcode %d, %d bytes", opcode, len(body))
	}
}

func TestWebSocketPingAndSmallMessage(t *testing.T) {
	ws, br, client := openTestWS(t)
	go func() {
		client.Write(clientFrame(true, 0x9, []byte("hi")))
		client.Write(clientFrame(true, 0x2, []byte("data")))
	}()
	type pong struct {
		opcode byte
		body   []byte
		err    error
	}
	pongs := make(chan pong, 1)
	go func() {
		opcode, body := readServerFrameSafely(br)
		pongs <- pong{opcode, body, nil}
	}()
	got, err := ws.ReadMessage()
	if err != nil || string(got) != "data" {
		t.Fatalf("message = %q, %v", got, err)
	}
	p := <-pongs
	if p.err != nil || p.opcode != 0xA || string(p.body) != "hi" {
		t.Fatalf("pong = %+v", p)
	}
}

func readServerFrameSafely(br *bufio.Reader) (byte, []byte) {
	var head [2]byte
	if _, err := io.ReadFull(br, head[:]); err != nil {
		return 0, nil
	}
	opcode := head[0] & 0x0F
	length := int(head[1] & 0x7F)
	payload := make([]byte, length)
	_, _ = io.ReadFull(br, payload)
	return opcode, payload
}

func TestWebSocketRejectsBadFrames(t *testing.T) {
	cases := []struct {
		name  string
		frame []byte
	}{
		{"unmasked binary", []byte{0x82, 0x01, 'x'}},
		{"text", clientFrame(true, 0x1, []byte("hi"))},
		{"fragmented", clientFrame(false, 0x2, []byte("hi"))},
		{"continuation", clientFrame(true, 0x0, []byte("hi"))},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			ws, _, client := openTestWS(t)
			go func() {
				client.SetWriteDeadline(time.Now().Add(2 * time.Second))
				client.Write(tc.frame)
			}()
			if payload, err := ws.ReadMessage(); err == nil {
				t.Fatalf("frame accepted as %q", payload)
			}
		})
	}
}

func TestWebSocketRejectsOversizedMessage(t *testing.T) {
	ws, _, client := openTestWS(t)
	go func() {
		client.SetWriteDeadline(time.Now().Add(2 * time.Second))
		// 70000-byte frame announced with the 64-bit length form.
		frame := []byte{0x82, 0x80 | 127}
		var size [8]byte
		binary.BigEndian.PutUint64(size[:], 70000)
		frame = append(frame, size[:]...)
		frame = append(frame, 0, 0, 0, 0)
		client.Write(frame)
	}()
	if _, err := ws.ReadMessage(); !errors.Is(err, ErrFrameTooLarge) {
		t.Fatalf("oversized message error = %v", err)
	}
}

func TestWebSocketHandshakeRefusal(t *testing.T) {
	server, client := net.Pipe()
	defer server.Close()
	defer client.Close()
	go func() {
		request := strings.Replace(testWSRequest, "chast-carrier.v1", "other", 1)
		io.WriteString(client, request)
		client.Close()
	}()
	if _, err := UpgradeServer(server, bufio.NewReader(server)); err == nil {
		t.Fatal("a wrong subprotocol was accepted")
	}
}
