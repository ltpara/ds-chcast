package agent

import (
	"bufio"
	"context"
	"io"
	"net"
	"net/http"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type countingProxy struct {
	listener net.Listener
	requests atomic.Int64
	connects atomic.Int64
}

// startCountingProxy is a plain HTTP proxy: absolute-form requests are
// forwarded, CONNECT is tunneled. It counts both so tests can prove the agent
// used the proxy instead of connecting directly.
func startCountingProxy(t *testing.T) *countingProxy {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("proxy listener: %v", err)
	}
	proxy := &countingProxy{listener: listener}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go proxy.handle(conn)
		}
	}()
	return proxy
}

func (p *countingProxy) handle(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	request, err := http.ReadRequest(reader)
	if err != nil {
		return
	}
	if request.Method == http.MethodConnect {
		p.connects.Add(1)
		upstream, err := net.Dial("tcp", request.Host)
		if err != nil {
			io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
			return
		}
		defer upstream.Close()
		io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\n")
		go io.Copy(upstream, reader)
		io.Copy(conn, upstream)
		return
	}
	p.requests.Add(1)
	target := request.URL.Host
	upstream, err := net.Dial("tcp", target)
	if err != nil {
		io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
		return
	}
	defer upstream.Close()
	request.RequestURI = ""
	request.URL.Scheme = ""
	request.URL.Host = ""
	request.Header.Set("Connection", "close")
	if err := request.Write(upstream); err != nil {
		return
	}
	response, err := http.ReadResponse(bufio.NewReader(upstream), request)
	if err != nil {
		io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
		return
	}
	defer response.Body.Close()
	response.Close = true
	response.Write(conn)
}

func (p *countingProxy) address() string { return p.listener.Addr().String() }

func TestProxyViewerModeError(t *testing.T) {
	if err := proxyViewerModeError("devbox", ModeDirect, true); err == nil {
		t.Fatal("direct mode with a proxy was accepted")
	}
	if err := proxyViewerModeError("devbox", ModeAuto, false); err == nil {
		t.Fatal("a proxy without a carrier was accepted")
	}
	for _, mode := range []string{ModeAuto, ModeRelay, ModeCarrier} {
		if err := proxyViewerModeError("devbox", mode, true); err != nil {
			t.Fatalf("%s with a carrier: %v", mode, err)
		}
	}
}

// TestProxyCarriesControlPlaneAndCarrier runs the whole host/viewer path with
// both agents behind the same HTTP proxy. Registration, lookup and both ends of
// the TCP carrier have to reach the proxy for the round trip to work.
func TestProxyCarriesControlPlaneAndCarrier(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	localCarrier := startEchoCarrier(t)
	proxyServer := startCountingProxy(t)
	proxy, err := netproxy.Parse("http://" + proxyServer.address())
	if err != nil {
		t.Fatal(err)
	}

	// The host registers and keeps its carrier link through the proxy.
	host, err := StartHost(ctx, HostConfig{
		ServerURL:   s.URL,
		ServerKey:   testutil.ServerKey,
		Proxy:       proxy,
		ID:          "devbox",
		Key:         "host-secret",
		LocalAddr:   echo.LocalAddr().String(),
		CarrierAddr: localCarrier.Addr().String(),
		ListenAddr:  "127.0.0.1:0",
		Keepalive:   300 * time.Millisecond,
		Logger:      quietLogger(),
	})
	if err != nil {
		t.Fatalf("start host through proxy: %v", err)
	}
	t.Cleanup(host.Close)
	readyCtx, readyCancel := context.WithTimeout(ctx, 5*time.Second)
	defer readyCancel()
	if err := host.WaitReady(readyCtx); err != nil {
		t.Fatalf("host readiness: %v", err)
	}
	waitForSharedHost(t, s)

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		Proxy:      proxy,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeAuto,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer through proxy: %v", err)
	}
	t.Cleanup(viewer.Close)
	if info := viewer.PathInfo(); info.Path != "carrier" {
		t.Fatalf("proxy should force the carrier, got %+v", info)
	}
	viewerRoundTrip(t, viewer, []byte("datagram-through-proxy"))

	deadline := time.Now().Add(5 * time.Second)
	for proxyServer.requests.Load() < 2 || proxyServer.connects.Load() < 2 {
		if time.Now().After(deadline) {
			t.Fatalf("agents did not use the proxy for every path: requests=%d connects=%d",
				proxyServer.requests.Load(), proxyServer.connects.Load())
		}
		time.Sleep(20 * time.Millisecond)
	}
}
