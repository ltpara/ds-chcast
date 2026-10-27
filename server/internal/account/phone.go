package account

import (
	"crypto/rand"
	"crypto/subtle"
	"database/sql"
	"errors"
	"fmt"
	"math/big"
	"strings"
	"time"
)

const (
	phoneMinDigits    = 6
	phoneMaxDigits    = 15
	phoneCodeDigits   = 8
	phoneResetTries   = 5
	phoneCodeHashSeed = "phone-reset:"
)

// normalizePhone strips cosmetic separators and an optional leading plus.
// Digits are kept as given so national prefixes survive; an empty value clears
// the recovery number.
func normalizePhone(phone string) (string, error) {
	phone = strings.TrimSpace(phone)
	if phone == "" {
		return "", nil
	}
	var b strings.Builder
	for i, r := range phone {
		switch {
		case r >= '0' && r <= '9':
			b.WriteRune(r)
		case r == '+' && i == 0:
		case r == ' ' || r == '-' || r == '(' || r == ')' || r == '.':
		default:
			return "", ErrInvalidPhone
		}
	}
	digits := b.String()
	if len(digits) < phoneMinDigits || len(digits) > phoneMaxDigits {
		return "", ErrInvalidPhone
	}
	return digits, nil
}

func newPhoneCode() (string, error) {
	limit := big.NewInt(100_000_000)
	n, err := rand.Int(rand.Reader, limit)
	if err != nil {
		return "", fmt.Errorf("generate phone reset code: %w", err)
	}
	return fmt.Sprintf("%08d", n.Int64()), nil
}

// normalizePhoneCode accepts the digits with optional spaces or dashes and
// rejects anything else, so a pasted code cannot smuggle other characters.
func normalizePhoneCode(code string) (string, bool) {
	var b strings.Builder
	for _, r := range strings.TrimSpace(code) {
		switch {
		case r >= '0' && r <= '9':
			b.WriteRune(r)
		case r == ' ' || r == '-':
		default:
			return "", false
		}
	}
	out := b.String()
	if len(out) != phoneCodeDigits {
		return "", false
	}
	return out, true
}

func phoneCodeHash(code string) string {
	return tokenHash(phoneCodeHashSeed + code)
}

