package account

import (
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

var ErrStrategyConflict = errors.New("client strategy revision changed; reload before saving")

// ClientStrategy has a revision independent of admission policies. Editing
// client preferences must not revoke otherwise valid access tickets.
type ClientStrategy struct {
	PolicyID    string            `json:"policy_id"`
	Revision    int64             `json:"revision"`
	Settings    strategy.Settings `json:"settings"`
	UpdatedAtMS int64             `json:"updated_at_ms"`
	UpdatedBy   string            `json:"updated_by,omitempty"`
}

func applyClientStrategies(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS policy_client_strategies (
			policy_id TEXT PRIMARY KEY REFERENCES policy_objects(id) ON DELETE CASCADE,
			revision BIGINT NOT NULL,
			settings_json TEXT NOT NULL DEFAULT '{}',
			updated_at BIGINT NOT NULL,
			updated_by TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE TABLE IF NOT EXISTS device_client_strategies (
			peer_id TEXT NOT NULL,
			target TEXT NOT NULL,
			revision BIGINT NOT NULL,
			policy_id TEXT NOT NULL DEFAULT '',
			settings_json TEXT NOT NULL DEFAULT '{}',
			updated_at BIGINT NOT NULL,
			PRIMARY KEY (peer_id, target)
		)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create client strategies: %w", err)
		}
	}
	return nil
}

func (s *Store) clientStrategyLocked(policyID string) (ClientStrategy, error) {
	var found int
	if err := s.db.QueryRow(`SELECT 1 FROM policy_objects WHERE id = ?`, policyID).Scan(&found); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ClientStrategy{}, ErrUnknownPolicyObject
		}
		return ClientStrategy{}, err
	}
	out := ClientStrategy{PolicyID: policyID, Settings: strategy.Settings{}}
	var encoded string
	err := s.db.QueryRow(`SELECT revision, settings_json, updated_at, updated_by FROM policy_client_strategies WHERE policy_id = ?`, policyID).
		Scan(&out.Revision, &encoded, &out.UpdatedAtMS, &out.UpdatedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return out, nil
	}
	if err != nil {
		return ClientStrategy{}, err
	}
	if err := json.Unmarshal([]byte(encoded), &out.Settings); err != nil {
		return ClientStrategy{}, err
	}
	return out, nil
}

func (s *Store) ClientStrategy(policyID string) (ClientStrategy, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.clientStrategyLocked(strings.TrimSpace(policyID))
}

// SetClientStrategy replaces the complete overlay with optimistic concurrency.
// An empty map removes all settings without deleting the version counter.
func (s *Store) SetClientStrategy(policyID string, expected int64, settings strategy.Settings, actor string) (ClientStrategy, error) {
	if expected < 0 || expected >= strategy.MaxRevision {
		return ClientStrategy{}, fmt.Errorf("%w: revision outside supported range", strategy.ErrInvalid)
	}
	encoded, err := strategy.Encode(settings)
	if err != nil {
		return ClientStrategy{}, err
	}
	policyID = strings.TrimSpace(policyID)
	s.mu.Lock()
	defer s.mu.Unlock()
	current, err := s.clientStrategyLocked(policyID)
	if err != nil {
		return ClientStrategy{}, err
	}
	if current.Revision != expected {
		return ClientStrategy{}, ErrStrategyConflict
	}
	previous, err := strategy.Encode(current.Settings)
	if err != nil {
		return ClientStrategy{}, err
	}
	if previous == encoded {
		return current, nil
	}
	now := s.now().UTC().UnixMilli()
	actor = normalizeText(actor, maxName)
	var result sql.Result
	if current.Revision == 0 {
		result, err = s.db.Exec(`INSERT INTO policy_client_strategies (policy_id, revision, settings_json, updated_at, updated_by)
			VALUES (?, 1, ?, ?, ?) ON CONFLICT(policy_id) DO NOTHING`, policyID, encoded, now, actor)
	} else {
		result, err = s.db.Exec(`UPDATE policy_client_strategies SET revision = ?, settings_json = ?, updated_at = ?, updated_by = ?
			WHERE policy_id = ? AND revision = ?`, expected+1, encoded, now, actor, policyID, expected)
	}
	if err != nil {
		return ClientStrategy{}, err
	}
	count, err := result.RowsAffected()
	if err != nil {
		return ClientStrategy{}, err
	}
	if count != 1 {
		return ClientStrategy{}, ErrStrategyConflict
	}
	// Return the saved version, not a later edit from another server instance.
	var normalized strategy.Settings
	if err := json.Unmarshal([]byte(encoded), &normalized); err != nil {
		return ClientStrategy{}, err
	}
	return ClientStrategy{PolicyID: policyID, Revision: expected + 1, Settings: normalized, UpdatedAtMS: now, UpdatedBy: actor}, nil
}

