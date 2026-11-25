package account

import (
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// AlarmEvent is a security-relevant event copied from the account audit trail
// into its own queryable stream. SourceAuditID and SourceKey make the copy
// idempotent while keeping alarms readable after ordinary audit retention.
type AlarmEvent struct {
	ID            int64
	CreatedAt     time.Time
	Username      string
	HostID        string
	ClientIP      string
	Location      string
	Category      string
	Severity      string
	Code          string
	Summary       string
	Detail        string
	SourceAction  string
	SourceAuditID int64
	SourceKey     string
}

// AlarmFilter narrows the security alarm stream. Empty dimensions are
// unrestricted, matching the account audit report filters.
type AlarmFilter struct {
	Category string
	Severity string
	Code     string
	Username string
	HostID   string
	Bounds   TimeBounds
}

func alarmClassification(event AuditEvent) (category, severity, code, summary string, ok bool) {
	switch strings.TrimSpace(event.Action) {
	case AuditHostDenied:
		return "security", "warning", "host_access_denied", "主机按访问策略拒绝了连接", true
	case AuditEmergencyAccess:
		return "access", "notice", "emergency_access_granted", "管理员授予了应急访问", true
	case AuditLogin:
		// The login limiter writes this suffix only on the failed attempt that
		// reaches its configured threshold. Later attempts during the lockout
		// are recorded as the separate detail "locked out", so they do not
		// create repeated alarms.
		if !event.Success && strings.HasSuffix(strings.TrimSpace(event.Detail), "; locked out") {
			return "security", "warning", "account_login_locked", "账号因连续身份验证失败而锁定", true
		}
	case AuditAccountPassword:
		if event.Success {
			return "account", "notice", "account_password_reset", "管理员重置了账号密码", true
		}
	case AuditPasswordChange:
		if event.Success {
			return "account", "notice", "account_password_changed", "账号密码已修改", true
		}
	case AuditTwoFactorEnable:
		if event.Success {
			return "security", "notice", "two_factor_enabled", "账号启用了双重验证", true
		}
	case AuditTwoFactorDisable:
		if event.Success {
			return "security", "warning", "two_factor_disabled", "账号关闭了双重验证", true
		}
	case AuditTwoFactorReset:
		if event.Success {
			return "security", "warning", "two_factor_reset", "管理员重置了账号双重验证", true
		}
	case AuditTwoFactorRecovery:
		if event.Success {
			return "security", "notice", "two_factor_recovery_codes_generated", "账号重新生成了双重验证恢复码", true
		}
	case AuditWebAuthnRegister:
		if event.Success {
			return "security", "notice", "webauthn_credential_added", "账号登记了 WebAuthn 凭据", true
		}
	case AuditWebAuthnRemove:
		if event.Success {
			return "security", "warning", "webauthn_credential_removed", "账号移除了 WebAuthn 凭据", true
		}
	case AuditLoginDevicePolicy, AuditLoginDeviceTrust, AuditLoginDeviceRevoke:
		if event.Success {
			return "security", "warning", "login_devices_changed", "账号登录设备信任设置已变更", true
		}
	case AuditRoleChange, AuditRoleCreate, AuditRoleUpdate, AuditRoleDelete, AuditRoleBind, AuditRoleUnbind:
		if event.Success {
			return "access", "warning", "admin_access_changed", "管理员角色或权限绑定已变更", true
		}
	case AuditDevicePolicyUpdate, AuditDevicePolicyDelete, AuditDeviceDisable, AuditDeviceEnable:
		if event.Success {
			return "access", "warning", "host_access_changed", "主机准入策略或设备状态已变更", true
		}
	case AuditAccessKidRevoke:
		if event.Success {
			return "security", "warning", "access_signing_key_revoked", "访问票据签名密钥已撤销", true
		}
	default:
	}
	return "", "", "", "", false
}

// appendAlarmForAuditTx stores only explicitly classified security events.
// Host reports carry stable event keys, so transport retries map to one alarm;
// account actions use the newly assigned audit row id as their source key.
func appendAlarmForAuditTx(tx *sql.Tx, id int64, event AuditEvent, at int64) (*AlarmEvent, error) {
	category, severity, code, summary, ok := alarmClassification(event)
	if !ok {
		return nil, nil
	}
	key := strings.TrimSpace(event.EventKey)
	if key != "" {
		key = strings.TrimSpace(event.Action) + ":" + key
	} else {
		key = fmt.Sprintf("audit:%d", id)
	}
	var alarmID int64
	err := tx.QueryRow(
		`INSERT INTO alarm_events
			(created_at, username, host_id, client_ip, location, category, severity,
			 code, summary, detail, source_action, source_audit_id, source_key)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		 ON CONFLICT(source_key) DO NOTHING
		 RETURNING id`,
		at, strings.TrimSpace(event.Username), strings.TrimSpace(event.HostID),
		strings.TrimSpace(event.ClientIP), strings.TrimSpace(event.Location),
		category, severity, code, summary, strings.TrimSpace(event.Detail),
		strings.TrimSpace(event.Action), id, key,
	).Scan(&alarmID)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("insert security alarm: %w", err)
	}
	if _, err := tx.Exec(
		`INSERT INTO alarm_email_deliveries
			(alarm_id, user_id, recipient, status, available_at, created_at)
		 SELECT ?, u.id, u.email, 'pending', ?, ?
		 FROM users u
		 WHERE u.alarm_email_enabled
		   AND u.email_verified_at > 0
		   AND u.email <> ''
		   AND NOT u.banned
		   AND (
		       u.role IN ('admin', 'operator', 'readonly')
		       OR EXISTS (
			       SELECT 1 FROM role_bindings b
			       JOIN admin_roles r ON r.id = b.role_id
			       WHERE b.user_id = u.id
			         AND b.scope_kind = 'global'
			         AND (',' || r.permissions || ',') LIKE '%,audit.read,%'
		       )
		   )`, alarmID, at, at,
	); err != nil {
		return nil, fmt.Errorf("enqueue security alarm email deliveries: %w", err)
	}
	if _, err := tx.Exec(
		`DELETE FROM alarm_events WHERE id <
			(SELECT id FROM alarm_events ORDER BY id DESC LIMIT 1 OFFSET ?)`,
		auditRetention-1,
	); err != nil {
		return nil, fmt.Errorf("trim security alarms: %w", err)
	}
	return &AlarmEvent{
		ID: alarmID, CreatedAt: timeFromMillis(at), Username: strings.TrimSpace(event.Username),
		HostID: strings.TrimSpace(event.HostID), ClientIP: strings.TrimSpace(event.ClientIP),
		Location: strings.TrimSpace(event.Location), Category: category, Severity: severity,
		Code: code, Summary: summary, Detail: strings.TrimSpace(event.Detail),
		SourceAction: strings.TrimSpace(event.Action), SourceAuditID: id, SourceKey: key,
	}, nil
}

