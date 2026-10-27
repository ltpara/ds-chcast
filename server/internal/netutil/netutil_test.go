package netutil

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestParsePorts(t *testing.T) {
	cases := []struct {
		in   string
		want []int
	}{
		{"41000", []int{41000}},
		{"41000-41002", []int{41000, 41001, 41002}},
		{"41000, 41002-41003", []int{41000, 41002, 41003}},
	}
	for _, tc := range cases {
		got, err := ParsePorts(tc.in)
		if err != nil {
			t.Fatalf("ParsePorts(%q): %v", tc.in, err)
		}
		if len(got) != len(tc.want) {
			t.Fatalf("ParsePorts(%q) = %v, want %v", tc.in, got, tc.want)
		}
		for i := range got {
			if got[i] != tc.want[i] {
				t.Fatalf("ParsePorts(%q) = %v, want %v", tc.in, got, tc.want)
			}
		}
	}
}

func TestParsePortsRejectsBadRanges(t *testing.T) {
	for _, in := range []string{"", "0", "65536", "41002-41000", "a-b", "41000-"} {
		if _, err := ParsePorts(in); err == nil {
			t.Fatalf("ParsePorts(%q) was accepted", in)
		}
	}
}

func writeSelfSigned(t *testing.T) (string, string) {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "chast-test"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, pub, priv)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(priv)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	certFile := filepath.Join(dir, "cert.pem")
	keyFile := filepath.Join(dir, "key.pem")
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
	if err := os.WriteFile(certFile, certPEM, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyFile, keyPEM, 0o600); err != nil {
		t.Fatal(err)
	}
	return certFile, keyFile
}

type testCA struct {
	cert *x509.Certificate
	key  ed25519.PrivateKey
	file string
}

func newTestCA(t *testing.T) *testCA {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber:          big.NewInt(100),
		Subject:               pkix.Name{CommonName: "chast-test-ca"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(time.Hour),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		BasicConstraintsValid: true,
		IsCA:                  true,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, pub, priv)
	if err != nil {
		t.Fatal(err)
	}
	cert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatal(err)
	}
	ca := &testCA{
		cert: cert,
		key:  priv,
		file: filepath.Join(t.TempDir(), "ca.pem"),
	}
	if err := os.WriteFile(ca.file, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	return ca
}

// issue signs a leaf certificate with the CA. Server certs carry the loopback
// IP SAN the tests dial; client certs carry ClientAuth only.
func (ca *testCA) issue(t *testing.T, server bool) (string, string) {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(time.Now().UnixNano()),
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
	}
	if server {
		template.Subject = pkix.Name{CommonName: "chast-test-server"}
		template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
		template.IPAddresses = []net.IP{net.ParseIP("127.0.0.1")}
	} else {
		template.Subject = pkix.Name{CommonName: "chast-test-client"}
		template.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
	}
	der, err := x509.CreateCertificate(rand.Reader, template, ca.cert, pub, ca.key)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(priv)
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	certFile := filepath.Join(dir, "cert.pem")
	keyFile := filepath.Join(dir, "key.pem")
	if err := os.WriteFile(certFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER}), 0o600); err != nil {
		t.Fatal(err)
	}
	return certFile, keyFile
}

func TestSetUDPReadBufferReportsEffectiveSize(t *testing.T) {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	if size, err := SetUDPReadBuffer(conn, 0); err != nil || size != 0 {
		t.Fatalf("disabled buffer = %d, %v", size, err)
	}
	size, err := SetUDPReadBuffer(conn, 1<<20)
	if err != nil {
		t.Fatalf("set read buffer: %v", err)
	}
	if size <= 0 {
		t.Fatalf("effective read buffer = %d", size)
	}
}

