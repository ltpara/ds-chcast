package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// Device policy modes. A device without a policy row is unmanaged and keeps
// the v61 local-authentication behavior. A managed open device still needs a
// central credential but does not restrict which account may connect; a
// restricted device admits only the listed users and groups. Disabling a
// device is expressed as restricted with an empty allow list, so there is no
// third mode that would silently bypass the ACL.
const (
	DevicePolicyOpen       = "open"
	DevicePolicyRestricted = "restricted"
)

const (
	maxDevicePolicyUsers  = 1024
	maxDevicePolicyGroups = 256
	maxPolicyDetail       = 512
)

var (
	ErrInvalidDevicePolicyMode  = errors.New("device policy mode must be open or restricted")
	ErrInvalidPermissionCeiling = errors.New("permission ceiling must fit the 32-bit permission mask")
	ErrUnknownDevicePolicy      = errors.New("unknown device policy")
	ErrStaleDevicePolicyVersion = errors.New("device policy version does not match the current version")
	ErrDevicePolicyUser         = errors.New("device policy references an unknown account")
	ErrDevicePolicyGroup        = errors.New("device policy references an unknown group")
	ErrDevicePolicyTooMany      = errors.New("device policy allow list is too large")
)

// DevicePolicy is the durable admission record for one device. PolicyVersion
// increases on every accepted edit; a host acknowledges the exact version it
// applied so a console never reports an older policy as current.
type DevicePolicy struct {
	PeerID            string               `json:"peer_id"`
	Mode              string               `json:"mode"`
	PermissionCeiling uint32               `json:"permission_ceiling"`
	PolicyVersion     uint32               `json:"policy_version"`
	UpdatedAt         time.Time            `json:"updated_at"`
	UpdatedBy         string               `json:"updated_by,omitempty"`
	AllowedUsers      []string             `json:"allowed_users"`
	AllowedGroups     []string             `json:"allowed_groups"`
	Receipt           *DevicePolicyReceipt `json:"receipt,omitempty"`
}

// DevicePolicyReceipt records one host's application result. It is not a
// promise that the device is protected: applied=false keeps the previous
// failure visible until a later version succeeds.
type DevicePolicyReceipt struct {
	PolicyVersion uint32    `json:"policy_version"`
	Applied       bool      `json:"applied"`
	Detail        string    `json:"detail,omitempty"`
	ReportedAt    time.Time `json:"reported_at"`
}

// DeviceAdmission is the stateless answer lookup needs before it mints a
// credential. PermissionCeiling is zero when the policy does not cap the
// normal permission mask.
type DeviceAdmission struct {
	Managed           bool
	Allowed           bool
	Reason            string
	PolicyVersion     uint32
	PermissionCeiling uint32
}

