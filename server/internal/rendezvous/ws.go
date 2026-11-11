package rendezvous

import (
	"bufio"
	"net"
	"net/http"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const signalHandshakeTimeout = 10 * time.Second

// SignalHandler upgrades an HTTP request to the WebSocket signalling channel
// and hands it to the same coordinator as the raw TCP punch port. Hosts keep
// their control link and both sides exchange punch announces on a connection
// the HTTP server already accepts, so a blocked raw TCP punch port no longer
// costs TCP punch coordination.
func (s *Server) SignalHandler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.tcpPunch == nil {
			http.NotFound(w, r)
			return
		}
		hijacker, ok := w.(http.Hijacker)
		if !ok {
			// HTTP/2 has no upgrade path; the signalling client speaks 1.1.
			http.Error(w, "the signalling endpoint requires HTTP/1.1", http.StatusBadRequest)
			return
		}
		req := carrier.UpgradeRequestFromHTTP(r)
		conn, brw, err := hijacker.Hijack()
		if err != nil {
			return
		}
		go s.serveSignalWS(conn, brw.Reader, req)
	})
}

func (s *Server) serveSignalWS(conn net.Conn, br *bufio.Reader, req carrier.UpgradeRequest) {
	// A hijacked connection keeps whatever deadlines the HTTP server set.
	_ = conn.SetDeadline(time.Now().Add(signalHandshakeTimeout))
	ws, err := carrier.CompleteUpgrade(conn, br, req, wire.SignalWSPath, wire.SignalWSSubprotocol)
	if err != nil {
		conn.Close()
		return
	}
	s.log.Debug("signalling WebSocket connected", "remote", conn.RemoteAddr().String())
	_ = conn.SetDeadline(time.Time{})
	done := make(chan struct{})
	go func() {
		select {
		case <-s.done:
			conn.Close()
		case <-done:
		}
	}()
	defer close(done)
	s.tcpPunch.handle(carrier.NewStreamConn(ws, conn), true)
}
