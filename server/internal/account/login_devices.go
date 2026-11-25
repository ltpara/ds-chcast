package account

import (
	"crypto/ed25519"
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

const LoginDeviceChallengeTTL = 12 * time.Minute

var (
	ErrInvalidLoginDeviceProof = errors.New("login device proof was not accepted")
	ErrLoginDeviceNotAllowed   = errors.New("this device is not allowed to sign in to the account")
	ErrLoginDeviceProofNeeded  = errors.New("a device proof is required to manage the login device allowlist")
	ErrInvalidLoginDeviceID    = errors.New("login device id is invalid")
	ErrUnknownLoginDevice      = errors.New("trusted login device not found")
)

// LoginDeviceProof proves possession of the Ed25519 installation key used by
// the viewer. The challenge is created before password, OIDC or WebAuthn login
// and is consumed in the same transaction that issues the account session.
type LoginDeviceProof struct {
	ChallengeID string
	PublicKey   string
	Signature   string
}

// LoginDevice is one installation that has successfully signed in while the
// account allowlist was disabled, or was explicitly trusted by an operator.
type LoginDevice struct {
	ID         string    `json:"id"`
	DeviceName string    `json:"device_name"`
	CreatedAt  time.Time `json:"created_at"`
	LastSeenAt time.Time `json:"last_seen_at"`
}

type LoginDeviceState struct {
	Enabled bool          `json:"enabled"`
	Devices []LoginDevice `json:"devices"`
}

// CreateLoginDeviceChallenge creates a short-lived, one-use challenge for an
// installation key. Reissuing for the same key replaces its older challenge.
func (s *Store) CreateLoginDeviceChallenge(publicKeyText string) (string, string, error) {
	if _, err := decodeLoginDevicePublicKey(publicKeyText); err != nil {
		return "", "", err
	}
	challengeID, err := newRandomHex(24)
	if err != nil {
		return "", "", fmt.Errorf("create login device challenge id: %w", err)
	}
	random := make([]byte, 32)
	if _, err := rand.Read(random); err != nil {
		return "", "", fmt.Errorf("create login device challenge: %w", err)
	}
	message := []byte("chast-account-login-device-v1\x00")
	message = append(message, []byte(challengeID)...)
	message = append(message, 0)
	message = append(message, random...)
	messageText := base64.RawURLEncoding.EncodeToString(message)
	now := s.now().UTC()
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`DELETE FROM account_login_device_challenges
		 WHERE expires_at <= ? OR public_key = ?`,
		unixMillis(now), publicKeyText,
	); err != nil {
		return "", "", fmt.Errorf("account store %s: replace login device challenge: %w", s.path, err)
	}
	if _, err := s.db.Exec(
		`INSERT INTO account_login_device_challenges
			(challenge_id, public_key, message, created_at, expires_at)
		 VALUES (?, ?, ?, ?, ?)`,
		challengeID, publicKeyText, messageText, unixMillis(now),
		unixMillis(now.Add(LoginDeviceChallengeTTL)),
	); err != nil {
		return "", "", fmt.Errorf("account store %s: create login device challenge: %w", s.path, err)
	}
	return challengeID, messageText, nil
}

