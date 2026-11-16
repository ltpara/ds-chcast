package account

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

type ClientStrategyReceipt struct {
	strategy.Receipt
	ReportedAtMS int64 `json:"reported_at_ms"`
	Changed      bool  `json:"-"`
}

func applyClientStrategyReceipts(db *sql.DB) error {
	_, err := db.Exec(`CREATE TABLE IF NOT EXISTS client_strategy_receipts (
		peer_id TEXT NOT NULL,
		target TEXT NOT NULL,
		revision BIGINT NOT NULL,
		sequence BIGINT NOT NULL,
		reporter_key TEXT NOT NULL,
		receipt_json TEXT NOT NULL,
		reported_at BIGINT NOT NULL,
		PRIMARY KEY (peer_id, target)
	)`)
	return err
}

// RecordHostStrategyReceipt only accepts the current materialised document.
// A host cannot acknowledge viewer application on the operator's machine.
func (s *Store) RecordHostStrategyReceipt(peerID, reporterKey string, receipt strategy.Receipt) (ClientStrategyReceipt, error) {
	if reporterKey == "" {
		return ClientStrategyReceipt{}, fmt.Errorf("%w: reporter key is required", strategy.ErrInvalid)
	}
	snapshot, err := s.ResolveClientStrategy(peerID, strategy.Host)
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	if snapshot.Revision != receipt.Revision || snapshot.Digest != receipt.Digest {
		return ClientStrategyReceipt{}, ErrStrategyConflict
	}
	if err := receipt.Validate(snapshot.Settings); err != nil {
		return ClientStrategyReceipt{}, err
	}
	encoded, err := strategy.CanonicalReceipt(receipt)
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	defer tx.Rollback()
	var previousJSON, previousKey string
	var previousRevision, previousSequence, reportedAt int64
	err = tx.QueryRow(`SELECT revision, sequence, receipt_json, reported_at, reporter_key FROM client_strategy_receipts WHERE peer_id = ? AND target = ?`, peerID, strategy.Host).
		Scan(&previousRevision, &previousSequence, &previousJSON, &reportedAt, &previousKey)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return ClientStrategyReceipt{}, err
	}
	if err == nil && previousKey == reporterKey && previousRevision == receipt.Revision && previousSequence == receipt.Sequence {
		if previousJSON != string(encoded) {
			return ClientStrategyReceipt{}, ErrStrategyConflict
		}
		return ClientStrategyReceipt{Receipt: receipt, ReportedAtMS: reportedAt}, nil
	}
	if err == nil && previousKey == reporterKey && (previousRevision > receipt.Revision || (previousRevision == receipt.Revision && previousSequence >= receipt.Sequence)) {
		return ClientStrategyReceipt{}, ErrStrategyConflict
	}
	reportedAt = s.now().UTC().UnixMilli()
	// The snapshot revision condition closes the race with another instance
	// materialising a new version after ResolveClientStrategy returned.
	result, err := tx.Exec(`INSERT INTO client_strategy_receipts (peer_id, target, revision, sequence, receipt_json, reported_at, reporter_key)
		SELECT ?, ?, ?, ?, ?, ?, ? WHERE EXISTS (
			SELECT 1 FROM device_client_strategies WHERE peer_id = ? AND target = ? AND revision = ?)
		ON CONFLICT(peer_id, target) DO UPDATE SET revision = excluded.revision, sequence = excluded.sequence,
		receipt_json = excluded.receipt_json, reported_at = excluded.reported_at, reporter_key = excluded.reporter_key
		WHERE client_strategy_receipts.reporter_key <> excluded.reporter_key OR client_strategy_receipts.revision < excluded.revision OR
		(client_strategy_receipts.revision = excluded.revision AND client_strategy_receipts.sequence < excluded.sequence)`,
		peerID, strategy.Host, receipt.Revision, receipt.Sequence, string(encoded), reportedAt, reporterKey, peerID, strategy.Host, receipt.Revision)
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	count, err := result.RowsAffected()
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	if count != 1 {
		return ClientStrategyReceipt{}, ErrStrategyConflict
	}
	if err := tx.Commit(); err != nil {
		return ClientStrategyReceipt{}, err
	}
	return ClientStrategyReceipt{Receipt: receipt, ReportedAtMS: reportedAt, Changed: true}, nil
}

// CurrentClientStrategyReceipt hides stale application results when a newer
// desired version exists. Absence means unconfirmed, never success.
func (s *Store) CurrentClientStrategyReceipt(snapshot ClientStrategySnapshot, reporterKey string) (*ClientStrategyReceipt, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var encoded string
	var out ClientStrategyReceipt
	err := s.db.QueryRow(`SELECT receipt_json, reported_at FROM client_strategy_receipts WHERE peer_id = ? AND target = ? AND revision = ? AND reporter_key = ?`,
		snapshot.PeerID, snapshot.Target, snapshot.Revision, reporterKey).Scan(&encoded, &out.ReportedAtMS)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal([]byte(encoded), &out.Receipt); err != nil {
		return nil, fmt.Errorf("decode strategy receipt: %w", err)
	}
	if out.Digest != snapshot.Digest {
		return nil, nil
	}
	return &out, nil
}
