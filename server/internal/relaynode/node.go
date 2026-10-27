// Package relaynode is the control plane of a standalone relay node, the chast
// equivalent of hbbr. hbbs provisions a lease (host id, public key, carrier
// token, TTL) over an authenticated HTTP API; the node then serves relay
// challenges, UDP forwarding and carriers on its own without calling back.
package relaynode

import (
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
)

const (
	maxBody     = 8 << 10
	minLeaseTTL = 5 * time.Second
	maxLeaseTTL = time.Hour
)

type Config struct {
	NodeID    string
	Advertise string
	// AdvertiseV6 is the public IPv6 address of the shared carrier. Empty
	// keeps the IPv4-only behaviour; setting it requires an IPv6 carrier
	// listener on the node.
	AdvertiseV6 string
	Key         string
	Logger      *slog.Logger
}

type Server struct {
	cfg     Config
	manager *relay.Manager
	auth    *Authenticator
	log     *slog.Logger
	started time.Time

	provisionMu sync.Mutex

	done      chan struct{}
	closeOnce sync.Once
	wg        sync.WaitGroup
}

type Lease struct {
	ID           string `json:"id"`
	Port         int    `json:"port"`
	Relay        string `json:"relay"`
	Carrier      string `json:"carrier,omitempty"`
	CarrierV6    string `json:"carrier_v6,omitempty"`
	ExpiresInSec int64  `json:"expires_in_sec"`
}

type Health struct {
	OK     bool     `json:"ok"`
	Node   string   `json:"node,omitempty"`
	Leases int      `json:"leases"`
	Linked []string `json:"linked,omitempty"`
}

type Stats struct {
	Node      string      `json:"node,omitempty"`
	UptimeSec int64       `json:"uptime_sec"`
	Leases    int         `json:"leases"`
	Linked    []string    `json:"linked,omitempty"`
	Relay     relay.Stats `json:"relay"`
}

func New(cfg Config, manager *relay.Manager, auth *Authenticator) (*Server, error) {
	if manager == nil {
		return nil, errors.New("relay node needs a relay manager")
	}
	if auth == nil {
		return nil, errors.New("relay node needs a lease authenticator")
	}
	if strings.TrimSpace(cfg.Key) == "" {
		return nil, errors.New("relay node needs a control key")
	}
	advertise, err := normalizeAdvertise(cfg.Advertise)
	if err != nil {
		return nil, err
	}
	cfg.Advertise = advertise
	if cfg.AdvertiseV6, err = normalizeAdvertiseV6(cfg.AdvertiseV6); err != nil {
		return nil, err
	}
	if cfg.AdvertiseV6 != "" && manager.CarrierV6Addr() == "" {
		return nil, errors.New("relay node advertises an IPv6 carrier but has no IPv6 carrier listener")
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	if len(cfg.NodeID) > 64 {
		return nil, errors.New("relay node id is longer than 64 characters")
	}
	return &Server{
		cfg:     cfg,
		manager: manager,
		auth:    auth,
		log:     cfg.Logger,
		started: time.Now(),
		done:    make(chan struct{}),
	}, nil
}

func normalizeAdvertise(advertise string) (string, error) {
	advertise = strings.TrimSpace(advertise)
	if advertise == "" {
		return "", errors.New("relay node needs an --advertise host name or IP")
	}
	if strings.HasPrefix(advertise, "[") && strings.HasSuffix(advertise, "]") {
		advertise = advertise[1 : len(advertise)-1]
	}
	if ip := net.ParseIP(advertise); ip != nil {
		return ip.String(), nil
	}
	if strings.Contains(advertise, ":") {
		return "", fmt.Errorf("relay advertise %q must not include a port", advertise)
	}
	return strings.ToLower(advertise), nil
}

// normalizeAdvertiseV6 accepts an empty value (IPv4-only node) or a literal
// IPv6 address without brackets or port.
func normalizeAdvertiseV6(advertise string) (string, error) {
	advertise = strings.TrimSpace(advertise)
	if advertise == "" {
		return "", nil
	}
	if strings.HasPrefix(advertise, "[") && strings.HasSuffix(advertise, "]") {
		advertise = advertise[1 : len(advertise)-1]
	}
	ip := net.ParseIP(advertise)
	if ip == nil || ip.To4() != nil {
		return "", fmt.Errorf("relay IPv6 advertise %q must be an IPv6 address", advertise)
	}
	return ip.String(), nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.handleHealth)
	mux.Handle("POST /v1/leases", s.requireKey(http.HandlerFunc(s.handleLease)))
	mux.Handle("DELETE /v1/leases/{id}", s.requireKey(http.HandlerFunc(s.handleRelease)))
	mux.Handle("GET /v1/stats", s.requireKey(http.HandlerFunc(s.handleStats)))
	return mux
}

