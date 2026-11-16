package account

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/liveconn"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const (
	liveConnectionRetention = 10000
	maxLiveDetail           = 512
	maxDisconnectReason     = 512
)

var (
	ErrLiveConnectionNotFound = errors.New("live connection not found")
	ErrLiveRuntimeStale       = errors.New("host runtime is stale")
	ErrLiveRuntimeTerminal    = errors.New("host runtime is terminal")
	ErrLiveSequenceConflict   = errors.New("live connection snapshot sequence conflict")
	ErrLiveCommandExpired     = errors.New("disconnect command expired")
	ErrLiveCommandReceipt     = errors.New("disconnect command receipt conflict")
)

// LiveConnection is the administrator-facing state of one admitted host
// session. Freshness is calculated by the server clock on every read.
type LiveConnection struct {
	HostID          string    `json:"host_id"`
	RuntimeID       string    `json:"runtime_id"`
	ConnectionID    string    `json:"connection_id"`
	PeerAddress     string    `json:"peer_address,omitempty"`
	Viewer          string    `json:"viewer,omitempty"`
	Mode            string    `json:"mode,omitempty"`
	Route           string    `json:"route,omitempty"`
	Permissions     uint32    `json:"permissions,omitempty"`
	Controller      bool      `json:"controller"`
	StartedAt       time.Time `json:"started_at"`
	State           string    `json:"state"`
	LastHeartbeatAt time.Time `json:"last_heartbeat_at"`
	CommandID       string    `json:"command_id,omitempty"`
	CommandStatus   string    `json:"command_status,omitempty"`
	CommandDeadline time.Time `json:"command_deadline,omitempty"`
	ReceiptDetail   string    `json:"receipt_detail,omitempty"`
}

type liveConnectionRow struct {
	liveconn.Connection
	HostID            string
	RuntimeID         string
	NativeState       string
	FirstSeenMS       int64
	LastSeenMS        int64
	ClosedAtMS        int64
	CloseReason       string
	CommandID         string
	CommandStatus     string
	CommandDeadlineMS int64
	ReceiptDetail     string
}

func applyLiveConnections(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS host_runtimes (
			host_id          TEXT NOT NULL,
			runtime_id       TEXT NOT NULL,
			last_sequence    BIGINT NOT NULL,
			snapshot_digest  TEXT NOT NULL,
			first_seen_at    BIGINT NOT NULL,
			last_heartbeat_at BIGINT NOT NULL,
			terminal         BOOLEAN NOT NULL DEFAULT FALSE,
			terminal_at      BIGINT NOT NULL DEFAULT 0,
			PRIMARY KEY (host_id, runtime_id)
		)`,
		`CREATE INDEX IF NOT EXISTS host_runtimes_freshness
			ON host_runtimes(last_heartbeat_at)`,
		`CREATE TABLE IF NOT EXISTS live_connections (
			host_id        TEXT NOT NULL,
			runtime_id     TEXT NOT NULL,
			connection_id  TEXT NOT NULL,
			peer_address   TEXT NOT NULL DEFAULT '',
			viewer         TEXT NOT NULL DEFAULT '',
			mode           TEXT NOT NULL DEFAULT '',
			route          TEXT NOT NULL DEFAULT '',
			permissions    BIGINT NOT NULL DEFAULT 0,
			controller     BOOLEAN NOT NULL DEFAULT FALSE,
			started_at     BIGINT NOT NULL,
			first_seen_at  BIGINT NOT NULL,
			last_seen_at   BIGINT NOT NULL,
			native_state   TEXT NOT NULL DEFAULT 'active',
			closed_at      BIGINT NOT NULL DEFAULT 0,
			close_reason   TEXT NOT NULL DEFAULT '',
			PRIMARY KEY (host_id, connection_id),
			FOREIGN KEY (host_id, runtime_id)
				REFERENCES host_runtimes(host_id, runtime_id) ON DELETE CASCADE
		)`,
		`CREATE INDEX IF NOT EXISTS live_connections_active
			ON live_connections(host_id, native_state, last_seen_at)`,
		`CREATE TABLE IF NOT EXISTS live_connection_commands (
			command_id     TEXT PRIMARY KEY,
			request_key    TEXT NOT NULL,
			host_id        TEXT NOT NULL,
			runtime_id     TEXT NOT NULL,
			connection_id  TEXT NOT NULL,
			operation      TEXT NOT NULL,
			actor          TEXT NOT NULL,
			reason         TEXT NOT NULL DEFAULT '',
			status         TEXT NOT NULL DEFAULT 'queued',
			issued_at      BIGINT NOT NULL,
			deadline_at    BIGINT NOT NULL,
			delivered_at   BIGINT NOT NULL DEFAULT 0,
			finished_at    BIGINT NOT NULL DEFAULT 0,
			receipt_detail TEXT NOT NULL DEFAULT '',
			UNIQUE (host_id, request_key)
		)`,
		`CREATE INDEX IF NOT EXISTS live_connection_commands_poll
			ON live_connection_commands(host_id, runtime_id, status, deadline_at)`,
		`CREATE TABLE IF NOT EXISTS live_connection_command_receipts (
			command_id     TEXT PRIMARY KEY,
			host_id        TEXT NOT NULL,
			runtime_id     TEXT NOT NULL,
			receipt_sequence BIGINT NOT NULL,
			status         TEXT NOT NULL,
			confirmed_absent BOOLEAN NOT NULL,
			detail         TEXT NOT NULL DEFAULT '',
			received_at    BIGINT NOT NULL
		)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create live connection schema: %w", err)
		}
	}
	return nil
}

