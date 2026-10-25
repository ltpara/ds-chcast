package account

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/authlimit"
)

type loginLockDB interface {
	QueryRowContext(ctx context.Context, query string, args ...any) *sql.Row
	ExecContext(ctx context.Context, query string, args ...any) (sql.Result, error)
}

// LoadLoginLock implements authlimit.Backend.
func (s *Store) LoadLoginLock(key string) (authlimit.Record, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.loadLoginLock(context.Background(), s.db, key)
}

func (s *Store) loadLoginLock(ctx context.Context, q loginLockDB, key string) (authlimit.Record, bool, error) {
	var (
		record                             authlimit.Record
		windowStart, lockedUntil, lastSeen int64
	)
	err := q.QueryRowContext(ctx,
		`SELECT failures, window_start, locked_until, last_seen
		 FROM login_locks WHERE login_key = ?`, key,
	).Scan(&record.Failures, &windowStart, &lockedUntil, &lastSeen)
	if errors.Is(err, sql.ErrNoRows) {
		return authlimit.Record{}, false, nil
	}
	if err != nil {
		return authlimit.Record{}, false,
			fmt.Errorf("account store %s: load login lock: %w", s.path, err)
	}
	record.WindowStart = timeFromMillis(windowStart)
	record.LastSeen = timeFromMillis(lastSeen)
	if lockedUntil > 0 {
		record.LockedUntil = timeFromMillis(lockedUntil)
	}
	return record, true, nil
}

// SaveLoginLock implements authlimit.Backend.
func (s *Store) SaveLoginLock(key string, record authlimit.Record) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.saveLoginLock(context.Background(), s.db, key, record)
}

func (s *Store) saveLoginLock(ctx context.Context, q loginLockDB, key string, record authlimit.Record) error {
	_, err := q.ExecContext(ctx,
		`INSERT INTO login_locks
			(login_key, failures, window_start, locked_until, last_seen)
		 VALUES (?, ?, ?, ?, ?)
		 ON CONFLICT(login_key) DO UPDATE SET
			failures = excluded.failures,
			window_start = excluded.window_start,
			locked_until = excluded.locked_until,
			last_seen = excluded.last_seen`,
		key, record.Failures, unixMillis(record.WindowStart),
		zeroMillis(record.LockedUntil), unixMillis(record.LastSeen),
	)
	if err != nil {
		return fmt.Errorf("account store %s: save login lock: %w", s.path, err)
	}
	return nil
}

// UpdateLoginLock implements authlimit.AtomicBackend. BEGIN IMMEDIATE takes the
// write lock before reading, so concurrent failure counts from another process
// sharing this database are serialized instead of overwriting each other.
func (s *Store) UpdateLoginLock(key string, update func(authlimit.Record, bool) authlimit.Record) (authlimit.Record, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ctx := context.Background()
	conn, err := s.db.Conn(ctx)
	if err != nil {
		return authlimit.Record{}, fmt.Errorf("account store %s: update login lock: %w", s.path, err)
	}
	defer conn.Close()
	if _, err := conn.ExecContext(ctx, "BEGIN IMMEDIATE"); err != nil {
		return authlimit.Record{}, fmt.Errorf("account store %s: update login lock: %w", s.path, err)
	}
	committed := false
	defer func() {
		if !committed {
			_, _ = conn.ExecContext(ctx, "ROLLBACK")
		}
	}()

	record, found, err := s.loadLoginLock(ctx, conn, key)
	if err != nil {
		return authlimit.Record{}, err
	}
	record = update(record, found)
	if err := s.saveLoginLock(ctx, conn, key, record); err != nil {
		return authlimit.Record{}, err
	}
	if _, err := conn.ExecContext(ctx, "COMMIT"); err != nil {
		return authlimit.Record{}, fmt.Errorf("account store %s: update login lock: %w", s.path, err)
	}
	committed = true
	return record, nil
}

// DeleteLoginLock implements authlimit.Backend.
func (s *Store) DeleteLoginLock(key string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(`DELETE FROM login_locks WHERE login_key = ?`, key); err != nil {
		return fmt.Errorf("account store %s: delete login lock: %w", s.path, err)
	}
	return nil
}

// PruneLoginLocks implements authlimit.Backend. Locked rows survive until the
// lock itself expires, even when last_seen is already old.
func (s *Store) PruneLoginLocks(seenBefore, lockedBefore time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`DELETE FROM login_locks
		 WHERE last_seen < ? AND (locked_until = 0 OR locked_until < ?)`,
		unixMillis(seenBefore), unixMillis(lockedBefore),
	); err != nil {
		return fmt.Errorf("account store %s: prune login locks: %w", s.path, err)
	}
	return nil
}

// ClearLoginLocks removes failure and lock records for a username across all
// client addresses. Keys are stored as lowercased "username|ip".
func (s *Store) ClearLoginLocks(username string) (int64, error) {
	username = strings.ToLower(strings.TrimSpace(username))
	if username == "" {
		return 0, fmt.Errorf("account store %s: clear login locks: username is empty", s.path)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(
		`DELETE FROM login_locks
		 WHERE login_key = ? OR substr(login_key, 1, length(?) + 1) = ?`,
		username, username, username+"|",
	)
	if err != nil {
		return 0, fmt.Errorf("account store %s: clear login locks: %w", s.path, err)
	}
	cleared, err := result.RowsAffected()
	if err != nil {
		return 0, fmt.Errorf("account store %s: clear login locks: %w", s.path, err)
	}
	return cleared, nil
}

func zeroMillis(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return unixMillis(t)
}
