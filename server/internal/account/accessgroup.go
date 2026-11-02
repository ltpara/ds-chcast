package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// Access-group roles are scoped to one group and independent of the global
// account role. Readonly may list the group and its audit records, write may
// also assign devices, and admin may additionally manage members and quotas.
const (
	GroupRoleReadonly = "readonly"
	GroupRoleWrite    = "write"
	GroupRoleAdmin    = "admin"

	maxGroupName        = 64
	maxGroupDescription = 256
)

// ValidGroupRole reports whether role names an access-group role.
func ValidGroupRole(role string) bool {
	switch role {
	case GroupRoleReadonly, GroupRoleWrite, GroupRoleAdmin:
		return true
	}
	return false
}

// AccessGroup authorizes a set of devices for its members. Device IDs are the
// same rendezvous IDs used by the registry; a device may belong to several
// groups. MaxDevices and MaxMembers are quotas where zero means unlimited.
type AccessGroup struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Description string    `json:"description,omitempty"`
	MaxDevices  int       `json:"max_devices"`
	MaxMembers  int       `json:"max_members"`
	CreatedAt   time.Time `json:"created_at"`
	DeviceCount int       `json:"device_count"`
	MemberCount int       `json:"member_count"`
	// Role is the caller's group role when the group was listed for one
	// account; it is empty on unscoped reads.
	Role string `json:"role,omitempty"`
}

// AccessGroupMember is one account's membership in a group. Banned accounts
// stay listed so the console can explain why a group lost its last usable
// admin, but they cannot sign in and use their membership.
type AccessGroupMember struct {
	Username  string    `json:"username"`
	Role      string    `json:"role"`
	CreatedAt time.Time `json:"created_at"`
	Banned    bool      `json:"banned,omitempty"`
}

// AccessGroupDevice is one device assignment inside a group.
type AccessGroupDevice struct {
	DeviceID  string    `json:"device_id"`
	CreatedAt time.Time `json:"created_at"`
}

// AccessGroupUpdate carries the fields a PATCH may change; nil leaves a field
// untouched.
type AccessGroupUpdate struct {
	Name        *string
	Description *string
	MaxDevices  *int
	MaxMembers  *int
}

var (
	ErrInvalidAccessGroupName        = errors.New("access group name must be 1..64 characters")
	ErrInvalidAccessGroupDescription = errors.New("access group description is too long")
	ErrInvalidAccessGroupQuota       = errors.New("access group quotas must not be negative")
	ErrAccessGroupExists             = errors.New("access group already exists")
	ErrUnknownAccessGroup            = errors.New("unknown access group")
	ErrInvalidGroupRole              = errors.New("group role must be readonly, write or admin")
	ErrLastGroupAdmin                = errors.New("at least one group admin must remain")
	ErrAccessGroupDeviceQuota        = errors.New("access group device quota is exhausted")
	ErrAccessGroupMemberQuota        = errors.New("access group member quota is exhausted")
	ErrAccessGroupQuotaBelow         = errors.New("access group quota cannot be below current usage")
	ErrInvalidGroupDeviceID          = errors.New("device id must be 1..64 characters of [A-Za-z0-9_.-]")
	ErrUnknownGroupDevice            = errors.New("device is not assigned to the group")
)

func normalizeAccessGroupName(name string) (string, error) {
	name = strings.TrimSpace(name)
	if name == "" || len(name) > maxGroupName {
		return "", ErrInvalidAccessGroupName
	}
	return name, nil
}

func validateAccessGroupQuotas(maxDevices, maxMembers int) error {
	if maxDevices < 0 || maxMembers < 0 {
		return ErrInvalidAccessGroupQuota
	}
	return nil
}

