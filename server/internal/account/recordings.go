package account

import (
	"database/sql"
	"errors"
	"fmt"
	"regexp"
	"strings"
)

// ErrUnknownRecording is returned when a recording id does not belong to the
// account that asked for it.
var ErrUnknownRecording = errors.New("unknown recording")

var recordingIDPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

// Recording is the metadata of one uploaded viewer recording. The MP4 itself
// lives in the recording archive directory, named after ID; the row lets the
// server list, authorize and expire files without touching the disk layout.
type Recording struct {
	ID          string `json:"id"`
	UserID      string `json:"-"`
	Username    string `json:"username,omitempty"`
	Name        string `json:"name"`
	PeerID      string `json:"peer_id"`
	Size        int64  `json:"size"`
	SHA256      string `json:"sha256"`
	DurationMS  int64  `json:"duration_ms"`
	StartedAtMS int64  `json:"started_at_ms"`
	CreatedAtMS int64  `json:"created_at_ms"`
	// Seq is the row id used as the operator page cursor; it never leaves the
	// server as part of a recording.
	Seq int64 `json:"-"`
}

// AddRecording stores metadata after the archive has written the file. The
// caller supplies the digest so both sides describe the same bytes.
func (s *Store) AddRecording(rec Recording) (Recording, error) {
	if !recordingIDPattern.MatchString(rec.ID) {
		return Recording{}, fmt.Errorf("account store %s: recording id is invalid", s.path)
	}
	rec.UserID = strings.TrimSpace(rec.UserID)
	if rec.UserID == "" {
		return Recording{}, fmt.Errorf("account store %s: recording owner is empty", s.path)
	}
	rec.Name = strings.TrimSpace(rec.Name)
	if rec.Name == "" {
		rec.Name = "recording.mp4"
	}
	if len(rec.Name) > 128 {
		rec.Name = rec.Name[:128]
	}
	if rec.Size <= 0 {
		return Recording{}, fmt.Errorf("account store %s: recording size must be positive", s.path)
	}
	if rec.CreatedAtMS <= 0 {
		rec.CreatedAtMS = unixMillis(s.now())
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec(
		`INSERT INTO recordings (id, user_id, name, peer_id, size, sha256,
			duration_ms, started_at, created_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		rec.ID, rec.UserID, rec.Name, strings.TrimSpace(rec.PeerID), rec.Size,
		rec.SHA256, rec.DurationMS, rec.StartedAtMS, rec.CreatedAtMS,
	)
	if err != nil {
		return Recording{}, fmt.Errorf("account store %s: add recording: %w", s.path, err)
	}
	return rec, nil
}

// Recordings lists one account's recordings, newest first.
func (s *Store) Recordings(userID string) ([]Recording, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT id, user_id, name, peer_id, size, sha256, duration_ms,
			started_at, created_at
		 FROM recordings WHERE user_id = ? ORDER BY created_at DESC, id DESC`,
		strings.TrimSpace(userID),
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list recordings: %w", s.path, err)
	}
	defer rows.Close()
	var out []Recording
	for rows.Next() {
		rec, err := scanRecording(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: read recording: %w", s.path, err)
		}
		out = append(out, rec)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list recordings: %w", s.path, err)
	}
	return out, nil
}

// Recording returns one account-owned recording's metadata.
func (s *Store) Recording(userID, id string) (Recording, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.recordingLocked(userID, id)
}

// RecordingByID returns one recording regardless of its owner, for operator
// tools that manage the archive across accounts.
func (s *Store) RecordingByID(id string) (Recording, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	row := s.db.QueryRow(
		`SELECT r.rowid, r.id, r.user_id, u.username, r.name, r.peer_id, r.size,
			r.sha256, r.duration_ms, r.started_at, r.created_at
		 FROM recordings r JOIN users u ON u.id = r.user_id
		 WHERE r.id = ?`,
		strings.TrimSpace(id),
	)
	rec, err := scanOperatorRecording(row)
	if errors.Is(err, sql.ErrNoRows) {
		return Recording{}, ErrUnknownRecording
	}
	if err != nil {
		return Recording{}, fmt.Errorf("account store %s: read recording: %w", s.path, err)
	}
	return rec, nil
}

// AllRecordings lists recordings across accounts newest first, optionally
// limited to one username. before is the Seq of the last row from the previous
// page; zero starts at the newest row.
func (s *Store) AllRecordings(username string, before int64, limit int) ([]Recording, error) {
	if limit <= 0 || limit > 200 {
		limit = 50
	}
	username = strings.TrimSpace(username)
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT r.rowid, r.id, r.user_id, u.username, r.name, r.peer_id, r.size,
			r.sha256, r.duration_ms, r.started_at, r.created_at
		 FROM recordings r JOIN users u ON u.id = r.user_id
		 WHERE (? = '' OR lower(u.username) = lower(?))
		   AND (? = 0 OR r.rowid < ?)
		 ORDER BY r.rowid DESC LIMIT ?`,
		username, username, before, before, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list all recordings: %w", s.path, err)
	}
	defer rows.Close()
	var out []Recording
	for rows.Next() {
		rec, err := scanOperatorRecording(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: read recording: %w", s.path, err)
		}
		out = append(out, rec)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list all recordings: %w", s.path, err)
	}
	return out, nil
}

// RecordingTotals reports bytes and files across accounts, optionally limited
// to one username.
func (s *Store) RecordingTotals(username string) (int64, int, error) {
	username = strings.TrimSpace(username)
	s.mu.Lock()
	defer s.mu.Unlock()
	var bytes int64
	var files int
	if err := s.db.QueryRow(
		`SELECT COALESCE(SUM(r.size), 0), COUNT(*)
		 FROM recordings r JOIN users u ON u.id = r.user_id
		 WHERE (? = '' OR lower(u.username) = lower(?))`,
		username, username,
	).Scan(&bytes, &files); err != nil {
		return 0, 0, fmt.Errorf("account store %s: recording totals: %w", s.path, err)
	}
	return bytes, files, nil
}


func (s *Store) recordingLocked(userID, id string) (Recording, error) {
	row := s.db.QueryRow(
		`SELECT id, user_id, name, peer_id, size, sha256, duration_ms,
			started_at, created_at
		 FROM recordings WHERE user_id = ? AND id = ?`,
		strings.TrimSpace(userID), strings.TrimSpace(id),
	)
	rec, err := scanRecording(row)
	if errors.Is(err, sql.ErrNoRows) {
		return Recording{}, ErrUnknownRecording
	}
	if err != nil {
		return Recording{}, fmt.Errorf("account store %s: read recording: %w", s.path, err)
	}
	return rec, nil
}

// DeleteRecording removes one row and reports whether it existed. The archive
// removes the file separately so a failed unlink can be retried by retention.
func (s *Store) DeleteRecording(userID, id string) (Recording, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rec, err := s.recordingLocked(userID, id)
	if errors.Is(err, ErrUnknownRecording) {
		return Recording{}, false, nil
	}
	if err != nil {
		return Recording{}, false, err
	}
	if _, err := s.db.Exec(
		`DELETE FROM recordings WHERE user_id = ? AND id = ?`,
		strings.TrimSpace(userID), strings.TrimSpace(id),
	); err != nil {
		return Recording{}, false, fmt.Errorf("account store %s: delete recording: %w", s.path, err)
	}
	return rec, true, nil
}

// RecordingUsage reports how many bytes and files one account stores.
func (s *Store) RecordingUsage(userID string) (int64, int, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var bytes int64
	var files int
	if err := s.db.QueryRow(
		`SELECT COALESCE(SUM(size), 0), COUNT(*) FROM recordings WHERE user_id = ?`,
		strings.TrimSpace(userID),
	).Scan(&bytes, &files); err != nil {
		return 0, 0, fmt.Errorf("account store %s: recording usage: %w", s.path, err)
	}
	return bytes, files, nil
}

// ExpiredRecordings lists rows older than beforeMS across accounts, oldest
// first, so the archive can delete both the file and its metadata.
func (s *Store) ExpiredRecordings(beforeMS int64, limit int) ([]Recording, error) {
	if limit <= 0 || limit > 500 {
		limit = 200
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT id, user_id, name, peer_id, size, sha256, duration_ms,
			started_at, created_at
		 FROM recordings WHERE created_at < ? ORDER BY created_at ASC, id ASC LIMIT ?`,
		beforeMS, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: expired recordings: %w", s.path, err)
	}
	defer rows.Close()
	var out []Recording
	for rows.Next() {
		rec, err := scanRecording(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: read recording: %w", s.path, err)
		}
		out = append(out, rec)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: expired recordings: %w", s.path, err)
	}
	return out, nil
}