// ApplyLiveSnapshot advances one host runtime by a complete native snapshot.
// Equal sequences must have the same canonical content. Lower sequences and
// terminal resurrection return conflicts.
func (s *Store) ApplyLiveSnapshot(hostID string, snapshot liveconn.Snapshot) (liveconn.Snapshot, error) {
	hostID = strings.TrimSpace(hostID)
	if !registry.ValidID(hostID) {
		return liveconn.Snapshot{}, ErrInvalidPeerID
	}
	normalized, err := snapshot.Normalize()
	if err != nil {
		return liveconn.Snapshot{}, err
	}
	digest, err := liveconn.Digest(normalized)
	if err != nil {
		return liveconn.Snapshot{}, err
	}
	now := s.now().UTC().UnixMilli()

	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return liveconn.Snapshot{}, err
	}
	defer tx.Rollback()

	var previousSequence int64
	var previousDigest string
	var previousHeartbeat int64
	var terminal bool
	err = tx.QueryRow(`SELECT last_sequence, snapshot_digest, last_heartbeat_at, terminal
		FROM host_runtimes WHERE host_id = ? AND runtime_id = ?`, hostID, normalized.RuntimeID).
		Scan(&previousSequence, &previousDigest, &previousHeartbeat, &terminal)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return liveconn.Snapshot{}, fmt.Errorf("read live runtime: %w", err)
	}
	missing := errors.Is(err, sql.ErrNoRows)
	if terminal {
		return liveconn.Snapshot{}, ErrLiveRuntimeTerminal
	}
	if !missing {
		if normalized.Sequence < uint64(previousSequence) {
			return liveconn.Snapshot{}, fmt.Errorf("%w: got %d after %d",
				ErrLiveSequenceConflict, normalized.Sequence, previousSequence)
		}
		if normalized.Sequence == uint64(previousSequence) {
			if previousDigest != digest {
				return liveconn.Snapshot{}, fmt.Errorf("%w: same sequence has different content",
					ErrLiveSequenceConflict)
			}
			if normalized.Sequence == 0 {
				return liveconn.Snapshot{}, ErrLiveSequenceConflict
			}
			if now > previousHeartbeat {
				if _, err := tx.Exec(`UPDATE host_runtimes SET last_heartbeat_at = ?
					WHERE host_id = ? AND runtime_id = ?`, now, hostID, normalized.RuntimeID); err != nil {
					return liveconn.Snapshot{}, err
				}
				if _, err := tx.Exec(`UPDATE live_connections SET last_seen_at = ?
					WHERE host_id = ? AND runtime_id = ? AND native_state = 'active'`,
					now, hostID, normalized.RuntimeID); err != nil {
					return liveconn.Snapshot{}, err
				}
			}
			if err := tx.Commit(); err != nil {
				return liveconn.Snapshot{}, err
			}
			return normalized, nil
		}
	}

	if missing {
		_, err = tx.Exec(`INSERT INTO host_runtimes
			(host_id, runtime_id, last_sequence, snapshot_digest, first_seen_at,
			 last_heartbeat_at, terminal, terminal_at)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			hostID, normalized.RuntimeID, normalized.Sequence, digest, now, now,
			normalized.Terminal, terminalAt(normalized.Terminal, now))
	} else {
		_, err = tx.Exec(`UPDATE host_runtimes SET last_sequence = ?, snapshot_digest = ?,
			last_heartbeat_at = ?, terminal = ?, terminal_at = ?
			WHERE host_id = ? AND runtime_id = ?`,
			normalized.Sequence, digest, now, normalized.Terminal,
			terminalAt(normalized.Terminal, now), hostID, normalized.RuntimeID)
	}
	if err != nil {
		return liveconn.Snapshot{}, fmt.Errorf("write live runtime: %w", err)
	}

	for _, item := range normalized.Connections {
		if _, err := tx.Exec(`INSERT INTO live_connections
			(host_id, runtime_id, connection_id, peer_address, viewer, mode, route,
			 permissions, controller, started_at, first_seen_at, last_seen_at,
			 native_state, closed_at, close_reason)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'active', 0, '')
			ON CONFLICT(host_id, connection_id) DO UPDATE SET
				peer_address = excluded.peer_address,
				viewer = excluded.viewer,
				mode = excluded.mode,
				route = excluded.route,
				permissions = excluded.permissions,
				controller = excluded.controller,
				last_seen_at = excluded.last_seen_at
			WHERE live_connections.runtime_id = excluded.runtime_id
				AND live_connections.native_state = 'active'`,
			hostID, normalized.RuntimeID, item.ConnectionID, item.PeerAddress,
			item.Viewer, item.Mode, item.Route, item.Permissions, item.Controller,
			item.StartedAtMS, now, now); err != nil {
			return liveconn.Snapshot{}, fmt.Errorf("upsert live connection: %w", err)
		}
		var storedRuntime, storedState string
		if err := tx.QueryRow(`SELECT runtime_id, native_state FROM live_connections
			WHERE host_id = ? AND connection_id = ?`, hostID, item.ConnectionID).
			Scan(&storedRuntime, &storedState); err != nil {
			return liveconn.Snapshot{}, fmt.Errorf("verify live connection: %w", err)
		}
		if storedRuntime != normalized.RuntimeID || storedState != liveconn.ConnectionActive {
			return liveconn.Snapshot{}, fmt.Errorf("%w: connection identity is already bound",
				liveconn.ErrConflict)
		}
	}

	active := make(map[string]bool, len(normalized.Connections))
	for _, item := range normalized.Connections {
		active[item.ConnectionID] = true
	}
	rows, err := tx.Query(`SELECT connection_id FROM live_connections
		WHERE host_id = ? AND runtime_id = ? AND native_state = 'active'`,
		hostID, normalized.RuntimeID)
	if err != nil {
		return liveconn.Snapshot{}, err
	}
	var absent []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return liveconn.Snapshot{}, err
		}
		if !active[id] {
			absent = append(absent, id)
		}
	}
	if err := rows.Close(); err != nil {
		return liveconn.Snapshot{}, err
	}
	closeReason := "connection ended in native snapshot"
	if normalized.Terminal {
		closeReason = "host runtime stopped"
	}
	for _, id := range absent {
		if _, err := tx.Exec(`UPDATE live_connections SET native_state = 'closed',
			closed_at = ?, close_reason = ? WHERE host_id = ? AND connection_id = ?
			AND runtime_id = ? AND native_state = 'active'`,
			now, closeReason, hostID, id, normalized.RuntimeID); err != nil {
			return liveconn.Snapshot{}, err
		}
	}
	if err := trimLiveConnectionsTx(tx); err != nil {
		return liveconn.Snapshot{}, err
	}
	if err := tx.Commit(); err != nil {
		return liveconn.Snapshot{}, err
	}
	return normalized, nil
}

func terminalAt(terminal bool, now int64) int64 {
	if terminal {
		return now
	}
	return 0
}

func trimLiveConnectionsTx(tx *sql.Tx) error {
	// Keep exactly the newest closed rows. OFFSET without a positive limit is
	// SQLite-only syntax, so select the cutoff and delete older rows instead.
	var cutoff int64
	var cutoffHost, cutoffConnection string
	err := tx.QueryRow(`SELECT last_seen_at, host_id, connection_id FROM live_connections
		WHERE native_state <> 'active'
		ORDER BY last_seen_at DESC, host_id DESC, connection_id DESC LIMIT 1 OFFSET ?`,
		liveConnectionRetention-1).Scan(&cutoff, &cutoffHost, &cutoffConnection)
	if errors.Is(err, sql.ErrNoRows) {
		return nil
	}
	if err != nil {
		return err
	}
	// The primary-key tie-break keeps the retained set deterministic and makes
	// the count exact even when many sessions closed in the same millisecond.
	_, err = tx.Exec(`DELETE FROM live_connections
		WHERE native_state <> 'active' AND (
			last_seen_at < ? OR (
				last_seen_at = ? AND (
					host_id < ? OR (host_id = ? AND connection_id < ?))))`,
		cutoff, cutoff, cutoffHost, cutoffHost, cutoffConnection)
	return err
}

// LiveConnections lists every active or stale connection. A terminal runtime
// has no active connections. The query is bounded and sorted newest first.
func (s *Store) LiveConnections(limit int) ([]LiveConnection, error) {
	return s.LiveConnectionsInGroup("", limit)
}

// LiveConnectionsInGroup applies a console token's group scope before the
// result limit. Filtering after a global LIMIT can omit eligible connections
// when unrelated groups have more recent activity.
func (s *Store) LiveConnectionsInGroup(groupID string, limit int) ([]LiveConnection, error) {
	if limit <= 0 {
		limit = 100
	}
	if limit > 500 {
		limit = 500
	}
	now := s.now().UTC()
	groupID = strings.TrimSpace(groupID)
	var rows []liveConnectionRow
	var err error
	if groupID == "" {
		rows, err = s.queryLiveConnections(`WHERE c.native_state = 'active'
			ORDER BY c.last_seen_at DESC, c.host_id, c.connection_id LIMIT ?`, limit)
	} else {
		if !registry.ValidID(groupID) {
			return nil, ErrInvalidAccountGroupID
		}
		rows, err = s.queryLiveConnections(`WHERE c.native_state = 'active'
			AND EXISTS (
				SELECT 1 FROM account_group_devices g
				WHERE g.group_id = ? AND g.peer_id = c.host_id
			)
			ORDER BY c.last_seen_at DESC, c.host_id, c.connection_id LIMIT ?`, groupID, limit)
	}
	if err != nil {
		return nil, err
	}
	out := make([]LiveConnection, 0, len(rows))
	for _, row := range rows {
		out = append(out, row.view(now))
	}
	return out, nil
}

// LiveConnection returns one connection visible to the administrator.
func (s *Store) LiveConnection(hostID, connectionID string) (LiveConnection, error) {
	hostID = strings.TrimSpace(hostID)
	connectionID = strings.ToLower(strings.TrimSpace(connectionID))
	if !registry.ValidID(hostID) {
		return LiveConnection{}, ErrInvalidPeerID
	}
	if !liveconn.ValidConnectionID(connectionID) {
		return LiveConnection{}, liveconn.ErrInvalid
	}
	now := s.now().UTC()
	if err := s.expireLiveCommands(hostID, connectionID, now); err != nil {
		return LiveConnection{}, err
	}
	rows, err := s.queryLiveConnections(`WHERE c.host_id = ? AND c.connection_id = ?
		ORDER BY c.last_seen_at DESC LIMIT 1`, hostID, connectionID)
	if err != nil {
		return LiveConnection{}, err
	}
	if len(rows) == 0 {
		return LiveConnection{}, ErrLiveConnectionNotFound
	}
	return rows[0].view(now), nil
}

// expireLiveCommands settles deadline-bound commands before an administrator
// reads their state. Polling also expires commands, but a host may be offline
// or stopped, in which case the console must not keep presenting a stale
// queued/delivered command as pending indefinitely.
func (s *Store) expireLiveCommands(hostID, connectionID string, now time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec(`UPDATE live_connection_commands
		SET status = 'expired', finished_at = ?
		WHERE host_id = ? AND connection_id = ?
		AND status IN ('queued', 'delivered') AND deadline_at <= ?`,
		now.UnixMilli(), hostID, connectionID, now.UnixMilli())
	if err != nil {
		return fmt.Errorf("expire live connection commands: %w", err)
	}
	return nil
}

func (s *Store) queryLiveConnections(suffix string, args ...any) ([]liveConnectionRow, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT c.host_id, c.runtime_id, c.connection_id,
		c.peer_address, c.viewer, c.mode, c.route, c.permissions, c.controller,
		c.started_at, c.first_seen_at, c.last_seen_at, c.native_state,
		c.closed_at, c.close_reason, COALESCE(k.command_id, ''),
		COALESCE(k.status, ''), COALESCE(k.deadline_at, 0),
		COALESCE(r.detail, '')
		FROM live_connections c
		JOIN host_runtimes h ON h.host_id = c.host_id AND h.runtime_id = c.runtime_id
		LEFT JOIN live_connection_commands k ON k.command_id = (
			SELECT command_id FROM live_connection_commands
			WHERE host_id = c.host_id AND runtime_id = c.runtime_id
				AND connection_id = c.connection_id
			ORDER BY issued_at DESC, command_id DESC LIMIT 1)
		LEFT JOIN live_connection_command_receipts r ON r.command_id = k.command_id
		`+suffix, args...)
	if err != nil {
		return nil, fmt.Errorf("query live connections: %w", err)
	}
	defer rows.Close()
	out := []liveConnectionRow{}
	for rows.Next() {
		var row liveConnectionRow
		if err := rows.Scan(&row.HostID, &row.RuntimeID, &row.ConnectionID,
			&row.PeerAddress, &row.Viewer, &row.Mode, &row.Route,
			&row.Permissions, &row.Controller, &row.StartedAtMS,
			&row.FirstSeenMS, &row.LastSeenMS, &row.NativeState,
			&row.ClosedAtMS, &row.CloseReason, &row.CommandID,
			&row.CommandStatus, &row.CommandDeadlineMS, &row.ReceiptDetail); err != nil {
			return nil, err
		}
		out = append(out, row)
	}
	return out, rows.Err()
}

