package account

import (
	"database/sql"
	"errors"
	"fmt"
	"net/mail"
	"strings"
	"time"
)

// validateEmail accepts a bare RFC 5322 address. Display names, whitespace and
// addresses longer than 254 bytes are rejected so a stored address can be used
// verbatim in SMTP headers without rewriting or quoting it.
func validateEmail(email string) error {
	if email == "" {
		return nil
	}
	if len(email) > 254 || strings.ContainsAny(email, "\r\n") {
		return ErrInvalidEmail
	}
	addr, err := mail.ParseAddress(email)
	if err != nil || addr.Name != "" || addr.Address != email {
		return ErrInvalidEmail
	}
	return nil
}

func (s *Store) findUserLocked(username string) (User, error) {
	var (
		user    User
		created int64
		banned  bool
	)
	err := s.db.QueryRow(
		`SELECT id, username, role, created_at, banned, email, phone FROM users WHERE username = ?`,
		strings.TrimSpace(username),
	).Scan(&user.ID, &user.Username, &user.Role, &created, &banned, &user.Email, &user.Phone)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrUnknownUser
	}
	if err != nil {
		return User{}, fmt.Errorf("account store %s: find user: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	return user, nil
}

// SetEmail stores an optional recovery address; an empty value clears it. The
// unique index keeps one address from receiving resets for several accounts.
func (s *Store) SetEmail(username, email string) (User, error) {
	username = strings.TrimSpace(username)
	email = strings.TrimSpace(email)
	if err := validateEmail(email); err != nil {
		return User{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, fmt.Errorf("account store %s: begin email change: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(`UPDATE users SET email = ? WHERE username = ?`, email, username)
	if err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			return User{}, ErrEmailTaken
		}
		return User{}, fmt.Errorf("account store %s: set email: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return User{}, err
	}
	if affected == 0 {
		return User{}, ErrUnknownUser
	}
	// A pending login code was issued for the previous mailbox; changing the
	// address must not leave it usable.
	if _, err := tx.Exec(
		`DELETE FROM login_email_codes WHERE user_id = (SELECT id FROM users WHERE username = ?)`,
		username,
	); err != nil {
		return User{}, fmt.Errorf("account store %s: drop login email code: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, fmt.Errorf("account store %s: commit email change: %w", s.path, err)
	}
	return s.findUserLocked(username)
}

// CheckPassword verifies a password without issuing tokens, for sensitive
// account changes that already hold a valid session. An unknown username still
// pays for one password hash so timing does not reveal which names exist.
func (s *Store) CheckPassword(username, password string) error {
	username = strings.TrimSpace(username)
	s.mu.Lock()
	var hash string
	err := s.db.QueryRow(`SELECT password_hash FROM users WHERE username = ?`, username).Scan(&hash)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		verifyDummy(password)
		return ErrInvalidCredentials
	}
	if err != nil {
		return fmt.Errorf("account store %s: check password: %w", s.path, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return ErrInvalidCredentials
	}
	return nil
}

// CreatePasswordReset mints a single-use token for an account with a recovery
// address. Creating a new token revokes any earlier unused one, so a leaked
// older mail cannot be replayed. Only the SHA-256 hash is stored.
func (s *Store) CreatePasswordReset(username string, ttl time.Duration) (string, User, time.Time, error) {
	if ttl <= 0 {
		return "", User{}, time.Time{}, errors.New("password reset lifetime must be positive")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	user, err := s.findUserLocked(username)
	if err != nil {
		return "", User{}, time.Time{}, err
	}
	if user.Email == "" {
		return "", User{}, time.Time{}, ErrNoRecoveryEmail
	}
	token, err := newToken()
	if err != nil {
		return "", User{}, time.Time{}, err
	}
	now := s.now().UTC()
	expires := now.Add(ttl)
	tx, err := s.db.Begin()
	if err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: begin password reset: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM password_resets WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: prune password resets: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM password_resets WHERE user_id = ? AND used_at = 0`, user.ID); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: replace password reset: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO password_resets (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)`,
		tokenHash(token), user.ID, unixMillis(now), unixMillis(expires),
	); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: create password reset: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: commit password reset: %w", s.path, err)
	}
	return token, user, expires, nil
}

// ConsumePasswordReset spends a token and replaces the password in one
// transaction. Every access and refresh token is revoked, so a stolen session
// cannot survive the recovery. A weak password is rejected before the token is
// touched, letting the user retry with a better one.
func (s *Store) ConsumePasswordReset(token, password string) (User, error) {
	token = strings.TrimSpace(token)
	if token == "" {
		return User{}, ErrInvalidResetToken
	}
	if err := validatePassword(password); err != nil {
		return User{}, err
	}
	hash, err := hashPassword(password)
	if err != nil {
		return User{}, fmt.Errorf("hash password: %w", err)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, fmt.Errorf("account store %s: begin password reset use: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		user    User
		created int64
		banned  bool
	)
	err = tx.QueryRow(
		`SELECT u.id, u.username, u.role, u.created_at, u.banned, u.email, u.phone
		 FROM password_resets r JOIN users u ON u.id = r.user_id
		 WHERE r.token_hash = ? AND r.used_at = 0 AND r.expires_at > ?`,
		tokenHash(token), unixMillis(now),
	).Scan(&user.ID, &user.Username, &user.Role, &created, &banned, &user.Email, &user.Phone)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrInvalidResetToken
	}
	if err != nil {
		return User{}, fmt.Errorf("account store %s: look up password reset: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	if _, err := tx.Exec(`UPDATE users SET password_hash = ? WHERE id = ?`, hash, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: update password: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`UPDATE password_resets SET used_at = ? WHERE token_hash = ?`,
		unixMillis(now), tokenHash(token),
	); err != nil {
		return User{}, fmt.Errorf("account store %s: spend password reset: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`DELETE FROM password_resets WHERE user_id = ? AND token_hash <> ?`,
		user.ID, tokenHash(token),
	); err != nil {
		return User{}, fmt.Errorf("account store %s: revoke other resets: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM user_tokens WHERE user_id = ?`, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: revoke tokens: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM refresh_tokens WHERE user_id = ?`, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: revoke refresh tokens: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM login_sms_codes WHERE user_id = ?`, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: drop login SMS code: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM login_email_codes WHERE user_id = ?`, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: drop login email code: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, fmt.Errorf("account store %s: commit password reset use: %w", s.path, err)
	}
	return user, nil
}
