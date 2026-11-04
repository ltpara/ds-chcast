package netproxy

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/base64"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func TestParse(t *testing.T) {
	valid := []string{
		"http://127.0.0.1:8080",
		"https://proxy.example:8443",
		"socks5://127.0.0.1:1080",
		"socks5h://user:pass@proxy.example:1080",
	}
	for _, raw := range valid {
		proxy, err := Parse(raw)
		if err != nil {
			t.Fatalf("Parse(%q): %v", raw, err)
		}
		if proxy.String() == "" || strings.Contains(proxy.String(), "pass") {
			t.Fatalf("Parse(%q) leaked credentials: %q", raw, proxy.String())
		}
	}
	invalid := []string{
		"",
		"proxy.example:1080",
		"ftp://proxy.example:21",
		"http://proxy.example",
		"http://proxy.example:0",
		"http://proxy.example:abc",
		"http://proxy.example:8080/path",
		"http://proxy.example:8080?x=1",
	}
	for _, raw := range invalid {
		if _, err := Parse(raw); err == nil {
			t.Fatalf("Parse(%q) accepted an invalid proxy", raw)
		}
	}
}

// startSOCKS5 is a minimal RFC 1928/1929 server for the dialer tests.
func startSOCKS5(t *testing.T, username, password string) net.Listener {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("socks5 listener: %v", err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				br := bufio.NewReader(conn)
				header := make([]byte, 2)
				if _, err := io.ReadFull(br, header); err != nil || header[0] != 5 {
					return
				}
				methods := make([]byte, int(header[1]))
				if _, err := io.ReadFull(br, methods); err != nil {
					return
				}
				if username == "" {
					if _, err := conn.Write([]byte{5, 0}); err != nil {
						return
					}
				} else {
					if _, err := conn.Write([]byte{5, 2}); err != nil {
						return
					}
					auth := make([]byte, 2)
					if _, err := io.ReadFull(br, auth); err != nil || auth[0] != 1 {
						return
					}
					user := make([]byte, int(auth[1]))
					if _, err := io.ReadFull(br, user); err != nil {
						return
					}
					size := make([]byte, 1)
					if _, err := io.ReadFull(br, size); err != nil {
						return
					}
					pass := make([]byte, int(size[0]))
					if _, err := io.ReadFull(br, pass); err != nil {
						return
					}
					if string(user) != username || string(pass) != password {
						conn.Write([]byte{1, 1})
						return
					}
					conn.Write([]byte{1, 0})
				}
				request := make([]byte, 4)
				if _, err := io.ReadFull(br, request); err != nil || request[0] != 5 || request[1] != 1 {
					return
				}
				var host string
				switch request[3] {
				case 1:
					ip := make([]byte, 4)
					io.ReadFull(br, ip)
					host = net.IP(ip).String()
				case 4:
					ip := make([]byte, 16)
					io.ReadFull(br, ip)
					host = net.IP(ip).String()
				case 3:
					size := make([]byte, 1)
					io.ReadFull(br, size)
					name := make([]byte, int(size[0]))
					io.ReadFull(br, name)
					host = string(name)
				default:
					return
				}
				port := make([]byte, 2)
				io.ReadFull(br, port)
				target := net.JoinHostPort(host, fmt.Sprintf("%d", int(port[0])<<8|int(port[1])))
				upstream, err := net.Dial("tcp", target)
				if err != nil {
					conn.Write([]byte{5, 5, 0, 1, 0, 0, 0, 0, 0, 0})
					return
				}
				defer upstream.Close()
				conn.Write([]byte{5, 0, 0, 1, 0, 0, 0, 0, 0, 0})
				go io.Copy(upstream, br)
				io.Copy(conn, upstream)
			}(conn)
		}
	}()
	return listener
}

// startConnectProxy accepts CONNECT and tunnels the stream.
func startConnectProxy(t *testing.T, username, password string) net.Listener {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("connect proxy listener: %v", err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				br := bufio.NewReader(conn)
				request, err := http.ReadRequest(br)
				if err != nil {
					return
				}
				if request.Method != http.MethodConnect {
					forwardHTTP(conn, request)
					return
				}
				if username != "" {
					want := "Basic " + base64.StdEncoding.EncodeToString([]byte(username+":"+password))
					if request.Header.Get("Proxy-Authorization") != want {
						io.WriteString(conn, "HTTP/1.1 407 Proxy Authentication Required\r\n\r\n")
						return
					}
				}
				upstream, err := net.Dial("tcp", request.Host)
				if err != nil {
					io.WriteString(conn, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
					return
				}
				defer upstream.Close()
				io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\n")
				go io.Copy(upstream, br)
				io.Copy(conn, upstream)
			}(conn)
		}
	}()
	return listener
}

func forwardHTTP(client net.Conn, request *http.Request) {
	target := request.URL.Host
	if target == "" {
		io.WriteString(client, "HTTP/1.1 400 Bad Request\r\n\r\n")
		return
	}
	upstream, err := net.Dial("tcp", target)
	if err != nil {
		io.WriteString(client, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
		return
	}
	defer upstream.Close()
	request.RequestURI = ""
	request.URL.Scheme = ""
	request.URL.Host = ""
	request.Header.Del("Proxy-Authorization")
	request.Header.Set("Connection", "close")
	if err := request.Write(upstream); err != nil {
		return
	}
	response, err := http.ReadResponse(bufio.NewReader(upstream), request)
	if err != nil {
		io.WriteString(client, "HTTP/1.1 502 Bad Gateway\r\n\r\n")
		return
	}
	defer response.Body.Close()
	response.Close = true
	response.Write(client)
}

func startTCPEcho(t *testing.T) net.Listener {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("echo listener: %v", err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				io.Copy(conn, conn)
			}(conn)
		}
	}()
	return listener
}