// AlarmEventsForFilterBefore pages newest first using an ID cursor. A group
// filter resolves current group membership from alarm usernames, like the
// account audit view; host-only events remain visible to global audit readers.
func (s *Store) AlarmEventsForFilterBefore(groupID string, filter AlarmFilter, beforeID int64, limit int) ([]AlarmEvent, error) {
	groupID = strings.TrimSpace(groupID)
	clauses := make([]string, 0, 7)
	args := make([]any, 0, 8)
	if groupID != "" {
		if !registry.ValidID(groupID) {
			return nil, ErrInvalidAccountGroupID
		}
		clauses = append(clauses, `username IN (
			SELECT u.username FROM account_group_members m
			JOIN users u ON u.id = m.user_id
			WHERE m.group_id = ?)`)
		args = append(args, groupID)
	}
	for _, item := range []struct{ column, value string }{
		{"category", filter.Category}, {"severity", filter.Severity},
		{"code", filter.Code}, {"username", filter.Username},
	} {
		if value := strings.TrimSpace(item.value); value != "" {
			clauses = append(clauses, item.column+" = ?")
			args = append(args, value)
		}
	}
	if hostID := strings.TrimSpace(filter.HostID); hostID != "" {
		clauses = append(clauses, "host_id = ? COLLATE NOCASE")
		args = append(args, hostID)
	}
	clauses, args = addTimeBounds(clauses, args, "created_at", filter.Bounds)
	if beforeID > 0 {
		clauses = append(clauses, "id < ?")
		args = append(args, beforeID)
	}
	if limit <= 0 {
		limit = 20
	}
	if limit > 500 {
		limit = 500
	}
	where := ""
	if len(clauses) > 0 {
		where = " WHERE " + strings.Join(clauses, " AND ")
	}
	args = append(args, limit)
	query := `SELECT id, created_at, username, host_id, client_ip, location,
		category, severity, code, summary, detail, source_action, source_audit_id, source_key
		FROM alarm_events` + where + ` ORDER BY id DESC LIMIT ?`

	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list security alarms: %w", s.path, err)
	}
	defer rows.Close()
	alarms := make([]AlarmEvent, 0)
	for rows.Next() {
		var alarm AlarmEvent
		var created int64
		if err := rows.Scan(&alarm.ID, &created, &alarm.Username, &alarm.HostID,
			&alarm.ClientIP, &alarm.Location, &alarm.Category, &alarm.Severity,
			&alarm.Code, &alarm.Summary, &alarm.Detail, &alarm.SourceAction,
			&alarm.SourceAuditID, &alarm.SourceKey); err != nil {
			return nil, fmt.Errorf("account store %s: scan security alarm: %w", s.path, err)
		}
		alarm.CreatedAt = timeFromMillis(created)
		alarms = append(alarms, alarm)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list security alarms: %w", s.path, err)
	}
	return alarms, nil
}
