package acmecert

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/pem"
	"io"
	"log/slog"
	"math/big"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func discardLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestParseDomains(t *testing.T) {
	got, err := ParseDomains(" Example.COM , rd.example.com. ,example.com,, ")
	if err != nil {
		t.Fatalf("ParseDomains: %v", err)
	}
	want := []string{"example.com", "rd.example.com"}
	if len(got) != len(want) {
		t.Fatalf("domains = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("domains = %v, want %v", got, want)
		}
	}
	for _, bad := range []string{"", " , ", "*.example.com", "127.0.0.1", "localhost", "example.com:443", "bad_name.example.com"} {
		if _, err := ParseDomains(bad); err == nil {
			t.Fatalf("ParseDomains(%q) accepted an invalid domain", bad)
		}
	}
}

func TestParseEABKey(t *testing.T) {
	key := []byte("0123456789abcdef0123456789abcdef")
	for _, encoded := range []string{
		base64.RawURLEncoding.EncodeToString(key),
		base64.URLEncoding.EncodeToString(key),
		base64.StdEncoding.EncodeToString(key),
		" " + base64.RawURLEncoding.EncodeToString(key) + " ",
	} {
		got, err := ParseEABKey(encoded)
		if err != nil {
			t.Fatalf("ParseEABKey(%q): %v", encoded, err)
		}
		if string(got) != string(key) {
			t.Fatalf("ParseEABKey(%q) = %q", encoded, got)
		}
	}
	for _, bad := range []string{"", "   ", "not-base64!!"} {
		if _, err := ParseEABKey(bad); err == nil {
			t.Fatalf("ParseEABKey(%q) accepted an invalid key", bad)
		}
	}
}

func TestNewValidation(t *testing.T) {
	cases := []Config{
		{Domains: nil, CacheDir: t.TempDir()},
		{Domains: []string{"acme.test"}},
		{Domains: []string{"*.acme.test"}, CacheDir: t.TempDir()},
		{Domains: []string{"acme.test"}, CacheDir: t.TempDir(), EABKID: "kid"},
		{Domains: []string{"acme.test"}, CacheDir: t.TempDir(), EABKey: []byte("key")},
	}
	for i, cfg := range cases {
		cfg.Logger = discardLogger()
		if _, err := New(cfg); err == nil {
			t.Fatalf("case %d: New accepted an invalid config", i)
		}
	}
}

func TestTLSConfigServesCachedCertificate(t *testing.T) {
	dir := t.TempDir()
	writeCertCache(t, dir, "acme.test")
	m, err := New(Config{Domains: []string{"acme.test"}, CacheDir: dir, Logger: discardLogger()})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	cfg, err := m.TLSConfig("", 0)
	if err != nil {
		t.Fatalf("TLSConfig: %v", err)
	}
	if cfg.MinVersion != tls.VersionTLS12 {
		t.Fatalf("MinVersion = %x", cfg.MinVersion)
	}
	// A modern client advertises ECDSA suites, so autocert looks up the
	// ECDSA cache entry instead of the "+rsa" fallback.
	hello := &tls.ClientHelloInfo{
		ServerName:       "acme.test",
		SignatureSchemes: []tls.SignatureScheme{tls.ECDSAWithP256AndSHA256},
		SupportedCurves:  []tls.CurveID{tls.CurveP256},
		CipherSuites:     []uint16{tls.TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256},
	}
	cert, err := cfg.GetCertificate(hello)
	if err != nil {
		t.Fatalf("GetCertificate: %v", err)
	}
	if cert == nil || cert.Leaf == nil || cert.Leaf.Subject.CommonName != "acme.test" {
		t.Fatalf("unexpected certificate %+v", cert)
	}
	if _, err := cfg.GetCertificate(&tls.ClientHelloInfo{ServerName: "other.test"}); err == nil {
		t.Fatal("GetCertificate accepted a host outside the whitelist")
	}
}

