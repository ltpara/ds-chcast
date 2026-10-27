package account

import "time"

// CreateLoginEmailCode verifies the password and mints a single-use login code
// for an account that enabled two-factor authentication and has a recovery
// email. See createLoginCode for the shared channel behavior.
func (s *Store) CreateLoginEmailCode(username, password string, ttl time.Duration) (string, User, time.Time, error) {
	return s.createLoginCode(username, password, ttl, loginEmailChannel)
}

func (s *Store) matchLoginEmailCodeLocked(userID, code string, now time.Time) (bool, error) {
	return s.matchLoginCodeLocked(userID, code, now, loginEmailChannel)
}
