// Package netutil holds the bootstrap helpers shared by the chast server
// binaries: relay port ranges, public address detection and TLS loading.
package netutil

import (
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func ParsePorts(text string) ([]int, error) {
	var ports []int
	for _, part := range strings.Split(text, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if start, end, ok := strings.Cut(part, "-"); ok {
			from, err := strconv.Atoi(strings.TrimSpace(start))
			if err != nil {
				return nil, fmt.Errorf("relay-ports %q: %w", part, err)
			}
			to, err := strconv.Atoi(strings.TrimSpace(end))
			if err != nil {
				return nil, fmt.Errorf("relay-ports %q: %w", part, err)
			}
			if from < 1 || to > 65535 || from > to {
				return nil, fmt.Errorf("relay-ports %q is not an ascending range within 1..65535", part)
			}
			for p := from; p <= to; p++ {
				ports = append(ports, p)
			}
		} else {
			p, err := strconv.Atoi(part)
			if err != nil || p < 1 || p > 65535 {
				return nil, fmt.Errorf("relay port %q is not 1..65535", part)
			}
			ports = append(ports, p)
		}
	}
	if len(ports) == 0 {
		return nil, errors.New("--relay-ports must name at least one port")
	}
	if len(ports) > 4096 {
		return nil, errors.New("too many relay ports (limit 4096)")
	}
	return ports, nil
}

// DetectAdvertise returns the first non-loopback IPv4 address, which is the
// address peers can usually reach on a single-homed host.
func DetectAdvertise() (string, error) {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return "", fmt.Errorf("discover a public address: %w; pass --advertise", err)
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok || ipnet.IP.IsLoopback() {
			continue
		}
		if ip4 := ipnet.IP.To4(); ip4 != nil {
			return ip4.String(), nil
		}
	}
	return "", errors.New("no non-loopback IPv4 address found; pass --advertise <ip-or-name>")
}

// LoadTLSConfig enables HTTPS only when both files are present. Loading the
// pair at startup surfaces a bad certificate before the listener opens. A
// clientCAFile additionally requires every HTTPS client to present a
// certificate signed by that bundle, which is the mTLS mode.
func LoadTLSConfig(certFile, keyFile, clientCAFile string) (*tls.Config, error) {
	if certFile == "" && keyFile == "" && clientCAFile == "" {
		return nil, nil
	}
	if certFile == "" || keyFile == "" {
		return nil, errors.New("--tls-cert and --tls-key must be given together")
	}
	cert, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		return nil, fmt.Errorf("load TLS certificate: %w", err)
	}
	cfg := &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	}
	if clientCAFile != "" {
		pool, err := loadCertPool(clientCAFile)
		if err != nil {
			return nil, err
		}
		cfg.ClientCAs = pool
		cfg.ClientAuth = tls.RequireAndVerifyClientCert
	}
	return cfg, nil
}

// CertificateReloadInterval is how often the reloading TLS config checks the
// certificate files for a new pair.
const CertificateReloadInterval = 5 * time.Second

// ClientCAReloader verifies client certificates against a PEM bundle that is
// re-read when it changes. A broken replacement is logged once and the
// previous pool stays in use. It is safe for concurrent use.
type ClientCAReloader struct {
	path     string
	interval time.Duration
	logger   *slog.Logger

	mu        sync.Mutex
	lastCheck time.Time
	digest    [sha256.Size]byte
	lastError string
	pool      atomic.Pointer[x509.CertPool]
}

// NewClientCAReloader loads the bundle once, so a bad file fails at startup,
// then refreshes it lazily on the first check after interval.
func NewClientCAReloader(path string, logger *slog.Logger, interval time.Duration) (*ClientCAReloader, error) {
	if path == "" {
		return nil, errors.New("client CA path is empty")
	}
	pool, digest, err := readCertPool(path)
	if err != nil {
		return nil, err
	}
	r := &ClientCAReloader{
		path:      path,
		interval:  interval,
		logger:    logger,
		lastCheck: time.Now(),
		digest:    digest,
	}
	r.pool.Store(pool)
	return r, nil
}

// Current returns the pool in use, re-reading the file when the interval has
// elapsed. With a non-positive interval it never re-reads.
func (r *ClientCAReloader) Current() *x509.CertPool {
	r.refresh()
	return r.pool.Load()
}