// CreateAccessGroup adds a group and, when creatorUserID is set, makes that
// account its first admin. A group created with the server key may start with
// no members; server-key operators can always manage it, and the last-admin
// guard starts applying as soon as an admin joins.
func (s *Store) CreateAccessGroup(name, description string, maxDevices, maxMembers int, creatorUserID string) (AccessGroup, error) {
	name, err := normalizeAccessGroupName(name)
	if err != nil {
		return AccessGroup{}, err
	}
	description = strings.TrimSpace(description)
	if len(description) > maxGroupDescription {
		return AccessGroup{}, fmt.Errorf("%w: at most %d characters", ErrInvalidAccessGroupDescription, maxGroupDescription)
	}
	if err := validateAccessGroupQuotas(maxDevices, maxMembers); err != nil {
		return AccessGroup{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	group, err := s.createAccessGroupLocked(name, description, maxDevices, maxMembers, creatorUserID)
	if err != nil {
		return AccessGroup{}, err
	}
	return group, nil
}

func (s *Store) createAccessGroupLocked(name, description string, maxDevices, maxMembers int, creatorUserID string) (AccessGroup, error) {
	id, err := newRandomHex(12)
	if err != nil {
		return AccessGroup{}, err
	}
	created := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return AccessGroup{}, fmt.Errorf("account store %s: begin access group: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(
		`INSERT INTO access_groups (id, name, description, max_devices, max_members, created_at)
		 VALUES (?, ?, ?, ?, ?, ?)`,
		id, name, description, maxDevices, maxMembers, unixMillis(created),
	); err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			return AccessGroup{}, ErrAccessGroupExists
		}
		return AccessGroup{}, fmt.Errorf("account store %s: create access group: %w", s.path, err)
	}
	memberCount := 0
	if creatorUserID = strings.TrimSpace(creatorUserID); creatorUserID != "" {
		if maxMembers > 0 && memberCount >= maxMembers {
			return AccessGroup{}, fmt.Errorf("%w: group %q allows %d members", ErrAccessGroupMemberQuota, name, maxMembers)
		}
		if _, err := tx.Exec(
			`INSERT INTO access_group_members (group_id, user_id, role, created_at) VALUES (?, ?, ?, ?)`,
			id, creatorUserID, GroupRoleAdmin, unixMillis(created),
		); err != nil {
			if strings.Contains(err.Error(), "FOREIGN KEY") {
				return AccessGroup{}, ErrUnknownUser
			}
			return AccessGroup{}, fmt.Errorf("account store %s: add group creator: %w", s.path, err)
		}
		memberCount = 1
	}
	if err := tx.Commit(); err != nil {
		return AccessGroup{}, fmt.Errorf("account store %s: commit access group: %w", s.path, err)
	}
	return AccessGroup{
		ID: id, Name: name, Description: description,
		MaxDevices: maxDevices, MaxMembers: maxMembers,
		CreatedAt: created, MemberCount: memberCount,
	}, nil
}

const accessGroupColumns = `g.id, g.name, g.description, g.max_devices, g.max_members, g.created_at,
	(SELECT COUNT(*) FROM access_group_devices d WHERE d.group_id = g.id),
	(SELECT COUNT(*) FROM access_group_members m WHERE m.group_id = g.id)`

func scanAccessGroup(row interface{ Scan(...any) error }) (AccessGroup, error) {
	var (
		group   AccessGroup
		created int64
	)
	if err := row.Scan(&group.ID, &group.Name, &group.Description,
		&group.MaxDevices, &group.MaxMembers, &created,
		&group.DeviceCount, &group.MemberCount); err != nil {
		return AccessGroup{}, err
	}
	group.CreatedAt = timeFromMillis(created)
	return group, nil
}

// ListAccessGroups returns every group; callers must be server-key or global
// admins.
func (s *Store) ListAccessGroups() ([]AccessGroup, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(`SELECT ` + accessGroupColumns + ` FROM access_groups g ORDER BY g.name COLLATE NOCASE, g.id`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list access groups: %w", s.path, err)
	}
	defer rows.Close()
	groups := make([]AccessGroup, 0)
	for rows.Next() {
		group, err := scanAccessGroup(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: scan access group: %w", s.path, err)
		}
		groups = append(groups, group)
	}
	return groups, rows.Err()
}