func TestTLSConfigClientCA(t *testing.T) {
	dir := t.TempDir()
	writeCertCache(t, dir, "acme.test")
	m, err := New(Config{Domains: []string{"acme.test"}, CacheDir: dir, Logger: discardLogger()})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	caFile := filepath.Join(dir, "ca.pem")
	data, err := os.ReadFile(filepath.Join(dir, "acme.test"))
	if err != nil {
		t.Fatalf("read cache: %v", err)
	}
	var certPEM []byte
	rest := data
	for {
		var block *pem.Block
		block, rest = pem.Decode(rest)
		if block == nil {
			break
		}
		if block.Type == "CERTIFICATE" {
			certPEM = pem.EncodeToMemory(block)
			break
		}
	}
	if len(certPEM) == 0 {
		t.Fatal("cache has no certificate")
	}
	if err := os.WriteFile(caFile, certPEM, 0o600); err != nil {
		t.Fatalf("write CA: %v", err)
	}
	cfg, err := m.TLSConfig(caFile, 0)
	if err != nil {
		t.Fatalf("TLSConfig: %v", err)
	}
	if cfg.ClientAuth != tls.RequireAndVerifyClientCert || cfg.ClientCAs == nil {
		t.Fatalf("client auth = %v, pool = %v", cfg.ClientAuth, cfg.ClientCAs)
	}
	if _, err := m.TLSConfig(filepath.Join(dir, "missing.pem"), 0); err == nil {
		t.Fatal("TLSConfig accepted a missing client CA")
	}
}

func TestTLSConfigClientCAHotReload(t *testing.T) {
	dir := t.TempDir()
	writeCertCache(t, dir, "acme.test")
	caA := newTestCA(t, "A")
	caB := newTestCA(t, "B")
	caFile := filepath.Join(dir, "client-ca.pem")
	if err := os.WriteFile(caFile, caA.pem, 0o600); err != nil {
		t.Fatalf("write CA: %v", err)
	}
	m, err := New(Config{Domains: []string{"acme.test"}, CacheDir: dir, Logger: discardLogger()})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	cfg, err := m.TLSConfig(caFile, 20*time.Millisecond)
	if err != nil {
		t.Fatalf("TLSConfig: %v", err)
	}
	if !clientCAVerifies(t, cfg, caA) || clientCAVerifies(t, cfg, caB) {
		t.Fatal("initial pool does not match the seeded client CA")
	}

	if err := os.WriteFile(caFile, caB.pem, 0o600); err != nil {
		t.Fatalf("overwrite CA: %v", err)
	}
	time.Sleep(50 * time.Millisecond)
	if !clientCAVerifies(t, cfg, caB) || clientCAVerifies(t, cfg, caA) {
		t.Fatal("rotated client CA was not picked up")
	}

	if err := os.WriteFile(caFile, []byte("not a certificate"), 0o600); err != nil {
		t.Fatalf("break CA: %v", err)
	}
	time.Sleep(50 * time.Millisecond)
	if !clientCAVerifies(t, cfg, caB) {
		t.Fatal("a broken replacement dropped the previous client CA pool")
	}
}

func TestTLSConfigClientCAStaticInterval(t *testing.T) {
	dir := t.TempDir()
	writeCertCache(t, dir, "acme.test")
	caA := newTestCA(t, "A")
	caB := newTestCA(t, "B")
	caFile := filepath.Join(dir, "client-ca.pem")
	if err := os.WriteFile(caFile, caA.pem, 0o600); err != nil {
		t.Fatalf("write CA: %v", err)
	}
	m, err := New(Config{Domains: []string{"acme.test"}, CacheDir: dir, Logger: discardLogger()})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	cfg, err := m.TLSConfig(caFile, 0)
	if err != nil {
		t.Fatalf("TLSConfig: %v", err)
	}
	if err := os.WriteFile(caFile, caB.pem, 0o600); err != nil {
		t.Fatalf("overwrite CA: %v", err)
	}
	time.Sleep(20 * time.Millisecond)
	if !clientCAVerifies(t, cfg, caA) || clientCAVerifies(t, cfg, caB) {
		t.Fatal("interval 0 reloaded the client CA")
	}
}

type testCA struct {
	pem    []byte
	client *x509.Certificate
}