// loginDeviceProofTx consumes a signed challenge and returns the stable device
// identifier that will be attached to the new session. When the allowlist is
// off, a successful authenticated login remembers the installation; once it is
// on, only remembered installations can issue new sessions.
func (s *Store) loginDeviceProofTx(tx *sql.Tx, userID, displayName, fallbackID string,
	proof LoginDeviceProof, now time.Time) (string, error) {
	var enabled bool
	if err := tx.QueryRow(
		`SELECT login_device_allowlist_enabled FROM users WHERE id = ?`, userID,
	).Scan(&enabled); err != nil {
		return "", fmt.Errorf("account store %s: read login device policy: %w", s.path, err)
	}
	if strings.TrimSpace(proof.ChallengeID) == "" &&
		strings.TrimSpace(proof.PublicKey) == "" &&
		strings.TrimSpace(proof.Signature) == "" {
		if enabled {
			return "", ErrInvalidLoginDeviceProof
		}
		return normalizeText(fallbackID, maxDeviceID), nil
	}
	if strings.TrimSpace(proof.ChallengeID) == "" ||
		strings.TrimSpace(proof.PublicKey) == "" ||
		strings.TrimSpace(proof.Signature) == "" {
		return "", ErrInvalidLoginDeviceProof
	}
	publicKey, err := decodeLoginDevicePublicKey(proof.PublicKey)
	if err != nil {
		return "", ErrInvalidLoginDeviceProof
	}
	signature, err := base64.RawURLEncoding.Strict().DecodeString(strings.TrimSpace(proof.Signature))
	if err != nil || len(signature) != ed25519.SignatureSize {
		return "", ErrInvalidLoginDeviceProof
	}
	var messageText string
	err = tx.QueryRow(
		`SELECT message FROM account_login_device_challenges
		 WHERE challenge_id = ? AND public_key = ? AND expires_at > ?`,
		strings.TrimSpace(proof.ChallengeID), proof.PublicKey, unixMillis(now),
	).Scan(&messageText)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrInvalidLoginDeviceProof
	}
	if err != nil {
		return "", fmt.Errorf("account store %s: read login device challenge: %w", s.path, err)
	}
	message, err := base64.RawURLEncoding.Strict().DecodeString(messageText)
	if err != nil || !ed25519.Verify(publicKey, message, signature) {
		return "", ErrInvalidLoginDeviceProof
	}
	result, err := tx.Exec(
		`DELETE FROM account_login_device_challenges
		 WHERE challenge_id = ? AND public_key = ? AND expires_at > ?`,
		strings.TrimSpace(proof.ChallengeID), proof.PublicKey, unixMillis(now),
	)
	if err != nil {
		return "", fmt.Errorf("account store %s: consume login device challenge: %w", s.path, err)
	}
	used, err := result.RowsAffected()
	if err != nil {
		return "", fmt.Errorf("account store %s: count consumed login device challenge: %w", s.path, err)
	}
	if used != 1 {
		return "", ErrInvalidLoginDeviceProof
	}
	deviceID := loginDeviceID(publicKey)
	if enabled {
		var trusted int
		err := tx.QueryRow(
			`SELECT 1 FROM account_login_devices WHERE user_id = ? AND device_id = ? AND public_key = ?`,
			userID, deviceID, proof.PublicKey,
		).Scan(&trusted)
		if errors.Is(err, sql.ErrNoRows) {
			return "", ErrLoginDeviceNotAllowed
		}
		if err != nil {
			return "", fmt.Errorf("account store %s: check trusted login device: %w", s.path, err)
		}
		if _, err := tx.Exec(
			`UPDATE account_login_devices SET device_name = ?, last_seen_at = ?
			 WHERE user_id = ? AND device_id = ?`,
			normalizeText(displayName, maxDeviceName), unixMillis(now), userID, deviceID,
		); err != nil {
			return "", fmt.Errorf("account store %s: update trusted login device: %w", s.path, err)
		}
		return deviceID, nil
	}
	if _, err := tx.Exec(
		`INSERT INTO account_login_devices
			(user_id, device_id, public_key, device_name, created_at, last_seen_at)
		 VALUES (?, ?, ?, ?, ?, ?)
		 ON CONFLICT(user_id, device_id) DO UPDATE SET
			 public_key = excluded.public_key,
			 device_name = excluded.device_name,
			 last_seen_at = excluded.last_seen_at`,
		userID, deviceID, proof.PublicKey, normalizeText(displayName, maxDeviceName),
		unixMillis(now), unixMillis(now),
	); err != nil {
		return "", fmt.Errorf("account store %s: remember login device: %w", s.path, err)
	}
	return deviceID, nil
}

func decodeLoginDevicePublicKey(text string) ([]byte, error) {
	text = strings.TrimSpace(text)
	key, err := base64.RawURLEncoding.Strict().DecodeString(text)
	if err != nil || len(key) != ed25519.PublicKeySize ||
		base64.RawURLEncoding.EncodeToString(key) != text {
		return nil, ErrInvalidLoginDeviceProof
	}
	return key, nil
}

func loginDeviceID(publicKey []byte) string {
	digest := sha256.Sum256(publicKey)
	return hex.EncodeToString(digest[:])
}

// LoginDevices returns the current account policy and trusted installations.
func (s *Store) LoginDevices(userID string) (LoginDeviceState, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var state LoginDeviceState
	if err := s.db.QueryRow(
		`SELECT login_device_allowlist_enabled FROM users WHERE id = ?`, userID,
	).Scan(&state.Enabled); errors.Is(err, sql.ErrNoRows) {
		return state, ErrUnknownUser
	} else if err != nil {
		return state, fmt.Errorf("account store %s: read login device policy: %w", s.path, err)
	}
	rows, err := s.db.Query(
		`SELECT device_id, device_name, created_at, last_seen_at
		 FROM account_login_devices WHERE user_id = ? ORDER BY last_seen_at DESC, device_id`,
		userID,
	)
	if err != nil {
		return state, fmt.Errorf("account store %s: list login devices: %w", s.path, err)
	}
	defer rows.Close()
	state.Devices = make([]LoginDevice, 0)
	for rows.Next() {
		var device LoginDevice
		var createdAt, lastSeenAt int64
		if err := rows.Scan(&device.ID, &device.DeviceName, &createdAt, &lastSeenAt); err != nil {
			return LoginDeviceState{}, fmt.Errorf("account store %s: read login device: %w", s.path, err)
		}
		device.CreatedAt = timeFromMillis(createdAt)
		device.LastSeenAt = timeFromMillis(lastSeenAt)
		state.Devices = append(state.Devices, device)
	}
	if err := rows.Err(); err != nil {
		return LoginDeviceState{}, fmt.Errorf("account store %s: scan login devices: %w", s.path, err)
	}
	return state, nil
}