// SetDevicePolicy creates or edits one policy and increments its version in
// the same transaction as the allow lists, so a host cannot observe a new
// version with an old ACL. Usernames are resolved to stable account ids.
func (s *Store) SetDevicePolicy(peerID, mode string, ceiling uint32, usernames, groupIDs []string, updatedBy string) (DevicePolicy, error) {
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return DevicePolicy{}, ErrInvalidPeerID
	}
	mode = strings.TrimSpace(mode)
	if mode == "" {
		mode = DevicePolicyOpen
	}
	if mode != DevicePolicyOpen && mode != DevicePolicyRestricted {
		return DevicePolicy{}, ErrInvalidDevicePolicyMode
	}
	if len(usernames) > maxDevicePolicyUsers || len(groupIDs) > maxDevicePolicyGroups {
		return DevicePolicy{}, ErrDevicePolicyTooMany
	}
	updatedBy = normalizeText(updatedBy, maxName)
	updated := s.now().UTC()

	userIDs, err := s.resolvePolicyUsers(usernames)
	if err != nil {
		return DevicePolicy{}, err
	}
	groups, err := s.resolvePolicyGroups(groupIDs)
	if err != nil {
		return DevicePolicy{}, err
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: begin device policy: %w", s.path, err)
	}
	defer tx.Rollback()

	var current uint32
	err = tx.QueryRow(`SELECT policy_version FROM device_policies WHERE peer_id = ?`, peerID).Scan(&current)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		if _, err := tx.Exec(
			`INSERT INTO device_policies
				(peer_id, mode, permission_ceiling, policy_version, updated_at, updated_by)
			 VALUES (?, ?, ?, 1, ?, ?)`,
			peerID, mode, int64(ceiling), unixMillis(updated), updatedBy,
		); err != nil {
			return DevicePolicy{}, fmt.Errorf("account store %s: create device policy: %w", s.path, err)
		}
	case err != nil:
		return DevicePolicy{}, fmt.Errorf("account store %s: read device policy: %w", s.path, err)
	default:
		if _, err := tx.Exec(
			`UPDATE device_policies
			 SET mode = ?, permission_ceiling = ?, policy_version = policy_version + 1,
			     updated_at = ?, updated_by = ?
			 WHERE peer_id = ?`,
			mode, int64(ceiling), unixMillis(updated), updatedBy, peerID,
		); err != nil {
			return DevicePolicy{}, fmt.Errorf("account store %s: update device policy: %w", s.path, err)
		}
	}

	if _, err := tx.Exec(`DELETE FROM device_policy_users WHERE peer_id = ?`, peerID); err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: clear device policy users: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM device_policy_groups WHERE peer_id = ?`, peerID); err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: clear device policy groups: %w", s.path, err)
	}
	for _, userID := range userIDs {
		if _, err := tx.Exec(
			`INSERT INTO device_policy_users (peer_id, user_id, created_at) VALUES (?, ?, ?)`,
			peerID, userID, unixMillis(updated),
		); err != nil {
			return DevicePolicy{}, fmt.Errorf("account store %s: add device policy user: %w", s.path, err)
		}
	}
	for _, groupID := range groups {
		if _, err := tx.Exec(
			`INSERT INTO device_policy_groups (peer_id, group_id, created_at) VALUES (?, ?, ?)`,
			peerID, groupID, unixMillis(updated),
		); err != nil {
			return DevicePolicy{}, fmt.Errorf("account store %s: add device policy group: %w", s.path, err)
		}
	}
	if err := tx.Commit(); err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: commit device policy: %w", s.path, err)
	}
	return s.devicePolicyLocked(peerID)
}

func (s *Store) resolvePolicyUsers(usernames []string) ([]string, error) {
	seen := make(map[string]struct{}, len(usernames))
	out := make([]string, 0, len(usernames))
	for _, username := range usernames {
		username = strings.TrimSpace(username)
		if username == "" {
			return nil, ErrDevicePolicyUser
		}
		if _, ok := seen[strings.ToLower(username)]; ok {
			continue
		}
		var userID string
		err := s.db.QueryRow(`SELECT id FROM users WHERE username = ?`, username).Scan(&userID)
		if errors.Is(err, sql.ErrNoRows) {
			return nil, fmt.Errorf("%w: %s", ErrDevicePolicyUser, username)
		}
		if err != nil {
			return nil, fmt.Errorf("account store %s: resolve policy user: %w", s.path, err)
		}
		seen[strings.ToLower(username)] = struct{}{}
		out = append(out, userID)
	}
	return out, nil
}

func (s *Store) resolvePolicyGroups(groupIDs []string) ([]string, error) {
	seen := make(map[string]struct{}, len(groupIDs))
	out := make([]string, 0, len(groupIDs))
	for _, groupID := range groupIDs {
		groupID = strings.TrimSpace(groupID)
		if !registry.ValidID(groupID) {
			return nil, fmt.Errorf("%w: %s", ErrDevicePolicyGroup, groupID)
		}
		if _, ok := seen[groupID]; ok {
			continue
		}
		var exists int
		if err := s.db.QueryRow(`SELECT COUNT(*) FROM account_groups WHERE id = ?`, groupID).Scan(&exists); err != nil {
			return nil, fmt.Errorf("account store %s: resolve policy group: %w", s.path, err)
		}
		if exists == 0 {
			return nil, fmt.Errorf("%w: %s", ErrDevicePolicyGroup, groupID)
		}
		seen[groupID] = struct{}{}
		out = append(out, groupID)
	}
	return out, nil
}

// DevicePolicy returns the stored policy and whether the device is managed.
// A missing row is not an error; it is the unmanaged open state that keeps the
// existing local-authentication behavior.
func (s *Store) DevicePolicy(peerID string) (DevicePolicy, bool, error) {
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return DevicePolicy{}, false, ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	policy, err := s.devicePolicyLocked(peerID)
	if errors.Is(err, ErrUnknownDevicePolicy) {
		return DevicePolicy{}, false, nil
	}
	if err != nil {
		return DevicePolicy{}, false, err
	}
	return policy, true, nil
}

func (s *Store) devicePolicyLocked(peerID string) (DevicePolicy, error) {
	var (
		policy    DevicePolicy
		ceiling   int64
		updated   int64
		receipt   DevicePolicyReceipt
		applied   int
		reported  int64
		hasResult bool
	)
	err := s.db.QueryRow(
		`SELECT peer_id, mode, permission_ceiling, policy_version, updated_at, updated_by
		 FROM device_policies WHERE peer_id = ?`, peerID,
	).Scan(&policy.PeerID, &policy.Mode, &ceiling, &policy.PolicyVersion, &updated, &policy.UpdatedBy)
	if errors.Is(err, sql.ErrNoRows) {
		return DevicePolicy{}, ErrUnknownDevicePolicy
	}
	if err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: read device policy: %w", s.path, err)
	}
	policy.PermissionCeiling = uint32(ceiling)
	policy.UpdatedAt = timeFromMillis(updated)

	rows, err := s.db.Query(
		`SELECT u.username FROM device_policy_users pu
		 JOIN users u ON u.id = pu.user_id
		 WHERE pu.peer_id = ? ORDER BY u.username`, peerID)
	if err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: list device policy users: %w", s.path, err)
	}
	policy.AllowedUsers = make([]string, 0)
	for rows.Next() {
		var username string
		if err := rows.Scan(&username); err != nil {
			rows.Close()
			return DevicePolicy{}, fmt.Errorf("account store %s: scan device policy user: %w", s.path, err)
		}
		policy.AllowedUsers = append(policy.AllowedUsers, username)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return DevicePolicy{}, fmt.Errorf("account store %s: list device policy users: %w", s.path, err)
	}
	rows.Close()

	rows, err = s.db.Query(
		`SELECT group_id FROM device_policy_groups WHERE peer_id = ? ORDER BY group_id`, peerID)
	if err != nil {
		return DevicePolicy{}, fmt.Errorf("account store %s: list device policy groups: %w", s.path, err)
	}
	policy.AllowedGroups = make([]string, 0)
	for rows.Next() {
		var groupID string
		if err := rows.Scan(&groupID); err != nil {
			rows.Close()
			return DevicePolicy{}, fmt.Errorf("account store %s: scan device policy group: %w", s.path, err)
		}
		policy.AllowedGroups = append(policy.AllowedGroups, groupID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return DevicePolicy{}, fmt.Errorf("account store %s: list device policy groups: %w", s.path, err)
	}
	rows.Close()

	err = s.db.QueryRow(
		`SELECT policy_version, applied, detail, reported_at FROM device_policy_receipts WHERE peer_id = ?`,
		peerID,
	).Scan(&receipt.PolicyVersion, &applied, &receipt.Detail, &reported)
	switch {
	case err == nil:
		hasResult = true
		receipt.Applied = applied != 0
		receipt.ReportedAt = timeFromMillis(reported)
	case errors.Is(err, sql.ErrNoRows):
	default:
		return DevicePolicy{}, fmt.Errorf("account store %s: read device policy receipt: %w", s.path, err)
	}
	if hasResult {
		policy.Receipt = &receipt
	}
	return policy, nil
}

// DevicePolicies lists every managed device. Unmanaged devices have no row and
// are therefore absent from the console list instead of appearing as open.
func (s *Store) DevicePolicies() ([]DevicePolicy, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT peer_id FROM device_policies ORDER BY peer_id`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list device policies: %w", s.path, err)
	}
	ids := make([]string, 0)
	for rows.Next() {
		var peerID string
		if err := rows.Scan(&peerID); err != nil {
			rows.Close()
			return nil, fmt.Errorf("account store %s: scan device policy: %w", s.path, err)
		}
		ids = append(ids, peerID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, fmt.Errorf("account store %s: list device policies: %w", s.path, err)
	}
	rows.Close()
	out := make([]DevicePolicy, 0, len(ids))
	for _, peerID := range ids {
		policy, err := s.devicePolicyLocked(peerID)
		if err != nil {
			return nil, err
		}
		out = append(out, policy)
	}
	return out, nil
}

