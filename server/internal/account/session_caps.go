package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
)

// SessionPermissionCap returns the account-wide session permission ceiling.
// Zero means the account is not capped. A device policy keeps its own ceiling;
// admission intersects the two so neither layer can widen the other.
func (s *Store) SessionPermissionCap(userID string) (uint32, error) {
	userID = strings.TrimSpace(userID)
	if userID == "" {
		return 0, ErrUnknownUser
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var ceiling int64
	err := s.db.QueryRow(
		`SELECT session_permission_ceiling FROM users WHERE id = ?`, userID,
	).Scan(&ceiling)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, ErrUnknownUser
	}
	if err != nil {
		return 0, fmt.Errorf("account store %s: read session permission cap: %w", s.path, err)
	}
	return uint32(ceiling), nil
}

// SessionPermissionCaps maps every capped username to its ceiling. Uncapped
// accounts are absent, which lets a console list them as unlimited without a
// second round trip per row.
func (s *Store) SessionPermissionCaps() (map[string]uint32, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT username, session_permission_ceiling FROM users WHERE session_permission_ceiling <> 0`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list session permission caps: %w", s.path, err)
	}
	defer rows.Close()
	caps := make(map[string]uint32)
	for rows.Next() {
		var (
			username string
			ceiling  int64
		)
		if err := rows.Scan(&username, &ceiling); err != nil {
			return nil, fmt.Errorf("account store %s: scan session permission cap: %w", s.path, err)
		}
		caps[username] = uint32(ceiling)
	}
	return caps, rows.Err()
}

// SetSessionPermissionCap stores the account-wide ceiling; zero removes it and
// returns the account to the device policy's own limit. Admission reads the
// value at the next ticket issuance, so an already-issued 60-second ticket is
// the longest a tightened cap can lag.
func (s *Store) SetSessionPermissionCap(username string, ceiling uint32) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(
		`UPDATE users SET session_permission_ceiling = ? WHERE username = ?`,
		int64(ceiling), strings.TrimSpace(username))
	if err != nil {
		return fmt.Errorf("account store %s: set session permission cap: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownUser
	}
	return nil
}

// accountPermissionCapLocked reads the account-wide ceiling for admission. A
// missing or blank subject has no cap; the caller's policy check decides whether
// that subject is allowed at all.
func (s *Store) accountPermissionCapLocked(userID string) (uint32, error) {
	if userID == "" {
		return 0, nil
	}
	var ceiling int64
	err := s.db.QueryRow(
		`SELECT session_permission_ceiling FROM users WHERE id = ?`, userID,
	).Scan(&ceiling)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, nil
	}
	if err != nil {
		return 0, fmt.Errorf("account store %s: read session permission cap: %w", s.path, err)
	}
	return uint32(ceiling), nil
}

// intersectPermissionCeilings combines the device and account masks, where zero
// means no restriction on that layer.
func intersectPermissionCeilings(device, account uint32) uint32 {
	switch {
	case device == 0:
		return account
	case account == 0:
		return device
	default:
		return device & account
	}
}
