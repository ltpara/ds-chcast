package api

import (
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type fileTransferReportRequest struct {
	PeerID       string `json:"peer_id"`
	Direction    string `json:"direction"`
	Name         string `json:"name"`
	Size         int64  `json:"size"`
	Files        int    `json:"files"`
	Success      *bool  `json:"success"`
	Detail       string `json:"detail"`
	OccurredAtMS int64  `json:"occurred_at_ms"`
	AuditRef     string `json:"audit_ref,omitempty"`
}

type fileTransferView struct {
	ID           int64  `json:"id"`
	Username     string `json:"username"`
	PeerID       string `json:"peer_id"`
	Direction    string `json:"direction"`
	HostID       string `json:"host_id,omitempty"`
	AuditRef     string `json:"audit_ref,omitempty"`
	Source       string `json:"source,omitempty"`
	Name         string `json:"name"`
	Size         int64  `json:"size"`
	Files        int    `json:"files"`
	ClientIP     string `json:"client_ip"`
	OccurredAtMS int64  `json:"occurred_at_ms"`
	Success      bool   `json:"success"`
	Detail       string `json:"detail"`
}

type fileTransfersResponse struct {
	Transfers    []fileTransferView `json:"transfers"`
	NextBeforeID int64              `json:"next_before_id,omitempty"`
}

func fileTransferToView(event account.FileEvent) fileTransferView {
	return fileTransferView{
		ID:           event.ID,
		Username:     event.Username,
		PeerID:       event.PeerID,
		Direction:    event.Direction,
		HostID:       event.HostID,
		AuditRef:     event.AuditRef,
		Source:       event.Source,
		Name:         event.Name,
		Size:         event.Size,
		Files:        event.Files,
		ClientIP:     event.ClientIP,
		OccurredAtMS: event.OccurredAt.UnixMilli(),
		Success:      event.Success,
		Detail:       event.Detail,
	}
}

// recordAccountFileTransfer accepts one poll window of finished files. The
// account and client address come from the request, never from the body.
func (s *server) recordAccountFileTransfer(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req fileTransferReportRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	success := true
	if req.Success != nil {
		success = *req.Success
	}
	files := req.Files
	if files == 0 {
		files = 1
	}
	var occurredAt time.Time
	if req.OccurredAtMS > 0 {
		occurredAt = time.UnixMilli(req.OccurredAtMS)
	}
	event, err := s.cfg.Accounts.RecordFileTransfer(
		user.ID, user.Username, req.PeerID, req.Direction, req.Name,
		s.clientIP(r), req.AuditRef, req.Size, files, occurredAt, success, req.Detail,
	)
	if err != nil {
		if errors.Is(err, account.ErrInvalidTransfer) {
			writeError(w, http.StatusBadRequest, "file transfer report is invalid")
			return
		}
		s.cfg.Logger.Error("account file transfer report", "error", err)
		writeError(w, http.StatusInternalServerError, "could not store the file transfer report")
		return
	}
	writeJSON(w, http.StatusCreated, fileTransferToView(event))
}

func (s *server) accountFileTransfers(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	beforeID, ok := s.listBeforeID(w, r)
	if !ok {
		return
	}
	events, err := s.cfg.Accounts.FileTransfersBefore(user.ID, beforeID, limit)
	if err != nil {
		s.cfg.Logger.Error("account file transfer query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the file transfer log")
		return
	}
	out := fileTransfersResponse{Transfers: make([]fileTransferView, 0, len(events))}
	for _, event := range events {
		out.Transfers = append(out.Transfers, fileTransferToView(event))
	}
	if len(out.Transfers) > 0 {
		out.NextBeforeID = nextBeforeID(out.Transfers[len(out.Transfers)-1].ID,
			len(out.Transfers), limit)
	}
	writeJSON(w, http.StatusOK, out)
}

// auditFileTransfers is the operator view across accounts. Account tokens are
// rejected here.
func (s *server) auditFileTransfers(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	if asCSV, ok := exportRequest(w, r); !ok {
		return
	} else if asCSV {
		s.fileTransfersCSV(w, r)
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	beforeID, ok := s.listBeforeID(w, r)
	if !ok {
		return
	}
	var (
		events []account.FileEvent
		err    error
	)
	if host := strings.TrimSpace(r.URL.Query().Get("host")); host != "" {
		events, err = s.cfg.Accounts.FileTransfersForHostBefore(host, beforeID, limit)
	} else {
		events, err = s.cfg.Accounts.FileTransfersForBefore(r.URL.Query().Get("username"), beforeID, limit)
	}
	if err != nil {
		s.cfg.Logger.Error("account file transfer audit query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the file transfer log")
		return
	}
	out := fileTransfersResponse{Transfers: make([]fileTransferView, 0, len(events))}
	for _, event := range events {
		out.Transfers = append(out.Transfers, fileTransferToView(event))
	}
	if len(out.Transfers) > 0 {
		out.NextBeforeID = nextBeforeID(out.Transfers[len(out.Transfers)-1].ID,
			len(out.Transfers), limit)
	}
	writeJSON(w, http.StatusOK, out)
}
