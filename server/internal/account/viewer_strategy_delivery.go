package account

import (
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

const viewerStrategyDeliveryTTL = 10 * time.Minute

// ViewerStrategyDelivery is returned only on an authenticated single-host
// lookup. The opaque context lets the same viewer report application without
// sharing the account token with the native session or the host.
type ViewerStrategyDelivery struct {
	Context     string            `json:"context"`
	ExpiresAtMS int64             `json:"expires_at_ms"`
	Snapshot    strategy.Snapshot `json:"snapshot"`
}

func applyViewerStrategyDeliveries(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS viewer_strategy_deliveries (
			context_id TEXT PRIMARY KEY,
			user_id    TEXT NOT NULL,
			peer_id    TEXT NOT NULL,
			revision   BIGINT NOT NULL,
			digest     TEXT NOT NULL,
			client_key TEXT NOT NULL,
			created_at BIGINT NOT NULL,
			expires_at BIGINT NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS viewer_strategy_deliveries_expiry
			ON viewer_strategy_deliveries(expires_at)`,
		`CREATE TABLE IF NOT EXISTS viewer_strategy_receipts (
			context_id  TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL,
			peer_id     TEXT NOT NULL,
			revision    BIGINT NOT NULL,
			digest      TEXT NOT NULL,
			client_key  TEXT NOT NULL,
			sequence    BIGINT NOT NULL,
			receipt_json TEXT NOT NULL,
			reported_at BIGINT NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS viewer_strategy_receipts_user_peer
			ON viewer_strategy_receipts(user_id, peer_id, reported_at)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create viewer strategy delivery: %w", err)
		}
	}
	return nil
}

func newViewerStrategyContext() (string, error) {
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", fmt.Errorf("generate viewer strategy context: %w", err)
	}
	return base64.RawURLEncoding.EncodeToString(raw[:]), nil
}

