package account

import (
	"errors"
	"fmt"
	"strings"
	"time"
)

// ErrInvalidTransfer marks a file-transfer report that cannot be stored.
var ErrInvalidTransfer = errors.New("invalid file transfer record")

const (
	transferRetention = 10000
	maxTransferPeer   = 128
	maxTransferName   = 256
	maxTransferDetail = 256
	maxTransferIP     = 64
	maxTransferFiles  = 1000
	maxTransferSize   = 1 << 50
	transferUpload    = "upload"
	transferDownload  = "download"
)

// FileEvent is one viewer- or host-reported upload or download. A viewer
// report covers every file that finished between two polls, so Files can
// exceed one while Name and Size describe the last file in that window. Host
// reports describe one finished file and share the session AuditRef.
type FileEvent struct {
	ID         int64
	Username   string
	PeerID     string
	Direction  string
	HostID     string
	AuditRef   string
	Source     string
	Name       string
	Size       int64
	Files      int
	ClientIP   string
	OccurredAt time.Time
	Success    bool
	Detail     string
}

func (s *Store) RecordFileTransfer(
	userID, username, peerID, direction, name, clientIP, auditRef string,
	size int64,
	files int,
	occurredAt time.Time,
	success bool,
	detail string,
) (FileEvent, error) {
	userID = strings.TrimSpace(userID)
	if userID == "" {
		return FileEvent{}, fmt.Errorf("%w: user is empty", ErrInvalidTransfer)
	}
	peerID = normalizeText(peerID, maxTransferPeer)
	if peerID == "" {
		return FileEvent{}, fmt.Errorf("%w: peer id is empty", ErrInvalidTransfer)
	}
	direction = strings.TrimSpace(direction)
	if direction != transferUpload && direction != transferDownload {
		return FileEvent{}, fmt.Errorf("%w: direction %q", ErrInvalidTransfer, direction)
	}
	name = normalizeText(name, maxTransferName)
	ref, err := normalizeAuditRef(auditRef)
	if err != nil {
		return FileEvent{}, fmt.Errorf("%w: %v", ErrInvalidTransfer, err)
	}
	if size < 0 || size > maxTransferSize {
		return FileEvent{}, fmt.Errorf("%w: size %d", ErrInvalidTransfer, size)
	}
	if files < 1 || files > maxTransferFiles {
		return FileEvent{}, fmt.Errorf("%w: files %d", ErrInvalidTransfer, files)
	}
	username = strings.TrimSpace(username)
	if username == "" {
		username = "-"
	}
	clientIP = normalizeText(clientIP, maxTransferIP)
	detail = normalizeText(detail, maxTransferDetail)
	if occurredAt.IsZero() {
		occurredAt = s.now()
	}
	occurredAt = occurredAt.UTC()

	event := FileEvent{
		Username:   username,
		PeerID:     peerID,
		Direction:  direction,
		AuditRef:   ref,
		Source:     connectionViewer,
		Name:       name,
		Size:       size,
		Files:      files,
		ClientIP:   clientIP,
		OccurredAt: occurredAt,
		Success:    success,
		Detail:     detail,
	}
	return s.insertFileEventLocked(event, userID)
}

// RecordHostFileTransfer stores one finished file seen by the host. The host
// knows the session's audit reference but not the account, so the record is
// linked by reference when the viewer report arrives.
func (s *Store) RecordHostFileTransfer(
	hostID, auditRef, direction, name string,
	size int64,
	occurredAt time.Time,
	success bool,
	detail string,
) (FileEvent, error) {
	hostID = normalizeText(hostID, maxTransferPeer)
	if hostID == "" {
		return FileEvent{}, fmt.Errorf("%w: host id is empty", ErrInvalidTransfer)
	}
	direction = strings.TrimSpace(direction)
	if direction != transferUpload && direction != transferDownload {
		return FileEvent{}, fmt.Errorf("%w: direction %q", ErrInvalidTransfer, direction)
	}
	name = normalizeText(name, maxTransferName)
	ref, err := normalizeAuditRef(auditRef)
	if err != nil {
		return FileEvent{}, fmt.Errorf("%w: %v", ErrInvalidTransfer, err)
	}
	if size < 0 || size > maxTransferSize {
		return FileEvent{}, fmt.Errorf("%w: size %d", ErrInvalidTransfer, size)
	}
	detail = normalizeText(detail, maxTransferDetail)
	if occurredAt.IsZero() {
		occurredAt = s.now()
	}
	event := FileEvent{
		Username:   "-",
		PeerID:     hostID,
		Direction:  direction,
		HostID:     hostID,
		AuditRef:   ref,
		Source:     connectionHost,
		Name:       name,
		Size:       size,
		Files:      1,
		OccurredAt: occurredAt.UTC(),
		Success:    success,
		Detail:     detail,
	}
	return s.insertFileEventLocked(event, "")
}

