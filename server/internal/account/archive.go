package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
)

// HistoryStream names one table the external archive copies. The names double
// as checkpoint keys and JSONL file prefixes.
const (
	HistoryAudit         = "audit"
	HistoryConnections   = "connections"
	HistoryFileTransfers = "file-transfers"
)

// archivePageLimit caps one ascending page read.
const archivePageLimit = 500

// ArchiveCheckpoint returns the last row id the external archive copied for
// stream; 0 means nothing has been archived yet.
func (s *Store) ArchiveCheckpoint(stream string) (int64, error) {
	stream = strings.TrimSpace(stream)
	if stream == "" {
		return 0, fmt.Errorf("account store %s: archive checkpoint: stream is empty", s.path)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var last int64
	err := s.db.QueryRow(
		`SELECT last_id FROM archive_checkpoints WHERE stream = ?`, stream).Scan(&last)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, nil
	}
	if err != nil {
		return 0, fmt.Errorf("account store %s: read archive checkpoint: %w", s.path, err)
	}
	return last, nil
}

// AdvanceArchiveCheckpoint records that the archive file data through lastID
// is on disk. The value only moves forward, so a stale writer cannot rewind a
// stream; a crash between the file write and this call replays a page on the
// next run and never loses rows.
func (s *Store) AdvanceArchiveCheckpoint(stream string, lastID int64) error {
	stream = strings.TrimSpace(stream)
	if stream == "" {
		return fmt.Errorf("account store %s: archive checkpoint: stream is empty", s.path)
	}
	if lastID <= 0 {
		return fmt.Errorf("account store %s: archive checkpoint: id %d", s.path, lastID)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`INSERT INTO archive_checkpoints (stream, last_id, updated_at) VALUES (?, ?, ?)
		 ON CONFLICT(stream) DO UPDATE SET
			last_id = MAX(archive_checkpoints.last_id, excluded.last_id),
			updated_at = excluded.updated_at`,
		stream, lastID, unixMillis(s.now().UTC()),
	); err != nil {
		return fmt.Errorf("account store %s: advance archive checkpoint: %w", s.path, err)
	}
	return nil
}

// AuditEventsAfter returns audit rows newer than afterID, oldest first. The
// external archive walks the table this way so one id is enough to resume.
func (s *Store) AuditEventsAfter(afterID int64, limit int) ([]AuditEvent, error) {
	limit = archivePage(limit)
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT id, created_at, username, action, detail, client_ip, location, success
		 FROM audit_events WHERE id > ? ORDER BY id ASC LIMIT ?`, afterID, limit)
	if err != nil {
		return nil, fmt.Errorf("account store %s: archive audit: %w", s.path, err)
	}
	defer rows.Close()
	events := make([]AuditEvent, 0)
	for rows.Next() {
		var (
			event   AuditEvent
			created int64
		)
		if err := rows.Scan(&event.ID, &created, &event.Username, &event.Action,
			&event.Detail, &event.ClientIP, &event.Location, &event.Success); err != nil {
			return nil, fmt.Errorf("account store %s: archive audit: %w", s.path, err)
		}
		event.CreatedAt = timeFromMillis(created)
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: archive audit: %w", s.path, err)
	}
	return events, nil
}

// ConnectionsAfter returns connection rows newer than afterID, oldest first.
func (s *Store) ConnectionsAfter(afterID int64, limit int) ([]ConnectionEvent, error) {
	limit = archivePage(limit)
	query := `SELECT id, username, peer_id, direction, client_ip, started_at, ended_at,
			success, detail, host_id, audit_ref, source
		 FROM connection_events WHERE id > ? ORDER BY id ASC LIMIT ?`
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(query, afterID, limit)
	if err != nil {
		return nil, fmt.Errorf("account store %s: archive connections: %w", s.path, err)
	}
	defer rows.Close()
	events := make([]ConnectionEvent, 0)
	for rows.Next() {
		event, err := scanConnection(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: archive connections: %w", s.path, err)
		}
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: archive connections: %w", s.path, err)
	}
	return events, nil
}

// FileTransfersAfter returns file-transfer rows newer than afterID, oldest
// first.
func (s *Store) FileTransfersAfter(afterID int64, limit int) ([]FileEvent, error) {
	limit = archivePage(limit)
	query := `SELECT id, username, peer_id, direction, name, size, files,
			client_ip, occurred_at, success, detail, host_id, audit_ref, source
		 FROM file_events WHERE id > ? ORDER BY id ASC LIMIT ?`
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(query, afterID, limit)
	if err != nil {
		return nil, fmt.Errorf("account store %s: archive file transfers: %w", s.path, err)
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
			return nil, fmt.Errorf("account store %s: archive file transfers: %w", s.path, err)
		}
		event.OccurredAt = timeFromMillis(occurred)
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: archive file transfers: %w", s.path, err)
	}
	return events, nil
}

func archivePage(limit int) int {
	if limit <= 0 || limit > archivePageLimit {
		return archivePageLimit
	}
	return limit
}
