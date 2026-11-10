package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// maxAccessKid matches the ticket envelope's key-id bound. A longer value can
// never appear in a ticket, so revoking it would be a silent no-op.
const maxAccessKid = 64

var ErrInvalidAccessKid = errors.New("access key id must be 1..64 printable ASCII characters")

// RevokedAccessKid is one retired ticket-signing key. The row is durable so a
// server restart keeps refusing new tickets under that key and hosts that
// reconnect still learn the key is gone.
type RevokedAccessKid struct {
	Kid       string    `json:"kid"`
	RevokedAt time.Time `json:"revoked_at"`
	RevokedBy string    `json:"revoked_by,omitempty"`
	Reason    string    `json:"reason,omitempty"`
}

func validAccessKid(kid string) bool {
	if kid == "" || len(kid) > maxAccessKid {
		return false
	}
	for i := 0; i < len(kid); i++ {
		if kid[i] < 0x21 || kid[i] > 0x7e {
			return false
		}
	}
	return true
}

// RevokeAccessKid retires one issuer key. Revoking an already-retired key is
// idempotent and keeps the original timestamp so an operator retrying a
// request cannot reorder the audit history.
func (s *Store) RevokeAccessKid(kid, actor, reason string) (RevokedAccessKid, error) {
	kid = strings.TrimSpace(kid)
	if !validAccessKid(kid) {
		return RevokedAccessKid{}, ErrInvalidAccessKid
	}
	actor = normalizeText(actor, maxName)
	reason = normalizeText(reason, maxPolicyDetail)

	s.mu.Lock()
	defer s.mu.Unlock()

	var (
		record RevokedAccessKid
		atMS   int64
	)
	err := s.db.QueryRow(
		`SELECT kid, revoked_at, revoked_by, reason FROM revoked_access_kids WHERE kid = ?`, kid,
	).Scan(&record.Kid, &atMS, &record.RevokedBy, &record.Reason)
	switch {
	case err == nil:
		record.RevokedAt = timeFromMillis(atMS)
		return record, nil
	case !errors.Is(err, sql.ErrNoRows):
		return RevokedAccessKid{}, fmt.Errorf("account store %s: read revoked access key: %w", s.path, err)
	}

	atMS = unixMillis(s.now().UTC())
	if _, err := s.db.Exec(
		`INSERT INTO revoked_access_kids (kid, revoked_at, revoked_by, reason) VALUES (?, ?, ?, ?)`,
		kid, atMS, actor, reason,
	); err != nil {
		return RevokedAccessKid{}, fmt.Errorf("account store %s: revoke access key: %w", s.path, err)
	}
	return RevokedAccessKid{Kid: kid, RevokedAt: timeFromMillis(atMS), RevokedBy: actor, Reason: reason}, nil
}

// AccessKidRevoked reports whether the issuer key was retired. A lookup error
// is returned so a caller refuses to sign rather than assuming the key is live.
func (s *Store) AccessKidRevoked(kid string) (bool, error) {
	kid = strings.TrimSpace(kid)
	if !validAccessKid(kid) {
		return false, ErrInvalidAccessKid
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var one int
	err := s.db.QueryRow(`SELECT 1 FROM revoked_access_kids WHERE kid = ?`, kid).Scan(&one)
	switch {
	case err == nil:
		return true, nil
	case errors.Is(err, sql.ErrNoRows):
		return false, nil
	default:
		return false, fmt.Errorf("account store %s: read revoked access key: %w", s.path, err)
	}
}

// RevokedAccessKids lists retired keys newest first for the admin view.
func (s *Store) RevokedAccessKids() ([]RevokedAccessKid, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT kid, revoked_at, revoked_by, reason FROM revoked_access_kids ORDER BY revoked_at DESC, kid`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list revoked access keys: %w", s.path, err)
	}
	defer rows.Close()
	var out []RevokedAccessKid
	for rows.Next() {
		var (
			record RevokedAccessKid
			atMS   int64
		)
		if err := rows.Scan(&record.Kid, &atMS, &record.RevokedBy, &record.Reason); err != nil {
			return nil, fmt.Errorf("account store %s: scan revoked access key: %w", s.path, err)
		}
		record.RevokedAt = timeFromMillis(atMS)
		out = append(out, record)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list revoked access keys: %w", s.path, err)
	}
	return out, nil
}
