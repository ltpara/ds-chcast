package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// EmailVerified reports whether the account currently owns its saved recovery
// address according to the local verification flow.
func (s *Store) EmailVerified(username string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var email string
	var verifiedAt int64
	err := s.db.QueryRow(
		`SELECT email, email_verified_at FROM users WHERE username = ?`, strings.TrimSpace(username),
	).Scan(&email, &verifiedAt)
	if errors.Is(err, sql.ErrNoRows) {
		return false, ErrUnknownUser
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: read email verification: %w", s.path, err)
	}
	return email != "" && verifiedAt > 0, nil
}

// CreateEmailVerification replaces outstanding verification tokens and
// returns a raw single-use token. Only its hash is stored.
func (s *Store) CreateEmailVerification(username string, ttl time.Duration) (string, User, time.Time, error) {
	if ttl <= 0 {
		return "", User{}, time.Time{}, errors.New("email verification lifetime must be positive")
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
	var verifiedAt int64
	if err := s.db.QueryRow(`SELECT email_verified_at FROM users WHERE id = ?`, user.ID).Scan(&verifiedAt); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: read email verification: %w", s.path, err)
	}
	if verifiedAt > 0 {
		return "", User{}, time.Time{}, ErrEmailAlreadyVerified
	}
	token, err := newToken()
	if err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("create email verification token: %w", err)
	}
	now := s.now().UTC()
	expires := now.Add(ttl)
	tx, err := s.db.Begin()
	if err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: begin email verification: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM email_verification_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: prune email verification tokens: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM email_verification_tokens WHERE user_id = ?`, user.ID); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: replace email verification token: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO email_verification_tokens (token_hash, user_id, email, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?)`,
		tokenHash(token), user.ID, user.Email, unixMillis(now), unixMillis(expires),
	); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: create email verification token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: commit email verification: %w", s.path, err)
	}
	return token, user, expires, nil
}

// ConsumeEmailVerification verifies the address only if it still matches the
// address for which the token was issued. The token and state change are spent
// together so it cannot be replayed.
func (s *Store) ConsumeEmailVerification(token, expectedUsername string) error {
	token = strings.TrimSpace(token)
	if token == "" {
		return ErrInvalidEmailVerificationToken
	}
	expectedUsername = strings.TrimSpace(expectedUsername)
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin email verification use: %w", s.path, err)
	}
	defer tx.Rollback()
	var userID, email string
	err = tx.QueryRow(
		`SELECT u.id, t.email
		 FROM email_verification_tokens t JOIN users u ON u.id = t.user_id
		 WHERE t.token_hash = ? AND t.expires_at > ? AND u.username = ?`,
		tokenHash(token), unixMillis(now), expectedUsername,
	).Scan(&userID, &email)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrInvalidEmailVerificationToken
	}
	if err != nil {
		return fmt.Errorf("account store %s: read email verification token: %w", s.path, err)
	}
	result, err := tx.Exec(
		`UPDATE users SET email_verified_at = ? WHERE id = ? AND email = ?`,
		unixMillis(now), userID, email,
	)
	if err != nil {
		return fmt.Errorf("account store %s: verify email address: %w", s.path, err)
	}
	updated, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("account store %s: verify email result: %w", s.path, err)
	}
	if updated != 1 {
		return ErrInvalidEmailVerificationToken
	}
	if _, err := tx.Exec(`DELETE FROM email_verification_tokens WHERE user_id = ?`, userID); err != nil {
		return fmt.Errorf("account store %s: spend email verification token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit email verification use: %w", s.path, err)
	}
	return nil
}
