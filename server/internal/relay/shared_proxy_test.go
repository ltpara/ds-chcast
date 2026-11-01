package relay

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
)

// startCarrierTLSProxy puts a TLS-terminating HTTP reverse proxy in front of
// the shared carrier, the way nginx or a CDN edge is deployed. stripPrefix
// mirrors a location block that mounts the carrier below a path prefix.
func startCarrierTLSProxy(t *testing.T, backend, stripPrefix string) (string, *x509.CertPool) {
	t.Helper()
	backendURL, err := url.Parse("http://" + backend)
	if err != nil {
		t.Fatalf("backend url: %v", err)
	}
	proxy := httputil.NewSingleHostReverseProxy(backendURL)
	proxy.FlushInterval = -1
	director := proxy.Director
	proxy.Director = func(req *http.Request) {
		director(req)
		req.Host = backendURL.Host
		if stripPrefix != "" {
			req.URL.Path = strings.TrimPrefix(req.URL.Path, stripPrefix)
			if req.URL.Path == "" {
				req.URL.Path = "/"
			}
		}
	}
	cert, roots := selfSignedProxyCert(t)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("proxy listener: %v", err)
	}
	tlsLn := tls.NewListener(ln, &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	})
	srv := &http.Server{Handler: proxy, ReadHeaderTimeout: 5 * time.Second}
	go func() { _ = srv.Serve(tlsLn) }()
	t.Cleanup(func() { srv.Close() })
	return ln.Addr().String(), roots
}

func selfSignedProxyCert(t *testing.T) (tls.Certificate, *x509.CertPool) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("proxy key: %v", err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()),
		Subject:      pkix.Name{CommonName: "carrier-proxy"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"carrier-proxy"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("proxy certificate: %v", err)
	}
	leaf, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse proxy certificate: %v", err)
	}
	roots := x509.NewCertPool()
	roots.AddCert(leaf)
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, roots
}

// dialProxyWebSocket upgrades a wss:// request through the proxy and returns
// the connection and a buffered reader positioned after the 101 response.
func dialProxyWebSocket(t *testing.T, addr, path string, roots *x509.CertPool) (net.Conn, *bufio.Reader) {
	t.Helper()
	conn, err := tls.Dial("tcp", addr, &tls.Config{
		RootCAs:    roots,
		ServerName: "carrier-proxy",
		MinVersion: tls.VersionTLS12,
	})
	if err != nil {
		t.Fatalf("dial proxy: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	request := "GET " + path + " HTTP/1.1\r\n" +
		"Host: carrier-proxy\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
		"Sec-WebSocket-Version: 13\r\n" +
		"Sec-WebSocket-Protocol: chast-carrier.v1\r\n\r\n"
	if _, err := io.WriteString(conn, request); err != nil {
		t.Fatalf("write upgrade through proxy: %v", err)
	}
	br := bufio.NewReader(conn)
	conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		t.Fatalf("read upgrade through proxy: %v", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("proxy upgrade status = %s", resp.Status)
	}
	if got := resp.Header.Get("Sec-WebSocket-Protocol"); got != carrier.WSSubprotocol {
		t.Fatalf("proxy subprotocol = %q", got)
	}
	return conn, br
}

func TestSharedCarrierWebSocketThroughTLSProxy(t *testing.T) {
	cases := []struct {
		name       string
		strip      string
		requestURI string
	}{
		{name: "root", requestURI: carrier.WSPath},
		{name: "prefixed", strip: "/carrier", requestURI: "/carrier" + carrier.WSPath},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-ws-proxy")
			host := openSharedHost(t, addr, "devbox", secret)
			proxyAddr, roots := startCarrierTLSProxy(t, addr, tc.strip)
			conn, br := dialProxyWebSocket(t, proxyAddr, tc.requestURI, roots)

			hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: token})
			if _, err := conn.Write(clientBinaryFrame(hello)); err != nil {
				t.Fatalf("write hello through proxy: %v", err)
			}
			uplink := []byte("quic-initial-through-tls-proxy")
			if _, err := conn.Write(clientBinaryFrame(uplink)); err != nil {
				t.Fatalf("write uplink through proxy: %v", err)
			}
			if got := host.read(t); string(got) != string(uplink) {
				t.Fatalf("host received %q", got)
			}
			downlink := []byte("quic-server-flight-through-tls-proxy")
			if err := host.send.WriteFrame(downlink); err != nil {
				t.Fatalf("host send: %v", err)
			}
			if got := readWSBinary(t, br); string(got) != string(downlink) {
				t.Fatalf("viewer received %q", got)
			}
			waitForSharedStats(t, m, func(s SharedCarrierStats) bool {
				return s.Hosts == 1 && s.Viewers == 1 &&
					s.ToHostBytes == uint64(len(uplink)) &&
					s.ToViewerBytes == uint64(len(downlink))
			})
		})
	}
}
