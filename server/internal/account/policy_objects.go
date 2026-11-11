package account

import (
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// Assignment scopes, from broadest to narrowest. A device can match several
// assignments at once (a group plus a direct device rule plus the global
// default); the resolver picks one and materialises it into device_policies,
// which stays the only record the admission path reads.
const (
	PolicyScopeAll    = "all"
	PolicyScopeGroup  = "group"
	PolicyScopeDevice = "device"
)

const (
	maxPolicyObjects     = 256
	maxPolicyAssignments = 1024
	maxPolicyPriority    = 10000
)

// MaxPolicyAssignmentBatch caps one device-scope bulk call. The console uses
// the same number so a pasted list is refused before it reaches the API.
const MaxPolicyAssignmentBatch = 200

var (
	ErrInvalidPolicyObjectName      = errors.New("policy object name is required and must be at most 64 characters")
	ErrPolicyObjectExists           = errors.New("a policy object with this name already exists")
	ErrUnknownPolicyObject          = errors.New("unknown policy object")
	ErrPolicyObjectInUse            = errors.New("policy object still has assignments")
	ErrPolicyObjectQuota            = errors.New("policy object quota reached")
	ErrInvalidPolicyScope           = errors.New("policy scope must be all, group or device")
	ErrUnknownPolicyScope           = errors.New("policy scope references an unknown group")
	ErrPolicyAssignmentExists       = errors.New("this scope already has a policy assignment")
	ErrUnknownPolicyAssignment      = errors.New("unknown policy assignment")
	ErrPolicyAssignmentQuota        = errors.New("policy assignment quota reached")
	ErrInvalidPolicyPriority        = errors.New("policy priority must be between -10000 and 10000")
	ErrInvalidPolicyAssignmentBatch = fmt.Errorf(
		"policy assignment batch must list 1 to %d device ids", MaxPolicyAssignmentBatch)
)

// PolicyObject is a named, reusable policy template. Its version increments on
// every edit; the version materialised onto a device is independent because
// the per-device counter must stay monotonic when assignments switch between
// templates.
type PolicyObject struct {
	ID                string    `json:"id"`
	Name              string    `json:"name"`
	Mode              string    `json:"mode"`
	PermissionCeiling uint32    `json:"permission_ceiling"`
	PolicyVersion     uint32    `json:"policy_version"`
	AllowedUsers      []string  `json:"allowed_users"`
	AllowedGroups     []string  `json:"allowed_groups"`
	CreatedAt         time.Time `json:"created_at"`
	UpdatedAt         time.Time `json:"updated_at"`
	UpdatedBy         string    `json:"updated_by,omitempty"`
}

// PolicyAssignment binds one policy object to one scope. Scopes are unique, so
// re-assigning a group replaces the previous rule instead of stacking order
// rules inside one scope; priority decides between different scopes.
type PolicyAssignment struct {
	ID         string    `json:"id"`
	PolicyID   string    `json:"policy_id"`
	PolicyName string    `json:"policy_name"`
	ScopeKind  string    `json:"scope_kind"`
	ScopeValue string    `json:"scope_value"`
	Priority   int       `json:"priority"`
	CreatedAt  time.Time `json:"created_at"`
	CreatedBy  string    `json:"created_by,omitempty"`
}

// DevicePolicySource names the assignment that produced a materialised
// device policy. A manual per-device edit has no source and wins over every
// assignment until the console deletes it.
type DevicePolicySource struct {
	Kind     string `json:"kind"`
	ID       string `json:"id"`
	Name     string `json:"name"`
	Version  uint32 `json:"version"`
	Priority int    `json:"priority"`
}

func validPolicyScope(kind, value string) bool {
	switch kind {
	case PolicyScopeAll:
		return value == ""
	case PolicyScopeGroup, PolicyScopeDevice:
		return registry.ValidID(value)
	}
	return false
}

func (s *Store) CreatePolicyObject(name, mode string, ceiling uint32, usernames, groupIDs []string, actor string) (PolicyObject, error) {
	name = normalizeText(name, maxName)
	if name == "" {
		return PolicyObject{}, ErrInvalidPolicyObjectName
	}
	mode = strings.TrimSpace(mode)
	if mode == "" {
		mode = DevicePolicyOpen
	}
	if mode != DevicePolicyOpen && mode != DevicePolicyRestricted {
		return PolicyObject{}, ErrInvalidDevicePolicyMode
	}
	if len(usernames) > maxDevicePolicyUsers || len(groupIDs) > maxDevicePolicyGroups {
		return PolicyObject{}, ErrDevicePolicyTooMany
	}
	userIDs, err := s.resolvePolicyUsers(usernames)
	if err != nil {
		return PolicyObject{}, err
	}
	groups, err := s.resolvePolicyGroups(groupIDs)
	if err != nil {
		return PolicyObject{}, err
	}
	id, err := newRandomHex(8)
	if err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: generate policy id: %w", s.path, err)
	}
	actor = normalizeText(actor, maxName)
	now := s.now().UTC()

	s.mu.Lock()
	defer s.mu.Unlock()
	var count int
	if err := s.db.QueryRow(`SELECT COUNT(*) FROM policy_objects`).Scan(&count); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: count policy objects: %w", s.path, err)
	}
	if count >= maxPolicyObjects {
		return PolicyObject{}, ErrPolicyObjectQuota
	}
	tx, err := s.db.Begin()
	if err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: begin policy object: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`INSERT INTO policy_objects
			(id, name, mode, permission_ceiling, policy_version, created_at, updated_at, updated_by)
		 VALUES (?, ?, ?, ?, 1, ?, ?, ?)`,
		id, name, mode, int64(ceiling), unixMillis(now), unixMillis(now), actor,
	); err != nil {
		if isUniqueViolation(err) {
			return PolicyObject{}, ErrPolicyObjectExists
		}
		return PolicyObject{}, fmt.Errorf("account store %s: create policy object: %w", s.path, err)
	}
	if err := insertPolicyObjectChildren(tx, id, userIDs, groups, now); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: commit policy object: %w", s.path, err)
	}
	return s.policyObjectLocked(id)
}

