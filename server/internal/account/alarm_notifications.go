package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

var ErrAlarmEmailRequiresVerifiedAddress = errors.New("security alert email requires a verified email address")

// AlarmEmailDelivery is one recipient's durable delivery of a security alarm.
// Recipient is snapshotted when the alarm is written so changing an address
// cannot redirect a message already in the outbox.
type AlarmEmailDelivery struct {
	ID        int64
	Alarm     AlarmEvent
	UserID    string
	Username  string
	Recipient string
	Attempts  int
}

type AlarmEmailDeliveryStatus struct {
	Username    string
	Recipient   string
	Status      string
	Attempts    int
	AvailableAt time.Time
	LastError   string
	CreatedAt   time.Time
	SentAt      time.Time
}

func (s *Store) AlarmEmailPreference(userID string) (bool, error) {
	userID = strings.TrimSpace(userID)
	s.mu.Lock()
	defer s.mu.Unlock()
	var enabled bool
	err := s.db.QueryRow(
		`SELECT alarm_email_enabled FROM users WHERE id = ?`, userID,
	).Scan(&enabled)
	if errors.Is(err, sql.ErrNoRows) {
		return false, ErrUnknownUser
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read alarm email preference: %w", s.path, err)
	}
	return enabled, nil
}

func (s *Store) SetAlarmEmailPreference(userID string, enabled bool) error {
	userID = strings.TrimSpace(userID)
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		verified int64
		email    string
	)
	err := s.db.QueryRow(
		`SELECT email, email_verified_at FROM users WHERE id = ?`, userID,
	).Scan(&email, &verified)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownUser
	}
	if err != nil {
		return fmt.Errorf("account store %s: read alarm email address: %w", s.path, err)
	}
	if enabled && (strings.TrimSpace(email) == "" || verified == 0) {
		return ErrAlarmEmailRequiresVerifiedAddress
	}
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin alarm email preference update: %w", s.path, err)
	}
	defer tx.Rollback()
	result, err := tx.Exec(`UPDATE users SET alarm_email_enabled = ? WHERE id = ?`, enabled, userID)
	if err != nil {
		return fmt.Errorf("account store %s: update alarm email preference: %w", s.path, err)
	}
	present, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("account store %s: check alarm email preference update: %w", s.path, err)
	}
	if present == 0 {
		return ErrUnknownUser
	}
	if !enabled {
		if _, err := tx.Exec(
			`UPDATE alarm_email_deliveries SET status = 'cancelled', lease_until = 0
			 WHERE user_id = ? AND status IN ('pending', 'retry')`, userID,
		); err != nil {
			return fmt.Errorf("account store %s: cancel alarm email deliveries: %w", s.path, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit alarm email preference: %w", s.path, err)
	}
	return nil
}

// ClaimAlarmEmailDelivery leases one ready or abandoned delivery. The
// conditional update makes competing server processes claim a row only once.
func (s *Store) ClaimAlarmEmailDelivery(now time.Time, lease time.Duration) (*AlarmEmailDelivery, error) {
	if lease <= 0 {
		lease = 30 * time.Second
	}
	nowMS := unixMillis(now)
	leaseUntil := unixMillis(now.Add(lease))
	s.mu.Lock()
	defer s.mu.Unlock()
	for range 4 {
		tx, err := s.db.Begin()
		if err != nil {
			return nil, fmt.Errorf("account store %s: begin alarm email claim: %w", s.path, err)
		}
		var id int64
		var eligible bool
		err = tx.QueryRow(
			`SELECT d.id,
				(u.alarm_email_enabled AND u.email_verified_at > 0 AND u.email = d.recipient
				 AND u.email <> '' AND NOT u.banned
				 AND (u.role IN ('admin', 'operator', 'readonly') OR EXISTS (
					SELECT 1 FROM role_bindings b
					JOIN admin_roles r ON r.id = b.role_id
					WHERE b.user_id = u.id AND b.scope_kind = 'global'
					  AND (',' || r.permissions || ',') LIKE '%,audit.read,%'
				 )))
			 FROM alarm_email_deliveries d JOIN users u ON u.id = d.user_id
			 WHERE ((d.status IN ('pending', 'retry') AND d.available_at <= ?)
			    OR (d.status = 'sending' AND d.lease_until <= ?))
			 ORDER BY d.available_at, d.id LIMIT 1`, nowMS, nowMS,
		).Scan(&id, &eligible)
		if errors.Is(err, sql.ErrNoRows) {
			if err := tx.Commit(); err != nil {
				return nil, fmt.Errorf("account store %s: finish empty alarm email claim: %w", s.path, err)
			}
			return nil, nil
		}
		if err != nil {
			tx.Rollback()
			return nil, fmt.Errorf("account store %s: find alarm email delivery: %w", s.path, err)
		}
		if !eligible {
			if _, err := tx.Exec(
				`UPDATE alarm_email_deliveries SET status = 'cancelled', lease_until = 0
				 WHERE id = ?`, id,
			); err != nil {
				tx.Rollback()
				return nil, fmt.Errorf("account store %s: cancel ineligible alarm email delivery: %w", s.path, err)
			}
			if err := tx.Commit(); err != nil {
				return nil, fmt.Errorf("account store %s: commit cancelled alarm email delivery: %w", s.path, err)
			}
			continue
		}
		result, err := tx.Exec(
			`UPDATE alarm_email_deliveries SET status = 'sending', attempts = attempts + 1, lease_until = ?
			 WHERE id = ? AND (
				(status IN ('pending', 'retry') AND available_at <= ?)
				OR (status = 'sending' AND lease_until <= ?)
			 )`, leaseUntil, id, nowMS, nowMS,
		)
		if err != nil {
			tx.Rollback()
			return nil, fmt.Errorf("account store %s: lease alarm email delivery: %w", s.path, err)
		}
		affected, err := result.RowsAffected()
		if err != nil {
			tx.Rollback()
			return nil, fmt.Errorf("account store %s: check alarm email lease: %w", s.path, err)
		}
		if affected == 0 {
			tx.Rollback()
			continue
		}
		var delivery AlarmEmailDelivery
		var created int64
		err = tx.QueryRow(
			`SELECT d.id, d.user_id, u.username, d.recipient, d.attempts,
			       a.id, a.created_at, a.username, a.host_id, a.client_ip, a.location,
			       a.category, a.severity, a.code, a.summary, a.detail,
			       a.source_action, a.source_audit_id, a.source_key
			 FROM alarm_email_deliveries d
			 JOIN users u ON u.id = d.user_id
			 JOIN alarm_events a ON a.id = d.alarm_id
			 WHERE d.id = ?`, id,
		).Scan(&delivery.ID, &delivery.UserID, &delivery.Username, &delivery.Recipient,
			&delivery.Attempts, &delivery.Alarm.ID, &created, &delivery.Alarm.Username,
			&delivery.Alarm.HostID, &delivery.Alarm.ClientIP, &delivery.Alarm.Location,
			&delivery.Alarm.Category, &delivery.Alarm.Severity, &delivery.Alarm.Code,
			&delivery.Alarm.Summary, &delivery.Alarm.Detail, &delivery.Alarm.SourceAction,
			&delivery.Alarm.SourceAuditID, &delivery.Alarm.SourceKey)
		if err != nil {
			tx.Rollback()
			return nil, fmt.Errorf("account store %s: read claimed alarm email delivery: %w", s.path, err)
		}
		delivery.Alarm.CreatedAt = timeFromMillis(created)
		if err := tx.Commit(); err != nil {
			return nil, fmt.Errorf("account store %s: commit alarm email lease: %w", s.path, err)
		}
		return &delivery, nil
	}
	return nil, nil
}

func (s *Store) MarkAlarmEmailDeliverySent(id int64, sentAt time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`UPDATE alarm_email_deliveries
		 SET status = 'sent', sent_at = ?, lease_until = 0, last_error = ''
		 WHERE id = ? AND status = 'sending'`, unixMillis(sentAt), id,
	)
	return checkAlarmEmailDeliveryUpdate(s.path, result, err, id)
}

