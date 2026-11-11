package agent

import (
	"bufio"
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func TestSignalEndpointDerivesHTTPAddress(t *testing.T) {
	for _, tc := range []struct {
		name   string
		url    string
		addr   string
		secure bool
		wantOK bool
	}{
		{"http default port", "http://meet.example.com", "meet.example.com:80", false, true},
		{"https default port", "https://meet.example.com", "meet.example.com:443", true, true},
		{"explicit port and path", "https://meet.example.com:8443/base", "meet.example.com:8443", true, true},
		{"IPv6 literal", "https://[2001:db8::1]", "[2001:db8::1]:443", true, true},
		{"spaces", " https://meet.example.com ", "meet.example.com:443", true, true},
		{"other scheme", "ftp://meet.example.com", "", false, false},
		{"missing host", "https://", "", false, false},
		{"empty", "", "", false, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			addr, host, secure, ok := signalEndpoint(tc.url)
			if ok != tc.wantOK {
				t.Fatalf("ok = %v, want %v", ok, tc.wantOK)
			}
			if !ok {
				return
			}
			if addr != tc.addr || secure != tc.secure {
				t.Fatalf("addr = %q secure = %v, want %q %v", addr, secure, tc.addr, tc.secure)
			}
			if host != tc.addr {
				t.Fatalf("host = %q, want %q", host, tc.addr)
			}
		})
	}
}

// signalEcho upgrades the connection and echoes framed payloads, standing in
// for the signalling endpoint without a whole rendezvous server.
func signalEcho(conn net.Conn, br *bufio.Reader, req carrier.UpgradeRequest) {
	defer conn.Close()
	ws, err := carrier.CompleteUpgrade(conn, br, req, wire.SignalWSPath, wire.SignalWSSubprotocol)
	if err != nil {
		return
	}
	stream := carrier.NewStreamConn(ws, conn)
	reader := carrier.NewFrameReader(stream)
	writer := carrier.NewFrameWriter(stream)
	for {
		payload, err := reader.ReadFrame()
		if err != nil {
			return
		}
		if writer.WriteFrame(payload) != nil {
			return
		}
	}
}

func echoHandler(w http.ResponseWriter, r *http.Request) {
	hijacker, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "no hijack", http.StatusInternalServerError)
		return
	}
	conn, brw, err := hijacker.Hijack()
	if err != nil {
		return
	}
	go signalEcho(conn, brw.Reader, carrier.UpgradeRequestFromHTTP(r))
}

func testHandler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != wire.SignalWSPath {
		http.NotFound(w, r)
		return
	}
	echoHandler(w, r)
}

func TestDialSignalCarriesFramesOverWebSocket(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(testHandler))
	defer srv.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, err := dialSignal(ctx, srv.URL, nil, nil)
	if err != nil {
		t.Fatalf("dial signalling: %v", err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	if err := carrier.NewFrameWriter(conn).WriteFrame([]byte("hello signalling")); err != nil {
		t.Fatal(err)
	}
	payload, err := carrier.NewFrameReader(conn).ReadFrame()
	if err != nil {
		t.Fatal(err)
	}
	if string(payload) != "hello signalling" {
		t.Fatalf("echo = %q", payload)
	}
}

func TestDialSignalUsesTLSForHTTPS(t *testing.T) {
	srv := httptest.NewTLSServer(http.HandlerFunc(testHandler))
	defer srv.Close()
	transport, ok := srv.Client().Transport.(*http.Transport)
	if !ok {
		t.Fatal("the TLS test server has no HTTP transport")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	conn, err := dialSignal(ctx, srv.URL, nil, transport.TLSClientConfig)
	if err != nil {
		t.Fatalf("dial signalling over TLS: %v", err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(3 * time.Second))
	if err := carrier.NewFrameWriter(conn).WriteFrame([]byte("secure")); err != nil {
		t.Fatal(err)
	}
	payload, err := carrier.NewFrameReader(conn).ReadFrame()
	if err != nil || string(payload) != "secure" {
		t.Fatalf("echo = %q, %v", payload, err)
	}
}

func TestDialSignalRejectsNonHTTPServerURL(t *testing.T) {
	if _, err := dialSignal(context.Background(), "unix:///tmp/chast.sock", nil, nil); err == nil ||
		!strings.Contains(err.Error(), "http://") {
		t.Fatalf("error = %v, want a URL scheme complaint", err)
	}
}