func insertPolicyObjectChildren(tx *sql.Tx, id string, userIDs, groups []string, now time.Time) error {
	for _, userID := range userIDs {
		if _, err := tx.Exec(
			`INSERT INTO policy_object_users (policy_id, user_id, created_at) VALUES (?, ?, ?)`,
			id, userID, unixMillis(now),
		); err != nil {
			return fmt.Errorf("add policy object user: %w", err)
		}
	}
	for _, groupID := range groups {
		if _, err := tx.Exec(
			`INSERT INTO policy_object_groups (policy_id, group_id, created_at) VALUES (?, ?, ?)`,
			id, groupID, unixMillis(now),
		); err != nil {
			return fmt.Errorf("add policy object group: %w", err)
		}
	}
	return nil
}

func (s *Store) UpdatePolicyObject(id, name, mode string, ceiling uint32, usernames, groupIDs []string, actor string) (PolicyObject, error) {
	id = strings.TrimSpace(id)
	if !registry.ValidID(id) {
		return PolicyObject{}, ErrUnknownPolicyObject
	}
	name = normalizeText(name, maxName)
	if name == "" {
		return PolicyObject{}, ErrInvalidPolicyObjectName
	}
	mode = strings.TrimSpace(mode)
	if mode == "" {
		mode = DevicePolicyOpen
	}
	if mode != DevicePolicyOpen && mode != DevicePolicyRestricted {
		return PolicyObject{}, ErrInvalidDevicePolicyMode
	}
	if len(usernames) > maxDevicePolicyUsers || len(groupIDs) > maxDevicePolicyGroups {
		return PolicyObject{}, ErrDevicePolicyTooMany
	}
	userIDs, err := s.resolvePolicyUsers(usernames)
	if err != nil {
		return PolicyObject{}, err
	}
	groups, err := s.resolvePolicyGroups(groupIDs)
	if err != nil {
		return PolicyObject{}, err
	}
	actor = normalizeText(actor, maxName)
	now := s.now().UTC()

	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: begin policy object update: %w", s.path, err)
	}
	defer tx.Rollback()
	res, err := tx.Exec(
		`UPDATE policy_objects
		 SET name = ?, mode = ?, permission_ceiling = ?, policy_version = policy_version + 1,
		     updated_at = ?, updated_by = ?
		 WHERE id = ?`,
		name, mode, int64(ceiling), unixMillis(now), actor, id,
	)
	if err != nil {
		if isUniqueViolation(err) {
			return PolicyObject{}, ErrPolicyObjectExists
		}
		return PolicyObject{}, fmt.Errorf("account store %s: update policy object: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return PolicyObject{}, err
	}
	if affected == 0 {
		return PolicyObject{}, ErrUnknownPolicyObject
	}
	if _, err := tx.Exec(`DELETE FROM policy_object_users WHERE policy_id = ?`, id); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: clear policy object users: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM policy_object_groups WHERE policy_id = ?`, id); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: clear policy object groups: %w", s.path, err)
	}
	if err := insertPolicyObjectChildren(tx, id, userIDs, groups, now); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: commit policy object update: %w", s.path, err)
	}
	return s.policyObjectLocked(id)
}

func (s *Store) policyObjectLocked(id string) (PolicyObject, error) {
	var (
		policy  PolicyObject
		ceiling int64
		created int64
		updated int64
	)
	err := s.db.QueryRow(
		`SELECT id, name, mode, permission_ceiling, policy_version, created_at, updated_at, updated_by
		 FROM policy_objects WHERE id = ?`, id,
	).Scan(&policy.ID, &policy.Name, &policy.Mode, &ceiling, &policy.PolicyVersion,
		&created, &updated, &policy.UpdatedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return PolicyObject{}, ErrUnknownPolicyObject
	}
	if err != nil {
		return PolicyObject{}, fmt.Errorf("account store %s: read policy object: %w", s.path, err)
	}
	policy.PermissionCeiling = uint32(ceiling)
	policy.CreatedAt = timeFromMillis(created)
	policy.UpdatedAt = timeFromMillis(updated)
	policy.AllowedUsers, err = s.policyObjectUsersLocked(id)
	if err != nil {
		return PolicyObject{}, err
	}
	policy.AllowedGroups, err = s.policyObjectGroupsLocked(id)
	if err != nil {
		return PolicyObject{}, err
	}
	return policy, nil
}

func (s *Store) policyObjectUsersLocked(id string) ([]string, error) {
	rows, err := s.db.Query(
		`SELECT u.username FROM policy_object_users pu
		 JOIN users u ON u.id = pu.user_id
		 WHERE pu.policy_id = ? ORDER BY u.username`, id)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list policy object users: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]string, 0)
	for rows.Next() {
		var username string
		if err := rows.Scan(&username); err != nil {
			return nil, fmt.Errorf("account store %s: scan policy object user: %w", s.path, err)
		}
		out = append(out, username)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list policy object users: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) policyObjectGroupsLocked(id string) ([]string, error) {
	rows, err := s.db.Query(
		`SELECT group_id FROM policy_object_groups WHERE policy_id = ? ORDER BY group_id`, id)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list policy object groups: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]string, 0)
	for rows.Next() {
		var groupID string
		if err := rows.Scan(&groupID); err != nil {
			return nil, fmt.Errorf("account store %s: scan policy object group: %w", s.path, err)
		}
		out = append(out, groupID)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list policy object groups: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) PolicyObject(id string) (PolicyObject, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.policyObjectLocked(strings.TrimSpace(id))
}

func (s *Store) PolicyObjects() ([]PolicyObject, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
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

// DeletePolicyObject refuses while assignments still exist. The console must
// remove them first so a delete never silently unmanages a device set.
func (s *Store) DeletePolicyObject(id string) error {
	id = strings.TrimSpace(id)
	s.mu.Lock()
	defer s.mu.Unlock()
	var assignments int
	if err := s.db.QueryRow(
		`SELECT COUNT(*) FROM policy_assignments WHERE policy_id = ?`, id,
	).Scan(&assignments); err != nil {
		return fmt.Errorf("account store %s: count policy assignments: %w", s.path, err)
	}
	if assignments > 0 {
		return ErrPolicyObjectInUse
	}
	res, err := s.db.Exec(`DELETE FROM policy_objects WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("account store %s: delete policy object: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownPolicyObject
	}
	return nil
}

