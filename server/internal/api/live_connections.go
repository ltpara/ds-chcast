package api

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const maxLiveConnectionBody = 128 << 10

type liveConnectionsResponse struct {
	Connections []account.LiveConnection `json:"connections"`
}

type liveDisconnectRequest struct {
	RequestKey string `json:"request_key"`
	Reason     string `json:"reason,omitempty"`
}

type hostLiveSnapshotRequest struct {
	Snapshot  liveconn.Snapshot `json:"snapshot"`
	Timestamp int64             `json:"timestamp"`
	Signature string            `json:"signature"`
}

type hostLiveReceiptRequest struct {
	Receipt   liveconn.Receipt `json:"receipt"`
	Timestamp int64            `json:"timestamp"`
	Signature string           `json:"signature"`
}

func (s *server) listLiveConnections(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.consolePrincipalFor(w, r, consoleRead, account.TokenResourceDevices)
	if !ok {
		return
	}
	limit, ok := intQuery(w, r, "limit", 100, 1, 500)
	if !ok {
		return
	}
	connections, err := s.cfg.Accounts.LiveConnectionsInGroup(principal.GroupID, limit)
	if err != nil {
		s.cfg.Logger.Error("list live connections", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list live connections")
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, liveConnectionsResponse{Connections: connections})
}

func (s *server) getLiveConnection(w http.ResponseWriter, r *http.Request) {
	hostID := r.PathValue("host")
	_, ok := s.deviceActor(w, r, consoleRead, hostID)
	if !ok {
		return
	}
	connection, err := s.cfg.Accounts.LiveConnection(hostID, r.PathValue("connection"))
	if err != nil {
		s.writeLiveConnectionError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, connection)
}

func (s *server) disconnectLiveConnection(w http.ResponseWriter, r *http.Request) {
	hostID := r.PathValue("host")
	principal, ok := s.deviceActor(w, r, consoleWrite, hostID)
	if !ok {
		return
	}
	var req liveDisconnectRequest
	if err := decodeBodySize(w, r, &req, maxBody); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	command, err := s.cfg.Accounts.DisconnectLiveConnection(
		hostID, r.PathValue("connection"), req.RequestKey, principal.Actor, req.Reason,
	)
	if err != nil {
		s.writeLiveConnectionError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	// Queued or delivered deliberately returns 202: the exact native session
	// must acknowledge its absence before the console can call it disconnected.
	status := http.StatusAccepted
	if command.Status == liveconn.CommandExpired || command.Status == liveconn.CommandFailed {
		status = http.StatusConflict
	}
	writeJSON(w, status, command)
}

func (s *server) reportHostLiveConnections(w http.ResponseWriter, r *http.Request) {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req hostLiveSnapshotRequest
	if err := decodeBodySize(w, r, &req, maxLiveConnectionBody); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	peerID := r.PathValue("peer")
	normalized, err := req.Snapshot.Normalize()
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	digest, err := liveconn.Digest(normalized)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !s.verifyHostLiveRequest(w, r, req.Timestamp, req.Signature,
		identity.HostLiveSnapshotMessage(peerID, normalized.RuntimeID,
			normalized.Sequence, normalized.Terminal, digest, req.Timestamp)) {
		return
	}
	snapshot, err := s.cfg.Accounts.ApplyLiveSnapshot(peerID, normalized)
	if err != nil {
		s.writeLiveConnectionError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, snapshot)
}

func (s *server) fetchHostLiveCommands(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	timestamp, _ := strconv.ParseInt(query.Get("timestamp"), 10, 64)
	peerID := r.PathValue("peer")
	runtimeID := strings.ToLower(strings.TrimSpace(r.PathValue("runtime")))
	if !s.verifyHostLiveRequest(w, r, timestamp, query.Get("signature"),
		identity.HostLiveCommandsFetchMessage(peerID, runtimeID, timestamp)) {
		return
	}
	commands, err := s.cfg.Accounts.PollLiveCommands(peerID, runtimeID,
		liveconn.MaxQueuedCommands)
	if err != nil {
		s.writeLiveConnectionError(w, err)
		return
	}
	if commands == nil {
		commands = []liveconn.Command{}
	}
	digest, err := liveconn.CommandsDigest(commands)
	if err != nil {
		s.writeLiveConnectionError(w, err)
		return
	}
	responseTimestamp := time.Now().Unix()
	signature := s.cfg.Identity.SignLiveCommandsResponse(
		peerID, runtimeID, responseTimestamp, digest)
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, liveconn.SignedCommands{
		Commands: commands, Timestamp: responseTimestamp, Signature: signature,
	})
}

func (s *server) reportHostLiveReceipt(w http.ResponseWriter, r *http.Request) {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req hostLiveReceiptRequest
	if err := decodeBodySize(w, r, &req, maxBody); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	normalized, err := req.Receipt.Normalize()
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	peerID := r.PathValue("peer")
	runtimeID := strings.ToLower(strings.TrimSpace(r.PathValue("runtime")))
	if !s.verifyHostLiveRequest(w, r, req.Timestamp, req.Signature,
		identity.HostLiveReceiptMessage(peerID, runtimeID, normalized.ReceiptSequence,
			normalized.CommandID, normalized.Status, normalized.ConfirmedAbsent,
			normalized.Detail, req.Timestamp)) {
		return
	}
	receipt, err := s.cfg.Accounts.RecordLiveCommandReceipt(peerID, runtimeID, normalized)
	if err != nil {
		s.writeLiveConnectionError(w, err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, receipt)
}

// verifyHostLiveRequest binds every live-connection message to the registered
// device key. The shared host credential alone cannot report or poll for a
// different device id.
func (s *server) verifyHostLiveRequest(w http.ResponseWriter, r *http.Request,
	timestamp int64, signature string, message []byte) bool {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return false
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return false
	}
	peerID := r.PathValue("peer")
	if !registry.ValidID(peerID) {
		writeError(w, http.StatusBadRequest, "invalid host id")
		return false
	}
	record, ok := s.cfg.Registry.Identity(peerID)
	if !ok {
		writeError(w, http.StatusNotFound, "unknown host")
		return false
	}
	if record.Disabled {
		writeError(w, http.StatusForbidden, "host identity is disabled")
		return false
	}
	now := time.Now()
	signedAt := time.Unix(timestamp, 0)
	if timestamp == 0 || now.Sub(signedAt) > maxSkew || signedAt.Sub(now) > maxSkew {
		writeError(w, http.StatusUnauthorized, "live connection timestamp is outside the allowed window")
		return false
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil || !identity.Verify(pub, message, signature) {
		writeError(w, http.StatusUnauthorized, "live connection signature was not accepted")
		return false
	}
	return true
}

func (s *server) writeLiveConnectionError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrLiveConnectionNotFound):
		writeError(w, http.StatusNotFound, "live connection is unknown")
	case errors.Is(err, account.ErrLiveRuntimeStale):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrLiveRuntimeTerminal),
		errors.Is(err, account.ErrLiveSequenceConflict),
		errors.Is(err, liveconn.ErrConflict):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrLiveCommandExpired):
		writeError(w, http.StatusGone, err.Error())
	case errors.Is(err, account.ErrLiveCommandReceipt):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, liveconn.ErrInvalid),
		errors.Is(err, account.ErrInvalidPeerID):
		writeError(w, http.StatusBadRequest, err.Error())
	default:
		s.cfg.Logger.Error("live connections", "error", err)
		writeError(w, http.StatusInternalServerError, "could not process live connection request")
	}
}

func decodeBodySize(w http.ResponseWriter, r *http.Request, dst any, limit int64) error {
	defer r.Body.Close()
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, limit))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}
	if err := dec.Decode(&struct{}{}); err != io.EOF {
		return errors.New("invalid JSON: trailing data")
	}
	return nil
}
