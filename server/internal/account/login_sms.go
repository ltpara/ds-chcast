package account

import "time"

// CreateLoginSMSCode verifies the password and mints a single-use login code
// for an account that enabled two-factor authentication and has a recovery
// phone. See createLoginCode for the shared channel behavior.
func (s *Store) CreateLoginSMSCode(username, password string, ttl time.Duration) (string, User, time.Time, error) {
	return s.createLoginCode(username, password, ttl, loginSMSChannel)
}

func (s *Store) matchLoginSMSCodeLocked(userID, code string, now time.Time) (bool, error) {
	return s.matchLoginCodeLocked(userID, code, now, loginSMSChannel)
}