func (s *Store) CreatePolicyAssignment(policyID, scopeKind, scopeValue string, priority int, actor string) (PolicyAssignment, error) {
	policyID = strings.TrimSpace(policyID)
	scopeKind = strings.TrimSpace(scopeKind)
	scopeValue = strings.TrimSpace(scopeValue)
	if !validPolicyScope(scopeKind, scopeValue) {
		return PolicyAssignment{}, ErrInvalidPolicyScope
	}
	if priority < -maxPolicyPriority || priority > maxPolicyPriority {
		return PolicyAssignment{}, ErrInvalidPolicyPriority
	}
	id, err := newRandomHex(8)
	if err != nil {
		return PolicyAssignment{}, fmt.Errorf("account store %s: generate assignment id: %w", s.path, err)
	}
	actor = normalizeText(actor, maxName)
	now := s.now().UTC()

	s.mu.Lock()
	defer s.mu.Unlock()
	var exists int
	if err := s.db.QueryRow(`SELECT COUNT(*) FROM policy_objects WHERE id = ?`, policyID).Scan(&exists); err != nil {
		return PolicyAssignment{}, fmt.Errorf("account store %s: check policy object: %w", s.path, err)
	}
	if exists == 0 {
		return PolicyAssignment{}, ErrUnknownPolicyObject
	}
	if scopeKind == PolicyScopeGroup {
		if err := s.db.QueryRow(`SELECT COUNT(*) FROM account_groups WHERE id = ?`, scopeValue).Scan(&exists); err != nil {
			return PolicyAssignment{}, fmt.Errorf("account store %s: check policy scope group: %w", s.path, err)
		}
		if exists == 0 {
			return PolicyAssignment{}, ErrUnknownPolicyScope
		}
	}
	var count int
	if err := s.db.QueryRow(`SELECT COUNT(*) FROM policy_assignments`).Scan(&count); err != nil {
		return PolicyAssignment{}, fmt.Errorf("account store %s: count policy assignments: %w", s.path, err)
	}
	if count >= maxPolicyAssignments {
		return PolicyAssignment{}, ErrPolicyAssignmentQuota
	}
	if _, err := s.db.Exec(
		`INSERT INTO policy_assignments
			(id, policy_id, scope_kind, scope_value, priority, created_at, created_by)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		id, policyID, scopeKind, scopeValue, priority, unixMillis(now), actor,
	); err != nil {
		if isUniqueViolation(err) {
			return PolicyAssignment{}, ErrPolicyAssignmentExists
		}
		return PolicyAssignment{}, fmt.Errorf("account store %s: create policy assignment: %w", s.path, err)
	}
	return s.policyAssignmentLocked(id)
}

// PolicyAssignmentBatch is the outcome of one device-scope bulk assignment:
// every requested device lands in Created or Skipped, never both. Skipped
// devices already carry an assignment, which the single-device endpoint would
// answer with ErrPolicyAssignmentExists.
type PolicyAssignmentBatch struct {
	PolicyID   string
	PolicyName string
	Created    []PolicyAssignment
	Skipped    []string
}

// CreateDevicePolicyAssignments binds one policy to many devices in a single
// transaction, so a quota failure or an insert error leaves no half-applied
// batch behind. Devices that already have an assignment are skipped rather
// than failing the call.
func (s *Store) CreateDevicePolicyAssignments(policyID string, devices []string, priority int, actor string) (PolicyAssignmentBatch, error) {
	policyID = strings.TrimSpace(policyID)
	if len(devices) == 0 || len(devices) > MaxPolicyAssignmentBatch {
		return PolicyAssignmentBatch{}, ErrInvalidPolicyAssignmentBatch
	}
	if priority < -maxPolicyPriority || priority > maxPolicyPriority {
		return PolicyAssignmentBatch{}, ErrInvalidPolicyPriority
	}
	actor = normalizeText(actor, maxName)
	now := s.now().UTC()

	s.mu.Lock()
	defer s.mu.Unlock()
	batch := PolicyAssignmentBatch{
		PolicyID: policyID,
		Created:  make([]PolicyAssignment, 0, len(devices)),
		Skipped:  make([]string, 0),
	}
	err := s.db.QueryRow(`SELECT name FROM policy_objects WHERE id = ?`, policyID).Scan(&batch.PolicyName)
	if errors.Is(err, sql.ErrNoRows) {
		return PolicyAssignmentBatch{}, ErrUnknownPolicyObject
	}
	if err != nil {
		return PolicyAssignmentBatch{}, fmt.Errorf("account store %s: check policy object: %w", s.path, err)
	}

	tx, err := s.db.Begin()
	if err != nil {
		return PolicyAssignmentBatch{}, fmt.Errorf("account store %s: begin policy assignment batch: %w", s.path, err)
	}
	defer tx.Rollback()
	seen := make(map[string]bool, len(devices))
	for _, deviceID := range devices {
		deviceID = strings.TrimSpace(deviceID)
		if seen[deviceID] {
			continue
		}
		if !registry.ValidID(deviceID) {
			return PolicyAssignmentBatch{}, ErrInvalidPolicyScope
		}
		seen[deviceID] = true
		id, err := newRandomHex(8)
		if err != nil {
			return PolicyAssignmentBatch{}, fmt.Errorf("account store %s: generate assignment id: %w", s.path, err)
		}
		res, err := tx.Exec(
			`INSERT INTO policy_assignments
				(id, policy_id, scope_kind, scope_value, priority, created_at, created_by)
			 VALUES (?, ?, ?, ?, ?, ?, ?)
			 ON CONFLICT(scope_kind, scope_value) DO NOTHING`,
			id, policyID, PolicyScopeDevice, deviceID, priority, unixMillis(now), actor,
		)
		if err != nil {
			return PolicyAssignmentBatch{}, fmt.Errorf("account store %s: create policy assignment batch: %w", s.path, err)
		}
		affected, err := res.RowsAffected()
		if err != nil {
			return PolicyAssignmentBatch{}, err
		}
		if affected == 0 {
			batch.Skipped = append(batch.Skipped, deviceID)
			continue
		}
		batch.Created = append(batch.Created, PolicyAssignment{
			ID:         id,
			PolicyID:   policyID,
			PolicyName: batch.PolicyName,
			ScopeKind:  PolicyScopeDevice,
			ScopeValue: deviceID,
			Priority:   priority,
			CreatedAt:  now,
			CreatedBy:  actor,
		})
	}
	var count int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM policy_assignments`).Scan(&count); err != nil {
		return PolicyAssignmentBatch{}, fmt.Errorf("account store %s: count policy assignments: %w", s.path, err)
	}
	if count > maxPolicyAssignments {
		return PolicyAssignmentBatch{}, ErrPolicyAssignmentQuota
	}
	if err := tx.Commit(); err != nil {
		return PolicyAssignmentBatch{}, fmt.Errorf("account store %s: commit policy assignment batch: %w", s.path, err)
	}
	return batch, nil
}

