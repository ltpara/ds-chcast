package carrier

import (
	"net"
	"time"
)

// StreamConn adapts a WebSocket connection to the byte stream the framed
// carrier protocol expects. Each Write becomes one binary message and Read
// concatenates message bodies, so one frame may span messages and the raw TCP
// handshake code runs unchanged over a signalling WebSocket.
type StreamConn struct {
	ws      *WSConn
	conn    net.Conn
	pending []byte
}

func NewStreamConn(ws *WSConn, conn net.Conn) *StreamConn {
	return &StreamConn{ws: ws, conn: conn}
}

func (s *StreamConn) Read(p []byte) (int, error) {
	for len(s.pending) == 0 {
		msg, err := s.ws.ReadMessage()
		if err != nil {
			return 0, err
		}
		// An empty binary message is a keepalive, not a frame.
		s.pending = msg
	}
	n := copy(p, s.pending)
	s.pending = s.pending[n:]
	return n, nil
}

func (s *StreamConn) Write(p []byte) (int, error) {
	if err := s.ws.WriteBinary(p); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (s *StreamConn) Close() error { return s.ws.Close() }

func (s *StreamConn) LocalAddr() net.Addr  { return s.conn.LocalAddr() }
func (s *StreamConn) RemoteAddr() net.Addr { return s.conn.RemoteAddr() }

func (s *StreamConn) SetDeadline(t time.Time) error      { return s.conn.SetDeadline(t) }
func (s *StreamConn) SetReadDeadline(t time.Time) error  { return s.conn.SetReadDeadline(t) }
func (s *StreamConn) SetWriteDeadline(t time.Time) error { return s.conn.SetWriteDeadline(t) }
