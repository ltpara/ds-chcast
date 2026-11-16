package account

import (
	"errors"
	"fmt"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

const (
	PolicyImportConflictReject = "reject"
	PolicyImportConflictUpdate = "update"
)

var (
	ErrInvalidPolicyImportConflict = errors.New("policy import conflict must be reject or update")
	ErrPolicyImportDuplicateName   = errors.New("policy import contains duplicate names")
	ErrPolicyImportTooMany         = fmt.Errorf("policy import must contain at most %d objects", maxPolicyObjects)
)

// PolicyObjectImport is the database-independent shape accepted by the bulk
// import path. Local ids, versions and timestamps are deliberately omitted so
// the same records can be replayed into another installation.
type PolicyObjectImport struct {
	Name              string
	Mode              string
	PermissionCeiling uint32
	AllowedUsers      []string
	AllowedGroups     []string
	// Nil preserves existing settings when importing a legacy document.
	ClientSettings *strategy.Settings
}

type PolicyImportResult struct {
	Created   []PolicyObject
	Updated   []PolicyObject
	Unchanged []PolicyObject
}

type preparedPolicyImport struct {
	input            PolicyObjectImport
	name             string
	mode             string
	userIDs          []string
	groups           []string
	existing         *PolicyObject
	id               string
	unchanged        bool
	strategyJSON     string
	strategyRevision int64
	strategyChanged  bool
}

// ImportPolicyObjects validates the complete document before opening a write
// transaction. With conflict=reject any existing name aborts the package; with
// conflict=update identical records are left byte-for-byte untouched and only
// changed records advance their policy version.
func (s *Store) ImportPolicyObjects(items []PolicyObjectImport, conflict, actor string) (PolicyImportResult, error) {
	conflict = strings.TrimSpace(conflict)
	if conflict == "" {
		conflict = PolicyImportConflictReject
	}
	if conflict != PolicyImportConflictReject && conflict != PolicyImportConflictUpdate {
		return PolicyImportResult{}, ErrInvalidPolicyImportConflict
	}
	if len(items) > maxPolicyObjects {
		return PolicyImportResult{}, ErrPolicyImportTooMany
	}

	prepared := make([]preparedPolicyImport, 0, len(items))
	seenNames := make(map[string]struct{}, len(items))
	for _, item := range items {
		strategyJSON := ""
		if item.ClientSettings != nil {
			var err error
			strategyJSON, err = strategy.Encode(*item.ClientSettings)
			if err != nil {
				return PolicyImportResult{}, err
			}
		}
		name := normalizeText(item.Name, maxName)
		if name == "" {
			return PolicyImportResult{}, ErrInvalidPolicyObjectName
		}
		if _, exists := seenNames[name]; exists {
			return PolicyImportResult{}, fmt.Errorf("%w: %s", ErrPolicyImportDuplicateName, name)
		}
		seenNames[name] = struct{}{}

		mode := strings.TrimSpace(item.Mode)
		if mode == "" {
			mode = DevicePolicyOpen
		}
		if mode != DevicePolicyOpen && mode != DevicePolicyRestricted {
			return PolicyImportResult{}, ErrInvalidDevicePolicyMode
		}
		if len(item.AllowedUsers) > maxDevicePolicyUsers || len(item.AllowedGroups) > maxDevicePolicyGroups {
			return PolicyImportResult{}, ErrDevicePolicyTooMany
		}
		userIDs, err := s.resolvePolicyUsers(item.AllowedUsers)
		if err != nil {
			return PolicyImportResult{}, err
		}
		groups, err := s.resolvePolicyGroups(item.AllowedGroups)
		if err != nil {
			return PolicyImportResult{}, err
		}
		prepared = append(prepared, preparedPolicyImport{
			strategyJSON: strategyJSON,
			input: PolicyObjectImport{
				Name:              name,
				Mode:              mode,
				PermissionCeiling: item.PermissionCeiling,
				AllowedUsers:      item.AllowedUsers,
				AllowedGroups:     item.AllowedGroups,
			},
			name:    name,
			mode:    mode,
			userIDs: userIDs,
			groups:  groups,
		})
	}

	actor = normalizeText(actor, maxName)
	now := s.now().UTC()

	s.mu.Lock()
	defer s.mu.Unlock()

	existing, err := s.policyObjectsLocked()
	if err != nil {
		return PolicyImportResult{}, err
	}
	byName := make(map[string]PolicyObject, len(existing))
	for _, policy := range existing {
		byName[policy.Name] = policy
	}

	newCount := 0
	for i := range prepared {
		if policy, ok := byName[prepared[i].name]; ok {
			prepared[i].existing = &policy
			if conflict == PolicyImportConflictReject {
				return PolicyImportResult{}, fmt.Errorf("%w: %s", ErrPolicyObjectExists, prepared[i].name)
			}
			continue
		}
		newCount++
		id, err := newRandomHex(8)
		if err != nil {
			return PolicyImportResult{}, fmt.Errorf("account store %s: generate policy id: %w", s.path, err)
		}
		prepared[i].id = id
	}
	if len(existing)+newCount > maxPolicyObjects {
		return PolicyImportResult{}, ErrPolicyObjectQuota
	}
	for i := range prepared {
		entry := &prepared[i]
		if entry.strategyJSON == "" {
			continue
		}
		before := "{}"
		if entry.existing != nil {
			config, err := s.clientStrategyLocked(entry.existing.ID)
			if err != nil {
				return PolicyImportResult{}, err
			}
			entry.strategyRevision = config.Revision
			before, err = strategy.Encode(config.Settings)
			if err != nil {
				return PolicyImportResult{}, err
			}
		}
		entry.strategyChanged = before != entry.strategyJSON
		if entry.strategyChanged && entry.strategyRevision >= strategy.MaxRevision {
			return PolicyImportResult{}, fmt.Errorf("%w: revision exhausted", strategy.ErrInvalid)
		}
	}

	tx, err := s.db.Begin()
	if err != nil {
		return PolicyImportResult{}, fmt.Errorf("account store %s: begin policy import: %w", s.path, err)
	}
	defer tx.Rollback()

	changed := false
	for i := range prepared {
		entry := &prepared[i]
		if entry.existing == nil {
			if _, err := tx.Exec(
				`INSERT INTO policy_objects
					(id, name, mode, permission_ceiling, policy_version, created_at, updated_at, updated_by)
				 VALUES (?, ?, ?, ?, 1, ?, ?, ?)`,
				entry.id, entry.name, entry.mode, int64(entry.input.PermissionCeiling),
				unixMillis(now), unixMillis(now), actor,
			); err != nil {
				if isUniqueViolation(err) {
					return PolicyImportResult{}, ErrPolicyObjectExists
				}
				return PolicyImportResult{}, fmt.Errorf("account store %s: import policy object: %w", s.path, err)
			}
			if err := insertPolicyObjectChildren(tx, entry.id, entry.userIDs, entry.groups, now); err != nil {
				return PolicyImportResult{}, fmt.Errorf("account store %s: %w", s.path, err)
			}
			changed = true
			continue
		}

		entry.id = entry.existing.ID
		if policyImportMatches(*entry.existing, entry) {
			entry.unchanged = true
			continue
		}
		if _, err := tx.Exec(
			`UPDATE policy_objects
			 SET mode = ?, permission_ceiling = ?, policy_version = policy_version + 1,
			     updated_at = ?, updated_by = ?
			 WHERE id = ?`,
			entry.mode, int64(entry.input.PermissionCeiling), unixMillis(now), actor, entry.id,
		); err != nil {
			return PolicyImportResult{}, fmt.Errorf("account store %s: update imported policy object: %w", s.path, err)
		}
		if _, err := tx.Exec(`DELETE FROM policy_object_users WHERE policy_id = ?`, entry.id); err != nil {
			return PolicyImportResult{}, fmt.Errorf("account store %s: clear imported policy users: %w", s.path, err)
		}
		if _, err := tx.Exec(`DELETE FROM policy_object_groups WHERE policy_id = ?`, entry.id); err != nil {
			return PolicyImportResult{}, fmt.Errorf("account store %s: clear imported policy groups: %w", s.path, err)
		}
		if err := insertPolicyObjectChildren(tx, entry.id, entry.userIDs, entry.groups, now); err != nil {
			return PolicyImportResult{}, fmt.Errorf("account store %s: %w", s.path, err)
		}
		changed = true
	}

	for i := range prepared {
		entry := &prepared[i]
		if !entry.strategyChanged {
			continue
		}
		result, err := tx.Exec(`INSERT INTO policy_client_strategies (policy_id, revision, settings_json, updated_at, updated_by)
			VALUES (?, ?, ?, ?, ?) ON CONFLICT(policy_id) DO UPDATE SET revision = excluded.revision,
			settings_json = excluded.settings_json, updated_at = excluded.updated_at, updated_by = excluded.updated_by
			WHERE policy_client_strategies.revision = ?`,
			entry.id, entry.strategyRevision+1, entry.strategyJSON, unixMillis(now), actor, entry.strategyRevision)
		if err != nil {
			return PolicyImportResult{}, err
		}
		count, err := result.RowsAffected()
		if err != nil {
			return PolicyImportResult{}, err
		}
		if count != 1 {
			return PolicyImportResult{}, ErrStrategyConflict
		}
		entry.unchanged = false
	}
	if err := tx.Commit(); err != nil {
		return PolicyImportResult{}, fmt.Errorf("account store %s: commit policy import: %w", s.path, err)
	}
	if changed {
		s.invalidatePolicyReconcileLocked()
	}

	result := PolicyImportResult{
		Created:   make([]PolicyObject, 0, newCount),
		Updated:   make([]PolicyObject, 0, len(prepared)-newCount),
		Unchanged: make([]PolicyObject, 0, len(prepared)-newCount),
	}
	for i := range prepared {
		entry := &prepared[i]
		policy, err := s.policyObjectLocked(entry.id)
		if err != nil {
			return PolicyImportResult{}, err
		}
		switch {
		case entry.existing == nil:
			result.Created = append(result.Created, policy)
		case entry.unchanged:
			result.Unchanged = append(result.Unchanged, policy)
		default:
			result.Updated = append(result.Updated, policy)
		}
	}
	return result, nil
}

func (s *Store) policyObjectsLocked() ([]PolicyObject, error) {
	rows, err := s.db.Query(`SELECT id FROM policy_objects ORDER BY name`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list policy objects: %w", s.path, err)
	}
	ids := make([]string, 0)
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return nil, fmt.Errorf("account store %s: scan policy object: %w", s.path, err)
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, fmt.Errorf("account store %s: list policy objects: %w", s.path, err)
	}
	rows.Close()
	return s.policyObjectsByIDsLocked(ids)
}