func (s *Store) policyAssignmentLocked(id string) (PolicyAssignment, error) {
	var (
		assignment PolicyAssignment
		created    int64
	)
	err := s.db.QueryRow(
		`SELECT a.id, a.policy_id, p.name, a.scope_kind, a.scope_value, a.priority, a.created_at, a.created_by
		 FROM policy_assignments a JOIN policy_objects p ON p.id = a.policy_id
		 WHERE a.id = ?`, id,
	).Scan(&assignment.ID, &assignment.PolicyID, &assignment.PolicyName,
		&assignment.ScopeKind, &assignment.ScopeValue, &assignment.Priority,
		&created, &assignment.CreatedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return PolicyAssignment{}, ErrUnknownPolicyAssignment
	}
	if err != nil {
		return PolicyAssignment{}, fmt.Errorf("account store %s: read policy assignment: %w", s.path, err)
	}
	assignment.CreatedAt = timeFromMillis(created)
	return assignment, nil
}

func (s *Store) PolicyAssignments() ([]PolicyAssignment, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT a.id, a.policy_id, p.name, a.scope_kind, a.scope_value, a.priority, a.created_at, a.created_by
		 FROM policy_assignments a JOIN policy_objects p ON p.id = a.policy_id
		 ORDER BY a.priority DESC, a.scope_kind, a.scope_value`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list policy assignments: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]PolicyAssignment, 0)
	for rows.Next() {
		var (
			assignment PolicyAssignment
			created    int64
		)
		if err := rows.Scan(&assignment.ID, &assignment.PolicyID, &assignment.PolicyName,
			&assignment.ScopeKind, &assignment.ScopeValue, &assignment.Priority,
			&created, &assignment.CreatedBy); err != nil {
			return nil, fmt.Errorf("account store %s: scan policy assignment: %w", s.path, err)
		}
		assignment.CreatedAt = timeFromMillis(created)
		out = append(out, assignment)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list policy assignments: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) DeletePolicyAssignment(id string) error {
	id = strings.TrimSpace(id)
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(`DELETE FROM policy_assignments WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("account store %s: delete policy assignment: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownPolicyAssignment
	}
	return nil
}

