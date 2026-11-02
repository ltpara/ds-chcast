package netproxy_test

import (
	"context"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestParseValid(t *testing.T) {
	cases := []struct {
		input string
		addr  string
	}{
		{"socks5://127.0.0.1:1080", "127.0.0.1:1080"},
		{"socks5h://proxy.example:1081", "proxy.example:1081"},
		{"socks5://user:secret@127.0.0.1:1080", "127.0.0.1:1080"},
		{"socks5://proxy.example", "proxy.example:1080"},
		{"127.0.0.1:9050", "127.0.0.1:9050"},
		{"  socks5://127.0.0.1:1080  ", "127.0.0.1:1080"},
	}
	for _, tc := range cases {
		proxy, err := netproxy.Parse(tc.input)
		if err != nil {
			t.Fatalf("Parse(%q): %v", tc.input, err)
		}
		if proxy.Address() != tc.addr {
			t.Fatalf("Parse(%q).Address() = %q, want %q", tc.input, proxy.Address(), tc.addr)
		}
		if strings.Contains(proxy.String(), "secret") {
			t.Fatalf("String() leaks the password: %q", proxy.String())
		}
	}
}

func TestParseEmpty(t *testing.T) {
	proxy, err := netproxy.Parse("   ")
	if err != nil || proxy != nil {
		t.Fatalf("Parse(empty) = %v, %v; want nil, nil", proxy, err)
	}
	if proxy.Address() != "" {
		t.Fatalf("nil proxy Address() = %q", proxy.Address())
	}
}

func TestParseInvalid(t *testing.T) {
	cases := []string{
		"http://127.0.0.1:8080",
		"socks5://",
		"socks5://127.0.0.1:0",
		"socks5://127.0.0.1:70000",
		"socks5://127.0.0.1:1080/path",
		"socks5://127.0.0.1:1080?x=1",
		"127.0.0.1",
		":1080",
	}
	for _, input := range cases {
		if _, err := netproxy.Parse(input); err == nil {
			t.Fatalf("Parse(%q) succeeded, want an error", input)
		}
	}
}

func TestDialContextThroughProxy(t *testing.T) {
	echo, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("echo listener: %v", err)
	}
	defer echo.Close()
	go func() {
		conn, err := echo.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		io.Copy(conn, conn)
	}()

	socks := testutil.StartSocks5(t, "", "")
	proxy, err := netproxy.Parse(socks.URL())
	if err != nil {
		t.Fatalf("parse proxy: %v", err)
	}
	_, port, _ := net.SplitHostPort(echo.Addr().String())
	// A hostname makes the client send ATYP=domain, so the proxy resolves it.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, err := proxy.DialContext(ctx, "tcp", net.JoinHostPort("localhost", port))
	if err != nil {
		t.Fatalf("dial through proxy: %v", err)
	}
	defer conn.Close()
	if _, err := conn.Write([]byte("ping")); err != nil {
		t.Fatalf("write: %v", err)
	}
	reply := make([]byte, 4)
	if _, err := io.ReadFull(conn, reply); err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(reply) != "ping" {
		t.Fatalf("echo = %q", reply)
	}
	targets := socks.Targets()
	if len(targets) != 1 || targets[0] != net.JoinHostPort("localhost", port) {
		t.Fatalf("proxy targets = %v, want localhost:%s", targets, port)
	}
}

func TestDialContextAuthenticated(t *testing.T) {
	socks := testutil.StartSocks5(t, "alice", "wonder")
	proxy, err := netproxy.Parse(socks.URL())
	if err != nil {
		t.Fatalf("parse proxy: %v", err)
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "ok")
	}))
	defer server.Close()
	res, err := proxy.HTTPClient(5 * time.Second).Get(server.URL)
	if err != nil {
		t.Fatalf("GET through authenticated proxy: %v", err)
	}
	defer res.Body.Close()
	body, _ := io.ReadAll(res.Body)
	if string(body) != "ok" {
		t.Fatalf("body = %q", body)
	}
	if len(socks.Targets()) != 1 {
		t.Fatalf("proxy targets = %v", socks.Targets())
	}
}

func TestHTTPClientThroughProxy(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "proxied")
	}))
	defer server.Close()
	socks := testutil.StartSocks5(t, "", "")
	proxy, err := netproxy.Parse(socks.URL())
	if err != nil {
		t.Fatalf("parse proxy: %v", err)
	}
	res, err := proxy.HTTPClient(5 * time.Second).Get(server.URL)
	if err != nil {
		t.Fatalf("GET through proxy: %v", err)
	}
	defer res.Body.Close()
	body, _ := io.ReadAll(res.Body)
	if string(body) != "proxied" {
		t.Fatalf("body = %q", body)
	}
	_, port, _ := net.SplitHostPort(strings.TrimPrefix(server.URL, "http://"))
	targets := socks.Targets()
	if len(targets) != 1 || targets[0] != net.JoinHostPort("127.0.0.1", port) {
		t.Fatalf("proxy targets = %v", targets)
	}
}

func TestHTTPClientNilProxy(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, strconv.Itoa(http.StatusOK))
	}))
	defer server.Close()
	socks := testutil.StartSocks5(t, "", "")
	var proxy *netproxy.Proxy
	res, err := proxy.HTTPClient(5 * time.Second).Get(server.URL)
	if err != nil {
		t.Fatalf("GET without proxy: %v", err)
	}
	defer res.Body.Close()
	if len(socks.Targets()) != 0 {
		t.Fatalf("unused proxy recorded %v", socks.Targets())
	}
}
