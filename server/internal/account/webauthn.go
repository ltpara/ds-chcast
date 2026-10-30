package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// WebAuthn challenge purposes stored in webauthn_challenges.purpose.
const (
	WebAuthnRegister = "register"
	WebAuthnLogin    = "login"
	// WebAuthnLoginPasswordless is a login challenge issued without a password
	// check. It is a separate purpose so the two login ceremonies can be
	// pending at the same time without replacing each other.
	WebAuthnLoginPasswordless = "login-passwordless"
)

const maxWebAuthnName = 64

var (
	ErrWebAuthnNotEnabled       = errors.New("account has no security keys")
	ErrWebAuthnChallenge        = errors.New("security key challenge is missing or expired")
	ErrWebAuthnCredential       = errors.New("security key is not registered")
	ErrWebAuthnCredentialExists = errors.New("security key is already registered")
)

// WebAuthnCredential is one registered security key. ID is the credential ID
// chosen by the authenticator and PublicKey is the COSE_Key it attests.
type WebAuthnCredential struct {
	ID         []byte    `json:"-"`
	UserID     string    `json:"-"`
	PublicKey  []byte    `json:"-"`
	Alg        int       `json:"alg"`
	SignCount  uint32    `json:"-"`
	Name       string    `json:"name"`
	CreatedAt  time.Time `json:"created_at"`
	LastUsedAt time.Time `json:"last_used_at,omitempty"`
}

func validWebAuthnPurpose(purpose string) bool {
	return purpose == WebAuthnRegister || validLoginPurpose(purpose)
}

func validLoginPurpose(purpose string) bool {
	return purpose == WebAuthnLogin || purpose == WebAuthnLoginPasswordless
}