func (r *ClientCAReloader) refresh() {
	if r.interval <= 0 {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if time.Since(r.lastCheck) < r.interval {
		return
	}
	r.lastCheck = time.Now()
	pool, digest, err := readCertPool(r.path)
	if err != nil {
		if r.logger != nil && err.Error() != r.lastError {
			r.lastError = err.Error()
			r.logger.Warn("TLS client CA reload skipped", "ca", r.path, "error", err)
		}
		return
	}
	r.lastError = ""
	if digest == r.digest {
		return
	}
	r.pool.Store(pool)
	r.digest = digest
	if r.logger != nil {
		r.logger.Info("TLS client CA reloaded", "ca", r.path)
	}
}

// LoadReloadingTLSConfig is LoadTLSConfig plus automatic rotation: the
// certificate and key, and the client CA bundle when mTLS is enabled, are
// re-read lazily on the first handshake after interval, so replacing them
// (for example from certbot) does not require a restart. A broken replacement
// is logged and the previous material stays in use.
func LoadReloadingTLSConfig(certFile, keyFile, clientCAFile string, logger *slog.Logger, interval time.Duration) (*tls.Config, error) {
	if interval <= 0 {
		return LoadTLSConfig(certFile, keyFile, clientCAFile)
	}
	if certFile == "" && keyFile == "" && clientCAFile == "" {
		return nil, nil
	}
	if certFile == "" || keyFile == "" {
		return nil, errors.New("--tls-cert and --tls-key must be given together")
	}
	material, err := readCertMaterial(certFile, keyFile)
	if err != nil {
		return nil, err
	}
	source := &reloadingCert{
		certFile:  certFile,
		keyFile:   keyFile,
		interval:  interval,
		logger:    logger,
		lastCheck: time.Now(),
		digest:    material.digest,
	}
	source.current.Store(material)
	cfg := &tls.Config{
		MinVersion:     tls.VersionTLS12,
		GetCertificate: source.getCertificate,
	}
	if clientCAFile != "" {
		loader, err := NewClientCAReloader(clientCAFile, logger, interval)
		if err != nil {
			return nil, err
		}
		source.clientCAs = loader
		cfg.ClientCAs = loader.Current()
		cfg.ClientAuth = tls.RequireAndVerifyClientCert
		// The per-handshake config swaps in a rotated client CA pool without
		// mutating the shared config, which crypto/tls reads concurrently.
		cfg.GetConfigForClient = source.getConfigForClient
	}
	source.base = cfg
	if logger != nil {
		logger.Info("TLS certificate reload enabled", "cert", certFile, "interval", interval)
	}
	return cfg, nil
}

type certMaterial struct {
	cert   tls.Certificate
	digest [sha256.Size]byte
}

type reloadingCert struct {
	certFile string
	keyFile  string
	interval time.Duration
	logger   *slog.Logger
	// base is the config handed to the listener. getConfigForClient clones it
	// per handshake so a rotated client CA never mutates a live config.
	base *tls.Config

	mu        sync.Mutex
	lastCheck time.Time
	digest    [sha256.Size]byte
	lastError string
	current   atomic.Pointer[certMaterial]
	clientCAs *ClientCAReloader
}

func (r *reloadingCert) getCertificate(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	r.refresh()
	material := r.current.Load()
	if material == nil {
		return nil, errors.New("TLS certificate is unavailable")
	}
	return &material.cert, nil
}

func (r *reloadingCert) getConfigForClient(*tls.ClientHelloInfo) (*tls.Config, error) {
	r.refresh()
	clone := r.base.Clone()
	clone.GetConfigForClient = nil
	if r.clientCAs != nil {
		clone.ClientCAs = r.clientCAs.Current()
	}
	return clone, nil
}

func (r *reloadingCert) refresh() {
	r.mu.Lock()
	defer r.mu.Unlock()
	if time.Since(r.lastCheck) < r.interval {
		return
	}
	r.lastCheck = time.Now()
	r.refreshCertificate()
}

func (r *reloadingCert) refreshCertificate() {
	material, err := readCertMaterial(r.certFile, r.keyFile)
	if err != nil {
		if r.logger != nil && err.Error() != r.lastError {
			r.lastError = err.Error()
			r.logger.Warn("TLS certificate reload skipped", "cert", r.certFile, "error", err)
		}
		return
	}
	r.lastError = ""
	if material.digest == r.digest {
		return
	}
	r.current.Store(material)
	r.digest = material.digest
	if r.logger != nil {
		r.logger.Info("TLS certificate reloaded", "cert", r.certFile)
	}
}

func readCertMaterial(certFile, keyFile string) (*certMaterial, error) {
	certPEM, err := os.ReadFile(certFile)
	if err != nil {
		return nil, fmt.Errorf("load TLS certificate: %w", err)
	}
	keyPEM, err := os.ReadFile(keyFile)
	if err != nil {
		return nil, fmt.Errorf("load TLS certificate: %w", err)
	}
	cert, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return nil, fmt.Errorf("load TLS certificate: %w", err)
	}
	material := &certMaterial{cert: cert}
	hash := sha256.New()
	hash.Write(certPEM)
	hash.Write(keyPEM)
	copy(material.digest[:], hash.Sum(nil))
	return material, nil
}

// ClientTLSConfig configures outbound HTTPS. caFile adds a private CA to the
// system roots, and certFile/keyFile present a client certificate when the
// peer demands mTLS. With every argument empty it returns nil, so callers can
// keep the default transport.
func ClientTLSConfig(caFile, certFile, keyFile string) (*tls.Config, error) {
	if caFile == "" && certFile == "" && keyFile == "" {
		return nil, nil
	}
	cfg := &tls.Config{MinVersion: tls.VersionTLS12}
	if caFile != "" {
		pool, err := x509.SystemCertPool()
		if err != nil || pool == nil {
			pool = x509.NewCertPool()
		}
		data, err := os.ReadFile(caFile)
		if err != nil {
			return nil, fmt.Errorf("read client CA: %w", err)
		}
		if !pool.AppendCertsFromPEM(data) {
			return nil, fmt.Errorf("client CA %q contains no certificates", caFile)
		}
		cfg.RootCAs = pool
	}
	if (certFile == "") != (keyFile == "") {
		return nil, errors.New("client certificate and key must be given together")
	}
	if certFile != "" {
		cert, err := tls.LoadX509KeyPair(certFile, keyFile)
		if err != nil {
			return nil, fmt.Errorf("load client certificate: %w", err)
		}
		cfg.Certificates = []tls.Certificate{cert}
	}
	return cfg, nil
}

// LoadClientCAPool reads a PEM bundle used to verify client certificates.
func LoadClientCAPool(path string) (*x509.CertPool, error) {
	return loadCertPool(path)
}

func loadCertPool(path string) (*x509.CertPool, error) {
	pool, _, err := readCertPool(path)
	return pool, err
}

func readCertPool(path string) (*x509.CertPool, [sha256.Size]byte, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, [sha256.Size]byte{}, fmt.Errorf("read client CA: %w", err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(data) {
		return nil, [sha256.Size]byte{}, fmt.Errorf("client CA %q contains no certificates", path)
	}
	return pool, sha256.Sum256(data), nil
}