// resolvedPolicy is one assignment winner plus the template fields needed to
// materialise a device policy.
type resolvedPolicy struct {
	assignmentID  string
	policyID      string
	policyName    string
	policyVersion uint32
	priority      int
	scopeKind     string
	mode          string
	ceiling       uint32
	userIDs       []string
	groupIDs      []string
}

// resolveDevicePolicyLocked picks the assignment that governs peerID. Manual
// per-device policies are not considered here: callers check for them first.
func (s *Store) resolveDevicePolicyLocked(peerID string) (*resolvedPolicy, error) {
	var (
		resolved resolvedPolicy
		ceiling  int64
	)
	err := s.db.QueryRow(
		`SELECT a.id, p.id, p.name, p.policy_version, a.priority, a.scope_kind,
		        p.mode, p.permission_ceiling
		 FROM policy_assignments a JOIN policy_objects p ON p.id = a.policy_id
		 WHERE a.scope_kind = ?
		    OR (a.scope_kind = ? AND a.scope_value = ?)
		    OR (a.scope_kind = ? AND a.scope_value IN (
		          SELECT group_id FROM account_group_devices WHERE peer_id = ?))
		 ORDER BY a.priority DESC,
		          CASE a.scope_kind WHEN ? THEN 3 WHEN ? THEN 2 ELSE 1 END DESC,
		          p.updated_at DESC, p.id ASC
		 LIMIT 1`,
		PolicyScopeAll, PolicyScopeDevice, peerID, PolicyScopeGroup, peerID,
		PolicyScopeDevice, PolicyScopeGroup,
	).Scan(&resolved.assignmentID, &resolved.policyID, &resolved.policyName,
		&resolved.policyVersion, &resolved.priority, &resolved.scopeKind,
		&resolved.mode, &ceiling)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("account store %s: resolve device policy: %w", s.path, err)
	}
	resolved.ceiling = uint32(ceiling)
	resolved.userIDs, err = s.policyObjectUserIDsLocked(resolved.policyID)
	if err != nil {
		return nil, err
	}
	resolved.groupIDs, err = s.policyObjectGroupIDsLocked(resolved.policyID)
	if err != nil {
		return nil, err
	}
	return &resolved, nil
}

