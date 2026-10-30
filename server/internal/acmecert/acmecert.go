// Package acmecert obtains and renews the HTTP API certificate over ACME.
package acmecert

import (
	"context"
	"crypto/tls"
	"encoding/base64"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/netip"
	"os"
	"regexp"
	"strings"
	"time"

	"golang.org/x/crypto/acme"
	"golang.org/x/crypto/acme/autocert"

	"github.com/ltpara/ds-chcast/server/internal/netutil"
)

// Config enables the ACME manager. HTTPListen may be empty for tests that only
// need cached certificates; the server binary requires a challenge listener.
type Config struct {
	Domains      []string
	Email        string
	CacheDir     string
	DirectoryURL string
	EABKID       string
	EABKey       []byte
	HTTPListen   string
	Logger       *slog.Logger
}

// Manager owns the autocert manager and the optional HTTP-01 listener.
type Manager struct {
	manager    *autocert.Manager
	handler    http.Handler
	logger     *slog.Logger
	httpListen string
	httpServer *http.Server
	httpLn     net.Listener
}

var domainPattern = regexp.MustCompile(`^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$`)

// ParseDomains splits, normalises and validates a comma-separated domain list.
func ParseDomains(raw string) ([]string, error) {
	parts := strings.Split(raw, ",")
	domains := make([]string, 0, len(parts))
	seen := make(map[string]bool, len(parts))
	for _, part := range parts {
		domain := strings.ToLower(strings.TrimSpace(part))
		domain = strings.TrimSuffix(domain, ".")
		if domain == "" {
			continue
		}
		if err := validateDomain(domain); err != nil {
			return nil, err
		}
		if seen[domain] {
			continue
		}
		seen[domain] = true
		domains = append(domains, domain)
	}
	if len(domains) == 0 {
		return nil, errors.New("no ACME domain given")
	}
	return domains, nil
}

func validateDomain(domain string) error {
	if len(domain) > 253 {
		return fmt.Errorf("ACME domain %q is longer than 253 characters", domain)
	}
	if strings.Contains(domain, "*") {
		return fmt.Errorf("ACME domain %q: wildcards are not supported by HTTP-01", domain)
	}
	if _, err := netip.ParseAddr(domain); err == nil {
		return fmt.Errorf("ACME domain %q: IP addresses are not supported", domain)
	}
	if !domainPattern.MatchString(domain) {
		return fmt.Errorf("ACME domain %q: expected a bare DNS name such as rd.example.com", domain)
	}
	return nil
}

// ParseEABKey decodes an external account binding HMAC key as issued by a CA.
// It accepts padded or unpadded base64 in the URL or standard alphabet.
func ParseEABKey(encoded string) ([]byte, error) {
	encoded = strings.TrimSpace(encoded)
	if encoded == "" {
		return nil, errors.New("empty external account binding key")
	}
	for _, enc := range []*base64.Encoding{base64.RawURLEncoding, base64.URLEncoding, base64.StdEncoding, base64.RawStdEncoding} {
		if key, err := enc.DecodeString(encoded); err == nil {
			return key, nil
		}
	}
	return nil, errors.New("external account binding key is not valid base64")
}

// New builds the autocert manager. It also enables the HTTP-01 challenge type,
// which autocert otherwise leaves off in favour of TLS-ALPN-01.
func New(cfg Config) (*Manager, error) {
	domains := make([]string, 0, len(cfg.Domains))
	seen := make(map[string]bool, len(cfg.Domains))
	for _, raw := range cfg.Domains {
		domain := strings.ToLower(strings.TrimSuffix(strings.TrimSpace(raw), "."))
		if err := validateDomain(domain); err != nil {
			return nil, err
		}
		if !seen[domain] {
			seen[domain] = true
			domains = append(domains, domain)
		}
	}
	if len(domains) == 0 {
		return nil, errors.New("no ACME domain given")
	}
	if strings.TrimSpace(cfg.CacheDir) == "" {
		return nil, errors.New("ACME cache directory is required")
	}
	if (cfg.EABKID == "") != (len(cfg.EABKey) == 0) {
		return nil, errors.New("ACME external account binding needs both a key id and a key")
	}
	if err := os.MkdirAll(cfg.CacheDir, 0o700); err != nil {
		return nil, fmt.Errorf("create ACME cache directory: %w", err)
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}
	manager := &autocert.Manager{
		Prompt:     autocert.AcceptTOS,
		HostPolicy: autocert.HostWhitelist(domains...),
		Cache:      autocert.DirCache(cfg.CacheDir),
		Email:      strings.TrimSpace(cfg.Email),
	}
	if cfg.DirectoryURL != "" {
		manager.Client = &acme.Client{DirectoryURL: cfg.DirectoryURL}
	}
	if cfg.EABKID != "" {
		manager.ExternalAccountBinding = &acme.ExternalAccountBinding{KID: cfg.EABKID, Key: cfg.EABKey}
	}
	m := &Manager{
		manager:    manager,
		handler:    manager.HTTPHandler(nil),
		logger:     logger,
		httpListen: cfg.HTTPListen,
	}
	logger.Info("ACME manager configured",
		"domains", domains,
		"cache", cfg.CacheDir,
		"directory", cfg.DirectoryURL,
		"eab", cfg.EABKID != "",
	)
	return m, nil
}

// TLSConfig returns a server TLS config backed by ACME certificates. When
// clientCAFile is set the listener also demands a client certificate signed by
// that bundle; the bundle is re-read when interval elapses, so rotating it does
// not require a restart.
func (m *Manager) TLSConfig(clientCAFile string, interval time.Duration) (*tls.Config, error) {
	cfg := m.manager.TLSConfig()
	cfg.MinVersion = tls.VersionTLS12
	if clientCAFile == "" {
		return cfg, nil
	}
	loader, err := netutil.NewClientCAReloader(clientCAFile, m.logger, interval)
	if err != nil {
		return nil, err
	}
	cfg.ClientCAs = loader.Current()
	cfg.ClientAuth = tls.RequireAndVerifyClientCert
	// A per-handshake clone carries the current pool, so a rotated client CA
	// never mutates the shared config that crypto/tls reads concurrently.
	cfg.GetConfigForClient = func(*tls.ClientHelloInfo) (*tls.Config, error) {
		clone := cfg.Clone()
		clone.GetConfigForClient = nil
		clone.ClientCAs = loader.Current()
		return clone, nil
	}
	return cfg, nil
}

// StartHTTP binds and serves the HTTP-01 challenge listener. Redirects for
// every other GET/HEAD request follow autocert's HTTPS redirect.
func (m *Manager) StartHTTP() error {
	if m.httpListen == "" {
		return nil
	}
	ln, err := net.Listen("tcp", m.httpListen)
	if err != nil {
		return fmt.Errorf("ACME HTTP-01 listen %q: %w", m.httpListen, err)
	}
	m.httpLn = ln
	m.httpServer = &http.Server{
		Handler:           m.handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    8 << 10,
	}
	go func() {
		if err := m.httpServer.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			m.logger.Error("ACME HTTP-01 server stopped", "error", err)
		}
	}()
	m.logger.Info("ACME HTTP-01 listening", "on", ln.Addr().String())
	return nil
}

// HTTPAddr reports the bound challenge address, or an empty string when the
// listener was not started.
func (m *Manager) HTTPAddr() string {
	if m.httpLn == nil {
		return ""
	}
	return m.httpLn.Addr().String()
}

// Shutdown stops the challenge listener.
func (m *Manager) Shutdown(ctx context.Context) error {
	if m.httpServer == nil {
		return nil
	}
	return m.httpServer.Shutdown(ctx)
}
