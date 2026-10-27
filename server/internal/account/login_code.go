package account

import (
	"crypto/subtle"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

const loginCodeTries = 5

// loginCodeChannel describes one second-factor delivery channel: the table
// holding its single pending code, the hash domain separator, and the user
// field the code is sent to. Table and seed are code constants, never input.
type loginCodeChannel struct {
	name    string
	table   string
	seed    string
	target  func(User) string
	missing error
}

var (
	loginSMSChannel = loginCodeChannel{
		name: "login SMS", table: "login_sms_codes", seed: "login-sms:",
		target: func(u User) string { return u.Phone }, missing: ErrNoRecoveryPhone,
	}
	loginEmailChannel = loginCodeChannel{
		name: "login email", table: "login_email_codes", seed: "login-email:",
		target: func(u User) string { return u.Email }, missing: ErrNoRecoveryEmail,
	}
)

func (ch loginCodeChannel) hash(code string) string {
	return tokenHash(ch.seed + code)
}

// createLoginCode verifies the password and mints a single-use login code for
// an account that enabled two-factor authentication and has a destination for
// the channel. The password check precedes every account-state answer, so the
// endpoint cannot reveal who enabled 2FA or which contact details are set. A
// new code replaces any earlier one.
func (s *Store) createLoginCode(username, password string, ttl time.Duration, ch loginCodeChannel) (string, User, time.Time, error) {
	username = strings.TrimSpace(username)
	if ttl <= 0 {
		return "", User{}, time.Time{}, errors.New("login code lifetime must be positive")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		user        User
		hash        string
		created     int64
		banned      bool
		totpEnabled bool
	)
	err := s.db.QueryRow(
		`SELECT id, username, role, password_hash, created_at, banned, email, phone, totp_enabled
		 FROM users WHERE username = ?`,
		username,
	).Scan(&user.ID, &user.Username, &user.Role, &hash, &created, &banned,
		&user.Email, &user.Phone, &totpEnabled)
	if errors.Is(err, sql.ErrNoRows) {
		verifyDummy(password)
		return "", User{}, time.Time{}, ErrInvalidCredentials
	}
	if err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: %s lookup: %w", s.path, ch.name, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return "", User{}, time.Time{}, ErrInvalidCredentials
	}
	if banned {
		return "", User{}, time.Time{}, ErrAccountBanned
	}
	if !totpEnabled {
		return "", User{}, time.Time{}, ErrTwoFactorNotEnabled
	}
	if ch.target(user) == "" {
		return "", User{}, time.Time{}, ch.missing
	}
	code, err := newPhoneCode()
	if err != nil {
		return "", User{}, time.Time{}, err
	}
	now := s.now().UTC()
	expires := now.Add(ttl)
	tx, err := s.db.Begin()
	if err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: begin %s: %w", s.path, ch.name, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		fmt.Sprintf(`DELETE FROM %s WHERE expires_at <= ?`, ch.table), unixMillis(now),
	); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: prune %s codes: %w", s.path, ch.name, err)
	}
	if _, err := tx.Exec(
		fmt.Sprintf(`DELETE FROM %s WHERE user_id = ?`, ch.table), user.ID,
	); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: replace %s code: %w", s.path, ch.name, err)
	}
	if _, err := tx.Exec(
		fmt.Sprintf(`INSERT INTO %s (user_id, code_hash, created_at, expires_at, attempts) VALUES (?, ?, ?, ?, 0)`, ch.table),
		user.ID, ch.hash(code), unixMillis(now), unixMillis(expires),
	); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: create %s code: %w", s.path, ch.name, err)
	}
	if err := tx.Commit(); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: commit %s: %w", s.path, ch.name, err)
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	return code, user, expires, nil
}

// matchLoginCodeLocked checks a login code without spending it. Wrong codes
// are counted and the fifth wrong attempt retires the code. The caller holds
// s.mu and deletes the row in the same transaction that issues the token, so
// two concurrent logins cannot both spend one code.
func (s *Store) matchLoginCodeLocked(userID, code string, now time.Time, ch loginCodeChannel) (bool, error) {
	normalized, ok := normalizePhoneCode(code)
	if !ok {
		return false, nil
	}
	var (
		stored   string
		expires  int64
		attempts int64
	)
	err := s.db.QueryRow(
		fmt.Sprintf(`SELECT code_hash, expires_at, attempts FROM %s WHERE user_id = ?`, ch.table),
		userID,
	).Scan(&stored, &expires, &attempts)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read %s code: %w", s.path, ch.name, err)
	}
	if expires <= unixMillis(now) || attempts >= loginCodeTries {
		if _, err := s.db.Exec(
			fmt.Sprintf(`DELETE FROM %s WHERE user_id = ?`, ch.table), userID,
		); err != nil {
			return false, fmt.Errorf("account store %s: retire expired %s code: %w", s.path, ch.name, err)
		}
		return false, nil
	}
	if subtle.ConstantTimeCompare([]byte(stored), []byte(ch.hash(normalized))) != 1 {
		var err error
		if attempts+1 >= loginCodeTries {
			_, err = s.db.Exec(fmt.Sprintf(`DELETE FROM %s WHERE user_id = ?`, ch.table), userID)
		} else {
			_, err = s.db.Exec(
				fmt.Sprintf(`UPDATE %s SET attempts = ? WHERE user_id = ?`, ch.table), attempts+1, userID,
			)
		}
		if err != nil {
			return false, fmt.Errorf("account store %s: count %s attempt: %w", s.path, ch.name, err)
		}
		return false, nil
	}
	return true, nil
}
