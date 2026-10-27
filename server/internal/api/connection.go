package api

import (
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type connectionReportRequest struct {
	PeerID      string `json:"peer_id"`
	Direction   string `json:"direction"`
	StartedAtMS int64  `json:"started_at_ms"`
	EndedAtMS   int64  `json:"ended_at_ms"`
	Success     *bool  `json:"success"`
	Detail      string `json:"detail"`
	AuditRef    string `json:"audit_ref,omitempty"`
}

type connectionView struct {
	ID          int64  `json:"id"`
	Username    string `json:"username"`
	PeerID      string `json:"peer_id"`
	Direction   string `json:"direction"`
	HostID      string `json:"host_id,omitempty"`
	AuditRef    string `json:"audit_ref,omitempty"`
	Source      string `json:"source,omitempty"`
	ClientIP    string `json:"client_ip"`
	StartedAtMS int64  `json:"started_at_ms"`
	EndedAtMS   int64  `json:"ended_at_ms"`
	DurationMS  int64  `json:"duration_ms"`
	Success     bool   `json:"success"`
	Detail      string `json:"detail"`
}

type connectionsResponse struct {
	Connections []connectionView `json:"connections"`
}

func connectionToView(event account.ConnectionEvent) connectionView {
	return connectionView{
		ID:          event.ID,
		Username:    event.Username,
		PeerID:      event.PeerID,
		Direction:   event.Direction,
		HostID:      event.HostID,
		AuditRef:    event.AuditRef,
		Source:      event.Source,
		ClientIP:    event.ClientIP,
		StartedAtMS: event.StartedAt.UnixMilli(),
		EndedAtMS:   event.EndedAt.UnixMilli(),
		DurationMS:  event.Duration().Milliseconds(),
		Success:     event.Success,
		Detail:      event.Detail,
	}
}

func (s *server) connectionLimit(w http.ResponseWriter, r *http.Request) (int, bool) {
	limit := 50
	if raw := strings.TrimSpace(r.URL.Query().Get("limit")); raw != "" {
		parsed, err := strconv.Atoi(raw)
		if err != nil || parsed < 1 || parsed > 200 {
			writeError(w, http.StatusBadRequest, "limit must be between 1 and 200")
			return 0, false
		}
		limit = parsed
	}
	return limit, true
}

// recordAccountConnection accepts one finished session from the signed-in
// viewer. The account and client address come from the request, never from
// the body, so a token cannot forge another user's trail.
func (s *server) recordAccountConnection(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req connectionReportRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	success := true
	if req.Success != nil {
		success = *req.Success
	}
	var startedAt, endedAt time.Time
	if req.StartedAtMS > 0 {
		startedAt = time.UnixMilli(req.StartedAtMS)
	}
	if req.EndedAtMS > 0 {
		endedAt = time.UnixMilli(req.EndedAtMS)
	}
	event, err := s.cfg.Accounts.RecordConnection(
		user.ID, user.Username, req.PeerID, req.Direction, s.clientIP(r),
		req.AuditRef, startedAt, endedAt, success, req.Detail,
	)
	if err != nil {
		if errors.Is(err, account.ErrInvalidConnection) {
			writeError(w, http.StatusBadRequest, "connection report is invalid")
			return
		}
		s.cfg.Logger.Error("account connection report", "error", err)
		writeError(w, http.StatusInternalServerError, "could not store the connection report")
		return
	}
	writeJSON(w, http.StatusCreated, connectionToView(event))
}

func (s *server) accountConnections(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	events, err := s.cfg.Accounts.Connections(user.ID, limit)
	if err != nil {
		s.cfg.Logger.Error("account connection query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the connection log")
		return
	}
	out := connectionsResponse{Connections: make([]connectionView, 0, len(events))}
	for _, event := range events {
		out.Connections = append(out.Connections, connectionToView(event))
	}
	writeJSON(w, http.StatusOK, out)
}

// auditConnections is the operator view: every account's reports, optionally
// narrowed to one username. Account tokens are rejected here.
func (s *server) auditConnections(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	var (
		events []account.ConnectionEvent
		err    error
	)
	if host := strings.TrimSpace(r.URL.Query().Get("host")); host != "" {
		events, err = s.cfg.Accounts.ConnectionsForHost(host, limit)
	} else {
		events, err = s.cfg.Accounts.ConnectionsFor(r.URL.Query().Get("username"), limit)
	}
	if err != nil {
		s.cfg.Logger.Error("account connection audit query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the connection log")
		return
	}
	out := connectionsResponse{Connections: make([]connectionView, 0, len(events))}
	for _, event := range events {
		out.Connections = append(out.Connections, connectionToView(event))
	}
	writeJSON(w, http.StatusOK, out)
}
