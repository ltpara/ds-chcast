package api

import (
	"encoding/hex"
	"errors"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/recordings"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const (
	hostRecordingTimestampHeader   = "X-Host-Timestamp"
	hostRecordingSignatureHeader   = "X-Host-Signature"
	hostRecordingChunkDigestHeader = "X-Chunk-SHA256"
)

type beginHostRecordingUploadRequest struct {
	Name           string `json:"name"`
	Size           int64  `json:"size"`
	StartedAtMS    int64  `json:"started_at_ms"`
	DurationMS     int64  `json:"duration_ms"`
	IdempotencyKey string `json:"idempotency_key"`
}

// hostRecordingOwner authenticates both the installed host credential and
// the registered device identity. The archive owner is always resolved from
// the server-side registry, never from a caller-supplied account name.
func (s *server) hostRecordingOwner(w http.ResponseWriter, r *http.Request,
	uploadID string, offset, size int64, metadata ...string) (account.User, string, bool) {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential was not accepted")
		return account.User{}, "", false
	}
	if !secureHostRecordingTransport(r) {
		writeError(w, http.StatusUpgradeRequired, "host recording uploads require HTTPS")
		return account.User{}, "", false
	}
	if s.cfg.Accounts == nil || s.cfg.Registry == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return account.User{}, "", false
	}
	hostID := strings.TrimSpace(r.PathValue("id"))
	if !registry.ValidID(hostID) {
		writeError(w, http.StatusBadRequest, "host id must be 1..64 characters of [A-Za-z0-9_.-]")
		return account.User{}, "", false
	}
	record, exists := s.cfg.Registry.Identity(hostID)
	if !exists {
		writeError(w, http.StatusNotFound, "unknown host")
		return account.User{}, "", false
	}
	if record.Disabled {
		writeError(w, http.StatusForbidden, "device is disabled")
		return account.User{}, "", false
	}
	timestamp, err := strconv.ParseInt(r.Header.Get(hostRecordingTimestampHeader), 10, 64)
	if err != nil || timestamp == 0 {
		writeError(w, http.StatusUnauthorized, "host recording timestamp is invalid")
		return account.User{}, "", false
	}
	now := time.Now()
	signedAt := time.Unix(timestamp, 0)
	if now.Sub(signedAt) > maxSkew || signedAt.Sub(now) > maxSkew {
		writeError(w, http.StatusUnauthorized, "host recording timestamp is outside the allowed window")
		return account.User{}, "", false
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil {
		s.cfg.Logger.Error("host recording identity", "id", hostID, "error", err)
		writeError(w, http.StatusInternalServerError, "host identity is unreadable")
		return account.User{}, "", false
	}
	message := identity.HostRecordingUploadMessage(hostID, r.Method, uploadID,
		timestamp, offset, size, metadata...)
	if !identity.Verify(pub, message, r.Header.Get(hostRecordingSignatureHeader)) {
		writeError(w, http.StatusUnauthorized, "host recording signature was not accepted")
		return account.User{}, "", false
	}
	if record.Owner == "" {
		writeError(w, http.StatusConflict, "host has no archive owner")
		return account.User{}, "", false
	}
	user, err := s.cfg.Accounts.FindUser(record.Owner)
	if err != nil {
		if errors.Is(err, account.ErrUnknownUser) {
			writeError(w, http.StatusConflict, "host archive owner is unavailable")
			return account.User{}, "", false
		}
		s.cfg.Logger.Error("host recording owner", "id", hostID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not resolve host archive owner")
		return account.User{}, "", false
	}
	if user.Banned {
		writeError(w, http.StatusForbidden, "host archive owner is disabled")
		return account.User{}, "", false
	}
	return user, hostID, true
}

func secureHostRecordingTransport(r *http.Request) bool {
	if r.TLS != nil {
		return true
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return false
	}
	ip := net.ParseIP(strings.Trim(host, "[]"))
	return ip != nil && ip.IsLoopback()
}

func (s *server) beginHostRecordingUpload(w http.ResponseWriter, r *http.Request) {
	archive, ok := s.recordingArchive(w)
	if !ok {
		return
	}
	var req beginHostRecordingUploadRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, "recording upload metadata is invalid")
		return
	}
	if strings.TrimSpace(req.IdempotencyKey) == "" {
		writeError(w, http.StatusBadRequest, "host recording upload needs an idempotency key")
		return
	}
	metadata := []string{req.Name, strconv.FormatInt(req.StartedAtMS, 10),
		strconv.FormatInt(req.DurationMS, 10), req.IdempotencyKey}
	user, hostID, ok := s.hostRecordingOwner(w, r, "", 0, req.Size, metadata...)
	if !ok {
		return
	}
	if err := archive.ValidateBeginUpload(req.Size, req.IdempotencyKey); err != nil {
		s.recordingUploadError(w, user, "host begin", err)
		return
	}
	if err := s.cfg.Accounts.BindHostRecordingUploadOwner(hostID, req.IdempotencyKey, user.ID); err != nil {
		if errors.Is(err, account.ErrHostRecordingOwnerConflict) {
			writeError(w, http.StatusConflict, "host recording upload is bound to a different owner")
			return
		}
		s.cfg.Logger.Error("host recording owner binding", "id", hostID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not bind host recording owner")
		return
	}
	upload, completed, err := archive.BeginUploadWithKey(user.ID, req.Name, hostID,
		req.StartedAtMS, req.DurationMS, req.Size, req.IdempotencyKey)
	if err != nil {
		s.recordingUploadError(w, user, "host begin", err)
		return
	}
	if completed != nil {
		writeJSON(w, http.StatusOK, recordingUploadView{Recording: completed})
		return
	}
	writeJSON(w, http.StatusCreated, recordingUploadView{Upload: &upload})
}

