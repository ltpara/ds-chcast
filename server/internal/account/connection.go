package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// ErrInvalidConnection marks a viewer report that cannot be stored. Callers
// map it to a client error instead of a server failure.
var ErrInvalidConnection = errors.New("invalid connection record")

const (
	connectionRetention   = 10000
	maxConnectionPeer     = 128
	maxConnectionDetail   = 256
	maxConnectionIP       = 64
	maxConnectionLocation = 64
	maxConnectionRoute    = 32
	maxConnectionSpan     = 30 * 24 * time.Hour
	maxAuditRef           = 64
	connectionViewer      = "viewer"
	connectionHost        = "host"
	connectionBoth        = "both"
)

// Route values describe how the viewer's media reached the host. "direct"
// means no rendezvous server was involved, "punch" is a server-assisted
// direct path, "relay" is the per-host relay and "carrier" the server's
// shared TCP/WebSocket carrier. Unknown values are stored as empty rather
// than rejecting the report.
const (
	RouteDirect  = "direct"
	RoutePunch   = "punch"
	RouteRelay   = "relay"
	RouteCarrier = "carrier"
)

func normalizeRoute(raw string) string {
	switch strings.ToLower(strings.TrimSpace(raw)) {
	case RouteDirect:
		return RouteDirect
	case RoutePunch:
		return RoutePunch
	case RouteRelay:
		return RouteRelay
	case RouteCarrier:
		return RouteCarrier
	default:
		return ""
	}
}