func (s *Store) RetryAlarmEmailDelivery(id int64, availableAt time.Time, sendErr error) error {
	message := "delivery failed"
	if sendErr != nil {
		message = strings.Join(strings.Fields(sendErr.Error()), " ")
	}
	if len(message) > 512 {
		message = message[:512]
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`UPDATE alarm_email_deliveries
		 SET status = 'retry', available_at = ?, lease_until = 0, last_error = ?
		 WHERE id = ? AND status = 'sending'`, unixMillis(availableAt), message, id,
	)
	return checkAlarmEmailDeliveryUpdate(s.path, result, err, id)
}

func checkAlarmEmailDeliveryUpdate(path string, result sql.Result, err error, id int64) error {
	if err != nil {
		return fmt.Errorf("account store %s: update alarm email delivery %d: %w", path, id, err)
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("account store %s: check alarm email delivery %d: %w", path, id, err)
	}
	if affected == 0 {
		return fmt.Errorf("account store %s: alarm email delivery %d is not leased", path, id)
	}
	return nil
}

func (s *Store) AlarmEmailDeliveries(alarmID int64) ([]AlarmEmailDeliveryStatus, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT u.username, d.recipient, d.status, d.attempts, d.available_at,
		       d.last_error, d.created_at, d.sent_at
		 FROM alarm_email_deliveries d JOIN users u ON u.id = d.user_id
		 WHERE d.alarm_id = ? ORDER BY u.username`, alarmID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list alarm email deliveries: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]AlarmEmailDeliveryStatus, 0)
	for rows.Next() {
		var item AlarmEmailDeliveryStatus
		var available, created, sent int64
		if err := rows.Scan(&item.Username, &item.Recipient, &item.Status, &item.Attempts,
			&available, &item.LastError, &created, &sent); err != nil {
			return nil, fmt.Errorf("account store %s: scan alarm email delivery: %w", s.path, err)
		}
		item.AvailableAt = timeFromMillis(available)
		item.CreatedAt = timeFromMillis(created)
		if sent != 0 {
			item.SentAt = timeFromMillis(sent)
		}
		out = append(out, item)
	}
	return out, rows.Err()
}