// DeleteDevicePolicy returns a device to the unmanaged state. It is an
// explicit, audited console action; damage to a managed configuration must not
// be repaired by silently deleting the record on the host side.
func (s *Store) DeleteDevicePolicy(peerID string) error {
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin device policy delete: %w", s.path, err)
	}
	defer tx.Rollback()
	for _, statement := range []string{
		`DELETE FROM device_policy_users WHERE peer_id = ?`,
		`DELETE FROM device_policy_groups WHERE peer_id = ?`,
		`DELETE FROM device_policy_receipts WHERE peer_id = ?`,
	} {
		if _, err := tx.Exec(statement, peerID); err != nil {
			return fmt.Errorf("account store %s: delete device policy children: %w", s.path, err)
		}
	}
	res, err := tx.Exec(`DELETE FROM device_policies WHERE peer_id = ?`, peerID)
	if err != nil {
		return fmt.Errorf("account store %s: delete device policy: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownDevicePolicy
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit device policy delete: %w", s.path, err)
	}
	return nil
}

// RecordDevicePolicyReceipt stores the host's application result for the exact
// current version. A stale report is rejected rather than overwriting the
// console's view of a newer policy.
func (s *Store) RecordDevicePolicyReceipt(peerID string, version uint32, applied bool, detail string) (DevicePolicyReceipt, error) {
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return DevicePolicyReceipt{}, ErrInvalidPeerID
	}
	detail = normalizeText(detail, maxPolicyDetail)
	reported := s.now().UTC()
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return DevicePolicyReceipt{}, fmt.Errorf("account store %s: begin device policy receipt: %w", s.path, err)
	}
	defer tx.Rollback()
	var current uint32
	err = tx.QueryRow(`SELECT policy_version FROM device_policies WHERE peer_id = ?`, peerID).Scan(&current)
	if errors.Is(err, sql.ErrNoRows) {
		return DevicePolicyReceipt{}, ErrUnknownDevicePolicy
	}
	if err != nil {
		return DevicePolicyReceipt{}, fmt.Errorf("account store %s: read device policy version: %w", s.path, err)
	}
	if version != current {
		return DevicePolicyReceipt{}, fmt.Errorf("%w: got %d, current %d", ErrStaleDevicePolicyVersion, version, current)
	}
	stored := 0
	if applied {
		stored = 1
	}
	var exists int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM device_policy_receipts WHERE peer_id = ?`, peerID).Scan(&exists); err != nil {
		return DevicePolicyReceipt{}, fmt.Errorf("account store %s: check device policy receipt: %w", s.path, err)
	}
	if exists == 0 {
		_, err = tx.Exec(
			`INSERT INTO device_policy_receipts (peer_id, policy_version, applied, detail, reported_at)
			 VALUES (?, ?, ?, ?, ?)`,
			peerID, version, stored, detail, unixMillis(reported),
		)
	} else {
		_, err = tx.Exec(
			`UPDATE device_policy_receipts SET policy_version = ?, applied = ?, detail = ?, reported_at = ?
			 WHERE peer_id = ?`,
			version, stored, detail, unixMillis(reported), peerID,
		)
	}
	if err != nil {
		return DevicePolicyReceipt{}, fmt.Errorf("account store %s: store device policy receipt: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return DevicePolicyReceipt{}, fmt.Errorf("account store %s: commit device policy receipt: %w", s.path, err)
	}
	return DevicePolicyReceipt{
		PolicyVersion: version,
		Applied:       applied,
		Detail:        detail,
		ReportedAt:    reported,
	}, nil
}

// CheckDeviceAdmission answers whether an account may reach a device under the
// current policy. It is intentionally stateless: lease issuance, clock bounds
// and transport credentials belong to the next G02 slices. An empty userID is
// never admitted by a restricted policy because there is no subject to match.
func (s *Store) CheckDeviceAdmission(peerID, userID string) (DeviceAdmission, error) {
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return DeviceAdmission{}, ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		mode    string
		ceiling int64
		version uint32
	)
	err := s.db.QueryRow(
		`SELECT mode, permission_ceiling, policy_version FROM device_policies WHERE peer_id = ?`, peerID,
	).Scan(&mode, &ceiling, &version)
	if errors.Is(err, sql.ErrNoRows) {
		return DeviceAdmission{Managed: false, Allowed: true, Reason: "unmanaged"}, nil
	}
	if err != nil {
		return DeviceAdmission{}, fmt.Errorf("account store %s: read device admission: %w", s.path, err)
	}
	admission := DeviceAdmission{
		Managed:           true,
		PolicyVersion:     version,
		PermissionCeiling: uint32(ceiling),
	}
	if mode == DevicePolicyOpen {
		admission.Allowed = true
		admission.Reason = "policy open"
		return admission, nil
	}
	userID = strings.TrimSpace(userID)
	if userID == "" {
		admission.Reason = "no account subject"
		return admission, nil
	}
	var banned int
	err = s.db.QueryRow(`SELECT banned FROM users WHERE id = ?`, userID).Scan(&banned)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		admission.Reason = "no account subject"
		return admission, nil
	case err != nil:
		return DeviceAdmission{}, fmt.Errorf("account store %s: read policy subject: %w", s.path, err)
	case banned != 0:
		admission.Reason = "account disabled"
		return admission, nil
	}
	var direct int
	if err := s.db.QueryRow(
		`SELECT COUNT(*) FROM device_policy_users WHERE peer_id = ? AND user_id = ?`, peerID, userID,
	).Scan(&direct); err != nil {
		return DeviceAdmission{}, fmt.Errorf("account store %s: check device policy user: %w", s.path, err)
	}
	if direct > 0 {
		admission.Allowed = true
		admission.Reason = "user allowed"
		return admission, nil
	}
	var viaGroup int
	if err := s.db.QueryRow(
		`SELECT COUNT(*) FROM device_policy_groups pg
		 JOIN account_group_members gm ON gm.group_id = pg.group_id
		 WHERE pg.peer_id = ? AND gm.user_id = ?`, peerID, userID,
	).Scan(&viaGroup); err != nil {
		return DeviceAdmission{}, fmt.Errorf("account store %s: check device policy group: %w", s.path, err)
	}
	if viaGroup > 0 {
		admission.Allowed = true
		admission.Reason = "group allowed"
		return admission, nil
	}
	admission.Reason = "not allowed by device policy"
	return admission, nil
}
