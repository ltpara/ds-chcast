// Package carrier implements the framing shared by the TCP and WebSocket
// carriers. A carrier moves opaque QUIC datagrams; it never authenticates or
// interprets them.
package carrier

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sync"
)

const (
	// MagicRaw opens a raw TCP carrier from a viewer.
	MagicRaw = "CHASTT1\n"
	// MagicViewer opens a raw TCP viewer link on the shared carrier. A JSON
	// SharedViewerHello line follows, then the usual frames.
	MagicViewer = "CHASTT-V2\n"
	// MagicHost opens the long-lived link a host agent keeps to a relay port.
	// The link is only authenticated after the challenge-response that
	// follows this line.
	MagicHost = "CHASTT-HOST2\n"
	// MagicChallenge prefixes the JSON nonce line the relay sends to a host.
	MagicChallenge = "CHASTT-CHALLENGE1 "
	// MagicAuth prefixes the JSON signed response from a host.
	MagicAuth = "CHASTT-AUTH1 "
	// MagicOK ends a successful host handshake.
	MagicOK = "CHASTT-OK2\n"
	// MagicErrorPrefix starts a refusal line on a host link.
	MagicErrorPrefix = "CHASTT-ERR1 "

	// MaxPayload is one QUIC datagram and one carrier frame.
	MaxPayload = 65535

	// MaxHandshake bounds the magic and host hello lines.
	MaxHandshake = 2048
)

var ErrFrameTooLarge = errors.New("carrier frame is larger than 65535 bytes")

// HostHello is the JSON line a host agent sends after MagicHost.
type HostHello struct {
	HostID string `json:"host_id"`
}

// HostChallenge is the relay's JSON line starting with MagicChallenge.
type HostChallenge struct {
	Nonce string `json:"nonce"`
}

// HostAuth is the host agent's JSON line starting with MagicAuth.
type HostAuth struct {
	HostID    string `json:"host_id"`
	Nonce     string `json:"nonce"`
	Signature string `json:"signature"`
}

// SharedViewerHello selects a host on the shared carrier. The token comes from
// the authenticated lookup; raw TCP sends it as a line after MagicViewer and
// WebSocket sends it as the first binary message.
type SharedViewerHello struct {
	HostID string `json:"host_id"`
	Token  string `json:"token"`
}

// FrameReader decodes u32le length-prefixed payloads from a byte stream.
type FrameReader struct {
	r      io.Reader
	header [4]byte
}

func NewFrameReader(r io.Reader) *FrameReader { return &FrameReader{r: r} }

// ReadFrame returns the next payload. A keepalive frame returns a nil slice and
// no error so callers can reset their idle timer and continue.
func (f *FrameReader) ReadFrame() ([]byte, error) {
	if _, err := io.ReadFull(f.r, f.header[:]); err != nil {
		return nil, err
	}
	size := binary.LittleEndian.Uint32(f.header[:])
	if size > MaxPayload {
		return nil, ErrFrameTooLarge
	}
	if size == 0 {
		return nil, nil
	}
	payload := make([]byte, size)
	if _, err := io.ReadFull(f.r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

// FrameWriter serializes frames from concurrent senders; a host link and a
// viewer link each have one writer shared by the two forwarding directions.
type FrameWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func NewFrameWriter(w io.Writer) *FrameWriter { return &FrameWriter{w: w} }

func (f *FrameWriter) WriteFrame(payload []byte) error {
	if len(payload) > MaxPayload {
		return ErrFrameTooLarge
	}
	var header [4]byte
	binary.LittleEndian.PutUint32(header[:], uint32(len(payload)))
	f.mu.Lock()
	defer f.mu.Unlock()
	if err := writeAll(f.w, header[:]); err != nil {
		return err
	}
	if len(payload) == 0 {
		return nil
	}
	return writeAll(f.w, payload)
}

func writeAll(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if n > 0 {
			data = data[n:]
		}
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
	}
	return nil
}

// ReadLine reads one \n-terminated protocol line without letting a peer grow
// the buffer past limit. The line is returned without its terminator.
func ReadLine(br *bufio.Reader, limit int) (string, error) {
	line := make([]byte, 0, 64)
	for {
		b, err := br.ReadByte()
		if err != nil {
			return "", err
		}
		if b == '\n' {
			if n := len(line); n > 0 && line[n-1] == '\r' {
				line = line[:n-1]
			}
			return string(line), nil
		}
		if len(line) >= limit {
			return "", fmt.Errorf("carrier handshake line is longer than %d bytes", limit)
		}
		line = append(line, b)
	}
}