// AccessGroupsForUser returns the groups one account belongs to, annotated
// with that account's group role.
func (s *Store) AccessGroupsForUser(userID string) ([]AccessGroup, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT `+accessGroupColumns+`, m.role FROM access_groups g
		 JOIN access_group_members m ON m.group_id = g.id
		 WHERE m.user_id = ? ORDER BY g.name COLLATE NOCASE, g.id`,
		strings.TrimSpace(userID),
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list account access groups: %w", s.path, err)
	}
	defer rows.Close()
	groups := make([]AccessGroup, 0)
	for rows.Next() {
		group, err := scanAccessGroupWithRole(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: scan account access group: %w", s.path, err)
		}
		groups = append(groups, group)
	}
	return groups, rows.Err()
}

func scanAccessGroupWithRole(row interface{ Scan(...any) error }) (AccessGroup, error) {
	var (
		group   AccessGroup
		created int64
	)
	if err := row.Scan(&group.ID, &group.Name, &group.Description,
		&group.MaxDevices, &group.MaxMembers, &created,
		&group.DeviceCount, &group.MemberCount, &group.Role); err != nil {
		return AccessGroup{}, err
	}
	group.CreatedAt = timeFromMillis(created)
	return group, nil
}

// AccessGroup returns one group with its usage counts.
func (s *Store) AccessGroup(id string) (AccessGroup, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.accessGroupLocked(id)
}

func (s *Store) accessGroupLocked(id string) (AccessGroup, error) {
	group, err := scanAccessGroup(s.db.QueryRow(
		`SELECT `+accessGroupColumns+` FROM access_groups g WHERE g.id = ?`, strings.TrimSpace(id)))
	if errors.Is(err, sql.ErrNoRows) {
		return AccessGroup{}, ErrUnknownAccessGroup
	}
	if err != nil {
		return AccessGroup{}, fmt.Errorf("account store %s: read access group: %w", s.path, err)
	}
	return group, nil
}

// AccessGroupRole returns the account's role in one group. The boolean is
// false when the account is not a member; an unknown group is an error.
func (s *Store) AccessGroupRole(groupID, userID string) (string, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.accessGroupLocked(groupID); err != nil {
		return "", false, err
	}
	var role string
	err := s.db.QueryRow(
		`SELECT role FROM access_group_members WHERE group_id = ? AND user_id = ?`,
		strings.TrimSpace(groupID), strings.TrimSpace(userID),
	).Scan(&role)
	if errors.Is(err, sql.ErrNoRows) {
		return "", false, nil
	}
	if err != nil {
		return "", false, fmt.Errorf("account store %s: read group role: %w", s.path, err)
	}
	return role, true, nil
}

// UpdateAccessGroup changes group settings. Lowering a quota below the current
// usage is rejected so existing assignments never exceed the stored limit.
func (s *Store) UpdateAccessGroup(id string, update AccessGroupUpdate) (AccessGroup, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	group, err := s.accessGroupLocked(id)
	if err != nil {
		return AccessGroup{}, err
	}
	name := group.Name
	if update.Name != nil {
		if name, err = normalizeAccessGroupName(*update.Name); err != nil {
			return AccessGroup{}, err
		}
	}
	description := group.Description
	if update.Description != nil {
		description = strings.TrimSpace(*update.Description)
		if len(description) > maxGroupDescription {
			return AccessGroup{}, fmt.Errorf("%w: at most %d characters", ErrInvalidAccessGroupDescription, maxGroupDescription)
		}
	}
	maxDevices, maxMembers := group.MaxDevices, group.MaxMembers
	if update.MaxDevices != nil {
		maxDevices = *update.MaxDevices
	}
	if update.MaxMembers != nil {
		maxMembers = *update.MaxMembers
	}
	if err := validateAccessGroupQuotas(maxDevices, maxMembers); err != nil {
		return AccessGroup{}, err
	}
	if maxDevices > 0 && maxDevices < group.DeviceCount {
		return AccessGroup{}, fmt.Errorf("%w: %d devices are assigned, quota %d", ErrAccessGroupQuotaBelow, group.DeviceCount, maxDevices)
	}
	if maxMembers > 0 && maxMembers < group.MemberCount {
		return AccessGroup{}, fmt.Errorf("%w: %d members are assigned, quota %d", ErrAccessGroupQuotaBelow, group.MemberCount, maxMembers)
	}
	if _, err := s.db.Exec(
		`UPDATE access_groups SET name = ?, description = ?, max_devices = ?, max_members = ? WHERE id = ?`,
		name, description, maxDevices, maxMembers, group.ID,
	); err != nil {
		if strings.Contains(err.Error(), "UNIQUE") {
			return AccessGroup{}, ErrAccessGroupExists
		}
		return AccessGroup{}, fmt.Errorf("account store %s: update access group: %w", s.path, err)
	}
	group.Name, group.Description = name, description
	group.MaxDevices, group.MaxMembers = maxDevices, maxMembers
	return group, nil
}

// DeleteAccessGroup removes a group and its assignments. Only server-key or
// global-admin callers reach this method.
func (s *Store) DeleteAccessGroup(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(`DELETE FROM access_groups WHERE id = ?`, strings.TrimSpace(id))
	if err != nil {
		return fmt.Errorf("account store %s: delete access group: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownAccessGroup
	}
	return nil
}

// ListAccessGroupMembers lists members with their account names.
func (s *Store) ListAccessGroupMembers(groupID string) ([]AccessGroupMember, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.accessGroupLocked(groupID); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(
		`SELECT u.username, m.role, m.created_at, u.banned
		 FROM access_group_members m JOIN users u ON u.id = m.user_id
		 WHERE m.group_id = ? ORDER BY u.username COLLATE NOCASE`,
		strings.TrimSpace(groupID),
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list group members: %w", s.path, err)
	}
	defer rows.Close()
	members := make([]AccessGroupMember, 0)
	for rows.Next() {
		var (
			member  AccessGroupMember
			created int64
		)
		if err := rows.Scan(&member.Username, &member.Role, &created, &member.Banned); err != nil {
			return nil, fmt.Errorf("account store %s: scan group member: %w", s.path, err)
		}
		member.CreatedAt = timeFromMillis(created)
		members = append(members, member)
	}
	return members, rows.Err()
}

// SetAccessGroupMember adds an account to a group or changes its group role.
// The member quota applies to new memberships only, and the last admin of a
// group cannot be demoted.
func (s *Store) SetAccessGroupMember(groupID, username, role string) (AccessGroupMember, error) {
	role = strings.TrimSpace(role)
	if !ValidGroupRole(role) {
		return AccessGroupMember{}, ErrInvalidGroupRole
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	group, err := s.accessGroupLocked(groupID)
	if err != nil {
		return AccessGroupMember{}, err
	}
	var (
		userID  string
		banned  bool
		created = s.now().UTC()
	)
	err = s.db.QueryRow(
		`SELECT id, banned FROM users WHERE username = ?`, strings.TrimSpace(username),
	).Scan(&userID, &banned)
	if errors.Is(err, sql.ErrNoRows) {
		return AccessGroupMember{}, ErrUnknownUser
	}
	if err != nil {
		return AccessGroupMember{}, fmt.Errorf("account store %s: find group member: %w", s.path, err)
	}
	var current string
	err = s.db.QueryRow(
		`SELECT role FROM access_group_members WHERE group_id = ? AND user_id = ?`,
		group.ID, userID,
	).Scan(&current)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		if group.MaxMembers > 0 && group.MemberCount >= group.MaxMembers {
			return AccessGroupMember{}, fmt.Errorf("%w: group %q allows %d members, %d already joined",
				ErrAccessGroupMemberQuota, group.Name, group.MaxMembers, group.MemberCount)
		}
		if _, err := s.db.Exec(
			`INSERT INTO access_group_members (group_id, user_id, role, created_at) VALUES (?, ?, ?, ?)`,
			group.ID, userID, role, unixMillis(created),
		); err != nil {
			return AccessGroupMember{}, fmt.Errorf("account store %s: add group member: %w", s.path, err)
		}
	case err != nil:
		return AccessGroupMember{}, fmt.Errorf("account store %s: read group member: %w", s.path, err)
	default:
		if current == GroupRoleAdmin && role != GroupRoleAdmin && !banned {
			if err := s.requireAnotherGroupAdminLocked(group.ID, userID); err != nil {
				return AccessGroupMember{}, err
			}
		}
		if _, err := s.db.Exec(
			`UPDATE access_group_members SET role = ? WHERE group_id = ? AND user_id = ?`,
			role, group.ID, userID,
		); err != nil {
			return AccessGroupMember{}, fmt.Errorf("account store %s: update group member: %w", s.path, err)
		}
	}
	return AccessGroupMember{Username: strings.TrimSpace(username), Role: role, CreatedAt: created, Banned: banned}, nil
}

// RemoveAccessGroupMember drops a membership unless it is the group's last
// admin.
func (s *Store) RemoveAccessGroupMember(groupID, username string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.accessGroupLocked(groupID); err != nil {
		return err
	}
	var (
		userID string
		role   string
		banned bool
	)
	err := s.db.QueryRow(
		`SELECT u.id, m.role, u.banned FROM users u
		 JOIN access_group_members m ON m.user_id = u.id
		 WHERE m.group_id = ? AND u.username = ?`,
		strings.TrimSpace(groupID), strings.TrimSpace(username),
	).Scan(&userID, &role, &banned)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownUser
	}
	if err != nil {
		return fmt.Errorf("account store %s: find group member: %w", s.path, err)
	}
	if role == GroupRoleAdmin && !banned {
		if err := s.requireAnotherGroupAdminLocked(groupID, userID); err != nil {
			return err
		}
	}
	if _, err := s.db.Exec(
		`DELETE FROM access_group_members WHERE group_id = ? AND user_id = ?`,
		strings.TrimSpace(groupID), userID,
	); err != nil {
		return fmt.Errorf("account store %s: remove group member: %w", s.path, err)
	}
	return nil
}

// requireAnotherGroupAdminLocked rejects an operation that would leave the
// group without any usable admin. A banned admin cannot sign in, so it does
// not count, matching the global account role's last-admin rule.
func (s *Store) requireAnotherGroupAdminLocked(groupID, userID string) error {
	var others int
	if err := s.db.QueryRow(
		`SELECT COUNT(*) FROM access_group_members m
		 JOIN users u ON u.id = m.user_id
		 WHERE m.group_id = ? AND m.role = ? AND m.user_id <> ? AND u.banned = 0`,
		groupID, GroupRoleAdmin, userID,
	).Scan(&others); err != nil {
		return fmt.Errorf("account store %s: count group admins: %w", s.path, err)
	}
	if others == 0 {
		return ErrLastGroupAdmin
	}
	return nil
}

// lastUsableGroupAdminLocked reports whether userID is the only non-banned
// admin of at least one group. Deleting or banning that account would leave
// the group without anyone who can manage it.
func (s *Store) lastUsableGroupAdminLocked(userID string) (bool, error) {
	var groups int
	err := s.db.QueryRow(
		`SELECT COUNT(*) FROM access_group_members m
		 JOIN users u ON u.id = m.user_id
		 WHERE m.user_id = ? AND m.role = ? AND u.banned = 0
		   AND NOT EXISTS (
		     SELECT 1 FROM access_group_members other
		     JOIN users ou ON ou.id = other.user_id
		     WHERE other.group_id = m.group_id AND other.role = ?
		       AND other.user_id <> m.user_id AND ou.banned = 0)`,
		userID, GroupRoleAdmin, GroupRoleAdmin,
	).Scan(&groups)
	if err != nil {
		return false, fmt.Errorf("account store %s: check group admin floor: %w", s.path, err)
	}
	return groups > 0, nil
}

// ListAccessGroupDevices lists the devices assigned to one group.
func (s *Store) ListAccessGroupDevices(groupID string) ([]AccessGroupDevice, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.accessGroupLocked(groupID); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(
		`SELECT device_id, created_at FROM access_group_devices
		 WHERE group_id = ? ORDER BY device_id COLLATE NOCASE`,
		strings.TrimSpace(groupID),
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list group devices: %w", s.path, err)
	}
	defer rows.Close()
	devices := make([]AccessGroupDevice, 0)
	for rows.Next() {
		var (
			device  AccessGroupDevice
			created int64
		)
		if err := rows.Scan(&device.DeviceID, &created); err != nil {
			return nil, fmt.Errorf("account store %s: scan group device: %w", s.path, err)
		}
		device.CreatedAt = timeFromMillis(created)
		devices = append(devices, device)
	}
	return devices, rows.Err()
}

// AddAccessGroupDevice assigns a device to a group. Re-adding an existing
// device is idempotent and does not consume quota.
func (s *Store) AddAccessGroupDevice(groupID, deviceID string) (AccessGroupDevice, bool, error) {
	deviceID = strings.TrimSpace(deviceID)
	if !registry.ValidID(deviceID) {
		return AccessGroupDevice{}, false, ErrInvalidGroupDeviceID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	group, err := s.accessGroupLocked(groupID)
	if err != nil {
		return AccessGroupDevice{}, false, err
	}
	var exists int
	err = s.db.QueryRow(
		`SELECT 1 FROM access_group_devices WHERE group_id = ? AND device_id = ?`,
		group.ID, deviceID,
	).Scan(&exists)
	if err == nil {
		return AccessGroupDevice{DeviceID: deviceID}, false, nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return AccessGroupDevice{}, false, fmt.Errorf("account store %s: read group device: %w", s.path, err)
	}
	if group.MaxDevices > 0 && group.DeviceCount >= group.MaxDevices {
		return AccessGroupDevice{}, false, fmt.Errorf("%w: group %q allows %d devices, %d already assigned",
			ErrAccessGroupDeviceQuota, group.Name, group.MaxDevices, group.DeviceCount)
	}
	created := s.now().UTC()
	if _, err := s.db.Exec(
		`INSERT INTO access_group_devices (group_id, device_id, created_at) VALUES (?, ?, ?)`,
		group.ID, deviceID, unixMillis(created),
	); err != nil {
		return AccessGroupDevice{}, false, fmt.Errorf("account store %s: add group device: %w", s.path, err)
	}
	return AccessGroupDevice{DeviceID: deviceID, CreatedAt: created}, true, nil
}

// RemoveAccessGroupDevice drops one device assignment.
func (s *Store) RemoveAccessGroupDevice(groupID, deviceID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.accessGroupLocked(groupID); err != nil {
		return err
	}
	res, err := s.db.Exec(
		`DELETE FROM access_group_devices WHERE group_id = ? AND device_id = ?`,
		strings.TrimSpace(groupID), strings.TrimSpace(deviceID),
	)
	if err != nil {
		return fmt.Errorf("account store %s: remove group device: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownGroupDevice
	}
	return nil
}

// ConnectionsForGroupBefore pages session reports whose host or peer is
// assigned to the group.
func (s *Store) ConnectionsForGroupBefore(groupID string, beforeID int64, limit int) ([]ConnectionEvent, error) {
	return s.connectionEvents(
		`EXISTS (SELECT 1 FROM access_group_devices d
		 WHERE d.group_id = ? AND (d.device_id = host_id OR d.device_id = peer_id COLLATE NOCASE))`,
		[]any{strings.TrimSpace(groupID)}, beforeID, limit)
}

// FileTransfersForGroupBefore pages file reports whose host or peer is
// assigned to the group.
func (s *Store) FileTransfersForGroupBefore(groupID string, beforeID int64, limit int) ([]FileEvent, error) {
	return s.fileEvents(
		`EXISTS (SELECT 1 FROM access_group_devices d
		 WHERE d.group_id = ? AND (d.device_id = host_id OR d.device_id = peer_id COLLATE NOCASE))`,
		[]any{strings.TrimSpace(groupID)}, beforeID, limit)
}