func (s *Store) policyObjectsByIDsLocked(ids []string) ([]PolicyObject, error) {
	out := make([]PolicyObject, 0, len(ids))
	for _, id := range ids {
		policy, err := s.policyObjectLocked(id)
		if err != nil {
			return nil, err
		}
		out = append(out, policy)
	}
	return out, nil
}

func policyImportMatches(policy PolicyObject, entry *preparedPolicyImport) bool {
	return policy.Mode == entry.mode &&
		policy.PermissionCeiling == entry.input.PermissionCeiling &&
		sameFoldedStringSet(policy.AllowedUsers, entry.input.AllowedUsers) &&
		sameStringSet(policy.AllowedGroups, normalizedStringSet(entry.input.AllowedGroups))
}

func sameFoldedStringSet(left, right []string) bool {
	fold := func(values []string) []string {
		out := normalizedStringSet(values)
		for i := range out {
			out[i] = strings.ToLower(out[i])
		}
		return normalizedStringSet(out)
	}
	return sameStringSet(fold(left), fold(right))
}

func normalizedStringSet(values []string) []string {
	seen := make(map[string]struct{}, len(values))
	out := make([]string, 0, len(values))
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		if _, ok := seen[value]; ok {
			continue
		}
		seen[value] = struct{}{}
		out = append(out, value)
	}
	return out
}