func TestLoadTLSConfig(t *testing.T) {
	if cfg, err := LoadTLSConfig("", "", ""); err != nil || cfg != nil {
		t.Fatalf("empty TLS flags = %v, %v", cfg, err)
	}
	if _, err := LoadTLSConfig("cert.pem", "", ""); err == nil {
		t.Fatal("certificate without a key was accepted")
	}
	if _, err := LoadTLSConfig("", "key.pem", ""); err == nil {
		t.Fatal("key without a certificate was accepted")
	}

	certFile, keyFile := writeSelfSigned(t)
	cfg, err := LoadTLSConfig(certFile, keyFile, "")
	if err != nil {
		t.Fatal(err)
	}
	if cfg.MinVersion != tls.VersionTLS12 {
		t.Fatalf("MinVersion = %#x", cfg.MinVersion)
	}
	if len(cfg.Certificates) != 1 {
		t.Fatalf("certificates = %d", len(cfg.Certificates))
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{
		Handler: http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			io.WriteString(w, "ok\n")
		}),
		TLSConfig: cfg,
	}
	go srv.ServeTLS(ln, "", "")
	defer srv.Close()

	client := &http.Client{Transport: &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
	}}
	resp, err := client.Get("https://" + ln.Addr().String() + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK || strings.TrimSpace(string(body)) != "ok" {
		t.Fatalf("HTTPS health = %d %q", resp.StatusCode, body)
	}
	if resp.TLS == nil || resp.TLS.Version < tls.VersionTLS12 {
		t.Fatalf("negotiated TLS = %+v", resp.TLS)
	}
}

func TestClientTLSConfigValidatesFlags(t *testing.T) {
	if cfg, err := ClientTLSConfig("", "", ""); err != nil || cfg != nil {
		t.Fatalf("empty client TLS flags = %v, %v", cfg, err)
	}
	if _, err := ClientTLSConfig("", "cert.pem", ""); err == nil {
		t.Fatal("client certificate without a key was accepted")
	}
	if _, err := ClientTLSConfig("", "", "key.pem"); err == nil {
		t.Fatal("client key without a certificate was accepted")
	}
	badCA := filepath.Join(t.TempDir(), "bad.pem")
	if err := os.WriteFile(badCA, []byte("not a certificate"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := ClientTLSConfig(badCA, "", ""); err == nil {
		t.Fatal("CA bundle without certificates was accepted")
	}
	if _, err := ClientTLSConfig(filepath.Join(t.TempDir(), "missing.pem"), "", ""); err == nil {
		t.Fatal("missing CA bundle was accepted")
	}

	ca := newTestCA(t)
	certFile, keyFile := ca.issue(t, false)
	cfg, err := ClientTLSConfig(ca.file, certFile, keyFile)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.RootCAs == nil {
		t.Fatal("custom CA did not populate RootCAs")
	}
	if len(cfg.Certificates) != 1 {
		t.Fatalf("client certificates = %d", len(cfg.Certificates))
	}
	if cfg.MinVersion != tls.VersionTLS12 {
		t.Fatalf("MinVersion = %#x", cfg.MinVersion)
	}
}

func TestLoadTLSConfigRequiresClientCertificate(t *testing.T) {
	ca := newTestCA(t)
	serverCert, serverKey := ca.issue(t, true)
	cfg, err := LoadTLSConfig(serverCert, serverKey, ca.file)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.ClientAuth != tls.RequireAndVerifyClientCert {
		t.Fatalf("ClientAuth = %v", cfg.ClientAuth)
	}
	if cfg.ClientCAs == nil {
		t.Fatal("ClientCAs is nil")
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &http.Server{
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.TLS == nil || len(r.TLS.PeerCertificates) == 0 {
				http.Error(w, "no peer certificate", http.StatusUnauthorized)
				return
			}
			io.WriteString(w, r.TLS.PeerCertificates[0].Subject.CommonName)
		}),
		TLSConfig: cfg,
	}
	go srv.ServeTLS(ln, "", "")
	defer srv.Close()
	url := "https://" + ln.Addr().String() + "/"

	anonCfg, err := ClientTLSConfig(ca.file, "", "")
	if err != nil {
		t.Fatal(err)
	}
	anon := &http.Client{Transport: &http.Transport{TLSClientConfig: anonCfg}}
	if resp, err := anon.Get(url); err == nil {
		resp.Body.Close()
		t.Fatal("request without a client certificate succeeded")
	}

	clientCert, clientKey := ca.issue(t, false)
	mtlsCfg, err := ClientTLSConfig(ca.file, clientCert, clientKey)
	if err != nil {
		t.Fatal(err)
	}
	authed := &http.Client{Transport: &http.Transport{TLSClientConfig: mtlsCfg}}
	resp, err := authed.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK || strings.TrimSpace(string(body)) != "chast-test-client" {
		t.Fatalf("mTLS response = %d %q", resp.StatusCode, body)
	}
}