func (s *server) hostUploadForDevice(w http.ResponseWriter, user account.User,
	hostID, uploadID string) (recordings.Upload, bool) {
	upload, err := s.cfg.Recordings.UploadStatus(user.ID, uploadID)
	if err != nil {
		s.hostRecordingUploadError(w, user, "status", err)
		return recordings.Upload{}, false
	}
	if upload.PeerID != hostID {
		writeError(w, http.StatusNotFound, "recording upload is unknown")
		return recordings.Upload{}, false
	}
	return upload, true
}

func (s *server) hostRecordingUploadStatus(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.recordingArchive(w); !ok {
		return
	}
	uploadID := r.PathValue("upload")
	user, hostID, ok := s.hostRecordingOwner(w, r, uploadID, 0, 0)
	if !ok {
		return
	}
	if _, ok := s.hostUploadForDevice(w, user, hostID, uploadID); !ok {
		return
	}
	upload, err := s.cfg.Recordings.UploadStatus(user.ID, uploadID)
	if err != nil {
		s.hostRecordingUploadError(w, user, "status", err)
		return
	}
	writeJSON(w, http.StatusOK, recordingUploadView{Upload: &upload})
}

func (s *server) appendHostRecordingUpload(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.recordingArchive(w); !ok {
		return
	}
	if r.ContentLength <= 0 || r.ContentLength > maxRecordingChunkBytes {
		writeError(w, http.StatusBadRequest, "recording upload chunk size is invalid")
		return
	}
	offset, err := strconv.ParseInt(r.Header.Get(recordingUploadOffsetHeader), 10, 64)
	if err != nil || offset < 0 {
		writeError(w, http.StatusBadRequest, "recording upload offset is invalid")
		return
	}
	chunkDigest := r.Header.Get(hostRecordingChunkDigestHeader)
	decodedDigest, err := hex.DecodeString(chunkDigest)
	if err != nil || len(decodedDigest) != 32 || chunkDigest != strings.ToLower(chunkDigest) {
		writeError(w, http.StatusBadRequest, "recording upload chunk digest is invalid")
		return
	}
	uploadID := r.PathValue("upload")
	user, hostID, ok := s.hostRecordingOwner(w, r, uploadID, offset, r.ContentLength, chunkDigest)
	if !ok {
		return
	}
	if _, ok := s.hostUploadForDevice(w, user, hostID, uploadID); !ok {
		return
	}
	controller := http.NewResponseController(w)
	deadline := time.Now().Add(10 * time.Minute)
	if err := controller.SetReadDeadline(deadline); err != nil {
		s.cfg.Logger.Error("host recording chunk read deadline", "error", err)
		writeError(w, http.StatusInternalServerError, "could not configure recording upload")
		return
	}
	if err := controller.SetWriteDeadline(deadline.Add(30 * time.Second)); err != nil {
		s.cfg.Logger.Error("host recording chunk write deadline", "error", err)
		writeError(w, http.StatusInternalServerError, "could not configure recording upload")
		return
	}
	upload, err := s.cfg.Recordings.AppendUploadWithDigest(
		user.ID, uploadID, offset, r.ContentLength, r.Body, chunkDigest)
	if err != nil {
		s.hostRecordingUploadError(w, user, "append", err)
		return
	}
	writeJSON(w, http.StatusAccepted, recordingUploadView{Upload: &upload})
}

