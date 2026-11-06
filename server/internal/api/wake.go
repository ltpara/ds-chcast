package api

import (
	"errors"
	"net/http"
	"strconv"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wake"
	"github.com/ltpara/ds-chcast/server/internal/wol"
)

type wakeRequest struct {
	MAC string `json:"mac"`
}

type wakeResponse struct {
	Queued       bool   `json:"queued"`
	Job          string `json:"job"`
	ExpiresInSec int64  `json:"expires_in_sec"`
}

// wakeHost queues a cross-subnet Wake-on-LAN request. The MAC comes from the
// caller because the server never stores link-layer addresses; the queue is
// in-memory and a host agent on the target's link broadcasts the packet at its
// next registration refresh.
func (s *server) wakeHost(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if !registry.ValidID(id) {
		writeError(w, http.StatusNotFound, "host is not known")
		return
	}
	record, ok := s.cfg.Registry.Identity(id)
	if !ok {
		writeError(w, http.StatusNotFound, "host is not known")
		return
	}
	var req wakeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	mac, ok := wol.NormalizeMAC(req.MAC)
	if !ok {
		writeError(w, http.StatusBadRequest, "mac must contain twelve hexadecimal digits")
		return
	}
	actor, ok := s.wakeActor(w, r, id)
	if !ok {
		return
	}
	if len(record.WakeTargets) == 0 {
		// Without a link the target never reported, a relay agent would only
		// guess where to broadcast. Refuse instead of fanning the packet out
		// to every relay's own network.
		writeError(w, http.StatusConflict, "target has not reported a wake link")
		s.recordAudit(r, actor, account.AuditDeviceWake, "wake "+id+" refused: no wake link", false)
		return
	}
	job, wait, err := s.wake.Request(id, mac, record.WakeTargets)
	switch {
	case err == nil:
	case errors.Is(err, wake.ErrCooldown):
		w.Header().Set("Retry-After", strconv.Itoa(retrySeconds(wait)))
		writeError(w, http.StatusTooManyRequests, "target was woken recently; retry later")
		s.recordAudit(r, actor, account.AuditDeviceWake, "wake "+id+" refused: cooldown", false)
		return
	case errors.Is(err, wake.ErrQueueFull):
		writeError(w, http.StatusTooManyRequests, "wake relay queue is full")
		s.recordAudit(r, actor, account.AuditDeviceWake, "wake "+id+" refused: queue full", false)
		return
	default:
		s.cfg.Logger.Error("wake request", "target", id, "error", err)
		writeError(w, http.StatusInternalServerError, "wake request failed")
		return
	}
	s.cfg.Logger.Info("wake queued", "target", id, "actor", actor, "job", job.ID)
	s.recordAudit(r, actor, account.AuditDeviceWake, "queued wake for "+id, true)
	writeJSON(w, http.StatusAccepted, wakeResponse{
		Queued:       true,
		Job:          job.ID,
		ExpiresInSec: job.ExpiresInSec,
	})
}

// wakeActor authenticates one wake request. A server key always works; with
// accounts enabled a bearer token is required for everyone else. The owner of
// a target in their address book (or any admin) may wake it. A server that has
// neither a key nor accounts stays open, matching registration and lookup,
// with the per-target cooldown and per-IP HTTP limiter as the guard rails.
func (s *server) wakeActor(w http.ResponseWriter, r *http.Request, target string) (string, bool) {
	if s.serverKeyAuthorized(r) {
		return "server-key", true
	}
	if s.cfg.Accounts != nil {
		token, ok := bearerToken(r)
		if !ok {
			writeError(w, http.StatusUnauthorized, "server key or account token required")
			return "", false
		}
		user, err := s.cfg.Accounts.Authenticate(token)
		if err != nil {
			writeError(w, http.StatusUnauthorized, "server key or account token required")
			return "", false
		}
		if !s.wakeAllowed(user, target) {
			writeError(w, http.StatusForbidden, "target is not in this account's address book")
			return "", false
		}
		return user.Username, true
	}
	if s.cfg.ServerKey == "" {
		return "anonymous", true
	}
	writeError(w, http.StatusUnauthorized, "server key required")
	return "", false
}

func (s *server) wakeAllowed(user account.User, target string) bool {
	if user.Role == account.RoleAdmin {
		return true
	}
	peers, err := s.cfg.Accounts.Peers(user.ID)
	if err != nil {
		s.cfg.Logger.Error("wake address book", "user", user.Username, "error", err)
		return false
	}
	for _, peer := range peers {
		if peer.ID == target {
			return true
		}
	}
	books, err := s.cfg.Accounts.SharedBooks(user.ID)
	if err != nil {
		s.cfg.Logger.Error("wake shared address books", "user", user.Username, "error", err)
		return false
	}
	for _, book := range books {
		for _, peer := range book.Peers {
			if peer.ID == target {
				return true
			}
		}
	}
	return false
}

func retrySeconds(wait time.Duration) int {
	if wait <= 0 {
		return 1
	}
	seconds := int((wait + time.Second - 1) / time.Second)
	if seconds < 1 {
		return 1
	}
	return seconds
}
