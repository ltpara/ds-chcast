package netutil

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"io"
	"log/slog"
	"math/big"
	"net"
	"net/http"
	"os"
	"testing"
	"time"
)

func tlsReloadLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func tlsReloadSerial(t *testing.T, certFile string) *big.Int {
	t.Helper()
	data, err := os.ReadFile(certFile)
	if err != nil {
		t.Fatal(err)
	}
	block, _ := pem.Decode(data)
	if block == nil {
		t.Fatalf("%s has no PEM block", certFile)
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		t.Fatal(err)
	}
	return cert.SerialNumber
}

func tlsReloadServe(t *testing.T, cfg *tls.Config) string {
	t.Helper()
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
	t.Cleanup(func() { srv.Close() })
	return ln.Addr().String()
}

func tlsReloadPeerSerial(addr string) (*big.Int, error) {
	client := &http.Client{
		Timeout: 2 * time.Second,
		Transport: &http.Transport{
			DisableKeepAlives: true,
			TLSClientConfig:   &tls.Config{InsecureSkipVerify: true},
		},
	}
	resp, err := client.Get("https://" + addr + "/")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	io.Copy(io.Discard, resp.Body)
	if resp.TLS == nil || len(resp.TLS.PeerCertificates) == 0 {
		return nil, errNoPeerCertificate
	}
	return resp.TLS.PeerCertificates[0].SerialNumber, nil
}

type peerSerialError struct{}

var errNoPeerCertificate = peerSerialError{}

func (peerSerialError) Error() string { return "server presented no certificate" }
func tlsReloadAssertSerial(t *testing.T, addr string, want *big.Int) {
	t.Helper()
	got, err := tlsReloadPeerSerial(addr)
	if err != nil {
		t.Fatalf("handshake: %v", err)
	}
	if got.Cmp(want) != 0 {
		t.Fatalf("served serial %s, want %s", got, want)
	}
}

func tlsReloadWaitSerial(t *testing.T, addr string, want *big.Int) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	var last error
	for time.Now().Before(deadline) {
		got, err := tlsReloadPeerSerial(addr)
		last = err
		if err == nil && got.Cmp(want) == 0 {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("certificate was not reloaded within the deadline (last error: %v)", last)
}

func tlsReloadCopy(t *testing.T, src, dst string) {
	t.Helper()
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(dst, data, 0o600); err != nil {
		t.Fatal(err)
	}
}

func TestReloadingTLSConfigRotatesCertificate(t *testing.T) {
	ca := newTestCA(t)
	certFile, keyFile := ca.issue(t, true)
	first := tlsReloadSerial(t, certFile)
	cfg, err := LoadReloadingTLSConfig(certFile, keyFile, "", tlsReloadLogger(), 20*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	addr := tlsReloadServe(t, cfg)
	tlsReloadAssertSerial(t, addr, first)

	rotatedCert, rotatedKey := ca.issue(t, true)
	second := tlsReloadSerial(t, rotatedCert)
	if second.Cmp(first) == 0 {
		t.Fatal("issued certificates share a serial")
	}
	tlsReloadCopy(t, rotatedCert, certFile)
	tlsReloadCopy(t, rotatedKey, keyFile)
	tlsReloadWaitSerial(t, addr, second)
}

func TestReloadingTLSConfigKeepsPreviousCertificateOnBadReplacement(t *testing.T) {
	ca := newTestCA(t)
	certFile, keyFile := ca.issue(t, true)
	first := tlsReloadSerial(t, certFile)
	cfg, err := LoadReloadingTLSConfig(certFile, keyFile, "", tlsReloadLogger(), 20*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	addr := tlsReloadServe(t, cfg)
	tlsReloadAssertSerial(t, addr, first)

	if err := os.WriteFile(certFile, []byte("not a certificate"), 0o600); err != nil {
		t.Fatal(err)
	}
	settle := time.Now().Add(150 * time.Millisecond)
	for time.Now().Before(settle) {
		tlsReloadAssertSerial(t, addr, first)
		time.Sleep(10 * time.Millisecond)
	}

	rotatedCert, rotatedKey := ca.issue(t, true)
	recovered := tlsReloadSerial(t, rotatedCert)
	tlsReloadCopy(t, rotatedCert, certFile)
	tlsReloadCopy(t, rotatedKey, keyFile)
	tlsReloadWaitSerial(t, addr, recovered)
}

func TestReloadingTLSConfigFlagsAndMTLS(t *testing.T) {
	if cfg, err := LoadReloadingTLSConfig("", "", "", nil, time.Second); err != nil || cfg != nil {
		t.Fatalf("empty TLS flags = %v, %v", cfg, err)
	}
	if _, err := LoadReloadingTLSConfig("cert.pem", "", "", nil, time.Second); err == nil {
		t.Fatal("certificate without a key was accepted")
	}

	ca := newTestCA(t)
	certFile, keyFile := ca.issue(t, true)
	cfg, err := LoadReloadingTLSConfig(certFile, keyFile, ca.file, nil, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.MinVersion != tls.VersionTLS12 {
		t.Fatalf("MinVersion = %#x", cfg.MinVersion)
	}
	if cfg.GetCertificate == nil {
		t.Fatal("reloading config did not install GetCertificate")
	}
	if cfg.ClientAuth != tls.RequireAndVerifyClientCert || cfg.ClientCAs == nil {
		t.Fatalf("mTLS = %v, ClientCAs nil = %v", cfg.ClientAuth, cfg.ClientCAs == nil)
	}

	missingCA := ca.file + ".missing"
	if _, err := LoadReloadingTLSConfig(certFile, keyFile, missingCA, nil, time.Second); err == nil {
		t.Fatal("missing client CA was accepted")
	}

	disabled, err := LoadReloadingTLSConfig(certFile, keyFile, "", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	if disabled.GetCertificate != nil || len(disabled.Certificates) != 1 {
		t.Fatalf("zero interval did not fall back to the static config")
	}
}
