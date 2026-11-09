package account

import (
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
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
	AuditDeviceWake        = "device_wake"
	AuditRecordingUpload   = "recording_upload"
	AuditRecordingDelete   = "recording_delete"
	// AuditHostDenied is a Hello the host policy refused. The username is
	// unknown ("-"), the client IP is the refused address and the detail names
	// the host and refusal.
	AuditHostDenied = "host_denied"

	// AuditAccessTicket records one successful short-lived ticket issuance.
	AuditAccessTicket = "access_ticket"

	AuditGroupCreate       = "group_create"
	AuditGroupUpdate       = "group_update"
	AuditGroupDelete       = "group_delete"
	AuditGroupMemberAdd    = "group_member_add"
	AuditGroupMemberUpdate = "group_member_update"
	AuditGroupMemberRemove = "group_member_remove"
	AuditGroupDeviceAdd    = "group_device_add"
	AuditGroupDeviceRemove = "group_device_remove"

	AuditDevicePolicyUpdate  = "device_policy_update"
	AuditDevicePolicyDelete  = "device_policy_delete"
	AuditDevicePolicyReceipt = "device_policy_receipt"

	AuditAPITokenCreate = "api_token_create"
	AuditAPITokenRevoke = "api_token_revoke"
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
	HostID    string
	// EventKey identifies a host report across retries, independently of its
	// request timestamp. Empty leaves ordinary account events append-only.
	EventKey string
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
	key := strings.TrimSpace(event.EventKey)
	// The SELECT avoids advancing the sequence for routine retries; the
	// unique index also handles simultaneous writers on different servers.
	result, err := s.db.Exec(
		`INSERT INTO audit_events (created_at, username, action, detail, client_ip, location, success, host_id, event_key)
		 SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?
		 WHERE ? = '' OR NOT EXISTS (SELECT 1 FROM audit_events WHERE event_key = ?)
		 ON CONFLICT(event_key) WHERE event_key <> '' DO NOTHING`,
		unixMillis(at), username, action, strings.TrimSpace(event.Detail),
		strings.TrimSpace(event.ClientIP), strings.TrimSpace(event.Location), event.Success,
		strings.TrimSpace(event.HostID), key, key, key,
	)
	if err != nil {
		return fmt.Errorf("account store %s: append audit: %w", s.path, err)
	}
	if inserted, err := result.RowsAffected(); err != nil {
		return fmt.Errorf("account store %s: append audit result: %w", s.path, err)
	} else if inserted == 0 {
		return nil
	}
	// Conflict races can still leave sequence gaps, so retain actual rows.
	if _, err := s.db.Exec(
		`DELETE FROM audit_events WHERE id <
		 (SELECT id FROM audit_events ORDER BY id DESC LIMIT 1 OFFSET ?)`,
		auditRetention-1,
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
	action = strings.TrimSpace(action)
	clauses := make([]string, 0, 2)
	args := make([]any, 0, 3)
	if action != "" {
		clauses = append(clauses, "action = ?")
		args = append(args, action)
	}
	return s.auditEvents(clauses, args, beforeID, limit)
}

// AuditEventsForGroupBefore pages the audit trail of one group's members.
// Audit rows carry a username rather than a user id, so membership is resolved
// through the account table; a renamed or deleted user stops matching, which
// is the same rule the rest of the audit view follows.
func (s *Store) AuditEventsForGroupBefore(groupID, action string, beforeID int64, limit int) ([]AuditEvent, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return nil, ErrInvalidAccountGroupID
	}
	action = strings.TrimSpace(action)
	clauses := []string{`username IN (
		SELECT u.username FROM account_group_members m
		JOIN users u ON u.id = m.user_id
		WHERE m.group_id = ?)`}
	args := []any{groupID}
	if action != "" {
		clauses = append(clauses, "action = ?")
		args = append(args, action)
	}
	return s.auditEvents(clauses, args, beforeID, limit)
}

func (s *Store) auditEvents(clauses []string, args []any, beforeID int64, limit int) ([]AuditEvent, error) {
	if limit <= 0 {
		limit = 20
	}
	if limit > 500 {
		limit = 500
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
	query := `SELECT id, created_at, username, action, detail, client_ip, location, success, host_id, event_key
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
			&event.Detail, &event.ClientIP, &event.Location, &event.Success,
			&event.HostID, &event.EventKey); err != nil {
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
