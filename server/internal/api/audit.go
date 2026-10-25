package api

import (
	"net/http"
	"strconv"
	"strings"
)

type auditEventView struct {
	ID          int64  `json:"id"`
	CreatedAtMS int64  `json:"created_at_ms"`
	Username    string `json:"username"`
	Action      string `json:"action"`
	Detail      string `json:"detail"`
	ClientIP    string `json:"client_ip"`
	Success     bool   `json:"success"`
}

type auditResponse struct {
	Events []auditEventView `json:"events"`
}

// auditLog exposes the persisted account-security trail to operators holding
// the server key. Viewer account tokens are deliberately not accepted here.
func (s *server) auditLog(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	limit := 50
	if raw := strings.TrimSpace(r.URL.Query().Get("limit")); raw != "" {
		parsed, err := strconv.Atoi(raw)
		if err != nil || parsed < 1 || parsed > 200 {
			writeError(w, http.StatusBadRequest, "limit must be between 1 and 200")
			return
		}
		limit = parsed
	}
	events, err := s.cfg.Accounts.AuditEventsFor(r.URL.Query().Get("action"), limit)
	if err != nil {
		s.cfg.Logger.Error("account audit query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the audit log")
		return
	}
	out := auditResponse{Events: make([]auditEventView, 0, len(events))}
	for _, event := range events {
		out.Events = append(out.Events, auditEventView{
			ID:          event.ID,
			CreatedAtMS: event.CreatedAt.UnixMilli(),
			Username:    event.Username,
			Action:      event.Action,
			Detail:      event.Detail,
			ClientIP:    event.ClientIP,
			Success:     event.Success,
		})
	}
	writeJSON(w, http.StatusOK, out)
}
