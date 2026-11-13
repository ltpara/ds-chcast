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
	Route        string `json:"route,omitempty"`
}

// fileTransferBatchItem is one finished file in a detailed viewer report.
// The viewer's engine queues each completed upload or download instead of
// collapsing a poll window into one row with the last file's name.
type fileTransferBatchItem struct {
	Direction    string `json:"direction"`
	Name         string `json:"name"`
	Size         int64  `json:"size"`
	Success      *bool  `json:"success"`
	Detail       string `json:"detail"`
	OccurredAtMS int64  `json:"occurred_at_ms"`
}

type fileTransferBatchRequest struct {
	PeerID   string                  `json:"peer_id"`
	AuditRef string                  `json:"audit_ref,omitempty"`
	Route    string                  `json:"route,omitempty"`
	Items    []fileTransferBatchItem `json:"items"`
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
	Location     string `json:"location,omitempty"`
	Route        string `json:"route,omitempty"`
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
		Location:     event.Location,
		Route:        event.Route,
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
	ip := s.clientIP(r)
	event, err := s.cfg.Accounts.RecordFileTransfer(
		user.ID, user.Username, req.PeerID, req.Direction, req.Name,
		ip, s.clientLocation(ip), req.Route, req.AuditRef,
		req.Size, files, occurredAt, success, req.Detail,
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

// recordAccountFileTransferBatch accepts one poll's finished files, one row
// each. The account and client address come from the request, never from the
// body; a shared route and audit reference link the batch to its session.
func (s *server) recordAccountFileTransferBatch(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req fileTransferBatchRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if len(req.Items) == 0 || len(req.Items) > 200 {
		writeError(w, http.StatusBadRequest, "items must contain between 1 and 200 files")
		return
	}
	reports := make([]account.FileTransferReport, 0, len(req.Items))
	for _, item := range req.Items {
		success := true
		if item.Success != nil {
			success = *item.Success
		}
		var occurredAt time.Time
		if item.OccurredAtMS > 0 {
			occurredAt = time.UnixMilli(item.OccurredAtMS)
		}
		reports = append(reports, account.FileTransferReport{
			Direction:  item.Direction,
			Name:       item.Name,
			Size:       item.Size,
			Success:    success,
			Detail:     item.Detail,
			OccurredAt: occurredAt,
		})
	}
	ip := s.clientIP(r)
	events, err := s.cfg.Accounts.RecordFileTransfers(
		user.ID, user.Username, req.PeerID, ip, s.clientLocation(ip),
		req.Route, req.AuditRef, reports,
	)
	if err != nil {
		if errors.Is(err, account.ErrInvalidTransfer) {
			writeError(w, http.StatusBadRequest, "file transfer report is invalid")
			return
		}
		s.cfg.Logger.Error("account file transfer batch", "error", err)
		writeError(w, http.StatusInternalServerError, "could not store the file transfer report")
		return
	}
	out := make([]fileTransferView, 0, len(events))
	for _, event := range events {
		out = append(out, fileTransferToView(event))
	}
	writeJSON(w, http.StatusCreated, fileTransfersResponse{Transfers: out})
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

// auditFileTransfers is the operator view across accounts. Console readers may
// view it; viewer accounts may not.
func (s *server) auditFileTransfers(w http.ResponseWriter, r *http.Request) {
	groupID, ok := s.auditGroupFilter(w, r)
	if !ok {
		return
	}
	if groupID == "" {
		if _, _, ok := s.consoleActor(w, r, consoleRead, account.TokenResourceAudit); !ok {
			return
		}
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
	bounds, ok := reportTimeBounds(w, r)
	if !ok {
		return
	}
	var (
		events []account.FileEvent
		err    error
	)
	username := r.URL.Query().Get("username")
	host := strings.TrimSpace(r.URL.Query().Get("host"))
	if groupID != "" {
		events, err = s.cfg.Accounts.FileTransfersForGroupBeforeRange(
			groupID, username, host, bounds, beforeID, limit)
	} else {
		events, err = s.cfg.Accounts.FileTransfersForOperatorBeforeRange(
			username, host, bounds, beforeID, limit)
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