func newTestCA(t *testing.T, name string) *testCA {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate CA key: %v", err)
	}
	tmpl := &x509.Certificate{
		SerialNumber:          big.NewInt(1),
		Subject:               pkix.Name{CommonName: name + " test CA"},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(time.Hour),
		IsCA:                  true,
		BasicConstraintsValid: true,
		KeyUsage:              x509.KeyUsageCertSign,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create CA certificate: %v", err)
	}
	caCert, err := x509.ParseCertificate(der)
	if err != nil {
		t.Fatalf("parse CA certificate: %v", err)
	}
	clientKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate client key: %v", err)
	}
	clientTmpl := &x509.Certificate{
		SerialNumber: big.NewInt(2),
		Subject:      pkix.Name{CommonName: name + " client"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	clientDER, err := x509.CreateCertificate(rand.Reader, clientTmpl, caCert, &clientKey.PublicKey, key)
	if err != nil {
		t.Fatalf("create client certificate: %v", err)
	}
	clientCert, err := x509.ParseCertificate(clientDER)
	if err != nil {
		t.Fatalf("parse client certificate: %v", err)
	}
	return &testCA{
		pem:    pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}),
		client: clientCert,
	}
}

func clientCAVerifies(t *testing.T, cfg *tls.Config, ca *testCA) bool {
	t.Helper()
	per, err := cfg.GetConfigForClient(&tls.ClientHelloInfo{ServerName: "acme.test"})
	if err != nil {
		t.Fatalf("GetConfigForClient: %v", err)
	}
	if per == nil || per == cfg {
		t.Fatal("GetConfigForClient did not return a per-handshake clone")
	}
	if per.ClientAuth != tls.RequireAndVerifyClientCert || per.ClientCAs == nil {
		t.Fatalf("clone client auth = %v, pool = %v", per.ClientAuth, per.ClientCAs)
	}
	_, err = ca.client.Verify(x509.VerifyOptions{
		Roots:     per.ClientCAs,
		KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	})
	return err == nil
}

func TestStartHTTPServesChallengesAndRedirects(t *testing.T) {
	dir := t.TempDir()
	writeCertCache(t, dir, "acme.test")
	m, err := New(Config{Domains: []string{"acme.test"}, CacheDir: dir, HTTPListen: "127.0.0.1:0", Logger: discardLogger()})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := m.StartHTTP(); err != nil {
		t.Fatalf("StartHTTP: %v", err)
	}
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()
		_ = m.Shutdown(ctx)
	}()
	addr := m.HTTPAddr()
	if addr == "" {
		t.Fatal("HTTPAddr is empty")
	}
	client := &http.Client{
		Timeout:       5 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
	}
	request := func(path, host string) *http.Response {
		req, err := http.NewRequest(http.MethodGet, "http://"+addr+path, nil)
		if err != nil {
			t.Fatalf("new request: %v", err)
		}
		req.Host = host
		resp, err := client.Do(req)
		if err != nil {
			t.Fatalf("request %s: %v", path, err)
		}
		return resp
	}
	resp := request("/", "acme.test")
	resp.Body.Close()
	if resp.StatusCode != http.StatusFound || resp.Header.Get("Location") != "https://acme.test/" {
		t.Fatalf("redirect = %d %q", resp.StatusCode, resp.Header.Get("Location"))
	}
	resp = request("/.well-known/acme-challenge/missing", "acme.test")
	resp.Body.Close()
	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("missing token status = %d", resp.StatusCode)
	}
	resp = request("/.well-known/acme-challenge/missing", "evil.test")
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("foreign host status = %d", resp.StatusCode)
	}
}

func writeCertCache(t *testing.T, dir, domain string) {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatalf("generate key: %v", err)
	}
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: domain},
		DNSNames:     []string{domain},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		t.Fatalf("create certificate: %v", err)
	}
	keyDER, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		t.Fatalf("marshal key: %v", err)
	}
	data := append(
		pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER}),
		pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})...,
	)
	if err := os.WriteFile(filepath.Join(dir, domain), data, 0o600); err != nil {
		t.Fatalf("write cache: %v", err)
	}
}
