package netproxy

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strings"
	"time"
)

// Serve runs a loopback HTTP forward proxy that sends every request through
// upstream. It exists so clients without SOCKS5 support (the Flutter viewer's
// Dart HTTP stack) can reach the rendezvous API through the same proxy as the
// tunnel agent. It handles CONNECT and absolute-form HTTP requests.
func Serve(ctx context.Context, listener net.Listener, upstream *Proxy, log *slog.Logger) error {
	if upstream == nil {
		return errors.New("proxy bridge needs an upstream proxy")
	}
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		go func(conn net.Conn) {
			defer conn.Close()
			handleBridgeConn(ctx, conn, upstream, log)
		}(conn)
	}
}

func handleBridgeConn(ctx context.Context, conn net.Conn, upstream *Proxy, log *slog.Logger) {
	_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
	reader := bufio.NewReader(conn)
	request, err := http.ReadRequest(reader)
	if err != nil {
		if !errors.Is(err, io.EOF) {
			log.Debug("proxy bridge request", "error", err)
		}
		return
	}
	_ = conn.SetReadDeadline(time.Time{})

	if request.Method == http.MethodConnect {
		handleBridgeConnect(ctx, conn, reader, request, upstream, log)
		return
	}

	requestURI := request.URL
	if !requestURI.IsAbs() || requestURI.Host == "" {
		writeBridgeError(conn, http.StatusBadRequest, "proxy requests need an absolute URL")
		return
	}
	target := requestURI.Host
	if requestURI.Port() == "" {
		port := "80"
		if requestURI.Scheme == "https" {
			port = "443"
		}
		target = net.JoinHostPort(requestURI.Hostname(), port)
	}
	upstreamConn, err := upstream.DialContext(ctx, "tcp", target)
	if err != nil {
		log.Debug("proxy bridge dial", "target", target, "error", err)
		writeBridgeError(conn, http.StatusBadGateway, "proxy could not reach the target")
		return
	}
	defer upstreamConn.Close()

	request.RequestURI = ""
	request.URL.Scheme = ""
	request.URL.Host = ""
	request.Header.Del("Proxy-Authorization")
	request.Header.Del("Proxy-Connection")
	request.Header.Set("Connection", "close")
	request.Close = true
	if err := request.Write(upstreamConn); err != nil {
		log.Debug("proxy bridge forward", "target", target, "error", err)
		return
	}
	response, err := http.ReadResponse(bufio.NewReader(upstreamConn), request)
	if err != nil {
		log.Debug("proxy bridge response", "target", target, "error", err)
		writeBridgeError(conn, http.StatusBadGateway, "target did not answer")
		return
	}
	defer response.Body.Close()
	response.Close = true
	if err := response.Write(conn); err != nil {
		log.Debug("proxy bridge reply", "target", target, "error", err)
	}
}

func handleBridgeConnect(ctx context.Context, conn net.Conn, reader *bufio.Reader, request *http.Request, upstream *Proxy, log *slog.Logger) {
	target := request.Host
	if target == "" {
		target = request.URL.Host
	}
	if !strings.Contains(target, ":") {
		writeBridgeError(conn, http.StatusBadRequest, "CONNECT needs host:port")
		return
	}
	upstreamConn, err := upstream.DialContext(ctx, "tcp", target)
	if err != nil {
		log.Debug("proxy bridge connect", "target", target, "error", err)
		writeBridgeError(conn, http.StatusBadGateway, "proxy could not reach the target")
		return
	}
	defer upstreamConn.Close()
	if _, err := io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\n"); err != nil {
		return
	}

	client := &bufferedConn{Conn: conn, reader: reader}
	done := make(chan struct{}, 2)
	go func() {
		_, _ = io.Copy(upstreamConn, client)
		if closer, ok := upstreamConn.(interface{ CloseWrite() error }); ok {
			_ = closer.CloseWrite()
		}
		done <- struct{}{}
	}()
	go func() {
		_, _ = io.Copy(client, upstreamConn)
		done <- struct{}{}
	}()
	select {
	case <-ctx.Done():
	case <-done:
	}
}

func writeBridgeError(conn net.Conn, status int, message string) {
	body := message + "\n"
	fmt.Fprintf(conn, "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
		status, http.StatusText(status), len(body), body)
}