func roundTrip(t *testing.T, proxy *Proxy, target, payload string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, err := proxy.DialContext(ctx, "tcp", target)
	if err != nil {
		t.Fatalf("dial through %s: %v", proxy, err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := io.WriteString(conn, payload); err != nil {
		t.Fatalf("write: %v", err)
	}
	reply := make([]byte, len(payload))
	if _, err := io.ReadFull(conn, reply); err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(reply) != payload {
		t.Fatalf("reply = %q, want %q", reply, payload)
	}
}

func TestDialThroughSOCKS5(t *testing.T) {
	echo := startTCPEcho(t)
	proxyServer := startSOCKS5(t, "", "")
	proxy, err := Parse("socks5://" + proxyServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	roundTrip(t, proxy, echo.Addr().String(), "socks5-ip")
	// A name forces the domain address type; the proxy resolves it.
	roundTrip(t, proxy, net.JoinHostPort("localhost", portOf(t, echo.Addr())), "socks5-name")
}

func TestDialThroughSOCKS5WithPassword(t *testing.T) {
	echo := startTCPEcho(t)
	proxyServer := startSOCKS5(t, "alice", "secret")
	proxy, err := Parse("socks5h://alice:secret@" + proxyServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	roundTrip(t, proxy, echo.Addr().String(), "authenticated")
}

func TestDialThroughHTTPConnect(t *testing.T) {
	echo := startTCPEcho(t)
	proxyServer := startConnectProxy(t, "alice", "secret")
	proxy, err := Parse("http://alice:secret@" + proxyServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	roundTrip(t, proxy, echo.Addr().String(), "connect")
}

func TestDialRejectsInjectedTarget(t *testing.T) {
	proxyServer := startConnectProxy(t, "", "")
	proxy, err := Parse("http://" + proxyServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err = proxy.DialContext(ctx, "tcp", "evil\rX:443")
	if err == nil {
		t.Fatal("dial accepted a target with control characters")
	}
	if !strings.Contains(err.Error(), "invalid character") {
		t.Fatalf("error = %v, want an invalid-character error", err)
	}
}

func TestHTTPTransportUsesProxy(t *testing.T) {
	var seen string
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		seen = r.URL.Path
		io.WriteString(w, "ok")
	}))
	defer target.Close()
	proxyServer := startConnectProxy(t, "", "")
	proxy, err := Parse("http://" + proxyServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	client := &http.Client{Transport: proxy.HTTPTransport(nil), Timeout: 5 * time.Second}
	response, err := client.Get(target.URL + "/v1/server")
	if err != nil {
		t.Fatalf("GET through proxy: %v", err)
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(response.Body)
	if response.StatusCode != http.StatusOK || string(body) != "ok" || seen != "/v1/server" {
		t.Fatalf("proxy reply = %d %q, path %q", response.StatusCode, body, seen)
	}
}

func TestHTTPTransportTLSOrigin(t *testing.T) {
	target := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "secure")
	}))
	defer target.Close()
	proxyServer := startConnectProxy(t, "", "")
	proxy, err := Parse("http://" + proxyServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	// The self-signed test origin is only reachable with its own root; the
	// proxy itself is still exercised by the CONNECT.
	client := &http.Client{
		Transport: proxy.HTTPTransport(&tls.Config{InsecureSkipVerify: true}),
		Timeout:   5 * time.Second,
	}
	response, err := client.Get(target.URL)
	if err != nil {
		t.Fatalf("GET TLS origin through proxy: %v", err)
	}
	defer response.Body.Close()
	body, _ := io.ReadAll(response.Body)
	if string(body) != "secure" {
		t.Fatalf("body = %q", body)
	}
}

func TestBridgeForwardsHTTPAndConnect(t *testing.T) {
	target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "bridge-ok")
	}))
	defer target.Close()
	echo := startTCPEcho(t)
	upstreamServer := startConnectProxy(t, "", "")
	upstream, err := Parse("http://" + upstreamServer.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go Serve(ctx, listener, upstream, log)

	bridgeURL, _ := url.Parse("http://" + listener.Addr().String())
	transport := &http.Transport{Proxy: http.ProxyURL(bridgeURL)}
	client := &http.Client{Transport: transport, Timeout: 5 * time.Second}
	response, err := client.Get(target.URL + "/through-bridge")
	if err != nil {
		t.Fatalf("HTTP through bridge: %v", err)
	}
	body, _ := io.ReadAll(response.Body)
	response.Body.Close()
	if string(body) != "bridge-ok" {
		t.Fatalf("bridge body = %q", body)
	}

	connectTransport := &http.Transport{Proxy: http.ProxyURL(bridgeURL)}
	connectClient := &http.Client{Transport: connectTransport, Timeout: 5 * time.Second}
	httpsTarget := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "bridge-tls")
	}))
	defer httpsTarget.Close()
	connectClient.Transport.(*http.Transport).TLSClientConfig = &tls.Config{InsecureSkipVerify: true}
	response, err = connectClient.Get(httpsTarget.URL)
	if err != nil {
		t.Fatalf("CONNECT through bridge: %v", err)
	}
	body, _ = io.ReadAll(response.Body)
	response.Body.Close()
	if string(body) != "bridge-tls" {
		t.Fatalf("bridge TLS body = %q", body)
	}

	// The same bridge must carry a raw stream for the carrier link.
	proxyThroughBridge, err := Parse("http://" + listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	roundTrip(t, proxyThroughBridge, echo.Addr().String(), "carrier-through-bridge")
}

func portOf(t *testing.T, addr net.Addr) string {
	t.Helper()
	_, port, err := net.SplitHostPort(addr.String())
	if err != nil {
		t.Fatal(err)
	}
	return port
}