func (s *Server) Provision(id, publicKey, carrierToken string, ttl time.Duration) (Lease, error) {
	id = strings.TrimSpace(id)
	if !registry.ValidID(id) {
		return Lease{}, fmt.Errorf("id %q must be 1..64 characters of [A-Za-z0-9_.-]", id)
	}
	if _, err := identity.ParsePublicKey(publicKey); err != nil {
		return Lease{}, err
	}
	if carrierToken == "" || len(carrierToken) > 128 {
		return Lease{}, errors.New("carrier token must be 1..128 characters")
	}
	if ttl < minLeaseTTL {
		ttl = minLeaseTTL
	}
	if ttl > maxLeaseTTL {
		ttl = maxLeaseTTL
	}
	s.provisionMu.Lock()
	defer s.provisionMu.Unlock()
	if existing, ok := s.auth.Lookup(id); ok && existing.PublicKey != publicKey {
		return Lease{}, ErrKeyMismatch
	}
	port, err := s.manager.Allocate(id)
	if err != nil {
		return Lease{}, err
	}
	info, err := s.auth.Put(id, publicKey, carrierToken, port, ttl)
	if err != nil {
		return Lease{}, err
	}
	if err := s.manager.BindHost(port, s.cfg.Advertise); err != nil {
		s.log.Warn("relay node could not bind its advertised address",
			"port", port, "address", s.cfg.Advertise, "error", err)
	}
	return s.leaseResponse(info), nil
}

func (s *Server) Release(id string) {
	s.auth.Remove(id)
	s.manager.Release(id)
}

func (s *Server) HostLinked(id string) bool {
	return s.manager.HostLinked(id)
}

func (s *Server) StartJanitor(every time.Duration) {
	if every <= 0 {
		every = 5 * time.Second
	}
	s.wg.Add(1)
	go func() {
		defer s.wg.Done()
		ticker := time.NewTicker(every)
		defer ticker.Stop()
		for {
			select {
			case <-s.done:
				return
			case now := <-ticker.C:
				for _, id := range s.auth.Prune(now) {
					s.manager.Release(id)
					s.log.Info("relay lease expired", "host", id)
				}
			}
		}
	}()
}

func (s *Server) Close() {
	s.closeOnce.Do(func() {
		close(s.done)
		s.wg.Wait()
	})
}

func (s *Server) Health() Health {
	return Health{
		OK:     true,
		Node:   s.cfg.NodeID,
		Leases: s.auth.Count(),
		Linked: s.linked(),
	}
}

func (s *Server) Stats() Stats {
	return Stats{
		Node:      s.cfg.NodeID,
		UptimeSec: int64(time.Since(s.started) / time.Second),
		Leases:    s.auth.Count(),
		Linked:    s.linked(),
		Relay:     s.manager.Stats(),
	}
}

func (s *Server) leaseResponse(info LeaseInfo) Lease {
	out := Lease{
		ID:           info.ID,
		Port:         info.Port,
		Relay:        net.JoinHostPort(s.cfg.Advertise, fmt.Sprint(info.Port)),
		ExpiresInSec: int64(time.Until(info.ExpiresAt) / time.Second),
	}
	if out.ExpiresInSec < 0 {
		out.ExpiresInSec = 0
	}
	if carrier := s.manager.CarrierPort(); carrier != 0 {
		out.Carrier = net.JoinHostPort(s.cfg.Advertise, fmt.Sprint(carrier))
		if s.cfg.AdvertiseV6 != "" {
			out.CarrierV6 = net.JoinHostPort(s.cfg.AdvertiseV6, fmt.Sprint(carrier))
		}
	}
	return out
}

func (s *Server) linked() []string {
	ids := s.auth.IDs()
	sort.Strings(ids)
	out := make([]string, 0, len(ids))
	for _, id := range ids {
		if s.manager.HostLinked(id) {
			out = append(out, id)
		}
	}
	return out
}

type leaseRequest struct {
	ID           string `json:"id"`
	PublicKey    string `json:"public_key"`
	CarrierToken string `json:"carrier_token"`
	TTLSeconds   int64  `json:"ttl_sec"`
}

func (s *Server) handleLease(w http.ResponseWriter, r *http.Request) {
	var req leaseRequest
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxBody))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON: " + err.Error()})
		return
	}
	if dec.More() {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON: trailing data"})
		return
	}
	lease, err := s.Provision(req.ID, req.PublicKey, req.CarrierToken,
		time.Duration(req.TTLSeconds)*time.Second)
	switch {
	case err == nil:
		writeJSON(w, http.StatusOK, lease)
	case errors.Is(err, ErrKeyMismatch):
		writeJSON(w, http.StatusConflict, map[string]string{"error": err.Error()})
	case errors.Is(err, relay.ErrNoFreePort):
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "no relay port is available"})
	default:
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
	}
}

func (s *Server) handleRelease(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !registry.ValidID(id) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid id"})
		return
	}
	s.Release(id)
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.Health())
}

func (s *Server) handleStats(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, s.Stats())
}

func (s *Server) requireKey(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		header := r.Header.Get("Authorization")
		const prefix = "Bearer "
		if !strings.HasPrefix(header, prefix) {
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "relay node key was not accepted"})
			return
		}
		key := strings.TrimSpace(header[len(prefix):])
		if subtle.ConstantTimeCompare([]byte(key), []byte(s.cfg.Key)) != 1 {
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "relay node key was not accepted"})
			return
		}
		next.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}