// LoginDeviceIDForToken returns the installation that owns one access token.
// Callers use it only after Authenticate, so a missing row indicates that the
// session was revoked between authentication and the account operation.
func (s *Store) LoginDeviceIDForToken(userID, token string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var deviceID string
	err := s.db.QueryRow(
		`SELECT device_id FROM user_tokens WHERE user_id = ? AND token_hash = ?`,
		userID, tokenHash(token),
	).Scan(&deviceID)
	if errors.Is(err, sql.ErrNoRows) {
		return "", ErrInvalidToken
	}
	if err != nil {
		return "", fmt.Errorf("account store %s: identify current login device: %w", s.path, err)
	}
	return deviceID, nil
}

// SetLoginDevicePolicy toggles allowlist enforcement. Enabling removes live
// sessions from installations not already on the account's trusted list.
func (s *Store) SetLoginDevicePolicy(userID, currentToken string, enabled bool) (LoginDeviceState, int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return LoginDeviceState{}, 0, fmt.Errorf("account store %s: begin login device policy update: %w", s.path, err)
	}
	defer tx.Rollback()
	if enabled && strings.TrimSpace(currentToken) != "" {
		var deviceID string
		if err := tx.QueryRow(
			`SELECT device_id FROM user_tokens WHERE user_id = ? AND token_hash = ?`,
			userID, tokenHash(currentToken),
		).Scan(&deviceID); errors.Is(err, sql.ErrNoRows) {
			return LoginDeviceState{}, 0, ErrInvalidToken
		} else if err != nil {
			return LoginDeviceState{}, 0, fmt.Errorf("account store %s: identify current login device: %w", s.path, err)
		}
		var trusted int
		err := tx.QueryRow(
			`SELECT 1 FROM account_login_devices WHERE user_id = ? AND device_id = ?`, userID, deviceID,
		).Scan(&trusted)
		if errors.Is(err, sql.ErrNoRows) {
			return LoginDeviceState{}, 0, ErrLoginDeviceProofNeeded
		}
		if err != nil {
			return LoginDeviceState{}, 0, fmt.Errorf("account store %s: verify current login device: %w", s.path, err)
		}
	}
	var revoked int
	if enabled {
		if _, err := tx.Exec(`UPDATE users SET login_device_allowlist_enabled = TRUE WHERE id = ?`, userID); err != nil {
			return LoginDeviceState{}, 0, fmt.Errorf("account store %s: enable login device allowlist: %w", s.path, err)
		}
		if _, err := tx.Exec(
			`DELETE FROM refresh_tokens WHERE user_id = ? AND session_id IN (
			 SELECT session_id FROM user_tokens ut
			 WHERE ut.user_id = ? AND NOT EXISTS (
				 SELECT 1 FROM account_login_devices d
				 WHERE d.user_id = ut.user_id AND d.device_id = ut.device_id))`,
			userID, userID,
		); err != nil {
			return LoginDeviceState{}, 0, fmt.Errorf("account store %s: revoke untrusted refresh sessions: %w", s.path, err)
		}
		res, err := tx.Exec(
			`DELETE FROM user_tokens WHERE user_id = ? AND NOT EXISTS (
			 SELECT 1 FROM account_login_devices d
			 WHERE d.user_id = user_tokens.user_id AND d.device_id = user_tokens.device_id)`,
			userID,
		)
		if err != nil {
			return LoginDeviceState{}, 0, fmt.Errorf("account store %s: revoke untrusted sessions: %w", s.path, err)
		}
		count, err := res.RowsAffected()
		if err != nil {
			return LoginDeviceState{}, 0, fmt.Errorf("account store %s: count revoked sessions: %w", s.path, err)
		}
		revoked = int(count)
	} else if _, err := tx.Exec(
		`UPDATE users SET login_device_allowlist_enabled = FALSE WHERE id = ?`, userID,
	); err != nil {
		return LoginDeviceState{}, 0, fmt.Errorf("account store %s: disable login device allowlist: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return LoginDeviceState{}, 0, fmt.Errorf("account store %s: commit login device policy: %w", s.path, err)
	}
	state, err := s.loginDevicesLocked(userID)
	return state, revoked, err
}