// IssueWebAuthnChallenge stores the single active challenge for a user and
// purpose, replacing any earlier one. The caller holds an authenticated
// session (registration) or just verified the password (login).
func (s *Store) IssueWebAuthnChallenge(userID, purpose string, challenge []byte, ttl time.Duration) (time.Time, error) {
	if !validWebAuthnPurpose(purpose) {
		return time.Time{}, errors.New("invalid security key challenge purpose")
	}
	if len(challenge) == 0 {
		return time.Time{}, errors.New("security key challenge is empty")
	}
	if ttl <= 0 {
		return time.Time{}, errors.New("security key challenge lifetime must be positive")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	expires := now.Add(ttl)
	tx, err := s.db.Begin()
	if err != nil {
		return time.Time{}, fmt.Errorf("account store %s: begin security key challenge: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM webauthn_challenges WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return time.Time{}, fmt.Errorf("account store %s: prune security key challenges: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO webauthn_challenges (user_id, purpose, challenge, expires_at)
		 VALUES (?, ?, ?, ?)
		 ON CONFLICT(user_id, purpose) DO UPDATE SET challenge = excluded.challenge,
		   expires_at = excluded.expires_at`,
		userID, purpose, challenge, unixMillis(expires),
	); err != nil {
		return time.Time{}, fmt.Errorf("account store %s: save security key challenge: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return time.Time{}, fmt.Errorf("account store %s: commit security key challenge: %w", s.path, err)
	}
	return expires, nil
}

// WebAuthnChallenge returns the active challenge for a user and purpose.
// Reading does not spend it, so a failed verification can be retried with the
// same challenge until it expires.
func (s *Store) WebAuthnChallenge(userID, purpose string) ([]byte, error) {
	if !validWebAuthnPurpose(purpose) {
		return nil, errors.New("invalid security key challenge purpose")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		challenge []byte
		expires   int64
	)
	err := s.db.QueryRow(
		`SELECT challenge, expires_at FROM webauthn_challenges WHERE user_id = ? AND purpose = ?`,
		userID, purpose,
	).Scan(&challenge, &expires)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, ErrWebAuthnChallenge
	}
	if err != nil {
		return nil, fmt.Errorf("account store %s: read security key challenge: %w", s.path, err)
	}
	if expires <= unixMillis(s.now().UTC()) {
		if _, err := s.db.Exec(
			`DELETE FROM webauthn_challenges WHERE user_id = ? AND purpose = ?`, userID, purpose,
		); err != nil {
			return nil, fmt.Errorf("account store %s: retire expired security key challenge: %w", s.path, err)
		}
		return nil, ErrWebAuthnChallenge
	}
	return challenge, nil
}

// WebAuthnCredentials lists the caller's registered security keys, oldest
// first, so the account view and the login exclusion list share one source.
func (s *Store) WebAuthnCredentials(userID string) ([]WebAuthnCredential, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.webauthnCredentialsLocked(userID)
}

func (s *Store) webauthnCredentialsLocked(userID string) ([]WebAuthnCredential, error) {
	rows, err := s.db.Query(
		`SELECT credential_id, user_id, public_key, alg, sign_count, name, created_at, last_used_at
		 FROM webauthn_credentials WHERE user_id = ? ORDER BY created_at, name`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list security keys: %w", s.path, err)
	}
	defer rows.Close()
	var out []WebAuthnCredential
	for rows.Next() {
		var (
			cred    WebAuthnCredential
			created int64
			last    int64
		)
		if err := rows.Scan(&cred.ID, &cred.UserID, &cred.PublicKey, &cred.Alg, &cred.SignCount,
			&cred.Name, &created, &last); err != nil {
			return nil, err
		}
		cred.CreatedAt = timeFromMillis(created)
		cred.LastUsedAt = timeFromMillis(last)
		out = append(out, cred)
	}
	return out, rows.Err()
}

// BeginWebAuthnLogin verifies the password and, for an account that has at
// least one security key, stores a fresh assertion challenge. The password
// check precedes every account-state answer so the endpoint cannot reveal who
// registered a key, and a banned account stops after its password matched.
func (s *Store) BeginWebAuthnLogin(username, password string, challenge []byte, ttl time.Duration) (User, []WebAuthnCredential, error) {
	if len(challenge) == 0 {
		return User{}, nil, errors.New("security key challenge is empty")
	}
	if ttl <= 0 {
		return User{}, nil, errors.New("security key challenge lifetime must be positive")
	}
	username = strings.TrimSpace(username)
	s.mu.Lock()
	var (
		user    User
		hash    string
		created int64
		banned  bool
	)
	err := s.db.QueryRow(
		`SELECT id, username, role, password_hash, created_at, banned, email, phone
		 FROM users WHERE username = ?`,
		username,
	).Scan(&user.ID, &user.Username, &user.Role, &hash, &created, &banned, &user.Email, &user.Phone)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		verifyDummy(password)
		return User{}, nil, ErrInvalidCredentials
	}
	if err != nil {
		return User{}, nil, fmt.Errorf("account store %s: security key login lookup: %w", s.path, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return User{}, nil, ErrInvalidCredentials
	}
	if banned {
		return User{}, nil, ErrAccountBanned
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	s.mu.Lock()
	creds, err := s.webauthnCredentialsLocked(user.ID)
	s.mu.Unlock()
	if err != nil {
		return User{}, nil, err
	}
	if len(creds) == 0 {
		return User{}, nil, ErrWebAuthnNotEnabled
	}
	if _, err := s.IssueWebAuthnChallenge(user.ID, WebAuthnLogin, challenge, ttl); err != nil {
		return User{}, nil, err
	}
	return user, creds, nil
}

// BeginWebAuthnPasswordlessLogin stores an assertion challenge for an account
// with at least one registered key. There is no password to check first, so
// unknown accounts and accounts without keys return distinct errors only for
// the caller to answer them identically. A banned account still receives a
// challenge: only the holder of its key can finish, and CompleteWebAuthnLogin
// then rejects the session, so ban state is not exposed to unauthenticated
// probes.
func (s *Store) BeginWebAuthnPasswordlessLogin(username string, challenge []byte, ttl time.Duration) (User, []WebAuthnCredential, error) {
	if len(challenge) == 0 {
		return User{}, nil, errors.New("security key challenge is empty")
	}
	if ttl <= 0 {
		return User{}, nil, errors.New("security key challenge lifetime must be positive")
	}
	username = strings.TrimSpace(username)
	s.mu.Lock()
	var (
		user    User
		created int64
		banned  bool
	)
	err := s.db.QueryRow(
		`SELECT id, username, role, created_at, banned, email, phone
		 FROM users WHERE username = ?`,
		username,
	).Scan(&user.ID, &user.Username, &user.Role, &created, &banned, &user.Email, &user.Phone)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, nil, ErrInvalidCredentials
	}
	if err != nil {
		return User{}, nil, fmt.Errorf("account store %s: passwordless security key lookup: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	s.mu.Lock()
	creds, err := s.webauthnCredentialsLocked(user.ID)
	s.mu.Unlock()
	if err != nil {
		return User{}, nil, err
	}
	if len(creds) == 0 {
		return User{}, nil, ErrWebAuthnNotEnabled
	}
	if _, err := s.IssueWebAuthnChallenge(user.ID, WebAuthnLoginPasswordless, challenge, ttl); err != nil {
		return User{}, nil, err
	}
	return user, creds, nil
}

// WebAuthnCredentialForUser scopes a credential lookup to one account, so an
// assertion cannot name a key registered by somebody else.
func (s *Store) WebAuthnCredentialForUser(userID string, credentialID []byte) (WebAuthnCredential, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		cred    WebAuthnCredential
		created int64
		last    int64
	)
	err := s.db.QueryRow(
		`SELECT credential_id, user_id, public_key, alg, sign_count, name, created_at, last_used_at
		 FROM webauthn_credentials WHERE user_id = ? AND credential_id = ?`,
		userID, credentialID,
	).Scan(&cred.ID, &cred.UserID, &cred.PublicKey, &cred.Alg, &cred.SignCount,
		&cred.Name, &created, &last)
	if errors.Is(err, sql.ErrNoRows) {
		return WebAuthnCredential{}, ErrWebAuthnCredential
	}
	if err != nil {
		return WebAuthnCredential{}, fmt.Errorf("account store %s: read security key: %w", s.path, err)
	}
	cred.CreatedAt = timeFromMillis(created)
	cred.LastUsedAt = timeFromMillis(last)
	return cred, nil
}

// FinishWebAuthnRegistration stores a verified credential. The pending
// registration challenge is spent in the same transaction, so one browser
// ceremony cannot enroll two keys, and an unknown or expired challenge is
// rejected before any key is saved.
func (s *Store) FinishWebAuthnRegistration(userID string, expectedChallenge []byte, cred WebAuthnCredential, name string) (WebAuthnCredential, error) {
	name = strings.TrimSpace(name)
	if name == "" {
		name = "Security key"
	}
	if len(name) > maxWebAuthnName {
		name = name[:maxWebAuthnName]
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return WebAuthnCredential{}, fmt.Errorf("account store %s: begin security key registration: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(
		`DELETE FROM webauthn_challenges
		 WHERE user_id = ? AND purpose = ? AND challenge = ? AND expires_at > ?`,
		userID, WebAuthnRegister, expectedChallenge, unixMillis(now),
	)
	if err != nil {
		return WebAuthnCredential{}, fmt.Errorf("account store %s: spend security key challenge: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return WebAuthnCredential{}, fmt.Errorf("account store %s: spend security key challenge: %w", s.path, err)
	} else if rows == 0 {
		return WebAuthnCredential{}, ErrWebAuthnChallenge
	}
	created := unixMillis(now)
	if _, err := tx.Exec(
		`INSERT INTO webauthn_credentials
			(credential_id, user_id, public_key, alg, sign_count, name, created_at, last_used_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?, 0)`,
		cred.ID, userID, cred.PublicKey, cred.Alg, cred.SignCount, name, created,
	); err != nil {
		if strings.Contains(err.Error(), "UNIQUE") || strings.Contains(err.Error(), "PRIMARY KEY") {
			return WebAuthnCredential{}, ErrWebAuthnCredentialExists
		}
		return WebAuthnCredential{}, fmt.Errorf("account store %s: save security key: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return WebAuthnCredential{}, fmt.Errorf("account store %s: commit security key registration: %w", s.path, err)
	}
	return WebAuthnCredential{
		ID:         cred.ID,
		UserID:     userID,
		PublicKey:  cred.PublicKey,
		Alg:        cred.Alg,
		SignCount:  cred.SignCount,
		Name:       name,
		CreatedAt:  timeFromMillis(created),
		LastUsedAt: time.Time{},
	}, nil
}

// RemoveWebAuthnCredential deletes one of the caller's keys. The password is
// required so a stolen bearer token alone cannot strip the factor, and any
// pending login challenge is dropped because it may have been issued for the
// removed key.
func (s *Store) RemoveWebAuthnCredential(userID string, credentialID []byte, password string) error {
	s.mu.Lock()
	var hash string
	err := s.db.QueryRow(`SELECT password_hash FROM users WHERE id = ?`, userID).Scan(&hash)
	s.mu.Unlock()
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownUser
	}
	if err != nil {
		return fmt.Errorf("account store %s: check password: %w", s.path, err)
	}
	ok, err := verifyPassword(hash, password)
	if err != nil || !ok {
		return ErrInvalidCredentials
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin remove security key: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(
		`DELETE FROM webauthn_credentials WHERE user_id = ? AND credential_id = ?`,
		userID, credentialID,
	)
	if err != nil {
		return fmt.Errorf("account store %s: remove security key: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return fmt.Errorf("account store %s: remove security key: %w", s.path, err)
	} else if rows == 0 {
		return ErrWebAuthnCredential
	}
	if _, err := tx.Exec(
		`DELETE FROM webauthn_challenges WHERE user_id = ? AND purpose IN (?, ?)`,
		userID, WebAuthnLogin, WebAuthnLoginPasswordless,
	); err != nil {
		return fmt.Errorf("account store %s: drop security key challenge: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit remove security key: %w", s.path, err)
	}
	return nil
}

// CompleteWebAuthnLogin spends the assertion challenge, records the new
// signature counter and issues the same access and refresh token pair as a
// password login, all in one transaction. The caller verifies the assertion
// first; a second concurrent finish finds the challenge gone and fails.
// purpose selects which login ceremony the challenge belongs to.
func (s *Store) CompleteWebAuthnLogin(username, purpose string, credentialID, expectedChallenge []byte, signCount uint32, deviceID, deviceName string) (User, string, time.Time, string, time.Time, error) {
	if !validLoginPurpose(purpose) {
		return User{}, "", time.Time{}, "", time.Time{}, errors.New("invalid security key login purpose")
	}
	username = strings.TrimSpace(username)
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		user    User
		created int64
		banned  bool
	)
	err := s.db.QueryRow(
		`SELECT id, username, role, created_at, banned, email, phone
		 FROM users WHERE username = ?`,
		username,
	).Scan(&user.ID, &user.Username, &user.Role, &created, &banned, &user.Email, &user.Phone)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidCredentials
	}
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: security key login: %w", s.path, err)
	}
	if banned {
		return User{}, "", time.Time{}, "", time.Time{}, ErrAccountBanned
	}
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
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: begin security key login: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(
		`DELETE FROM webauthn_challenges
		 WHERE user_id = ? AND purpose = ? AND challenge = ? AND expires_at > ?`,
		user.ID, purpose, expectedChallenge, unixMillis(now),
	)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: spend security key challenge: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: spend security key challenge: %w", s.path, err)
	} else if rows == 0 {
		return User{}, "", time.Time{}, "", time.Time{}, ErrWebAuthnChallenge
	}
	res, err = tx.Exec(
		`UPDATE webauthn_credentials SET sign_count = ?, last_used_at = ?
		 WHERE user_id = ? AND credential_id = ?`,
		signCount, unixMillis(now), user.ID, credentialID,
	)
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: update security key: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: update security key: %w", s.path, err)
	} else if rows == 0 {
		return User{}, "", time.Time{}, "", time.Time{}, ErrWebAuthnCredential
	}
	if _, err := tx.Exec(
		`INSERT INTO user_tokens
			(token_hash, user_id, created_at, expires_at, access_expires_at,
			 session_id, device_id, device_name)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		tokenHash(token), user.ID, unixMillis(now), unixMillis(sessionExpires),
		unixMillis(accessExpires), sessionID, deviceID, deviceName,
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: save token: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO refresh_tokens
			(token_hash, user_id, session_id, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?)`,
		tokenHash(refresh), user.ID, sessionID, unixMillis(now), unixMillis(sessionExpires),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: save refresh token: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: commit security key login: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	return user, token, accessExpires, refresh, sessionExpires, nil
}