// SetPhone stores an optional SMS recovery number; an empty value clears it.
// Changing the number also drops any outstanding code, because that code was
// issued for the previous destination.
func (s *Store) SetPhone(username, phone string) (User, error) {
	username = strings.TrimSpace(username)
	phone, err := normalizePhone(phone)
	if err != nil {
		return User{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, fmt.Errorf("account store %s: begin phone change: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(`UPDATE users SET phone = ? WHERE username = ?`, phone, username)
	if err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			return User{}, ErrPhoneTaken
		}
		return User{}, fmt.Errorf("account store %s: set phone: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return User{}, err
	}
	if affected == 0 {
		return User{}, ErrUnknownUser
	}
	if _, err := tx.Exec(
		`DELETE FROM phone_resets WHERE user_id = (SELECT id FROM users WHERE username = ?)`,
		username,
	); err != nil {
		return User{}, fmt.Errorf("account store %s: drop phone reset: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`DELETE FROM login_sms_codes WHERE user_id = (SELECT id FROM users WHERE username = ?)`,
		username,
	); err != nil {
		return User{}, fmt.Errorf("account store %s: drop login SMS code: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, fmt.Errorf("account store %s: commit phone change: %w", s.path, err)
	}
	return s.findUserLocked(username)
}

// CreatePhoneReset mints a single-use SMS code for an account with a recovery
// number. A new code replaces any earlier one, and only its SHA-256 hash is
// stored.
func (s *Store) CreatePhoneReset(username string, ttl time.Duration) (string, User, time.Time, error) {
	if ttl <= 0 {
		return "", User{}, time.Time{}, errors.New("phone reset lifetime must be positive")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	user, err := s.findUserLocked(username)
	if err != nil {
		return "", User{}, time.Time{}, err
	}
	if user.Phone == "" {
		return "", User{}, time.Time{}, ErrNoRecoveryPhone
	}
	code, err := newPhoneCode()
	if err != nil {
		return "", User{}, time.Time{}, err
	}
	now := s.now().UTC()
	expires := now.Add(ttl)
	tx, err := s.db.Begin()
	if err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: begin phone reset: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM phone_resets WHERE expires_at <= ?`, unixMillis(now)); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: prune phone resets: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM phone_resets WHERE user_id = ?`, user.ID); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: replace phone reset: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO phone_resets (user_id, code_hash, created_at, expires_at, attempts) VALUES (?, ?, ?, ?, 0)`,
		user.ID, phoneCodeHash(code), unixMillis(now), unixMillis(expires),
	); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: create phone reset: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return "", User{}, time.Time{}, fmt.Errorf("account store %s: commit phone reset: %w", s.path, err)
	}
	return code, user, expires, nil
}

// ConsumePhoneReset spends a code and replaces the password in one transaction.
// Five wrong codes retire the code, so a short number cannot be brute-forced
// across retries. Every access and refresh token is revoked on success.
func (s *Store) ConsumePhoneReset(username, code, password string) (User, error) {
	username = strings.TrimSpace(username)
	code, ok := normalizePhoneCode(code)
	if !ok {
		return User{}, ErrInvalidResetCode
	}
	if err := validatePassword(password); err != nil {
		return User{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return User{}, fmt.Errorf("account store %s: begin phone reset use: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		user     User
		created  int64
		banned   bool
		stored   string
		expires  int64
		attempts int64
	)
	err = tx.QueryRow(
		`SELECT u.id, u.username, u.role, u.created_at, u.banned, u.email, u.phone,
		        r.code_hash, r.expires_at, r.attempts
		 FROM phone_resets r JOIN users u ON u.id = r.user_id
		 WHERE u.username = ?`,
		username,
	).Scan(&user.ID, &user.Username, &user.Role, &created, &banned, &user.Email, &user.Phone,
		&stored, &expires, &attempts)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, ErrInvalidResetCode
	}
	if err != nil {
		return User{}, fmt.Errorf("account store %s: look up phone reset: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	user.Banned = banned
	expired := expires <= unixMillis(now)
	if expired || attempts >= phoneResetTries {
		if _, err := tx.Exec(`DELETE FROM phone_resets WHERE user_id = ?`, user.ID); err != nil {
			return User{}, fmt.Errorf("account store %s: retire phone reset: %w", s.path, err)
		}
		if err := tx.Commit(); err != nil {
			return User{}, fmt.Errorf("account store %s: commit phone reset retire: %w", s.path, err)
		}
		return User{}, ErrInvalidResetCode
	}
	if subtle.ConstantTimeCompare([]byte(stored), []byte(phoneCodeHash(code))) != 1 {
		next := attempts + 1
		if next >= phoneResetTries {
			_, err = tx.Exec(`DELETE FROM phone_resets WHERE user_id = ?`, user.ID)
		} else {
			_, err = tx.Exec(
				`UPDATE phone_resets SET attempts = ? WHERE user_id = ?`, next, user.ID,
			)
		}
		if err != nil {
			return User{}, fmt.Errorf("account store %s: count phone reset attempt: %w", s.path, err)
		}
		if err := tx.Commit(); err != nil {
			return User{}, fmt.Errorf("account store %s: commit phone reset attempt: %w", s.path, err)
		}
		return User{}, ErrInvalidResetCode
	}
	hash, err := hashPassword(password)
	if err != nil {
		return User{}, fmt.Errorf("hash password: %w", err)
	}
	if _, err := tx.Exec(`UPDATE users SET password_hash = ? WHERE id = ?`, hash, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: update password: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM phone_resets WHERE user_id = ?`, user.ID); err != nil {
		return User{}, fmt.Errorf("account store %s: spend phone reset: %w", s.path, err)
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
		return User{}, fmt.Errorf("account store %s: commit phone reset use: %w", s.path, err)
	}
	return user, nil
}
