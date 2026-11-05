package account

import (
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"
)

// LoginOIDC signs in the account linked to an identity provider subject and
// issues the same access/refresh pair as a password login. A first-time
// subject is linked to a new account when autoCreate is set; the account gets
// a random password that is never revealed, so only the provider can sign it
// in. Local two-factor codes are not required because the provider performed
// the authentication; a banned account is still refused.
func (s *Store) LoginOIDC(
	issuer, subject, usernameHint, email string,
	emailVerified bool,
	deviceID, deviceName string,
	autoCreate bool,
) (User, string, time.Time, string, time.Time, error) {
	issuer = strings.TrimSpace(issuer)
	subject = strings.TrimSpace(subject)
	usernameHint = strings.TrimSpace(usernameHint)
	email = strings.TrimSpace(email)
	if issuer == "" || subject == "" {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidOIDCIdentity
	}

	s.mu.Lock()
	user, found, err := s.oidcUserLocked(issuer, subject)
	s.mu.Unlock()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	if found {
		if user.Banned {
			return User{}, "", time.Time{}, "", time.Time{}, ErrAccountBanned
		}
		s.mu.Lock()
		defer s.mu.Unlock()
		return s.issueOIDCSessionLocked(user, deviceID, deviceName)
	}
	if !autoCreate {
		return User{}, "", time.Time{}, "", time.Time{}, ErrOIDCAccountNotFound
	}

	// Hashing dominates the cost, so it happens outside the store lock. The
	// link is checked again below in case a concurrent callback won.
	hash, err := hashPassword(randomOIDCPassword())
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("hash OIDC password: %w", err)
	}
	if !emailVerified || validateEmail(email) != nil {
		email = ""
	}
	base := oidcUsername(usernameHint, email, subject)

	s.mu.Lock()
	defer s.mu.Unlock()
	if user, found, err = s.oidcUserLocked(issuer, subject); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	} else if found {
		if user.Banned {
			return User{}, "", time.Time{}, "", time.Time{}, ErrAccountBanned
		}
		return s.issueOIDCSessionLocked(user, deviceID, deviceName)
	}
	username, err := s.availableOIDCUsernameLocked(base)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	role := RoleUser
	var existing int
	if err := s.db.QueryRow(`SELECT COUNT(*) FROM users`).Scan(&existing); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: count users: %w", s.path, err)
	}
	if existing == 0 {
		role = RoleAdmin
	}
	id, err := newRandomHex(16)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: begin OIDC account: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`INSERT INTO users (id, username, password_hash, created_at, role, email, phone)
		 VALUES (?, ?, ?, ?, ?, ?, '')`,
		id, username, hash, unixMillis(now), role, email,
	); err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			if strings.Contains(err.Error(), "email") {
				return User{}, "", time.Time{}, "", time.Time{}, ErrEmailTaken
			}
			return User{}, "", time.Time{}, "", time.Time{}, ErrUserExists
		}
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: create OIDC account: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO oidc_identities (issuer, subject, user_id, created_at) VALUES (?, ?, ?, ?)`,
		issuer, subject, id, unixMillis(now),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: link OIDC identity: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: commit OIDC account: %w", s.path, err)
	}
	user = User{ID: id, Username: username, Role: role, CreatedAt: now, Email: email}
	return s.issueOIDCSessionLocked(user, deviceID, deviceName)
}

func (s *Store) oidcUserLocked(issuer, subject string) (User, bool, error) {
	var (
		user    User
		created int64
	)
	err := s.db.QueryRow(
		`SELECT u.id, u.username, u.role, u.created_at, u.banned, u.email, u.phone
		 FROM oidc_identities i JOIN users u ON u.id = i.user_id
		 WHERE i.issuer = ? AND i.subject = ?`,
		issuer, subject,
	).Scan(&user.ID, &user.Username, &user.Role, &created, &user.Banned, &user.Email, &user.Phone)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, false, nil
	}
	if err != nil {
		return User{}, false, fmt.Errorf("account store %s: look up OIDC identity: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	return user, true, nil
}

// issueOIDCSessionLocked writes one access/refresh pair. It mirrors the token
// half of LoginWithRefreshFactors without touching two-factor state.
func (s *Store) issueOIDCSessionLocked(user User, deviceID, deviceName string) (User, string, time.Time, string, time.Time, error) {
	now := s.now().UTC()
	token, err := newToken()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	refresh, err := newToken()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	sessionID, err := newRandomHex(sessionIDBytes)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	deviceID = normalizeText(deviceID, maxDeviceID)
	deviceName = normalizeText(deviceName, maxDeviceName)
	accessExpires := now.Add(s.accessTTL)
	sessionExpires := now.Add(s.tokenTTL)
	if _, err := s.db.Exec(`DELETE FROM user_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: prune tokens: %w", s.path, err)
	}
	if _, err := s.db.Exec(`DELETE FROM refresh_tokens WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: prune refresh tokens: %w", s.path, err)
	}
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: begin OIDC session: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`INSERT INTO user_tokens
			(token_hash, user_id, created_at, expires_at, access_expires_at,
			 session_id, device_id, device_name)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		tokenHash(token), user.ID, unixMillis(now), unixMillis(sessionExpires),
		unixMillis(accessExpires), sessionID, deviceID, deviceName,
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: save OIDC token: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO refresh_tokens
			(token_hash, user_id, session_id, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?)`,
		tokenHash(refresh), user.ID, sessionID, unixMillis(now), unixMillis(sessionExpires),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: save OIDC refresh token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: commit OIDC session: %w", s.path, err)
	}
	return user, token, accessExpires, refresh, sessionExpires, nil
}

func (s *Store) availableOIDCUsernameLocked(base string) (string, error) {
	for attempt := 0; attempt < 50; attempt++ {
		candidate := base
		if attempt > 0 {
			suffix := fmt.Sprintf("-%d", attempt+1)
			candidate = truncateUsername(base, 64-len(suffix)) + suffix
		}
		var exists int
		err := s.db.QueryRow(`SELECT 1 FROM users WHERE username = ?`, candidate).Scan(&exists)
		if errors.Is(err, sql.ErrNoRows) {
			return candidate, nil
		}
		if err != nil {
			return "", fmt.Errorf("account store %s: check OIDC username: %w", s.path, err)
		}
	}
	return "", ErrUserExists
}

func truncateUsername(name string, limit int) string {
	if limit < 1 {
		limit = 1
	}
	if len(name) <= limit {
		return name
	}
	return name[:limit]
}

// oidcUsername derives a local name from the provider's preferred username,
// the email local part or a subject digest, in that order. Characters the
// account rules do not allow become '-', and a suffix is added by the caller
// when the name is taken.
func oidcUsername(hint, email, subject string) string {
	name := strings.TrimSpace(hint)
	if name == "" && email != "" {
		if at := strings.IndexByte(email, '@'); at > 0 {
			name = email[:at]
		}
	}
	if name == "" {
		digest := sha256.Sum256([]byte(subject))
		name = "oidc-" + hex.EncodeToString(digest[:6])
	}
	var cleaned strings.Builder
	for _, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9',
			r == '_', r == '.', r == '@', r == '-':
			cleaned.WriteRune(r)
		default:
			cleaned.WriteByte('-')
		}
	}
	result := cleaned.String()
	for len(result) > 0 && !isUsernameStart(result[0]) {
		result = result[1:]
	}
	if len(result) > 64 {
		result = result[:64]
	}
	if result == "" {
		return "oidc-user"
	}
	return result
}

func isUsernameStart(b byte) bool {
	return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')
}

func randomOIDCPassword() string {
	buffer := make([]byte, 32)
	if _, err := rand.Read(buffer); err != nil {
		// crypto/rand failing means the process cannot mint tokens either;
		// hashPassword still has to receive something, so use a fixed value
		// that no one can guess from outside and let the session fail later.
		return "oidc-unusable-password-" + time.Now().UTC().Format(time.RFC3339Nano)
	}
	return base64.RawURLEncoding.EncodeToString(buffer)
}