func (r liveConnectionRow) view(now time.Time) LiveConnection {
	state := r.NativeState
	if state == liveconn.ConnectionActive && now.Sub(timeFromMillis(r.LastSeenMS)) > liveconn.Freshness {
		state = liveconn.ConnectionStale
	}
	out := LiveConnection{
		HostID: r.HostID, RuntimeID: r.RuntimeID, ConnectionID: r.ConnectionID,
		PeerAddress: r.PeerAddress, Viewer: r.Viewer, Mode: r.Mode,
		Route: r.Route, Permissions: r.Permissions, Controller: r.Controller,
		StartedAt: timeFromMillis(r.StartedAtMS), State: state,
		LastHeartbeatAt: timeFromMillis(r.LastSeenMS), CommandID: r.CommandID,
		CommandStatus: r.CommandStatus, ReceiptDetail: r.ReceiptDetail,
	}
	if r.CommandDeadlineMS > 0 {
		out.CommandDeadline = timeFromMillis(r.CommandDeadlineMS)
	}
	return out
}

// DisconnectLiveConnection creates or retries one exact disconnect command.
// Reusing an idempotency key with a different target is rejected.
func (s *Store) DisconnectLiveConnection(hostID, connectionID, requestKey, actor, reason string) (liveconn.Command, error) {
	hostID = strings.TrimSpace(hostID)
	connectionID = strings.ToLower(strings.TrimSpace(connectionID))
	requestKey = strings.TrimSpace(requestKey)
	actor = normalizeText(actor, maxName)
	reason = normalizeText(reason, maxDisconnectReason)
	if !registry.ValidID(hostID) {
		return liveconn.Command{}, ErrInvalidPeerID
	}
	if !liveconn.ValidConnectionID(connectionID) || !liveconn.ValidRequestKey(requestKey) {
		return liveconn.Command{}, liveconn.ErrInvalid
	}
	now := s.now().UTC()
	nowMS := now.UnixMilli()
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return liveconn.Command{}, err
	}
	defer tx.Rollback()
	var command liveconn.Command
	err = tx.QueryRow(`SELECT command_id, host_id, runtime_id, connection_id,
		operation, request_key, reason, status, issued_at, deadline_at
		FROM live_connection_commands WHERE host_id = ? AND request_key = ?`,
		hostID, requestKey).Scan(&command.CommandID, &command.HostID,
		&command.RuntimeID, &command.ConnectionID, &command.Operation,
		&command.RequestKey, &command.Reason, &command.Status,
		&command.IssuedAtMS, &command.DeadlineMS)
	if err == nil {
		if command.ConnectionID != connectionID {
			return liveconn.Command{}, fmt.Errorf("%w: request key was used for another connection",
				liveconn.ErrConflict)
		}
		if command.DeadlineMS <= nowMS &&
			(command.Status == liveconn.CommandQueued || command.Status == liveconn.CommandDelivered) {
			if _, err := tx.Exec(`UPDATE live_connection_commands SET status = 'expired',
				finished_at = ? WHERE command_id = ? AND status IN ('queued', 'delivered')`,
				nowMS, command.CommandID); err != nil {
				return liveconn.Command{}, err
			}
			command.Status = liveconn.CommandExpired
		}
		if err := tx.Commit(); err != nil {
			return liveconn.Command{}, err
		}
		return command, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return liveconn.Command{}, err
	}

	var row liveConnectionRow
	var heartbeat int64
	err = tx.QueryRow(`SELECT c.runtime_id, c.connection_id, c.native_state,
		h.last_heartbeat_at FROM live_connections c
		JOIN host_runtimes h ON h.host_id = c.host_id AND h.runtime_id = c.runtime_id
		WHERE c.host_id = ? AND c.connection_id = ?`, hostID, connectionID).
		Scan(&row.RuntimeID, &row.ConnectionID, &row.NativeState, &heartbeat)
	if errors.Is(err, sql.ErrNoRows) {
		return liveconn.Command{}, ErrLiveConnectionNotFound
	}
	if err != nil {
		return liveconn.Command{}, err
	}
	if row.NativeState != liveconn.ConnectionActive {
		return liveconn.Command{}, fmt.Errorf("%w: connection is %s", liveconn.ErrConflict, row.NativeState)
	}
	if now.Sub(timeFromMillis(heartbeat)) > liveconn.Freshness {
		return liveconn.Command{}, ErrLiveRuntimeStale
	}
	commandID, err := newRandomHex(16)
	if err != nil {
		return liveconn.Command{}, err
	}
	command = liveconn.Command{
		CommandID: commandID, HostID: hostID, RuntimeID: row.RuntimeID,
		ConnectionID: connectionID, Operation: liveconn.OperationDisconnect,
		RequestKey: requestKey, Reason: reason, Status: liveconn.CommandQueued,
		IssuedAtMS: nowMS, DeadlineMS: now.Add(liveconn.CommandTTL).UnixMilli(),
	}
	if _, err := tx.Exec(`INSERT INTO live_connection_commands
		(command_id, request_key, host_id, runtime_id, connection_id, operation,
		 actor, reason, status, issued_at, deadline_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		command.CommandID, command.RequestKey, command.HostID, command.RuntimeID,
		command.ConnectionID, command.Operation, actor, command.Reason,
		command.Status, command.IssuedAtMS, command.DeadlineMS); err != nil {
		if isUniqueViolation(err) {
			// Release the failed transaction before reading the winner. This is
			// required on PostgreSQL, where a failed statement aborts the
			// transaction until rollback.
			_ = tx.Rollback()
			var raced liveconn.Command
			if readErr := s.db.QueryRow(`SELECT command_id, host_id, runtime_id,
				connection_id, operation, request_key, reason, status, issued_at, deadline_at
				FROM live_connection_commands WHERE host_id = ? AND request_key = ?`,
				hostID, requestKey).Scan(&raced.CommandID, &raced.HostID,
				&raced.RuntimeID, &raced.ConnectionID, &raced.Operation,
				&raced.RequestKey, &raced.Reason, &raced.Status,
				&raced.IssuedAtMS, &raced.DeadlineMS); readErr != nil {
				return liveconn.Command{}, readErr
			}
			if raced.ConnectionID != connectionID {
				return liveconn.Command{}, fmt.Errorf("%w: request key was used for another connection",
					liveconn.ErrConflict)
			}
			return raced, nil
		}
		return liveconn.Command{}, err
	}
	if err := insertLiveAuditTx(tx, AuditEvent{
		CreatedAt: now, Username: actor, Action: AuditConnectionDisconnect,
		Detail: "disconnect " + command.ConnectionID + " on " + hostID,
		HostID: hostID, EventKey: "live-command:" + command.CommandID, Success: true,
	}); err != nil {
		return liveconn.Command{}, err
	}
	if err := tx.Commit(); err != nil {
		return liveconn.Command{}, err
	}
	return command, nil
}

// PollLiveCommands returns queued commands once, marks them delivered, and
// expires commands that missed their deadline. Re-delivery is safe because the
// command identity and request key are stable.
func (s *Store) PollLiveCommands(hostID, runtimeID string, limit int) ([]liveconn.Command, error) {
	hostID = strings.TrimSpace(hostID)
	runtimeID = strings.ToLower(strings.TrimSpace(runtimeID))
	if !registry.ValidID(hostID) {
		return nil, ErrInvalidPeerID
	}
	if !liveconn.ValidRuntimeID(runtimeID) {
		return nil, liveconn.ErrInvalid
	}
	if limit <= 0 || limit > liveconn.MaxQueuedCommands {
		limit = liveconn.MaxQueuedCommands
	}
	now := s.now().UTC().UnixMilli()
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`UPDATE live_connection_commands SET status = 'expired',
		finished_at = ? WHERE host_id = ? AND runtime_id = ?
		AND status IN ('queued', 'delivered') AND deadline_at <= ?`,
		now, hostID, runtimeID, now); err != nil {
		return nil, err
	}
	rows, err := tx.Query(`SELECT command_id, host_id, runtime_id, connection_id,
		operation, request_key, reason, status, issued_at, deadline_at
		FROM live_connection_commands WHERE host_id = ? AND runtime_id = ?
		AND status = 'queued' AND deadline_at > ?
		ORDER BY issued_at, command_id LIMIT ?`, hostID, runtimeID, now, limit)
	if err != nil {
		return nil, err
	}
	var out []liveconn.Command
	for rows.Next() {
		var command liveconn.Command
		if err := rows.Scan(&command.CommandID, &command.HostID,
			&command.RuntimeID, &command.ConnectionID, &command.Operation,
			&command.RequestKey, &command.Reason, &command.Status,
			&command.IssuedAtMS, &command.DeadlineMS); err != nil {
			rows.Close()
			return nil, err
		}
		out = append(out, command)
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}
	for _, command := range out {
		if _, err := tx.Exec(`UPDATE live_connection_commands SET status = 'delivered',
			delivered_at = CASE WHEN delivered_at = 0 THEN ? ELSE delivered_at END
			WHERE command_id = ? AND status = 'queued'`, now, command.CommandID); err != nil {
			return nil, err
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return out, nil
}

// RecordLiveCommandReceipt accepts at-least-once native receipts. A command is
// applied only when the native confirmation says the exact session is absent.
func (s *Store) RecordLiveCommandReceipt(hostID, runtimeID string, receipt liveconn.Receipt) (liveconn.Receipt, error) {
	hostID = strings.TrimSpace(hostID)
	runtimeID = strings.ToLower(strings.TrimSpace(runtimeID))
	if !registry.ValidID(hostID) {
		return liveconn.Receipt{}, ErrInvalidPeerID
	}
	if !liveconn.ValidRuntimeID(runtimeID) {
		return liveconn.Receipt{}, liveconn.ErrInvalid
	}
	normalized, err := receipt.Normalize()
	if err != nil {
		return liveconn.Receipt{}, err
	}
	now := s.now().UTC()
	nowMS := now.UnixMilli()
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return liveconn.Receipt{}, err
	}
	defer tx.Rollback()
	var boundHost, boundRuntime, connectionID, currentStatus string
	var deadline int64
	err = tx.QueryRow(`SELECT host_id, runtime_id, connection_id, status, deadline_at
		FROM live_connection_commands WHERE command_id = ?`,
		normalized.CommandID).Scan(&boundHost, &boundRuntime, &connectionID,
		&currentStatus, &deadline)
	if errors.Is(err, sql.ErrNoRows) {
		return liveconn.Receipt{}, ErrLiveCommandReceipt
	}
	if err != nil {
		return liveconn.Receipt{}, err
	}
	if boundHost != hostID || boundRuntime != runtimeID {
		return liveconn.Receipt{}, ErrLiveCommandReceipt
	}
	var previousSequence int64
	var previousStatus, previousDetail string
	var previousConfirmed bool
	var previousAt int64
	err = tx.QueryRow(`SELECT receipt_sequence, status, confirmed_absent, detail, received_at
		FROM live_connection_command_receipts WHERE command_id = ?`,
		normalized.CommandID).Scan(&previousSequence, &previousStatus,
		&previousConfirmed, &previousDetail, &previousAt)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return liveconn.Receipt{}, err
	}
	if err == nil {
		if normalized.ReceiptSequence < uint64(previousSequence) {
			return liveconn.Receipt{}, ErrLiveCommandReceipt
		}
		if normalized.ReceiptSequence == uint64(previousSequence) {
			if previousStatus != normalized.Status || previousConfirmed != normalized.ConfirmedAbsent ||
				previousDetail != normalized.Detail {
				return liveconn.Receipt{}, ErrLiveCommandReceipt
			}
			return liveconn.Receipt{
				ReceiptSequence: uint64(previousSequence), CommandID: normalized.CommandID,
				Status: previousStatus, ConfirmedAbsent: previousConfirmed,
				Detail: previousDetail,
			}, nil
		}
	}
	if normalized.Status == liveconn.CommandApplied {
		if currentStatus == liveconn.CommandExpired {
			return liveconn.Receipt{}, ErrLiveCommandExpired
		}
		if currentStatus == liveconn.CommandFailed {
			return liveconn.Receipt{}, ErrLiveCommandReceipt
		}
		if currentStatus != liveconn.CommandDelivered && currentStatus != liveconn.CommandApplied {
			return liveconn.Receipt{}, ErrLiveCommandReceipt
		}
		if normalized.ReceiptSequence == 0 {
			return liveconn.Receipt{}, ErrLiveCommandReceipt
		}
		if _, err := tx.Exec(`UPDATE live_connections SET native_state = 'closed',
			closed_at = ?, close_reason = 'native disconnect confirmed'
			WHERE host_id = ? AND runtime_id = ? AND connection_id = ?
			AND native_state = 'active'`, nowMS, hostID, runtimeID, connectionID); err != nil {
			return liveconn.Receipt{}, err
		}
	} else if currentStatus != liveconn.CommandDelivered && currentStatus != liveconn.CommandFailed {
		// Failure is a native result for a command that was actually handed to
		// this runtime. A queued command remains available for polling.
		return liveconn.Receipt{}, ErrLiveCommandReceipt
	}
	if err == nil {
		_, err = tx.Exec(`UPDATE live_connection_command_receipts SET
			receipt_sequence = ?, status = ?, confirmed_absent = ?, detail = ?, received_at = ?
			WHERE command_id = ?`, normalized.ReceiptSequence, normalized.Status,
			normalized.ConfirmedAbsent, normalized.Detail, nowMS, normalized.CommandID)
	} else {
		_, err = tx.Exec(`INSERT INTO live_connection_command_receipts
			(command_id, host_id, runtime_id, receipt_sequence, status,
			 confirmed_absent, detail, received_at)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, normalized.CommandID, hostID,
			runtimeID, normalized.ReceiptSequence, normalized.Status,
			normalized.ConfirmedAbsent, normalized.Detail, nowMS)
	}
	if err != nil {
		return liveconn.Receipt{}, err
	}
	terminalStatus := liveconn.CommandApplied
	if normalized.Status == liveconn.CommandFailed {
		terminalStatus = liveconn.CommandFailed
	}
	if _, err := tx.Exec(`UPDATE live_connection_commands SET status = ?,
		finished_at = ?, receipt_detail = ? WHERE command_id = ?
		AND status IN ('queued', 'delivered')`,
		terminalStatus, nowMS, normalized.Detail, normalized.CommandID); err != nil {
		return liveconn.Receipt{}, err
	}
	if err := insertLiveAuditTx(tx, AuditEvent{
		CreatedAt: now, Username: "host", Action: AuditConnectionDisconnectReceipt,
		Detail: normalized.CommandID + " " + terminalStatus,
		HostID: hostID, EventKey: fmt.Sprintf("live-receipt:%s:%d",
			normalized.CommandID, normalized.ReceiptSequence),
		Success: terminalStatus == liveconn.CommandApplied,
	}); err != nil {
		return liveconn.Receipt{}, err
	}
	if err := tx.Commit(); err != nil {
		return liveconn.Receipt{}, err
	}
	return normalized, nil
}