// ConnectionEvent is one remote session. Viewer reports carry the account;
// host reports carry the host id and an audit reference shared by both sides,
// which is what lets one session produce a single record.
type ConnectionEvent struct {
	ID        int64
	Username  string
	PeerID    string
	Direction string
	HostID    string
	AuditRef  string
	Source    string
	ClientIP  string
	Location  string
	Route     string
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

// mergeSource keeps the record's provenance honest: a row becomes "both" only
// when the two sides actually differ, so a retry from one side never claims the
// other one reported.
func mergeSource(existing, incoming string) string {
	if existing == "" || existing == incoming {
		return incoming
	}
	return connectionBoth
}

// normalizeAuditRef accepts the lowercase hex reference both sides generate
// for one session. An empty value means the report cannot be linked.
func normalizeAuditRef(raw string) (string, error) {
	ref := strings.ToLower(strings.TrimSpace(raw))
	if ref == "" {
		return "", nil
	}
	if len(ref) < 8 || len(ref) > maxAuditRef {
		return "", fmt.Errorf("%w: audit ref length", ErrInvalidConnection)
	}
	for _, c := range ref {
		if (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			return "", fmt.Errorf("%w: audit ref is not hex", ErrInvalidConnection)
		}
	}
	return ref, nil
}

func (s *Store) RecordConnection(
	userID, username, peerID, direction, clientIP, location, route, auditRef string,
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
		direction = connectionViewer
	}
	if direction != connectionViewer {
		return ConnectionEvent{}, fmt.Errorf("%w: direction %q", ErrInvalidConnection, direction)
	}
	username = strings.TrimSpace(username)
	if username == "" {
		username = "-"
	}
	clientIP = normalizeText(clientIP, maxConnectionIP)
	location = normalizeText(location, maxConnectionLocation)
	route = normalizeRoute(route)
	detail = normalizeText(detail, maxConnectionDetail)
	ref, err := normalizeAuditRef(auditRef)
	if err != nil {
		return ConnectionEvent{}, err
	}
	startedAt, endedAt, err = s.connectionWindow(startedAt, endedAt)
	if err != nil {
		return ConnectionEvent{}, err
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if ref != "" {
		existing, found, err := s.connectionByRefLocked(ref)
		if err != nil {
			return ConnectionEvent{}, err
		}
		if found {
			// The host may have reported first. Fill in the account identity
			// and keep the wider time window instead of adding a second row.
			merged := existing
			merged.Username = username
			merged.PeerID = peerID
			merged.Direction = connectionViewer
			merged.ClientIP = clientIP
			if location != "" {
				merged.Location = location
			}
			if route != "" {
				merged.Route = route
			}
			merged.StartedAt = earliest(existing.StartedAt, startedAt)
			merged.EndedAt = latest(existing.EndedAt, endedAt)
			merged.Success = existing.Success && success
			merged.Detail = mergeConnectionDetail(existing.Detail, detail)
			merged.Source = mergeSource(existing.Source, connectionViewer)
			if err := s.updateConnectionLocked(merged, userID); err != nil {
				return ConnectionEvent{}, err
			}
			return merged, nil
		}
	}
	event := ConnectionEvent{
		Username:  username,
		PeerID:    peerID,
		Direction: connectionViewer,
		AuditRef:  ref,
		Source:    connectionViewer,
		ClientIP:  clientIP,
		Location:  location,
		Route:     route,
		StartedAt: startedAt,
		EndedAt:   endedAt,
		Success:   success,
		Detail:    detail,
	}
	return s.insertConnectionLocked(event, userID)
}

// RecordHostConnection stores the host side of a session. The host knows its
// own id and the audit reference from the viewer's Hello, but not the account
// identity, so a record may start with username "-" and be completed by the
// viewer report that shares the reference.
func (s *Store) RecordHostConnection(
	hostID, auditRef string,
	startedAt, endedAt time.Time,
	success bool,
	detail string,
) (ConnectionEvent, error) {
	hostID = normalizeText(hostID, maxConnectionPeer)
	if hostID == "" {
		return ConnectionEvent{}, fmt.Errorf("%w: host id is empty", ErrInvalidConnection)
	}
	detail = normalizeText(detail, maxConnectionDetail)
	ref, err := normalizeAuditRef(auditRef)
	if err != nil {
		return ConnectionEvent{}, err
	}
	startedAt, endedAt, err = s.connectionWindow(startedAt, endedAt)
	if err != nil {
		return ConnectionEvent{}, err
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if ref != "" {
		existing, found, err := s.connectionByRefLocked(ref)
		if err != nil {
			return ConnectionEvent{}, err
		}
		if found {
			merged := existing
			merged.HostID = hostID
			merged.StartedAt = earliest(existing.StartedAt, startedAt)
			merged.EndedAt = latest(existing.EndedAt, endedAt)
			merged.Success = existing.Success && success
			merged.Detail = mergeConnectionDetail(existing.Detail, detail)
			merged.Source = mergeSource(existing.Source, connectionHost)
			if err := s.updateConnectionLocked(merged, ""); err != nil {
				return ConnectionEvent{}, err
			}
			return merged, nil
		}
	}
	event := ConnectionEvent{
		Username:  "-",
		PeerID:    hostID,
		Direction: connectionHost,
		HostID:    hostID,
		AuditRef:  ref,
		Source:    connectionHost,
		StartedAt: startedAt,
		EndedAt:   endedAt,
		Success:   success,
		Detail:    detail,
	}
	return s.insertConnectionLocked(event, "")
}

func (s *Store) connectionWindow(startedAt, endedAt time.Time) (time.Time, time.Time, error) {
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
		return time.Time{}, time.Time{}, fmt.Errorf("%w: ends before it starts", ErrInvalidConnection)
	}
	if endedAt.Sub(startedAt) > maxConnectionSpan {
		return time.Time{}, time.Time{}, fmt.Errorf("%w: session is longer than 30 days", ErrInvalidConnection)
	}
	return startedAt, endedAt, nil
}

func earliest(a, b time.Time) time.Time {
	if a.Before(b) {
		return a
	}
	return b
}

func latest(a, b time.Time) time.Time {
	if a.After(b) {
		return a
	}
	return b
}

func mergeConnectionDetail(existing, incoming string) string {
	if existing == "" {
		return incoming
	}
	if incoming == "" || existing == incoming {
		return existing
	}
	return normalizeText(existing+" | "+incoming, maxConnectionDetail)
}

func (s *Store) insertConnectionLocked(event ConnectionEvent, userID string) (ConnectionEvent, error) {
	var id int64
	err := s.db.QueryRow(
		`INSERT INTO connection_events
			(user_id, username, peer_id, direction, client_ip, location, route,
			 started_at, ended_at, success, detail, host_id, audit_ref, source)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		 RETURNING id`,
		userID, event.Username, event.PeerID, event.Direction, event.ClientIP,
		event.Location, event.Route,
		unixMillis(event.StartedAt), unixMillis(event.EndedAt), event.Success,
		event.Detail, event.HostID, event.AuditRef, event.Source,
	).Scan(&id)
	if err != nil {
		return ConnectionEvent{}, fmt.Errorf("account store %s: record connection: %w", s.path, err)
	}
	if _, err := s.db.Exec(
		`DELETE FROM connection_events WHERE id <= (SELECT COALESCE(MAX(id), 0) - ? FROM connection_events)`,
		connectionRetention,
	); err != nil {
		return ConnectionEvent{}, fmt.Errorf("account store %s: trim connections: %w", s.path, err)
	}
	event.ID = id
	return event, nil
}

// updateConnectionLocked merges into an existing row. userID is only written
// when the caller has an authenticated account; host reports never clear the
// account a viewer report may already have supplied.
func (s *Store) updateConnectionLocked(event ConnectionEvent, userID string) error {
	query := `UPDATE connection_events SET username=?, peer_id=?, direction=?,
			client_ip=?, location=?, route=?, started_at=?, ended_at=?, success=?, detail=?,
			host_id=?, audit_ref=?, source=? WHERE id=?`
	args := []any{
		event.Username, event.PeerID, event.Direction, event.ClientIP,
		event.Location, event.Route,
		unixMillis(event.StartedAt), unixMillis(event.EndedAt), event.Success,
		event.Detail, event.HostID, event.AuditRef, event.Source, event.ID,
	}
	if userID != "" {
		query = `UPDATE connection_events SET user_id=?, username=?, peer_id=?,
			direction=?, client_ip=?, location=?, route=?, started_at=?, ended_at=?,
			success=?, detail=?, host_id=?, audit_ref=?, source=? WHERE id=?`
		args = append([]any{userID}, args...)
	}
	if _, err := s.db.Exec(query, args...); err != nil {
		return fmt.Errorf("account store %s: update connection: %w", s.path, err)
	}
	return nil
}

func (s *Store) connectionByRefLocked(ref string) (ConnectionEvent, bool, error) {
	row := s.db.QueryRow(
		`SELECT id, username, peer_id, direction, client_ip, started_at, ended_at,
			success, detail, host_id, audit_ref, source, location, route
		 FROM connection_events WHERE audit_ref = ? ORDER BY id LIMIT 1`, ref)
	event, err := scanConnection(row)
	if err == errNoRows {
		return ConnectionEvent{}, false, nil
	}
	if err != nil {
		return ConnectionEvent{}, false, fmt.Errorf("account store %s: read connection by ref: %w", s.path, err)
	}
	return event, true, nil
}

// Connections lists one account's reports, newest first.
func (s *Store) Connections(userID string, limit int) ([]ConnectionEvent, error) {
	return s.ConnectionsBefore(userID, 0, limit)
}

// ConnectionsBefore pages one account's reports older than beforeID; 0 starts
// at the newest row.
func (s *Store) ConnectionsBefore(userID string, beforeID int64, limit int) ([]ConnectionEvent, error) {
	return s.connectionEvents("user_id = ?", []any{strings.TrimSpace(userID)}, beforeID, limit)
}

// ConnectionsFor lists reports across accounts. An empty username returns
// every account, which is what the operator console uses.
func (s *Store) ConnectionsFor(username string, limit int) ([]ConnectionEvent, error) {
	return s.ConnectionsForBefore(username, 0, limit)
}

// ConnectionsForBefore pages cross-account reports older than beforeID.
func (s *Store) ConnectionsForBefore(username string, beforeID int64, limit int) ([]ConnectionEvent, error) {
	return s.ConnectionsForBeforeRange(username, TimeBounds{}, beforeID, limit)
}

// ConnectionsForBeforeRange is ConnectionsForBefore narrowed to an inclusive
// window on started_at.
func (s *Store) ConnectionsForBeforeRange(username string, bounds TimeBounds, beforeID int64, limit int) ([]ConnectionEvent, error) {
	username = strings.TrimSpace(username)
	clauses := make([]string, 0, 2)
	args := make([]any, 0, 2)
	if username != "" {
		clauses = append(clauses, "username = ? COLLATE NOCASE")
		args = append(args, username)
	}
	clauses, args = addTimeBounds(clauses, args, "started_at", bounds)
	return s.connectionEvents(strings.Join(clauses, " AND "), args, beforeID, limit)
}

// ConnectionsForHost lists sessions that involve one host, including inbound
// reports that never got an account-side row.
func (s *Store) ConnectionsForHost(hostID string, limit int) ([]ConnectionEvent, error) {
	return s.ConnectionsForHostBefore(hostID, 0, limit)
}

// ConnectionsForHostBefore pages host-involved reports older than beforeID.
func (s *Store) ConnectionsForHostBefore(hostID string, beforeID int64, limit int) ([]ConnectionEvent, error) {
	return s.ConnectionsForHostBeforeRange(hostID, TimeBounds{}, beforeID, limit)
}

// ConnectionsForHostBeforeRange is ConnectionsForHostBefore narrowed to an
// inclusive window on started_at.
func (s *Store) ConnectionsForHostBeforeRange(hostID string, bounds TimeBounds, beforeID int64, limit int) ([]ConnectionEvent, error) {
	hostID = strings.TrimSpace(hostID)
	clauses := make([]string, 0, 2)
	args := make([]any, 0, 3)
	if hostID != "" {
		clauses = append(clauses, "(host_id = ? OR peer_id = ? COLLATE NOCASE)")
		args = append(args, hostID, hostID)
	}
	clauses, args = addTimeBounds(clauses, args, "started_at", bounds)
	return s.connectionEvents(strings.Join(clauses, " AND "), args, beforeID, limit)
}

func (s *Store) connectionEvents(where string, args []any, beforeID int64, limit int) ([]ConnectionEvent, error) {
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
	query := `SELECT id, username, peer_id, direction, client_ip, started_at, ended_at,
			success, detail, host_id, audit_ref, source, location, route
		 FROM connection_events WHERE ` + where + ` ORDER BY id DESC LIMIT ?`
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
		event, err := scanConnection(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: scan connection: %w", s.path, err)
		}
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list connections: %w", s.path, err)
	}
	return events, nil
}

type rowScanner interface {
	Scan(dest ...any) error
}

var errNoRows = errors.New("no rows")

func scanConnection(row rowScanner) (ConnectionEvent, error) {
	var (
		event   ConnectionEvent
		started int64
		ended   int64
	)
	if err := row.Scan(&event.ID, &event.Username, &event.PeerID,
		&event.Direction, &event.ClientIP, &started, &ended,
		&event.Success, &event.Detail, &event.HostID, &event.AuditRef,
		&event.Source, &event.Location, &event.Route); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ConnectionEvent{}, errNoRows
		}
		return ConnectionEvent{}, err
	}
	event.StartedAt = timeFromMillis(started)
	event.EndedAt = timeFromMillis(ended)
	return event, nil
}
