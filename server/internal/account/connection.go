package account

import (
	"errors"
	"fmt"
	"strings"
	"time"
)

// ErrInvalidConnection marks a viewer report that cannot be stored. Callers
// map it to a client error instead of a server failure.
var ErrInvalidConnection = errors.New("invalid connection record")

const (
	connectionRetention = 10000
	maxConnectionPeer   = 128
	maxConnectionDetail = 256
	maxConnectionIP     = 64
	maxConnectionSpan   = 30 * 24 * time.Hour
)

// ConnectionEvent is one viewer-reported remote session. It is kept apart
// from account login audit so operators can query remote access separately.
type ConnectionEvent struct {
	ID        int64
	Username  string
	PeerID    string
	Direction string
	ClientIP  string
	StartedAt time.Time
	EndedAt   time.Time
	Success   bool
	Detail    string
}

func (e ConnectionEvent) Duration() time.Duration {
	if e.EndedAt.Before(e.StartedAt) {
		return 0
	}
	return e.EndedAt.Sub(e.StartedAt)
}

func (s *Store) RecordConnection(
	userID, username, peerID, direction, clientIP string,
	startedAt, endedAt time.Time,
	success bool,
	detail string,
) (ConnectionEvent, error) {
	userID = strings.TrimSpace(userID)
	if userID == "" {
		return ConnectionEvent{}, fmt.Errorf("%w: user is empty", ErrInvalidConnection)
	}
	peerID = normalizeText(peerID, maxConnectionPeer)
	if peerID == "" {
		return ConnectionEvent{}, fmt.Errorf("%w: peer id is empty", ErrInvalidConnection)
	}
	direction = strings.TrimSpace(direction)
	if direction == "" {
		direction = "viewer"
	}
	if direction != "viewer" {
		return ConnectionEvent{}, fmt.Errorf("%w: direction %q", ErrInvalidConnection, direction)
	}
	username = strings.TrimSpace(username)
	if username == "" {
		username = "-"
	}
	clientIP = normalizeText(clientIP, maxConnectionIP)
	detail = normalizeText(detail, maxConnectionDetail)
	now := s.now().UTC()
	if startedAt.IsZero() {
		startedAt = now
	}
	if endedAt.IsZero() {
		endedAt = now
	}
	startedAt = startedAt.UTC()
	endedAt = endedAt.UTC()
	if endedAt.Before(startedAt) {
		return ConnectionEvent{}, fmt.Errorf("%w: ends before it starts", ErrInvalidConnection)
	}
	if endedAt.Sub(startedAt) > maxConnectionSpan {
		return ConnectionEvent{}, fmt.Errorf("%w: session is longer than 30 days", ErrInvalidConnection)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`INSERT INTO connection_events
			(user_id, username, peer_id, direction, client_ip, started_at, ended_at, success, detail)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		userID, username, peerID, direction, clientIP,
		unixMillis(startedAt), unixMillis(endedAt), success, detail,
	)
	if err != nil {
		return ConnectionEvent{}, fmt.Errorf("account store %s: record connection: %w", s.path, err)
	}
	id, err := result.LastInsertId()
	if err != nil {
		return ConnectionEvent{}, fmt.Errorf("account store %s: record connection id: %w", s.path, err)
	}
	if _, err := s.db.Exec(
		`DELETE FROM connection_events WHERE id <= (SELECT COALESCE(MAX(id), 0) - ? FROM connection_events)`,
		connectionRetention,
	); err != nil {
		return ConnectionEvent{}, fmt.Errorf("account store %s: trim connections: %w", s.path, err)
	}
	return ConnectionEvent{
		ID:        id,
		Username:  username,
		PeerID:    peerID,
		Direction: direction,
		ClientIP:  clientIP,
		StartedAt: startedAt,
		EndedAt:   endedAt,
		Success:   success,
		Detail:    detail,
	}, nil
}

// Connections lists one account's reports, newest first.
func (s *Store) Connections(userID string, limit int) ([]ConnectionEvent, error) {
	return s.connectionEvents("WHERE user_id = ?", []any{strings.TrimSpace(userID)}, limit)
}

// ConnectionsFor lists reports across accounts. An empty username returns
// every account, which is what the operator console uses.
func (s *Store) ConnectionsFor(username string, limit int) ([]ConnectionEvent, error) {
	username = strings.TrimSpace(username)
	if username == "" {
		return s.connectionEvents("", nil, limit)
	}
	return s.connectionEvents("WHERE username = ? COLLATE NOCASE", []any{username}, limit)
}

func (s *Store) connectionEvents(where string, args []any, limit int) ([]ConnectionEvent, error) {
	if limit <= 0 {
		limit = 50
	}
	if limit > 200 {
		limit = 200
	}
	query := `SELECT id, username, peer_id, direction, client_ip, started_at, ended_at, success, detail
		 FROM connection_events ` + where + ` ORDER BY id DESC LIMIT ?`
	args = append(args, limit)
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list connections: %w", s.path, err)
	}
	defer rows.Close()
	events := make([]ConnectionEvent, 0)
	for rows.Next() {
		var (
			event   ConnectionEvent
			started int64
			ended   int64
		)
		if err := rows.Scan(&event.ID, &event.Username, &event.PeerID,
			&event.Direction, &event.ClientIP, &started, &ended,
			&event.Success, &event.Detail); err != nil {
			return nil, fmt.Errorf("account store %s: scan connection: %w", s.path, err)
		}
		event.StartedAt = timeFromMillis(started)
		event.EndedAt = timeFromMillis(ended)
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list connections: %w", s.path, err)
	}
	return events, nil
}