func (s *server) completeHostRecordingUpload(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.recordingArchive(w); !ok {
		return
	}
	uploadID := r.PathValue("upload")
	user, hostID, ok := s.hostRecordingOwner(w, r, uploadID, 0, 0)
	if !ok {
		return
	}
	if _, ok := s.hostUploadForDevice(w, user, hostID, uploadID); !ok {
		return
	}
	controller := http.NewResponseController(w)
	if err := controller.SetWriteDeadline(time.Now().Add(30 * time.Minute)); err != nil {
		s.cfg.Logger.Error("host recording complete deadline", "error", err)
		writeError(w, http.StatusInternalServerError, "could not configure recording upload")
		return
	}
	rec, err := s.cfg.Recordings.CompleteUpload(user.ID, uploadID)
	if err != nil {
		s.hostRecordingUploadError(w, user, "complete", err)
		return
	}
	usage, _, err := s.cfg.Accounts.RecordingUsage(user.ID)
	if err != nil {
		s.cfg.Logger.Error("host recording usage", "user", user.Username, "error", err)
		usage = rec.Size
	}
	s.recordAudit(r, user.Username, account.AuditRecordingUpload,
		"id="+rec.ID+" host="+hostID+" name="+strconv.Quote(rec.Name)+" size="+strconv.FormatInt(rec.Size, 10), true)
	writeJSON(w, http.StatusCreated, recordingUploadResponse{
		Recording: rec, UsageBytes: usage, QuotaBytes: s.cfg.Recordings.Quota(),
	})
}

func (s *server) deleteHostRecordingUpload(w http.ResponseWriter, r *http.Request) {
	if _, ok := s.recordingArchive(w); !ok {
		return
	}
	uploadID := r.PathValue("upload")
	user, hostID, ok := s.hostRecordingOwner(w, r, uploadID, 0, 0)
	if !ok {
		return
	}
	if _, ok := s.hostUploadForDevice(w, user, hostID, uploadID); !ok {
		return
	}
	if err := s.cfg.Recordings.RemoveUpload(user.ID, uploadID); err != nil {
		s.hostRecordingUploadError(w, user, "remove", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) hostRecordingUploadError(w http.ResponseWriter, user account.User,
	action string, err error) {
	if errors.Is(err, recordings.ErrUnknownUpload) {
		// A device owner change makes the previous account's upload unavailable
		// through the new owner. Do not reveal the former owner or reassign it.
		writeError(w, http.StatusConflict, "recording upload is unavailable for the current device owner")
		return
	}
	s.recordingUploadError(w, user, action, err)
}
