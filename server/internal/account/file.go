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

// FileEvent is one viewer-reported upload or download. A report covers every
// file that finished between two viewer polls, so Files can exceed one while
// Name and Size describe the last file in that window.
type FileEvent struct {
	ID         int64
	Username   string
	PeerID     string
	Direction  string
	Name       string
	Size       int64
	Files      int
	ClientIP   string
	OccurredAt time.Time
	Success    bool
	Detail     string
}

func (s *Store) RecordFileTransfer(
	userID, username, peerID, direction, name, clientIP string,
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

	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`INSERT INTO file_events
			(user_id, username, peer_id, direction, name, size, files,
			 client_ip, occurred_at, success, detail)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		userID, username, peerID, direction, name, size, files,
		clientIP, unixMillis(occurredAt), success, detail,
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
	return FileEvent{
		ID:         id,
		Username:   username,
		PeerID:     peerID,
		Direction:  direction,
		Name:       name,
		Size:       size,
		Files:      files,
		ClientIP:   clientIP,
		OccurredAt: occurredAt,
		Success:    success,
		Detail:     detail,
	}, nil
}

// FileTransfers lists one account's reports, newest first.
func (s *Store) FileTransfers(userID string, limit int) ([]FileEvent, error) {
	return s.fileEvents("WHERE user_id = ?", []any{strings.TrimSpace(userID)}, limit)
}

// FileTransfersFor lists reports across accounts. An empty username returns
// every account, which is what the operator console uses.
func (s *Store) FileTransfersFor(username string, limit int) ([]FileEvent, error) {
	username = strings.TrimSpace(username)
	if username == "" {
		return s.fileEvents("", nil, limit)
	}
	return s.fileEvents("WHERE username = ? COLLATE NOCASE", []any{username}, limit)
}

func (s *Store) fileEvents(where string, args []any, limit int) ([]FileEvent, error) {
	if limit <= 0 {
		limit = 50
	}
	if limit > 200 {
		limit = 200
	}
	query := `SELECT id, username, peer_id, direction, name, size, files,
			client_ip, occurred_at, success, detail
		 FROM file_events ` + where + ` ORDER BY id DESC LIMIT ?`
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
			&event.ClientIP, &occurred, &event.Success, &event.Detail); err != nil {
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
