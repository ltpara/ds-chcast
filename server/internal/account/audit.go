package account

import (
	"fmt"
	"strings"
	"time"
)

// AuditLogin is recorded for every login attempt, including attempts for
// unknown usernames and lockouts. The other actions cover account changes.
const (
	AuditLogin             = "login"
	AuditLogout            = "logout"
	AuditRefresh           = "session_refresh"
	AuditPasswordChange    = "password_change"
	AuditTwoFactorEnable   = "2fa_enable"
	AuditTwoFactorDisable  = "2fa_disable"
	AuditTwoFactorReset    = "2fa_reset"
	AuditTwoFactorRecovery = "2fa_recovery"
	AuditSessionRevoke     = "session_revoke"
	AuditSessionRename     = "session_rename"
	AuditLoginUnlock       = "login_unlock"
	AuditRoleChange        = "role_change"
	AuditAccountCreate     = "account_create"
	AuditAccountDelete     = "account_delete"
	AuditAccountPassword   = "account_password_reset"
	AuditAccountBan        = "account_ban"
	AuditAccountUnban      = "account_unban"
	AuditAccountEmail      = "account_email"
	AuditAccountPhone      = "account_phone"
	AuditAccountRecover    = "account_recover"
	AuditWebAuthnRegister  = "webauthn_register"
	AuditWebAuthnRemove    = "webauthn_remove"
	AuditDeviceLabels      = "device_labels"
	AuditDeviceReset       = "device_reset"
	AuditDeviceLeaseRemove = "device_lease_remove"
	AuditRecordingUpload   = "recording_upload"
	AuditRecordingDelete   = "recording_delete"
)

// auditRetention keeps the table bounded without an external maintenance job.
const auditRetention = 10000

// AuditEvent is one persisted account-security event. Usernames are stored as
// attempted so failed logins for unknown accounts remain visible.
type AuditEvent struct {
	ID        int64
	CreatedAt time.Time
	Username  string
	Action    string
	Detail    string
	ClientIP  string
	// Location is the operator-supplied label for ClientIP. It stays empty
	// when no GeoIP table is configured or the address is not covered.
	Location string
	Success  bool
}

func (s *Store) AppendAudit(event AuditEvent) error {
	action := strings.TrimSpace(event.Action)
	if action == "" {
		return fmt.Errorf("account store %s: append audit: action is empty", s.path)
	}
	username := strings.TrimSpace(event.Username)
	if username == "" {
		username = "-"
	}
	at := event.CreatedAt
	if at.IsZero() {
		at = s.now().UTC()
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`INSERT INTO audit_events (created_at, username, action, detail, client_ip, location, success)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		unixMillis(at), username, action, strings.TrimSpace(event.Detail),
		strings.TrimSpace(event.ClientIP), strings.TrimSpace(event.Location), event.Success,
	); err != nil {
		return fmt.Errorf("account store %s: append audit: %w", s.path, err)
	}
	if _, err := s.db.Exec(
		`DELETE FROM audit_events WHERE id <= (SELECT COALESCE(MAX(id), 0) - ? FROM audit_events)`,
		auditRetention,
	); err != nil {
		return fmt.Errorf("account store %s: trim audit: %w", s.path, err)
	}
	return nil
}

// AuditEvents returns the newest events first. Limit defaults to 20 and is
// capped at 500 so an operator cannot accidentally stream the whole table.
func (s *Store) AuditEvents(limit int) ([]AuditEvent, error) {
	return s.AuditEventsFor("", limit)
}

// AuditEventsFor filters by exact action; an empty action returns every kind.
func (s *Store) AuditEventsFor(action string, limit int) ([]AuditEvent, error) {
	return s.AuditEventsForBefore(action, 0, limit)
}

// AuditEventsForBefore returns one page older than beforeID; 0 starts at the
// newest row. The cursor is an ID rather than an offset so rows appended while
// an operator pages do not shift the window.
func (s *Store) AuditEventsForBefore(action string, beforeID int64, limit int) ([]AuditEvent, error) {
	if limit <= 0 {
		limit = 20
	}
	if limit > 500 {
		limit = 500
	}
	action = strings.TrimSpace(action)
	clauses := make([]string, 0, 2)
	args := make([]any, 0, 3)
	if action != "" {
		clauses = append(clauses, "action = ?")
		args = append(args, action)
	}
	if beforeID > 0 {
		clauses = append(clauses, "id < ?")
		args = append(args, beforeID)
	}
	where := ""
	if len(clauses) > 0 {
		where = " WHERE " + strings.Join(clauses, " AND ")
	}
	args = append(args, limit)
	query := `SELECT id, created_at, username, action, detail, client_ip, location, success
		 FROM audit_events` + where + ` ORDER BY id DESC LIMIT ?`
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list audit: %w", s.path, err)
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
			return nil, fmt.Errorf("account store %s: scan audit: %w", s.path, err)
		}
		event.CreatedAt = timeFromMillis(created)
		events = append(events, event)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list audit: %w", s.path, err)
	}
	return events, nil
}
