package api

import (
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/recordings"
)

type recordingsResponse struct {
	Recordings     []account.Recording `json:"recordings"`
	UsageBytes     int64               `json:"usage_bytes"`
	QuotaBytes     int64               `json:"quota_bytes"`
	MaxUploadBytes int64               `json:"max_upload_bytes"`
}

type recordingUploadResponse struct {
	Recording  account.Recording `json:"recording"`
	UsageBytes int64             `json:"usage_bytes"`
	QuotaBytes int64             `json:"quota_bytes"`
}

// recordingArchive resolves the optional archive once per request. A server
// started without --recordings-dir answers 503 so clients can hide the
// feature instead of treating it as a missing recording.
func (s *server) recordingArchive(w http.ResponseWriter) (*recordings.Archive, bool) {
	if s.cfg.Recordings == nil {
		writeError(w, http.StatusServiceUnavailable, "recording archive is not enabled")
		return nil, false
	}
	return s.cfg.Recordings, true
}

// uploadRecording accepts the raw MP4 body plus metadata in the query string.
// A header would carry the name as Latin-1, which breaks non-ASCII file names,
// so everything user-visible travels URL-encoded.
func (s *server) uploadRecording(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	// The HTTP server bounds reads to 15s, which a large upload over a slow
	// link exceeds. The archive enforces its own byte limit while copying.
	_ = http.NewResponseController(w).SetReadDeadline(time.Time{})
	query := r.URL.Query()
	startedAt, _ := strconv.ParseInt(query.Get("started_ms"), 10, 64)
	duration, _ := strconv.ParseInt(query.Get("duration_ms"), 10, 64)
	rec, err := archive.Save(
		user.ID,
		query.Get("name"),
		query.Get("peer_id"),
		startedAt,
		duration,
		r.Body,
		r.ContentLength,
	)
	switch {
	case errors.Is(err, recordings.ErrEmpty):
		writeError(w, http.StatusBadRequest, "recording is empty")
		return
	case errors.Is(err, recordings.ErrTooLarge):
		writeError(w, http.StatusRequestEntityTooLarge, "recording exceeds the upload limit")
		return
	case errors.Is(err, recordings.ErrQuota):
		writeError(w, http.StatusRequestEntityTooLarge, "account recording quota is exhausted")
		return
	case err != nil:
		s.cfg.Logger.Error("recording upload", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not store the recording")
		return
	}
	usage, _, err := s.cfg.Accounts.RecordingUsage(user.ID)
	if err != nil {
		s.cfg.Logger.Error("recording usage", "user", user.Username, "error", err)
		usage = rec.Size
	}
	detail := fmt.Sprintf("id=%s name=%q size=%d", rec.ID, rec.Name, rec.Size)
	s.recordAudit(r, user.Username, account.AuditRecordingUpload, detail, true)
	writeJSON(w, http.StatusCreated, recordingUploadResponse{
		Recording:  rec,
		UsageBytes: usage,
		QuotaBytes: archive.Quota(),
	})
}

// accountRecordings lists one account's archive with its quota state, newest
// first. The client uses max_upload_bytes to refuse an oversized file before
// it starts sending.
func (s *server) accountRecordings(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	list, err := s.cfg.Accounts.Recordings(user.ID)
	if err != nil {
		s.cfg.Logger.Error("recording list", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recordings")
		return
	}
	usage, _, err := s.cfg.Accounts.RecordingUsage(user.ID)
	if err != nil {
		s.cfg.Logger.Error("recording usage", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recordings")
		return
	}
	if list == nil {
		list = []account.Recording{}
	}
	writeJSON(w, http.StatusOK, recordingsResponse{
		Recordings:     list,
		UsageBytes:     usage,
		QuotaBytes:     archive.Quota(),
		MaxUploadBytes: archive.MaxUpload(),
	})
}

func (s *server) downloadRecording(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	file, rec, err := archive.Open(user.ID, r.PathValue("id"))
	if errors.Is(err, account.ErrUnknownRecording) {
		writeError(w, http.StatusNotFound, "recording is unknown")
		return
	}
	if errors.Is(err, os.ErrNotExist) {
		writeError(w, http.StatusNotFound, "recording file is missing")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("recording open", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the recording")
		return
	}
	defer file.Close()
	// A long download can outlive the server's 30s write timeout; the client
	// still bounds its own receive time.
	_ = http.NewResponseController(w).SetWriteDeadline(time.Time{})
	w.Header().Set("Content-Type", "video/mp4")
	w.Header().Set("Content-Disposition", attachmentDisposition(rec.Name))
	http.ServeContent(w, r, rec.Name, time.UnixMilli(rec.CreatedAtMS), file)
}

func (s *server) deleteRecording(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	rec, err := archive.Remove(user.ID, r.PathValue("id"))
	if errors.Is(err, account.ErrUnknownRecording) {
		s.recordAudit(r, user.Username, account.AuditRecordingDelete, "unknown recording", false)
		writeError(w, http.StatusNotFound, "recording is unknown")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("recording delete", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not delete the recording")
		return
	}
	s.recordAudit(r, user.Username, account.AuditRecordingDelete,
		fmt.Sprintf("id=%s name=%q", rec.ID, rec.Name), true)
	w.WriteHeader(http.StatusNoContent)
}

// attachmentDisposition keeps the ASCII fallback for old clients and carries
// the real name in the RFC 5987 parameter.
func attachmentDisposition(name string) string {
	return fmt.Sprintf(`attachment; filename="recording.mp4"; filename*=UTF-8''%s`,
		url.PathEscape(name))
}