func insertLiveAuditTx(tx *sql.Tx, event AuditEvent) error {
	key := strings.TrimSpace(event.EventKey)
	_, err := tx.Exec(`INSERT INTO audit_events
		(created_at, username, action, detail, client_ip, location, success, host_id, event_key)
		VALUES (?, ?, ?, ?, '', '', ?, ?, ?)
		ON CONFLICT(event_key) WHERE event_key <> '' DO NOTHING`,
		unixMillis(event.CreatedAt), normalizeText(event.Username, maxName),
		event.Action, normalizeText(event.Detail, maxLiveDetail), event.Success,
		normalizeText(event.HostID, maxConnectionPeer), key)
	return err
}

// EncodeLiveSnapshot preserves the canonical content in diagnostics when a
// caller needs to compare an accepted retry.
func EncodeLiveSnapshot(snapshot liveconn.Snapshot) (string, error) {
	normalized, err := snapshot.Normalize()
	if err != nil {
		return "", err
	}
	encoded, err := json.Marshal(normalized)
	if err != nil {
		return "", err
	}
	return string(encoded), nil
}

// SortLiveConnections is used by tests and callers that merge bounded runtime
// responses before presenting them as one directory.
func SortLiveConnections(items []LiveConnection) {
	sort.Slice(items, func(i, j int) bool {
		if items[i].LastHeartbeatAt.Equal(items[j].LastHeartbeatAt) {
			return items[i].ConnectionID < items[j].ConnectionID
		}
		return items[i].LastHeartbeatAt.After(items[j].LastHeartbeatAt)
	})
}
