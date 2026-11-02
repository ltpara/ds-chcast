// Package netproxy turns the operator's SOCKS5 setting into a dialer for the
// two outbound paths that can cross a stream proxy: HTTP API requests and the
// TCP carriers. QUIC and the UDP relay stay direct: SOCKS5 cannot carry
// datagrams, and silently attempting them would leak the real address.
package netproxy

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	xproxy "golang.org/x/net/proxy"
)

// Proxy is a validated SOCKS5 endpoint. Its credentials never appear in logs;
// String and Address return only host:port.
type Proxy struct {
	addr   string
	dialer xproxy.ContextDialer
}

// Parse accepts socks5://[user:pass@]host[:port], socks5h:// (names are always
// resolved by the proxy, so h is only accepted for symmetry with browsers) and
// a bare host:port. Empty input means no proxy.
func Parse(input string) (*Proxy, error) {
	text := strings.TrimSpace(input)
	if text == "" {
		return nil, nil
	}
	address := text
	var user *url.Userinfo
	if strings.Contains(text, "://") {
		parsed, err := url.Parse(text)
		if err != nil {
			return nil, fmt.Errorf("代理地址无效: %w", err)
		}
		switch strings.ToLower(parsed.Scheme) {
		case "socks5", "socks5h":
		default:
			return nil, fmt.Errorf("代理只支持 socks5://，收到 %q", parsed.Scheme)
		}
		if parsed.Host == "" {
			return nil, fmt.Errorf("代理地址缺少主机")
		}
		if parsed.Path != "" && parsed.Path != "/" {
			return nil, fmt.Errorf("代理地址不能带路径")
		}
		if parsed.RawQuery != "" || parsed.Fragment != "" {
			return nil, fmt.Errorf("代理地址不能带查询或片段")
		}
		address = parsed.Host
		user = parsed.User
		if parsed.Port() == "" {
			address = net.JoinHostPort(parsed.Hostname(), "1080")
		}
	}
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return nil, fmt.Errorf("代理地址需要 host:port: %q", address)
	}
	if host == "" {
		return nil, fmt.Errorf("代理地址缺少主机")
	}
	number, err := strconv.Atoi(port)
	if err != nil || number < 1 || number > 65535 {
		return nil, fmt.Errorf("代理端口无效: %q", port)
	}
	var auth *xproxy.Auth
	if user != nil {
		password, _ := user.Password()
		auth = &xproxy.Auth{User: user.Username(), Password: password}
	}
	dialer, err := xproxy.SOCKS5("tcp", net.JoinHostPort(host, port), auth, xproxy.Direct)
	if err != nil {
		return nil, fmt.Errorf("SOCKS5 拨号器: %w", err)
	}
	contextDialer, ok := dialer.(xproxy.ContextDialer)
	if !ok {
		return nil, fmt.Errorf("SOCKS5 拨号器不支持 context")
	}
	return &Proxy{addr: net.JoinHostPort(host, port), dialer: contextDialer}, nil
}

// DialContext connects to address through the proxy. A hostname in address is
// sent to the proxy as a domain, so DNS happens on the proxy side.
func (p *Proxy) DialContext(ctx context.Context, network, address string) (net.Conn, error) {
	return p.dialer.DialContext(ctx, network, address)
}

// Address is the proxy endpoint without credentials.
func (p *Proxy) Address() string {
	if p == nil {
		return ""
	}
	return p.addr
}

// HTTPClient returns a client whose connections go through the proxy. A nil
// proxy yields a plain client, so callers do not need to branch.
func (p *Proxy) HTTPClient(timeout time.Duration) *http.Client {
	client := &http.Client{Timeout: timeout}
	if p == nil {
		return client
	}
	client.Transport = &http.Transport{DialContext: p.DialContext}
	return client
}

func (p *Proxy) String() string { return p.Address() }
