package api

import (
	"errors"
	"fmt"
	"net/http"
	"os"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type consoleRecordingsResponse struct {
	Recordings     []account.Recording `json:"recordings"`
	UsageBytes     int64               `json:"usage_bytes"`
	Total          int                 `json:"total"`
	NextBeforeID   int64               `json:"next_before_id,omitempty"`
	MaxUploadBytes int64               `json:"max_upload_bytes"`
}

// consoleRecordings lists the archive across accounts for operators. The
// username filter is the owner, not the caller, so an operator can inspect one
// account without knowing its user id.
func (s *server) consoleRecordings(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.consoleActor(w, r, consoleRead); !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	beforeID, ok := s.listBeforeID(w, r)
	if !ok {
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	username := r.URL.Query().Get("username")
	list, err := s.cfg.Accounts.AllRecordings(username, beforeID, limit)
	if err != nil {
		s.cfg.Logger.Error("recording list", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recordings")
		return
	}
	usage, total, err := s.cfg.Accounts.RecordingTotals(username)
	if err != nil {
		s.cfg.Logger.Error("recording totals", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recordings")
		return
	}
	if list == nil {
		list = []account.Recording{}
	}
	var lastID int64
	if len(list) > 0 {
		lastID = list[len(list)-1].Seq
	}
	writeJSON(w, http.StatusOK, consoleRecordingsResponse{
		Recordings:     list,
		UsageBytes:     usage,
		Total:          total,
		NextBeforeID:   nextBeforeID(lastID, len(list), limit),
		MaxUploadBytes: archive.MaxUpload(),
	})
}

// consoleDownloadRecording streams any account's recording to an operator. The
// owner comes from the row, so the archive lookup is a plain file open and
// never trusts a caller-supplied owner.
func (s *server) consoleDownloadRecording(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.consoleActor(w, r, consoleRead); !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	rec, err := s.cfg.Accounts.RecordingByID(r.PathValue("id"))
	if errors.Is(err, account.ErrUnknownRecording) {
		writeError(w, http.StatusNotFound, "recording is unknown")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("recording open", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recording")
		return
	}
	file, _, err := archive.Open(rec.UserID, rec.ID)
	if errors.Is(err, os.ErrNotExist) {
		writeError(w, http.StatusNotFound, "recording file is missing")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("recording open", "id", rec.ID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recording")
		return
	}
	defer file.Close()
	_ = http.NewResponseController(w).SetWriteDeadline(time.Time{})
	w.Header().Set("Content-Type", "video/mp4")
	w.Header().Set("Content-Disposition", attachmentDisposition(rec.Name))
	http.ServeContent(w, r, rec.Name, time.UnixMilli(rec.CreatedAtMS), file)
}

// consoleDeleteRecording removes any recording and writes the audit entry to
// the owner, with the operator in the detail, so the account trail still shows
// who removed the file.
func (s *server) consoleDeleteRecording(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.consoleActor(w, r, consoleWrite)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	rec, err := s.cfg.Accounts.RecordingByID(r.PathValue("id"))
	if errors.Is(err, account.ErrUnknownRecording) {
		s.recordAudit(r, actor, account.AuditRecordingDelete, "unknown recording", false)
		writeError(w, http.StatusNotFound, "recording is unknown")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("recording delete", "error", err)
		writeError(w, http.StatusInternalServerError, "could not delete the recording")
		return
	}
	if _, err := archive.Remove(rec.UserID, rec.ID); err != nil {
		s.cfg.Logger.Error("recording delete", "user", rec.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not delete the recording")
		return
	}
	detail := fmt.Sprintf("id=%s name=%q by=%s", rec.ID, rec.Name, actor)
	s.recordAudit(r, rec.Username, account.AuditRecordingDelete, detail, true)
	w.WriteHeader(http.StatusNoContent)
}