// CreateViewerStrategyDelivery binds a delivered snapshot to the account and
// connection identity that requested it. New lookups receive independent
// contexts so a reconnect cannot replace the snapshot of an active session.
func (s *Store) CreateViewerStrategyDelivery(userID, peerID, clientKey string, snapshot strategy.Snapshot) (ViewerStrategyDelivery, error) {
	userID = strings.TrimSpace(userID)
	peerID = strings.TrimSpace(peerID)
	clientKey = strings.TrimSpace(clientKey)
	if userID == "" || !registry.ValidID(peerID) || clientKey == "" || len(clientKey) > 4096 {
		return ViewerStrategyDelivery{}, fmt.Errorf("%w: invalid viewer strategy delivery identity", strategy.ErrInvalid)
	}
	if err := snapshot.Validate(peerID, strategy.Viewer); err != nil {
		return ViewerStrategyDelivery{}, err
	}
	now := s.now().UTC()
	expires := now.Add(viewerStrategyDeliveryTTL).UnixMilli()
	for attempt := 0; attempt < 3; attempt++ {
		contextID, err := newViewerStrategyContext()
		if err != nil {
			return ViewerStrategyDelivery{}, err
		}
		_, err = s.db.Exec(`INSERT INTO viewer_strategy_deliveries
			(context_id, user_id, peer_id, revision, digest, client_key, created_at, expires_at)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
			contextID, userID, peerID, snapshot.Revision, snapshot.Digest, clientKey, now.UnixMilli(), expires)
		if err == nil {
			return ViewerStrategyDelivery{Context: contextID, ExpiresAtMS: expires, Snapshot: snapshot}, nil
		}
	}
	return ViewerStrategyDelivery{}, errors.New("could not allocate viewer strategy context")
}

// RecordViewerStrategyReceipt validates the delivery context and the current
// desired revision in one transaction. Receipt sequences are scoped to one
// context; account, device, revision, digest and connection key cannot cross
// that boundary.
func (s *Store) RecordViewerStrategyReceipt(userID, peerID, contextID, clientKey string, receipt strategy.Receipt) (ClientStrategyReceipt, error) {
	userID = strings.TrimSpace(userID)
	peerID = strings.TrimSpace(peerID)
	contextID = strings.TrimSpace(contextID)
	clientKey = strings.TrimSpace(clientKey)
	if userID == "" || !registry.ValidID(peerID) || contextID == "" || len(contextID) > 128 || clientKey == "" || len(clientKey) > 4096 {
		return ClientStrategyReceipt{}, fmt.Errorf("%w: invalid viewer strategy receipt identity", strategy.ErrInvalid)
	}
	current, err := s.ResolveClientStrategy(peerID, strategy.Viewer)
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	if current.Revision != receipt.Revision || current.Digest != receipt.Digest {
		return ClientStrategyReceipt{}, ErrStrategyConflict
	}
	if err := receipt.Validate(current.Settings); err != nil {
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

	nowMS := s.now().UTC().UnixMilli()
	var deliveredRevision int64
	var deliveredDigest string
	err = tx.QueryRow(`SELECT d.revision, d.digest
		FROM viewer_strategy_deliveries d
		JOIN device_client_strategies s
		  ON s.peer_id = d.peer_id AND s.target = ? AND s.revision = d.revision
		WHERE d.context_id = ? AND d.user_id = ? AND d.peer_id = ? AND d.client_key = ? AND d.expires_at > ?`,
		strategy.Viewer, contextID, userID, peerID, clientKey, nowMS).
		Scan(&deliveredRevision, &deliveredDigest)
	if errors.Is(err, sql.ErrNoRows) {
		return ClientStrategyReceipt{}, ErrStrategyConflict
	}
	if err != nil {
		return ClientStrategyReceipt{}, err
	}
	if deliveredRevision != receipt.Revision || deliveredDigest != receipt.Digest {
		return ClientStrategyReceipt{}, ErrStrategyConflict
	}

	var previousJSON string
	var previousSequence, reportedAt int64
	err = tx.QueryRow(`SELECT sequence, receipt_json, reported_at
		FROM viewer_strategy_receipts WHERE context_id = ?`, contextID).
		Scan(&previousSequence, &previousJSON, &reportedAt)
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return ClientStrategyReceipt{}, err
	}
	if err == nil {
		if previousSequence == receipt.Sequence {
			if previousJSON != string(encoded) {
				return ClientStrategyReceipt{}, ErrStrategyConflict
			}
			return ClientStrategyReceipt{Receipt: receipt, ReportedAtMS: reportedAt}, nil
		}
		if previousSequence > receipt.Sequence {
			return ClientStrategyReceipt{}, ErrStrategyConflict
		}
	}

	reportedAt = nowMS
	result, err := tx.Exec(`INSERT INTO viewer_strategy_receipts
		(context_id, user_id, peer_id, revision, digest, client_key, sequence, receipt_json, reported_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(context_id) DO UPDATE SET
			sequence = excluded.sequence,
			receipt_json = excluded.receipt_json,
			reported_at = excluded.reported_at
		WHERE viewer_strategy_receipts.sequence < excluded.sequence`,
		contextID, userID, peerID, receipt.Revision, receipt.Digest, clientKey,
		receipt.Sequence, string(encoded), reportedAt)
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

// ViewerStrategyReceipt returns the last result for exactly one delivery
// context. Only the owning account can read it.
func (s *Store) ViewerStrategyReceipt(userID, peerID, contextID string) (*ClientStrategyReceipt, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	var encoded string
	var out ClientStrategyReceipt
	err := s.db.QueryRow(`SELECT receipt_json, reported_at FROM viewer_strategy_receipts
		WHERE context_id = ? AND user_id = ? AND peer_id = ?`,
		strings.TrimSpace(contextID), strings.TrimSpace(userID), strings.TrimSpace(peerID)).
		Scan(&encoded, &out.ReportedAtMS)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal([]byte(encoded), &out.Receipt); err != nil {
		return nil, fmt.Errorf("decode viewer strategy receipt: %w", err)
	}
	return &out, nil
}
