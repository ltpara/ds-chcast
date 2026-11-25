package account

import (
	"errors"
	"fmt"
	"time"
)

// PruneReport counts rows removed from each operator-facing history.
type PruneReport struct {
	AuditEvents   int64 `json:"audit_events"`
	Connections   int64 `json:"connections"`
	FileTransfers int64 `json:"file_transfers"`
}

// PruneBefore deletes security and session reports older than cutoff. A zero
// cutoff is refused so a misconfigured retention value cannot empty the tables.
func (s *Store) PruneBefore(cutoff time.Time) (PruneReport, error) {
	if cutoff.IsZero() {
		return PruneReport{}, errors.New("retention cutoff must not be zero")
	}
	millis := unixMillis(cutoff.UTC())
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return PruneReport{}, fmt.Errorf("account store %s: begin prune: %w", s.path, err)
	}
	var report PruneReport
	targets := []struct {
		count *int64
		query string
	}{
		{&report.AuditEvents, `DELETE FROM audit_events WHERE created_at < ?`},
		{&report.Connections, `DELETE FROM connection_events WHERE started_at < ?`},
		{&report.FileTransfers, `DELETE FROM file_events WHERE occurred_at < ?`},
	}
	for _, target := range targets {
		res, err := tx.Exec(target.query, millis)
		if err != nil {
			tx.Rollback()
			return PruneReport{}, fmt.Errorf("account store %s: prune: %w", s.path, err)
		}
		if *target.count, err = res.RowsAffected(); err != nil {
			tx.Rollback()
			return PruneReport{}, fmt.Errorf("account store %s: prune rows: %w", s.path, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return PruneReport{}, fmt.Errorf("account store %s: commit prune: %w", s.path, err)
	}
	return report, nil
}

// AlarmPruneReport counts removed security alarm events.
type AlarmPruneReport struct {
	Alarms int64 `json:"alarms"`
}

// PruneAlarmsBefore deletes only alarms older than cutoff. Alarm age retention
// is independent from audit-history retention so an operator can keep the
// security event stream longer than its source audit rows.
func (s *Store) PruneAlarmsBefore(cutoff time.Time) (AlarmPruneReport, error) {
	if cutoff.IsZero() {
		return AlarmPruneReport{}, errors.New("alarm retention cutoff must not be zero")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	result, err := s.db.Exec(`DELETE FROM alarm_events WHERE created_at < ?`, unixMillis(cutoff.UTC()))
	if err != nil {
		return AlarmPruneReport{}, fmt.Errorf("account store %s: prune security alarms: %w", s.path, err)
	}
	removed, err := result.RowsAffected()
	if err != nil {
		return AlarmPruneReport{}, fmt.Errorf("account store %s: count pruned security alarms: %w", s.path, err)
	}
	return AlarmPruneReport{Alarms: removed}, nil
}

// TombstoneReport counts address-book tombstones removed by retention.
type TombstoneReport struct {
	Peers  int64 `json:"peers"`
	Groups int64 `json:"groups"`
}

// PruneTombstones deletes deleted address-book peers and groups whose last
// update predates cutoff. Live entries are never touched. Clients that stay
// away longer than the retention window miss the tombstone and need a full
// snapshot to drop the entry, so the window must exceed expected offline time.
func (s *Store) PruneTombstones(cutoff time.Time) (TombstoneReport, error) {
	if cutoff.IsZero() {
		return TombstoneReport{}, errors.New("tombstone cutoff must not be zero")
	}
	millis := unixMillis(cutoff.UTC())
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return TombstoneReport{}, fmt.Errorf("account store %s: begin tombstone prune: %w", s.path, err)
	}
	var report TombstoneReport
	targets := []struct {
		count *int64
		query string
	}{
		{&report.Peers, `DELETE FROM address_book WHERE deleted = 1 AND updated_at < ?`},
		{&report.Groups, `DELETE FROM address_book_groups WHERE deleted = 1 AND updated_at < ?`},
	}
	for _, target := range targets {
		res, err := tx.Exec(target.query, millis)
		if err != nil {
			tx.Rollback()
			return TombstoneReport{}, fmt.Errorf("account store %s: prune tombstones: %w", s.path, err)
		}
		if *target.count, err = res.RowsAffected(); err != nil {
			tx.Rollback()
			return TombstoneReport{}, fmt.Errorf("account store %s: prune tombstone rows: %w", s.path, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return TombstoneReport{}, fmt.Errorf("account store %s: commit tombstone prune: %w", s.path, err)
	}
	return report, nil
}
