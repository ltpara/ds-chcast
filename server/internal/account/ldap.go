package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// LoginLDAP signs in the local account linked to a directory entry that has
// just proved its password. The DN is the identity: the first successful login
// creates an account with an unusable local password and records the link, and
// later logins reuse it even if the login name or mail address changed in the
// directory. A linked account is refused when it is banned locally, so the
// console keeps its veto over directory users.
func (s *Store) LoginLDAP(
	dn, username, email, deviceID, deviceName string,
	autoCreate bool,
) (User, string, time.Time, string, time.Time, error) {
	dn = strings.TrimSpace(dn)
	username = strings.TrimSpace(username)
	if dn == "" || username == "" {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidCredentials
	}

	s.mu.Lock()
	user, found, err := s.ldapUserLocked(dn)
	s.mu.Unlock()
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	if found {
		return s.ldapSession(user, deviceID, deviceName)
	}

	// A name that already belongs to an account without a directory link is
	// never adopted: the directory could otherwise sign itself into a local
	// administrator that happens to share the name.
	s.mu.Lock()
	local, err := s.findUserLocked(username)
	s.mu.Unlock()
	switch {
	case err == nil:
		if local.Banned {
			return User{}, "", time.Time{}, "", time.Time{}, ErrAccountBanned
		}
		return User{}, "", time.Time{}, "", time.Time{}, ErrLDAPNameTaken
	case !errors.Is(err, ErrUnknownUser):
		return User{}, "", time.Time{}, "", time.Time{}, err
	}
	if !autoCreate {
		return User{}, "", time.Time{}, "", time.Time{}, ErrLDAPAccountNotFound
	}
	if !usernamePattern.MatchString(username) {
		return User{}, "", time.Time{}, "", time.Time{}, ErrInvalidUsername
	}
	if validateEmail(email) != nil {
		email = ""
	}

	// Hashing dominates the cost, so it happens outside the store lock. The
	// link and the name are checked again below in case a concurrent login won.
	hash, err := hashPassword(randomManagedPassword())
	if err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("hash directory password: %w", err)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if user, found, err = s.ldapUserLocked(dn); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, err
	} else if found {
		if user.Banned {
			return User{}, "", time.Time{}, "", time.Time{}, ErrAccountBanned
		}
		return s.issueVerifiedSessionLocked(user, deviceID, deviceName)
	}
	if _, err := s.findUserLocked(username); err == nil {
		return User{}, "", time.Time{}, "", time.Time{}, ErrLDAPNameTaken
	} else if !errors.Is(err, ErrUnknownUser) {
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
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: begin directory account: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`INSERT INTO users (id, username, password_hash, created_at, role, email, phone)
		 VALUES (?, ?, ?, ?, ?, ?, '')`,
		id, username, hash, unixMillis(now), role, email,
	); err != nil {
		if isUniqueViolation(err) {
			if strings.Contains(err.Error(), "email") {
				return User{}, "", time.Time{}, "", time.Time{}, ErrEmailTaken
			}
			return User{}, "", time.Time{}, "", time.Time{}, ErrLDAPNameTaken
		}
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: create directory account: %w", s.path, err)
	}
	if _, err := tx.Exec(
		`INSERT INTO ldap_identities (dn, user_id, created_at) VALUES (?, ?, ?)`,
		dn, id, unixMillis(now),
	); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: link directory entry: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return User{}, "", time.Time{}, "", time.Time{}, fmt.Errorf("account store %s: commit directory account: %w", s.path, err)
	}
	user = User{ID: id, Username: username, Role: role, CreatedAt: now, Email: email}
	return s.issueVerifiedSessionLocked(user, deviceID, deviceName)
}

// LDAPLinked reports whether the named account signs in through the
// directory. A linked account never falls back to its local password, so a
// directory-side disable takes effect immediately.
func (s *Store) LDAPLinked(username string) (bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var one int
	err := s.db.QueryRow(
		`SELECT 1 FROM ldap_identities i JOIN users u ON u.id = i.user_id
		 WHERE u.username = ?`,
		strings.TrimSpace(username),
	).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: look up directory link: %w", s.path, err)
	}
	return true, nil
}

func (s *Store) ldapSession(user User, deviceID, deviceName string) (User, string, time.Time, string, time.Time, error) {
	if user.Banned {
		return User{}, "", time.Time{}, "", time.Time{}, ErrAccountBanned
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.issueVerifiedSessionLocked(user, deviceID, deviceName)
}

func (s *Store) ldapUserLocked(dn string) (User, bool, error) {
	var (
		user    User
		created int64
	)
	err := s.db.QueryRow(
		`SELECT u.id, u.username, u.role, u.created_at, u.banned, u.email, u.phone
		 FROM ldap_identities i JOIN users u ON u.id = i.user_id
		 WHERE i.dn = ?`,
		dn,
	).Scan(&user.ID, &user.Username, &user.Role, &created, &user.Banned, &user.Email, &user.Phone)
	if errors.Is(err, sql.ErrNoRows) {
		return User{}, false, nil
	}
	if err != nil {
		return User{}, false, fmt.Errorf("account store %s: look up directory link: %w", s.path, err)
	}
	user.CreatedAt = timeFromMillis(created)
	return user, true, nil
}