// RecordingIDs reports the recording ids one account still references, so the
// archive sweep can remove files whose row was deleted before the unlink.
func (s *Store) RecordingIDs(userID string) (map[string]struct{}, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT id FROM recordings WHERE user_id = ?`,
		strings.TrimSpace(userID))
	if err != nil {
		return nil, fmt.Errorf("account store %s: recording ids: %w", s.path, err)
	}
	defer rows.Close()
	ids := make(map[string]struct{})
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("account store %s: read recording id: %w", s.path, err)
		}
		ids[id] = struct{}{}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: recording ids: %w", s.path, err)
	}
	return ids, nil
}

// RecordingOwners reports the account ids the archive may still contain. A
// directory whose owner disappears was left by an account deletion, so the
// archive sweep can remove it without touching live data.
func (s *Store) RecordingOwners() (map[string]struct{}, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT DISTINCT user_id FROM recordings`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: recording owners: %w", s.path, err)
	}
	defer rows.Close()
	owners := make(map[string]struct{})
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("account store %s: read recording owner: %w", s.path, err)
		}
		owners[id] = struct{}{}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: recording owners: %w", s.path, err)
	}
	return owners, nil
}

type recordingScanner interface {
	Scan(dest ...any) error
}

func scanRecording(row recordingScanner) (Recording, error) {
	var rec Recording
	err := row.Scan(&rec.ID, &rec.UserID, &rec.Name, &rec.PeerID, &rec.Size,
		&rec.SHA256, &rec.DurationMS, &rec.StartedAtMS, &rec.CreatedAtMS)
	return rec, err
}

func scanOperatorRecording(row recordingScanner) (Recording, error) {
	var rec Recording
	err := row.Scan(&rec.Seq, &rec.ID, &rec.UserID, &rec.Username, &rec.Name,
		&rec.PeerID, &rec.Size, &rec.SHA256, &rec.DurationMS,
		&rec.StartedAtMS, &rec.CreatedAtMS)
	return rec, err
}
