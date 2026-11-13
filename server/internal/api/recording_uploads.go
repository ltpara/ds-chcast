package api

import (
	"errors"
	"net/http"
	"strconv"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/recordings"
)

const (
	recordingUploadOffsetHeader = "X-Upload-Offset"
	maxRecordingChunkBytes      = 32 << 20
)

type beginRecordingUploadRequest struct {
	Name        string `json:"name"`
	PeerID      string `json:"peer_id"`
	Size        int64  `json:"size"`
	StartedAtMS int64  `json:"started_at_ms"`
	DurationMS  int64  `json:"duration_ms"`
}

type recordingUploadView struct {
	Upload recordings.Upload `json:"upload"`
}

func (s *server) beginRecordingUpload(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	var req beginRecordingUploadRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, "recording upload metadata is invalid")
		return
	}
	upload, err := archive.BeginUpload(user.ID, req.Name, req.PeerID,
		req.StartedAtMS, req.DurationMS, req.Size)
	if err != nil {
		s.recordingUploadError(w, user, "begin", err)
		return
	}
	writeJSON(w, http.StatusCreated, recordingUploadView{Upload: upload})
}

func (s *server) recordingUploadStatus(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	upload, err := archive.UploadStatus(user.ID, r.PathValue("id"))
	if err != nil {
		s.recordingUploadError(w, user, "status", err)
		return
	}
	writeJSON(w, http.StatusOK, recordingUploadView{Upload: upload})
}

func (s *server) appendRecordingUpload(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	offset, err := strconv.ParseInt(r.Header.Get(recordingUploadOffsetHeader), 10, 64)
	if err != nil || offset < 0 {
		writeError(w, http.StatusBadRequest, "recording upload offset is invalid")
		return
	}
	if r.ContentLength <= 0 || r.ContentLength > maxRecordingChunkBytes {
		writeError(w, http.StatusBadRequest, "recording upload chunk size is invalid")
		return
	}
	controller := http.NewResponseController(w)
	deadline := time.Now().Add(10 * time.Minute)
	if err := controller.SetReadDeadline(deadline); err != nil {
		s.cfg.Logger.Error("recording chunk read deadline", "error", err)
		writeError(w, http.StatusInternalServerError, "could not configure recording upload")
		return
	}
	if err := controller.SetWriteDeadline(deadline.Add(30 * time.Second)); err != nil {
		s.cfg.Logger.Error("recording chunk write deadline", "error", err)
		writeError(w, http.StatusInternalServerError, "could not configure recording upload")
		return
	}
	upload, err := archive.AppendUpload(user.ID, r.PathValue("id"), offset, r.ContentLength, r.Body)
	if err != nil {
		s.recordingUploadError(w, user, "append", err)
		return
	}
	writeJSON(w, http.StatusAccepted, recordingUploadView{Upload: upload})
}

func (s *server) completeRecordingUpload(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	controller := http.NewResponseController(w)
	if err := controller.SetWriteDeadline(time.Now().Add(30 * time.Minute)); err != nil {
		s.cfg.Logger.Error("recording complete deadline", "error", err)
		writeError(w, http.StatusInternalServerError, "could not configure recording upload")
		return
	}
	rec, err := archive.CompleteUpload(user.ID, r.PathValue("id"))
	if err != nil {
		s.recordingUploadError(w, user, "complete", err)
		return
	}
	usage, _, err := s.cfg.Accounts.RecordingUsage(user.ID)
	if err != nil {
		s.cfg.Logger.Error("recording usage", "user", user.Username, "error", err)
		usage = rec.Size
	}
	s.recordAudit(r, user.Username, account.AuditRecordingUpload,
		"id="+rec.ID+" name="+strconv.Quote(rec.Name)+" size="+strconv.FormatInt(rec.Size, 10), true)
	writeJSON(w, http.StatusCreated, recordingUploadResponse{
		Recording: rec, UsageBytes: usage, QuotaBytes: archive.Quota(),
	})
}

func (s *server) deleteRecordingUpload(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	if err := archive.RemoveUpload(user.ID, r.PathValue("id")); err != nil {
		s.recordingUploadError(w, user, "remove", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) recordingUploadError(w http.ResponseWriter, user account.User, action string, err error) {
	var offsetErr *recordings.UploadOffsetError
	switch {
	case errors.Is(err, recordings.ErrUnknownUpload):
		writeError(w, http.StatusNotFound, "recording upload is unknown")
	case errors.As(err, &offsetErr):
		writeJSON(w, http.StatusConflict, map[string]any{
			"error": "recording upload offset is stale", "offset": offsetErr.Actual,
		})
	case errors.Is(err, recordings.ErrEmpty), errors.Is(err, recordings.ErrUploadIncomplete),
		errors.Is(err, recordings.ErrUploadSize):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, recordings.ErrTooLarge):
		writeError(w, http.StatusRequestEntityTooLarge, "recording exceeds the upload limit")
	case errors.Is(err, recordings.ErrQuota):
		writeError(w, http.StatusRequestEntityTooLarge, "account recording quota is exhausted")
	case errors.Is(err, recordings.ErrUploadLimit):
		writeError(w, http.StatusTooManyRequests, "too many active recording uploads")
	default:
		s.cfg.Logger.Error("recording upload", "user", user.Username, "action", action, "error", err)
		writeError(w, http.StatusInternalServerError, "could not store the recording")
	}
}
