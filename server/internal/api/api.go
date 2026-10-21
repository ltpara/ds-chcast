// Package api exposes registration and lookup over HTTP. It is deliberately
// small: the rendezvous and relay data planes never block on HTTP, and a
// reverse proxy can add TLS without the Go process knowing about it.
package api

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
)

const maxBody = 4096

type Config struct {
	Registry *registry.Registry
	Relay    *relay.Manager
	// Advertised public endpoint of the rendezvous UDP port, already joined.
	PunchAddr string
	// Hostname or IP advertised for relay ports.
	RelayHost string
	ServerKey string
	TTL       time.Duration
	Logger    *slog.Logger
}

type server struct {
	cfg Config
}

func New(cfg Config) http.Handler {
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if cfg.TTL <= 0 {
		cfg.TTL = 60 * time.Second
	}
	s := &server{cfg: cfg}
	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/hosts", s.register)
	mux.HandleFunc("GET /v1/hosts/{id}", s.lookup)
	mux.HandleFunc("DELETE /v1/hosts/{id}", s.unregister)
	mux.HandleFunc("GET /healthz", s.health)
	mux.HandleFunc("GET /v1/stats", s.stats)
	return mux
}

type registerRequest struct {
	ID          string `json:"id"`
	Key         string `json:"key"`
	Fingerprint string `json:"fingerprint"`
}

type registerResponse struct {
	ID           string `json:"id"`
	Relay        string `json:"relay"`
	Punch        string `json:"punch"`
	ExpiresInSec int64  `json:"expires_in_sec"`
}

type lookupResponse struct {
	ID          string `json:"id"`
	Online      bool   `json:"online"`
	Candidate   string `json:"candidate,omitempty"`
	Relay       string `json:"relay,omitempty"`
	Punch       string `json:"punch"`
	Fingerprint string `json:"fingerprint,omitempty"`
	LastSeenMS  int64  `json:"last_seen_ms,omitempty"`
	ExpiresInMS int64  `json:"expires_in_ms"`
}

func (s *server) register(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	var req registerRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !registry.ValidID(req.ID) {
		writeError(w, http.StatusBadRequest, "id must be 1..64 characters of [A-Za-z0-9_.-]")
		return
	}
	if len(req.Key) < 8 {
		writeError(w, http.StatusBadRequest, "host key must be at least 8 characters")
		return
	}
	keyHash := HashKey(req.Key)
	if _, err := s.cfg.Registry.Register(registry.RegisterRequest{
		ID:          req.ID,
		KeyHash:     keyHash,
		Fingerprint: req.Fingerprint,
		TTL:         s.cfg.TTL,
	}); err != nil {
		if errors.Is(err, registry.ErrKeyMismatch) {
			writeError(w, http.StatusForbidden, "host id is already registered with another key")
			return
		}
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	port, err := s.cfg.Relay.Allocate(req.ID)
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, "no relay port is available")
		return
	}
	s.cfg.Registry.SetRelayPort(req.ID, port)
	writeJSON(w, http.StatusOK, registerResponse{
		ID:           req.ID,
		Relay:        net.JoinHostPort(s.cfg.RelayHost, strconv.Itoa(port)),
		Punch:        s.cfg.PunchAddr,
		ExpiresInSec: int64(s.cfg.TTL / time.Second),
	})
}

func (s *server) lookup(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	host, ok := s.cfg.Registry.Lookup(id)
	if !ok {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	now := time.Now()
	resp := lookupResponse{
		ID:          host.ID,
		Online:      s.cfg.Registry.Online(host),
		Fingerprint: host.Fingerprint,
		Punch:       s.cfg.PunchAddr,
		ExpiresInMS: host.ExpiresAt.Sub(now).Milliseconds(),
	}
	if resp.Online {
		resp.Candidate = host.Candidate
	}
	if host.RelayPort != 0 {
		resp.Relay = net.JoinHostPort(s.cfg.RelayHost, strconv.Itoa(host.RelayPort))
	}
	if !host.LastSeen.IsZero() {
		resp.LastSeenMS = now.Sub(host.LastSeen).Milliseconds()
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) unregister(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	id := r.PathValue("id")
	if _, ok := s.cfg.Registry.Remove(id); !ok {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	s.cfg.Relay.Release(id)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) health(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	io.WriteString(w, "ok\n")
}

func (s *server) stats(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"hosts": s.cfg.Registry.Count(),
		"relay": s.cfg.Relay.Stats(),
	})
}

func (s *server) authorized(r *http.Request) bool {
	if s.cfg.ServerKey == "" {
		return true
	}
	header := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if !strings.HasPrefix(header, prefix) {
		return false
	}
	got := strings.TrimSpace(header[len(prefix):])
	return subtle.ConstantTimeCompare([]byte(got), []byte(s.cfg.ServerKey)) == 1
}

func decodeBody(w http.ResponseWriter, r *http.Request, dst any) error {
	defer r.Body.Close()
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxBody))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}
	if dec.More() {
		return errors.New("invalid JSON: trailing data")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}

func writeError(w http.ResponseWriter, status int, text string) {
	writeJSON(w, status, map[string]string{"error": text})
}

// HashKey is the only form in which a host key reaches disk-free server state.
func HashKey(key string) string {
	sum := sha256.Sum256([]byte(key))
	return hex.EncodeToString(sum[:])
}