func (s *Store) policyObjectUserIDsLocked(id string) ([]string, error) {
	rows, err := s.db.Query(`SELECT user_id FROM policy_object_users WHERE policy_id = ? ORDER BY user_id`, id)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list policy object user ids: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]string, 0)
	for rows.Next() {
		var userID string
		if err := rows.Scan(&userID); err != nil {
			return nil, fmt.Errorf("account store %s: scan policy object user id: %w", s.path, err)
		}
		out = append(out, userID)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list policy object user ids: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) policyObjectGroupIDsLocked(id string) ([]string, error) {
	rows, err := s.db.Query(`SELECT group_id FROM policy_object_groups WHERE policy_id = ? ORDER BY group_id`, id)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list policy object group ids: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]string, 0)
	for rows.Next() {
		var groupID string
		if err := rows.Scan(&groupID); err != nil {
			return nil, fmt.Errorf("account store %s: scan policy object group id: %w", s.path, err)
		}
		out = append(out, groupID)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list policy object group ids: %w", s.path, err)
	}
	return out, nil
}

// devicePolicyBindingLocked reports the current row's origin and content so a
// recompute can skip a manual override and avoid bumping an unchanged policy.
func (s *Store) devicePolicyBindingLocked(peerID string) (sourceKind, sourceID string, sourceVersion uint32, sourcePriority int, mode string, ceiling int64, userIDs, groupIDs []string, err error) {
	err = s.db.QueryRow(
		`SELECT mode, permission_ceiling, source_kind, source_id, source_version, source_priority
		 FROM device_policies WHERE peer_id = ?`, peerID,
	).Scan(&mode, &ceiling, &sourceKind, &sourceID, &sourceVersion, &sourcePriority)
	if errors.Is(err, sql.ErrNoRows) {
		err = nil
		sourceKind = ""
		return
	}
	if err != nil {
		err = fmt.Errorf("account store %s: read device policy binding: %w", s.path, err)
		return
	}
	userIDs, err = s.devicePolicyUserIDsLocked(peerID)
	if err != nil {
		return
	}
	groupIDs, err = s.devicePolicyGroupIDsLocked(peerID)
	return
}

