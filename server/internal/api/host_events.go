package api

import (
	"context"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const (
	// hostEventHistory bounds the per-host replay buffer. Policy and key
	// revocation events are rare, so a reconnect that lands after a long
	// outage still sees the recent ones; an overflowing agent falls back to
	// the policy carried by its next registration.
	hostEventHistory     = 64
	hostEventDefaultWait = 25 * time.Second
	hostEventMaxWait     = 55 * time.Second
)

// hostEvent is one statement delivered to a host over the event stream.
//
// Two kinds exist:
//   - policy: a device policy was set (Managed=true, Mode/Version filled) or
//     removed (Managed=false). A host applies the file change before acting.
//   - revoke: an issuer key id was retired. Every host must end sessions whose
//     access ticket was signed by that key instead of waiting for the 65 s
//     lease bound.
type hostEvent struct {
	Seq               uint64 `json:"seq"`
	Kind              string `json:"kind"`
	Mode              string `json:"mode,omitempty"`
	PermissionCeiling uint32 `json:"permission_ceiling,omitempty"`
	PolicyVersion     uint32 `json:"policy_version,omitempty"`
	UpdatedAtMS       int64  `json:"updated_at_ms,omitempty"`
	Managed           *bool  `json:"managed,omitempty"`
	Kid               string `json:"kid,omitempty"`
	Reason            string `json:"reason,omitempty"`
	AtMS              int64  `json:"at_ms"`
}

type hostEventsResponse struct {
	Events []hostEvent `json:"events"`
	Cursor uint64      `json:"cursor"`
}

// hostEventHub is the in-memory fan-out for hosts that keep a long poll open.
// Events are not durable: the registration response carries the authoritative
// policy, and the 65 s ticket lease (plus refused issuance) bounds the damage
// of a missed key revocation. The hub only shortens that window.
type hostEventHub struct {
	mu      sync.Mutex
	seq     uint64
	hosts   map[string][]hostEvent
	changed chan struct{}
}

func newHostEventHub() *hostEventHub {
	return &hostEventHub{
		hosts:   make(map[string][]hostEvent),
		changed: make(chan struct{}),
	}
}

func (h *hostEventHub) notifyLocked() {
	close(h.changed)
	h.changed = make(chan struct{})
}

func (h *hostEventHub) publish(hostID string, event hostEvent) {
	h.mu.Lock()
	h.seq++
	event.Seq = h.seq
	if event.AtMS == 0 {
		event.AtMS = time.Now().UnixMilli()
	}
	queue := append(h.hosts[hostID], event)
	if len(queue) > hostEventHistory {
		queue = queue[len(queue)-hostEventHistory:]
	}
	h.hosts[hostID] = queue
	h.notifyLocked()
	h.mu.Unlock()
}

// publishAll delivers a global revocation to every host that has ever polled
// since this server process started. The map includes hosts with an empty
// queue because subscribe creates the entry before waiting.
func (h *hostEventHub) publishAll(event hostEvent) {
	h.mu.Lock()
	h.seq++
	event.Seq = h.seq
	if event.AtMS == 0 {
		event.AtMS = time.Now().UnixMilli()
	}
	for hostID := range h.hosts {
		queue := append(h.hosts[hostID], event)
		if len(queue) > hostEventHistory {
			queue = queue[len(queue)-hostEventHistory:]
		}
		h.hosts[hostID] = queue
	}
	h.notifyLocked()
	h.mu.Unlock()
}

// wait blocks until an event after the cursor arrives, the caller goes away or
// the timeout expires. The first subscription for a host starts at the current
// sequence instead of replaying history: registration already carried the live
// policy, so old statements would only be re-applied. The same applies to a
// cursor ahead of this process's sequence, which covers a server restart. Later
// zeros are real cursors and replay the ring, because a host that has caught up
// may still sit at sequence zero when its first pushed event arrives.
//
// The request context matters for shutdown: without it a host that closed its
// connection would still hold this handler (and an httptest server's Close)
// until the full poll timeout.
func (h *hostEventHub) wait(ctx context.Context, hostID string, after uint64, timeout time.Duration) ([]hostEvent, uint64) {
	h.mu.Lock()
	_, subscribed := h.hosts[hostID]
	if !subscribed || after > h.seq {
		after = h.seq
	}
	if !subscribed {
		h.hosts[hostID] = nil
	}
	h.mu.Unlock()

	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	for {
		h.mu.Lock()
		cursor := h.seq
		var pending []hostEvent
		for _, event := range h.hosts[hostID] {
			if event.Seq > after {
				pending = append(pending, event)
			}
		}
		changed := h.changed
		h.mu.Unlock()

		if len(pending) > 0 {
			return pending, pending[len(pending)-1].Seq
		}
		select {
		case <-changed:
		case <-ctx.Done():
			return nil, cursor
		case <-deadline.C:
			return nil, cursor
		}
	}
}

func (h *hostEventHub) publishPolicy(peerID string, policy *account.DevicePolicy) {
	managed := policy != nil
	event := hostEvent{Kind: "policy", Managed: &managed}
	if policy != nil {
		event.Mode = policy.Mode
		event.PermissionCeiling = policy.PermissionCeiling
		event.PolicyVersion = policy.PolicyVersion
		event.UpdatedAtMS = policy.UpdatedAt.UnixMilli()
	}
	h.publish(peerID, event)
}

// hostEvents serves the host's revocation stream. The bearer credential is the
// shared host key; the device key signature binds the cursor to this host, so
// one controlled machine cannot read another machine's policy changes.
func (s *server) hostEvents(w http.ResponseWriter, r *http.Request) {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return
	}
	hostID := strings.TrimSpace(r.PathValue("id"))
	if !registry.ValidID(hostID) {
		writeError(w, http.StatusBadRequest, "host id must be 1..64 characters of [A-Za-z0-9_.-]")
		return
	}
	record, ok := s.cfg.Registry.Identity(hostID)
	if !ok {
		writeError(w, http.StatusNotFound, "unknown host")
		return
	}
	query := r.URL.Query()
	after, err := strconv.ParseUint(strings.TrimSpace(query.Get("after")), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "after must be an unsigned 64-bit integer")
		return
	}
	timestamp, err := strconv.ParseInt(strings.TrimSpace(query.Get("timestamp")), 10, 64)
	if err != nil || timestamp == 0 {
		writeError(w, http.StatusBadRequest, "timestamp is required")
		return
	}
	now := time.Now()
	signedAt := time.Unix(timestamp, 0)
	if now.Sub(signedAt) > maxSkew || signedAt.Sub(now) > maxSkew {
		writeError(w, http.StatusUnauthorized, "subscription timestamp is outside the allowed window")
		return
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil {
		s.cfg.Logger.Error("host events identity", "id", hostID, "error", err)
		writeError(w, http.StatusInternalServerError, "host identity is unreadable")
		return
	}
	msg := identity.HostEventsSubscribeMessage(hostID, after, timestamp)
	if !identity.Verify(pub, msg, query.Get("signature")) {
		writeError(w, http.StatusUnauthorized, "subscription signature was not accepted")
		return
	}

	waitFor := hostEventDefaultWait
	if raw := strings.TrimSpace(query.Get("wait_ms")); raw != "" {
		value, err := strconv.Atoi(raw)
		if err != nil || value < 0 {
			writeError(w, http.StatusBadRequest, "wait_ms must be a non-negative integer")
			return
		}
		waitFor = time.Duration(value) * time.Millisecond
		if waitFor > hostEventMaxWait {
			waitFor = hostEventMaxWait
		}
	}

	events, cursor := s.eventHub.wait(r.Context(), hostID, after, waitFor)
	if events == nil {
		events = []hostEvent{}
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, hostEventsResponse{Events: events, Cursor: cursor})
}