func (s *Store) loginDevicesLocked(userID string) (LoginDeviceState, error) {
	var state LoginDeviceState
	if err := s.db.QueryRow(
		`SELECT login_device_allowlist_enabled FROM users WHERE id = ?`, userID,
	).Scan(&state.Enabled); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return state, ErrUnknownUser
		}
		return state, fmt.Errorf("account store %s: read login device policy: %w", s.path, err)
	}
	rows, err := s.db.Query(
		`SELECT device_id, device_name, created_at, last_seen_at
		 FROM account_login_devices WHERE user_id = ? ORDER BY last_seen_at DESC, device_id`, userID,
	)
	if err != nil {
		return state, fmt.Errorf("account store %s: list login devices: %w", s.path, err)
	}
	defer rows.Close()
	state.Devices = make([]LoginDevice, 0)
	for rows.Next() {
		var device LoginDevice
		var createdAt, lastSeenAt int64
		if err := rows.Scan(&device.ID, &device.DeviceName, &createdAt, &lastSeenAt); err != nil {
			return LoginDeviceState{}, err
		}
		device.CreatedAt = timeFromMillis(createdAt)
		device.LastSeenAt = timeFromMillis(lastSeenAt)
		state.Devices = append(state.Devices, device)
	}
	if err := rows.Err(); err != nil {
		return LoginDeviceState{}, err
	}
	return state, nil
}

// AddLoginDevice lets an administrator restore access to an account by
// trusting an installation key they obtained out of band.
func (s *Store) AddLoginDevice(userID, publicKeyText, deviceName string) (LoginDevice, error) {
	publicKey, err := decodeLoginDevicePublicKey(publicKeyText)
	if err != nil {
		return LoginDevice{}, err
	}
	deviceID := loginDeviceID(publicKey)
	deviceName = normalizeText(deviceName, maxDeviceName)
	now := s.now().UTC()
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`INSERT INTO account_login_devices
			(user_id, device_id, public_key, device_name, created_at, last_seen_at)
		 VALUES (?, ?, ?, ?, ?, ?)
		 ON CONFLICT(user_id, device_id) DO UPDATE SET
			 public_key = excluded.public_key, device_name = excluded.device_name`,
		userID, deviceID, publicKeyText, deviceName, unixMillis(now), unixMillis(now),
	); err != nil {
		return LoginDevice{}, fmt.Errorf("account store %s: trust login device: %w", s.path, err)
	}
	return LoginDevice{ID: deviceID, DeviceName: deviceName, CreatedAt: now, LastSeenAt: now}, nil
}

// RevokeLoginDevice removes the installation and immediately terminates its
// live account sessions and refresh chains.
func (s *Store) RevokeLoginDevice(userID, deviceID string) (int, error) {
	deviceID = strings.TrimSpace(deviceID)
	if len(deviceID) != sha256.Size*2 {
		return 0, ErrInvalidLoginDeviceID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return 0, fmt.Errorf("account store %s: begin login device revoke: %w", s.path, err)
	}
	defer tx.Rollback()
	deviceResult, err := tx.Exec(
		`DELETE FROM account_login_devices WHERE user_id = ? AND device_id = ?`, userID, deviceID,
	)
	if err != nil {
		return 0, fmt.Errorf("account store %s: revoke login device: %w", s.path, err)
	}
	deviceCount, err := deviceResult.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("account store %s: count revoked login devices: %w", s.path, err)
	}
	if deviceCount == 0 {
		return 0, ErrUnknownLoginDevice
	}
	if _, err := tx.Exec(
		`DELETE FROM refresh_tokens WHERE user_id = ? AND session_id IN (
		 SELECT session_id FROM user_tokens WHERE user_id = ? AND device_id = ?)`,
		userID, userID, deviceID,
	); err != nil {
		return 0, fmt.Errorf("account store %s: revoke login device refresh tokens: %w", s.path, err)
	}
	res, err := tx.Exec(
		`DELETE FROM user_tokens WHERE user_id = ? AND device_id = ?`, userID, deviceID,
	)
	if err != nil {
		return 0, fmt.Errorf("account store %s: revoke login device sessions: %w", s.path, err)
	}
	count, err := res.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("account store %s: count revoked login device sessions: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("account store %s: commit login device revoke: %w", s.path, err)
	}
	return int(count), nil
}