// ClientStrategySnapshot is a durable per-device/target generation. Empty
// settings are an explicit removal, not absence of a server response.
type ClientStrategySnapshot = strategy.Snapshot

// ResolveClientStrategy uses the existing all/group/device assignment winner.
// Manual admission overrides remain independent of client configuration.
// Callers must authenticate device identity/console scope before calling it.
func (s *Store) ResolveClientStrategy(peerID, target string) (ClientStrategySnapshot, error) {
	if !registry.ValidID(peerID) {
		return ClientStrategySnapshot{}, ErrInvalidPeerID
	}
	if !strategy.ValidTarget(target) {
		return ClientStrategySnapshot{}, fmt.Errorf("%w: target must be host or viewer", strategy.ErrInvalid)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	// Read the generation before resolving the source. Otherwise another
	// instance could publish newer settings between our source read and this
	// generation read, allowing stale settings to replace them with a higher
	// revision. The conditional write below rejects that interleaving.
	out := ClientStrategySnapshot{PeerID: peerID, Target: target, Settings: strategy.Settings{}}
	var oldPolicy, oldSettings string
	err := s.db.QueryRow(`SELECT revision, policy_id, settings_json, updated_at FROM device_client_strategies WHERE peer_id = ? AND target = ?`, peerID, target).
		Scan(&out.Revision, &oldPolicy, &oldSettings, &out.UpdatedAtMS)
	missing := errors.Is(err, sql.ErrNoRows)
	if err != nil && !missing {
		return ClientStrategySnapshot{}, err
	}
	resolved, err := s.resolveDevicePolicyLocked(peerID)
	if err != nil {
		return ClientStrategySnapshot{}, err
	}
	if resolved != nil {
		config, err := s.clientStrategyLocked(resolved.policyID)
		if err != nil {
			return ClientStrategySnapshot{}, err
		}
		out.PolicyID = resolved.policyID
		out.Settings, err = strategy.ForTarget(config.Settings, target)
		if err != nil {
			return ClientStrategySnapshot{}, err
		}
	}
	encoded, err := strategy.Encode(out.Settings)
	if err != nil {
		return ClientStrategySnapshot{}, err
	}
	out.Digest = strategy.Digest(target, encoded)
	if !missing && oldPolicy == out.PolicyID && oldSettings == encoded {
		return out, nil
	}
	if out.Revision >= strategy.MaxRevision {
		return ClientStrategySnapshot{}, errors.New("client strategy revision exhausted")
	}
	out.UpdatedAtMS = s.now().UTC().UnixMilli()
	previous := out.Revision
	out.Revision++
	var result sql.Result
	if missing {
		result, err = s.db.Exec(`INSERT INTO device_client_strategies (peer_id, target, revision, policy_id, settings_json, updated_at)
			VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(peer_id, target) DO NOTHING`, peerID, target, out.Revision, out.PolicyID, encoded, out.UpdatedAtMS)
	} else {
		result, err = s.db.Exec(`UPDATE device_client_strategies SET revision = ?, policy_id = ?, settings_json = ?, updated_at = ?
			WHERE peer_id = ? AND target = ? AND revision = ?`, out.Revision, out.PolicyID, encoded, out.UpdatedAtMS, peerID, target, previous)
	}
	if err != nil {
		return ClientStrategySnapshot{}, err
	}
	count, err := result.RowsAffected()
	if err != nil {
		return ClientStrategySnapshot{}, err
	}
	if count != 1 {
		return ClientStrategySnapshot{}, ErrStrategyConflict
	}
	return out, nil
}