type revokedAccessKidView struct {
	Kid         string `json:"kid"`
	RevokedAtMS int64  `json:"revoked_at_ms"`
	RevokedBy   string `json:"revoked_by,omitempty"`
	Reason      string `json:"reason,omitempty"`
}

func viewRevokedAccessKid(record account.RevokedAccessKid) revokedAccessKidView {
	return revokedAccessKidView{
		Kid:         record.Kid,
		RevokedAtMS: record.RevokedAt.UnixMilli(),
		RevokedBy:   record.RevokedBy,
		Reason:      record.Reason,
	}
}

func (s *server) listRevokedAccessKids(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.adminActor(w, r, account.TokenResourceDevices); !ok {
		return
	}
	records, err := s.cfg.Accounts.RevokedAccessKids()
	if err != nil {
		s.cfg.Logger.Error("list revoked access keys", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read revoked access keys")
		return
	}
	out := make([]revokedAccessKidView, 0, len(records))
	for _, record := range records {
		out = append(out, viewRevokedAccessKid(record))
	}
	writeJSON(w, http.StatusOK, map[string]any{"kids": out})
}

type revokeAccessKidRequest struct {
	Kid    string `json:"kid"`
	Reason string `json:"reason"`
}

// revokeAccessKid retires one issuer key and pushes the statement to every
// connected host. New tickets cannot be signed with the key afterwards; the
// push ends sessions that are already running instead of waiting for their
// 60-second lease to lapse.
func (s *server) revokeAccessKid(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceDevices)
	if !ok {
		return
	}
	var req revokeAccessKidRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	record, err := s.cfg.Accounts.RevokeAccessKid(req.Kid, actor, req.Reason)
	if err != nil {
		if errors.Is(err, account.ErrInvalidAccessKid) {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		s.cfg.Logger.Error("revoke access key", "kid", req.Kid, "error", err)
		writeError(w, http.StatusInternalServerError, "could not revoke the access key")
		return
	}
	s.recordAudit(r, actor, account.AuditAccessKidRevoke,
		"revoked access key "+record.Kid+": "+record.Reason, true)
	if s.eventHub != nil {
		s.eventHub.publishAll(hostEvent{
			Kind:   "revoke",
			Kid:    record.Kid,
			Reason: record.Reason,
		})
	}
	writeJSON(w, http.StatusOK, viewRevokedAccessKid(record))
}

// accessIssuerRevoked refuses issuance when the configured signer was retired
// in this deployment. A store failure is treated as revoked: signing with a
// key whose status is unknown is the worse outcome.
func (s *server) accessIssuerRevoked() bool {
	if s.accessIssuer == nil || s.cfg.Accounts == nil {
		return false
	}
	revoked, err := s.cfg.Accounts.AccessKidRevoked(s.accessIssuer.KeyID())
	if err != nil {
		s.cfg.Logger.Error("read access key status", "kid", s.accessIssuer.KeyID(), "error", err)
		return true
	}
	return revoked
}