func (s *Store) devicePolicyUserIDsLocked(peerID string) ([]string, error) {
	rows, err := s.db.Query(`SELECT user_id FROM device_policy_users WHERE peer_id = ? ORDER BY user_id`, peerID)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list device policy user ids: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]string, 0)
	for rows.Next() {
		var userID string
		if err := rows.Scan(&userID); err != nil {
			return nil, fmt.Errorf("account store %s: scan device policy user id: %w", s.path, err)
		}
		out = append(out, userID)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list device policy user ids: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) devicePolicyGroupIDsLocked(peerID string) ([]string, error) {
	rows, err := s.db.Query(`SELECT group_id FROM device_policy_groups WHERE peer_id = ? ORDER BY group_id`, peerID)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list device policy group ids: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]string, 0)
	for rows.Next() {
		var groupID string
		if err := rows.Scan(&groupID); err != nil {
			return nil, fmt.Errorf("account store %s: scan device policy group id: %w", s.path, err)
		}
		out = append(out, groupID)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list device policy group ids: %w", s.path, err)
	}
	return out, nil
}

// ReconcileDevicePolicies re-materialises the assigned policy for peerIDs.
// Manual per-device policies are left untouched, unchanged assignments keep
// their version, changed assignments bump the per-device version, and devices
// that fall back to no assignment lose the automatic row (becoming unmanaged).
// It returns only rows that changed and the peers that became unmanaged so the
// caller can publish exactly the needed events.
func (s *Store) ReconcileDevicePolicies(peerIDs []string) ([]DevicePolicy, []string, error) {
	seen := make(map[string]struct{}, len(peerIDs))
	ordered := make([]string, 0, len(peerIDs))
	for _, peerID := range peerIDs {
		peerID = strings.TrimSpace(peerID)
		if peerID == "" {
			continue
		}
		if _, ok := seen[peerID]; ok {
			continue
		}
		seen[peerID] = struct{}{}
		ordered = append(ordered, peerID)
	}
	sort.Strings(ordered)

	s.mu.Lock()
	defer s.mu.Unlock()
	changed := make([]DevicePolicy, 0)
	unmanaged := make([]string, 0)
	for _, peerID := range ordered {
		policy, removed, err := s.reconcileDevicePolicyLocked(peerID)
		if err != nil {
			return nil, nil, err
		}
		if removed {
			unmanaged = append(unmanaged, peerID)
			continue
		}
		if policy != nil {
			changed = append(changed, *policy)
		}
	}
	return changed, unmanaged, nil
}

