package agent

import (
	"bufio"
	"context"
	"crypto/tls"
	"fmt"
	"net"
	"net/url"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// signalEndpoint derives the signalling WebSocket address from an API base
// URL. The WebSocket rides the HTTP(S) port, so it is reachable wherever the
// API is and passes a proxy that already admits the API.
func signalEndpoint(serverURL string) (addr, host string, secure, ok bool) {
	u, err := url.Parse(strings.TrimSpace(serverURL))
	if err != nil || u.Host == "" || u.Hostname() == "" {
		return "", "", false, false
	}
	switch strings.ToLower(u.Scheme) {
	case "https":
		secure = true
	case "http":
	default:
		return "", "", false, false
	}
	host = u.Host
	if u.Port() == "" {
		port := "80"
		if secure {
			port = "443"
		}
		host = net.JoinHostPort(u.Hostname(), port)
	}
	return host, host, secure, true
}

// dialSignal opens the WebSocket signalling stream through the same proxy and
// TLS configuration as the API client. The returned connection is a byte
// stream carrying the framed control protocol unchanged.
func dialSignal(ctx context.Context, serverURL string, proxy *netproxy.Proxy, tlsConfig *tls.Config) (net.Conn, error) {
	addr, host, secure, ok := signalEndpoint(serverURL)
	if !ok {
		return nil, fmt.Errorf("signalling requires an http:// or https:// server URL, got %q", serverURL)
	}
	var (
		conn net.Conn
		err  error
	)
	if proxy != nil {
		conn, err = proxy.DialContext(ctx, "tcp", addr)
	} else {
		var dialer net.Dialer
		conn, err = dialer.DialContext(ctx, "tcp", addr)
	}
	if err != nil {
		return nil, err
	}
	if deadline, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(deadline)
	}
	if secure {
		config := &tls.Config{}
		if tlsConfig != nil {
			config = tlsConfig.Clone()
		}
		if config.ServerName == "" {
			hostName, _, _ := net.SplitHostPort(host)
			config.ServerName = hostName
		}
		tlsConn := tls.Client(conn, config)
		if err := tlsConn.HandshakeContext(ctx); err != nil {
			conn.Close()
			return nil, err
		}
		conn = tlsConn
	}
	ws, err := carrier.NewWSClient(conn, bufio.NewReader(conn), host, wire.SignalWSPath, wire.SignalWSSubprotocol)
	if err != nil {
		conn.Close()
		return nil, err
	}
	_ = conn.SetDeadline(time.Time{})
	return carrier.NewStreamConn(ws, conn), nil
}