func (s *Store) insertFileEventLocked(event FileEvent, userID string) (FileEvent, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`INSERT INTO file_events
			(user_id, username, peer_id, direction, name, size, files,
			 client_ip, occurred_at, success, detail, host_id, audit_ref, source)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		userID, event.Username, event.PeerID, event.Direction, event.Name,
		event.Size, event.Files, event.ClientIP, unixMillis(event.OccurredAt),
		event.Success, event.Detail, event.HostID, event.AuditRef, event.Source,
	)
	if err != nil {
		return FileEvent{}, fmt.Errorf("account store %s: record file transfer: %w", s.path, err)
	}
	id, err := result.LastInsertId()
	if err != nil {
		return FileEvent{}, fmt.Errorf("account store %s: record file transfer id: %w", s.path, err)
	}
	if _, err := s.db.Exec(
		`DELETE FROM file_events WHERE id <= (SELECT COALESCE(MAX(id), 0) - ? FROM file_events)`,
		transferRetention,
	); err != nil {
		return FileEvent{}, fmt.Errorf("account store %s: trim file transfers: %w", s.path, err)
	}
	event.ID = id
	return event, nil
}

// FileTransfers lists one account's reports, newest first.
func (s *Store) FileTransfers(userID string, limit int) ([]FileEvent, error) {
	return s.FileTransfersBefore(userID, 0, limit)
}

// FileTransfersBefore pages one account's reports older than beforeID.
func (s *Store) FileTransfersBefore(userID string, beforeID int64, limit int) ([]FileEvent, error) {
	return s.fileEvents("user_id = ?", []any{strings.TrimSpace(userID)}, beforeID, limit)
}

// FileTransfersFor lists reports across accounts. An empty username returns
// every account, which is what the operator console uses.
func (s *Store) FileTransfersFor(username string, limit int) ([]FileEvent, error) {
	return s.FileTransfersForBefore(username, 0, limit)
}

// FileTransfersForBefore pages cross-account reports older than beforeID.
func (s *Store) FileTransfersForBefore(username string, beforeID int64, limit int) ([]FileEvent, error) {
	username = strings.TrimSpace(username)
	if username == "" {
		return s.fileEvents("", nil, beforeID, limit)
	}
	return s.fileEvents("username = ? COLLATE NOCASE", []any{username}, beforeID, limit)
}

// FileTransfersForHost lists transfers involving one host, including inbound
// host reports that have no account row.
func (s *Store) FileTransfersForHost(hostID string, limit int) ([]FileEvent, error) {
	return s.FileTransfersForHostBefore(hostID, 0, limit)
}

// FileTransfersForHostBefore pages host-involved reports older than beforeID.
func (s *Store) FileTransfersForHostBefore(hostID string, beforeID int64, limit int) ([]FileEvent, error) {
	hostID = strings.TrimSpace(hostID)
	if hostID == "" {
		return s.fileEvents("", nil, beforeID, limit)
	}
	return s.fileEvents(
		"(host_id = ? OR peer_id = ? COLLATE NOCASE)", []any{hostID, hostID}, beforeID, limit)
}

func (s *Store) fileEvents(where string, args []any, beforeID int64, limit int) ([]FileEvent, error) {
	if limit <= 0 {
		limit = 50
	}
	if limit > 200 {
		limit = 200
	}
	if strings.TrimSpace(where) == "" {
		where = "1 = 1"
	}
	if beforeID > 0 {
		where = "(" + where + ") AND id < ?"
		args = append(args, beforeID)
	}
	query := `SELECT id, username, peer_id, direction, name, size, files,
			client_ip, occurred_at, success, detail, host_id, audit_ref, source
		 FROM file_events WHERE ` + where + ` ORDER BY id DESC LIMIT ?`
	args = append(args, limit)
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list file transfers: %w", s.path, err)
	}
	defer rows.Close()
	events := make([]FileEvent, 0)
	for rows.Next() {
		var (
			event    FileEvent
			occurred int64
		)
		if err := rows.Scan(&event.ID, &event.Username, &event.PeerID,
			&event.Direction, &event.Name, &event.Size, &event.Files,
			&event.ClientIP, &occurred, &event.Success, &event.Detail,
			&event.HostID, &event.AuditRef, &event.Source); err != nil {
			return nil, fmt.Errorf("account store %s: scan file transfer: %w", s.path, err)
		}
		event.OccurredAt = timeFromMillis(occurred)
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list file transfers: %w", s.path, err)
	}
	return events, nil
}