// ReconcileAllDevicePolicies covers every device the store knows about: rows
// that already carry a policy plus devices that belong to account groups.
// Devices that register later are reconciled when the server reads their
// policy for registration.
func (s *Store) ReconcileAllDevicePolicies() ([]DevicePolicy, []string, error) {
	s.mu.Lock()
	rows, err := s.db.Query(
		`SELECT peer_id FROM device_policies
		 UNION SELECT peer_id FROM account_group_devices`)
	if err != nil {
		s.mu.Unlock()
		return nil, nil, fmt.Errorf("account store %s: list policy devices: %w", s.path, err)
	}
	peers := make([]string, 0)
	for rows.Next() {
		var peerID string
		if err := rows.Scan(&peerID); err != nil {
			rows.Close()
			s.mu.Unlock()
			return nil, nil, fmt.Errorf("account store %s: scan policy device: %w", s.path, err)
		}
		peers = append(peers, peerID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		s.mu.Unlock()
		return nil, nil, fmt.Errorf("account store %s: list policy devices: %w", s.path, err)
	}
	rows.Close()
	s.mu.Unlock()
	return s.ReconcileDevicePolicies(peers)
}

func (s *Store) reconcileDevicePolicyLocked(peerID string) (*DevicePolicy, bool, error) {
	if !registry.ValidID(peerID) {
		return nil, false, ErrInvalidPeerID
	}
	sourceKind, sourceID, sourceVersion, sourcePriority, mode, ceiling, userIDs, groupIDs, err :=
		s.devicePolicyBindingLocked(peerID)
	if err != nil {
		return nil, false, err
	}
	if sourceKind == "manual" {
		return nil, false, nil
	}
	resolved, err := s.resolveDevicePolicyLocked(peerID)
	if err != nil {
		return nil, false, err
	}
	if resolved == nil {
		if sourceKind == "" {
			return nil, false, nil
		}
		tx, err := s.db.Begin()
		if err != nil {
			return nil, false, fmt.Errorf("account store %s: begin policy removal: %w", s.path, err)
		}
		defer tx.Rollback()
		for _, statement := range []string{
			`DELETE FROM device_policy_users WHERE peer_id = ?`,
			`DELETE FROM device_policy_groups WHERE peer_id = ?`,
			`DELETE FROM device_policy_receipts WHERE peer_id = ?`,
		} {
			if _, err := tx.Exec(statement, peerID); err != nil {
				return nil, false, fmt.Errorf("account store %s: clear assigned policy: %w", s.path, err)
			}
		}
		if _, err := tx.Exec(`DELETE FROM device_policies WHERE peer_id = ?`, peerID); err != nil {
			return nil, false, fmt.Errorf("account store %s: delete assigned policy: %w", s.path, err)
		}
		if err := tx.Commit(); err != nil {
			return nil, false, fmt.Errorf("account store %s: commit policy removal: %w", s.path, err)
		}
		return nil, true, nil
	}

	unchanged := sourceKind == "policy" &&
		sourceID == resolved.policyID &&
		sourceVersion == resolved.policyVersion &&
		sourcePriority == resolved.priority &&
		mode == resolved.mode &&
		uint32(ceiling) == resolved.ceiling &&
		sameStringSet(userIDs, resolved.userIDs) &&
		sameStringSet(groupIDs, resolved.groupIDs)
	if unchanged {
		return nil, false, nil
	}

	now := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return nil, false, fmt.Errorf("account store %s: begin policy materialisation: %w", s.path, err)
	}
	defer tx.Rollback()
	next, err := s.nextDevicePolicyVersionTx(tx, peerID)
	if err != nil {
		return nil, false, err
	}
	updatedBy := normalizeText("policy:"+resolved.policyName, maxName)
	if sourceKind == "" {
		if _, err := tx.Exec(
			`INSERT INTO device_policies
				(peer_id, mode, permission_ceiling, policy_version, updated_at, updated_by,
				 source_kind, source_id, source_version, source_priority)
			 VALUES (?, ?, ?, ?, ?, ?, 'policy', ?, ?, ?)`,
			peerID, resolved.mode, int64(resolved.ceiling), next, unixMillis(now), updatedBy,
			resolved.policyID, int64(resolved.policyVersion), int64(resolved.priority),
		); err != nil {
			return nil, false, fmt.Errorf("account store %s: create assigned policy: %w", s.path, err)
		}
	} else {
		if _, err := tx.Exec(
			`UPDATE device_policies
			 SET mode = ?, permission_ceiling = ?, policy_version = ?,
			     updated_at = ?, updated_by = ?,
			     source_kind = 'policy', source_id = ?, source_version = ?, source_priority = ?
			 WHERE peer_id = ?`,
			resolved.mode, int64(resolved.ceiling), next, unixMillis(now), updatedBy,
			resolved.policyID, int64(resolved.policyVersion), int64(resolved.priority), peerID,
		); err != nil {
			return nil, false, fmt.Errorf("account store %s: update assigned policy: %w", s.path, err)
		}
	}
	if _, err := tx.Exec(`DELETE FROM device_policy_users WHERE peer_id = ?`, peerID); err != nil {
		return nil, false, fmt.Errorf("account store %s: clear assigned policy users: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM device_policy_groups WHERE peer_id = ?`, peerID); err != nil {
		return nil, false, fmt.Errorf("account store %s: clear assigned policy groups: %w", s.path, err)
	}
	for _, userID := range resolved.userIDs {
		if _, err := tx.Exec(
			`INSERT INTO device_policy_users (peer_id, user_id, created_at) VALUES (?, ?, ?)`,
			peerID, userID, unixMillis(now),
		); err != nil {
			return nil, false, fmt.Errorf("account store %s: add assigned policy user: %w", s.path, err)
		}
	}
	for _, groupID := range resolved.groupIDs {
		if _, err := tx.Exec(
			`INSERT INTO device_policy_groups (peer_id, group_id, created_at) VALUES (?, ?, ?)`,
			peerID, groupID, unixMillis(now),
		); err != nil {
			return nil, false, fmt.Errorf("account store %s: add assigned policy group: %w", s.path, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return nil, false, fmt.Errorf("account store %s: commit policy materialisation: %w", s.path, err)
	}
	policy, err := s.devicePolicyLocked(peerID)
	if err != nil {
		return nil, false, err
	}
	return &policy, false, nil
}

func sameStringSet(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	seen := make(map[string]int, len(a))
	for _, value := range a {
		seen[value]++
	}
	for _, value := range b {
		seen[value]--
		if seen[value] < 0 {
			return false
		}
	}
	return true
}
