package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

type AccountInvitation struct {
	ID               int64
	Email            string
	InvitedBy        string
	Status           string
	CreatedAt        time.Time
	ExpiresAt        time.Time
	AcceptedAt       time.Time
	AcceptedUsername string
}

type IssuedAccountInvitation struct {
	Invitation AccountInvitation
	Token      string
}

// CreateAccountInvitation issues a one-time invitation for a new viewer
// account. The plaintext token is returned once and only its hash is stored.
func (s *Store) CreateAccountInvitation(email, invitedBy string, ttl time.Duration) (IssuedAccountInvitation, error) {
	if ttl <= 0 {
		return IssuedAccountInvitation{}, errors.New("account invitation lifetime must be positive")
	}
	email = strings.TrimSpace(email)
	if email == "" {
		return IssuedAccountInvitation{}, ErrInvalidEmail
	}
	if err := validateEmail(email); err != nil {
		return IssuedAccountInvitation{}, err
	}
	token, err := newToken()
	if err != nil {
		return IssuedAccountInvitation{}, fmt.Errorf("create account invitation token: %w", err)
	}
	now := s.now().UTC()
	expires := now.Add(ttl)
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return IssuedAccountInvitation{}, fmt.Errorf("account store %s: begin invitation: %w", s.path, err)
	}
	defer tx.Rollback()
	var exists int
	err = tx.QueryRow(`SELECT 1 FROM users WHERE lower(email) = lower(?) AND email <> ''`, email).Scan(&exists)
	if err == nil {
		return IssuedAccountInvitation{}, ErrEmailTaken
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return IssuedAccountInvitation{}, fmt.Errorf("account store %s: check invitation email: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`UPDATE account_invitations SET status = 'cancelled'
		 WHERE lower(email) = lower(?) AND status = 'pending'`, email,
	); err != nil {
		return IssuedAccountInvitation{}, fmt.Errorf("account store %s: replace invitation: %w", s.path, err)
	}
	createdAt, expiresAt := unixMillis(now), unixMillis(expires)
	var id int64
	err = tx.QueryRow(
		`INSERT INTO account_invitations
		 (token_hash, email, invited_by, created_at, expires_at, status)
		 VALUES (?, ?, ?, ?, ?, 'pending') RETURNING id`,
		tokenHash(token), email, strings.TrimSpace(invitedBy), createdAt, expiresAt,
	).Scan(&id)
	if err != nil {
		return IssuedAccountInvitation{}, fmt.Errorf("account store %s: create invitation: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return IssuedAccountInvitation{}, fmt.Errorf("account store %s: commit invitation: %w", s.path, err)
	}
	return IssuedAccountInvitation{
		Invitation: AccountInvitation{
			ID: id, Email: email, InvitedBy: strings.TrimSpace(invitedBy),
			Status: "pending", CreatedAt: now, ExpiresAt: expires,
		},
		Token: token,
	}, nil
}

func (s *Store) ListAccountInvitations(limit int) ([]AccountInvitation, error) {
	if limit <= 0 || limit > 200 {
		limit = 200
	}
	now := unixMillis(s.now().UTC())
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT id, email, invited_by,
		 CASE WHEN status = 'pending' AND expires_at <= ? THEN 'expired' ELSE status END,
		 created_at, expires_at, accepted_at, accepted_username
		 FROM account_invitations ORDER BY id DESC LIMIT ?`, now, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list invitations: %w", s.path, err)
	}
	defer rows.Close()
	invitations := make([]AccountInvitation, 0)
	for rows.Next() {
		var invitation AccountInvitation
		var createdAt, expiresAt, acceptedAt int64
		if err := rows.Scan(&invitation.ID, &invitation.Email, &invitation.InvitedBy,
			&invitation.Status, &createdAt, &expiresAt, &acceptedAt,
			&invitation.AcceptedUsername); err != nil {
			return nil, fmt.Errorf("account store %s: scan invitation: %w", s.path, err)
		}
		invitation.CreatedAt = timeFromMillis(createdAt)
		invitation.ExpiresAt = timeFromMillis(expiresAt)
		if acceptedAt != 0 {
			invitation.AcceptedAt = timeFromMillis(acceptedAt)
		}
		invitations = append(invitations, invitation)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: read invitations: %w", s.path, err)
	}
	return invitations, nil
}

func (s *Store) CancelAccountInvitation(id int64) error {
	if id <= 0 {
		return ErrInvalidAccountInvitation
	}
	now := unixMillis(s.now().UTC())
	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`UPDATE account_invitations SET status = 'cancelled'
		 WHERE id = ? AND status = 'pending' AND expires_at > ?`, id, now,
	)
	if err != nil {
		return fmt.Errorf("account store %s: cancel invitation: %w", s.path, err)
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("account store %s: check cancelled invitation: %w", s.path, err)
	}
	if affected != 1 {
		return ErrInvalidAccountInvitation
	}
	return nil
}

// AcceptAccountInvitation creates an ordinary viewer account and spends the
// invite in the same transaction. The invited email is verified because the
// recipient had to present the secret delivered to that address.
func (s *Store) AcceptAccountInvitation(token, username, password string) (User, error) {
	token = strings.TrimSpace(token)
	username = strings.TrimSpace(username)
	if token == "" {
		return User{}, ErrInvalidAccountInvitation
	}
	if !usernamePattern.MatchString(username) {
		return User{}, ErrInvalidUsername
	}
	if err := validatePassword(password); err != nil {
		return User{}, err
	}
	hash, err := hashPassword(password)
	if err != nil {
		return User{}, fmt.Errorf("hash invited account password: %w", err)
	}
	id, err := newRandomHex(16)
	if err != nil {
		return User{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	nowMS := unixMillis(now)
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, fmt.Errorf("account store %s: begin invitation acceptance: %w", s.path, err)
	}
	defer tx.Rollback()
	var invitationID int64
	var email string
	err = tx.QueryRow(
		`SELECT id, email FROM account_invitations
		 WHERE token_hash = ? AND status = 'pending' AND expires_at > ?`,
		tokenHash(token), nowMS,
	).Scan(&invitationID, &email)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrInvalidAccountInvitation
	}
	if err != nil {
		return User{}, fmt.Errorf("account store %s: read invitation: %w", s.path, err)
	}
	result, err := tx.Exec(
		`UPDATE account_invitations SET status = 'accepted', accepted_at = ?, accepted_username = ?
		 WHERE id = ? AND status = 'pending' AND expires_at > ?`,
		nowMS, username, invitationID, nowMS,
	)
	if err != nil {
		return User{}, fmt.Errorf("account store %s: spend invitation: %w", s.path, err)
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return User{}, fmt.Errorf("account store %s: check spent invitation: %w", s.path, err)
	}
	if affected != 1 {
		return User{}, ErrInvalidAccountInvitation
	}
	if _, err := tx.Exec(
		`INSERT INTO users (id, username, password_hash, created_at, role, email, phone, email_verified_at)
		 VALUES (?, ?, ?, ?, ?, ?, '', ?)`,
		id, username, hash, nowMS, RoleUser, email, nowMS,
	); err != nil {
		if isUniqueViolation(err) {
			if strings.Contains(strings.ToLower(err.Error()), "email") {
				return User{}, ErrEmailTaken
			}
			return User{}, ErrUserExists
		}
		return User{}, fmt.Errorf("account store %s: create invited account: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, fmt.Errorf("account store %s: commit invitation acceptance: %w", s.path, err)
	}
	return User{ID: id, Username: username, Role: RoleUser, Email: email,
		CreatedAt: now}, nil
}
