package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// ErrInvalidAccountGroupID etc. describe the operator-managed groups that
// scope console work. These are deliberately not the per-user address-book
// groups: those live inside one book, while an account group spans accounts
// and grants console permissions over its members and devices.
var (
	ErrInvalidAccountGroupID   = errors.New("account group id must match [A-Za-z0-9][A-Za-z0-9_.-]{0,63}")
	ErrInvalidAccountGroupName = errors.New("account group name is required")
	ErrUnknownAccountGroup     = errors.New("unknown account group")
	ErrAccountGroupExists      = errors.New("account group already exists")
	ErrInvalidGroupRole        = errors.New("group role must be readonly, operator or admin")
	ErrUnknownGroupMember      = errors.New("unknown group member")
	ErrGroupMemberExists       = errors.New("account is already a group member")
	ErrGroupMemberQuota        = errors.New("group member quota reached")
	ErrUnknownGroupDevice      = errors.New("unknown group device")
	ErrGroupDeviceExists       = errors.New("device is already in the group")
	ErrGroupDeviceQuota        = errors.New("group device quota reached")
	ErrGroupQuotaTooSmall      = errors.New("quota is below the current usage")
	ErrLastGroupAdmin          = errors.New("at least one group admin must remain")
	ErrInvalidGroupQuota       = errors.New("group quota must not be negative")
)

const maxGroupDescription = 256

// AccountGroup is the durable scope record. Quotas are 0 when unlimited;
// MemberCount and DeviceCount are filled on read so a console can show how
// close a group is to its limit without another request.
type AccountGroup struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Description string    `json:"description,omitempty"`
	MemberQuota int       `json:"member_quota"`
	DeviceQuota int       `json:"device_quota"`
	MemberCount int       `json:"member_count"`
	DeviceCount int       `json:"device_count"`
	CreatedAt   time.Time `json:"created_at"`
	UpdatedAt   time.Time `json:"updated_at"`
}

// GroupAccess is one group plus the caller's membership role, which the API
// combines with a global console role to decide what the caller may do.
type GroupAccess struct {
	AccountGroup
	Role string `json:"role"`
}

type GroupMember struct {
	UserID    string    `json:"user_id"`
	Username  string    `json:"username"`
	Role      string    `json:"role"`
	CreatedAt time.Time `json:"created_at"`
}

type GroupDevice struct {
	PeerID    string    `json:"peer_id"`
	AddedBy   string    `json:"added_by,omitempty"`
	CreatedAt time.Time `json:"created_at"`
}

// ValidGroupRole reports whether role may be assigned inside a group. The
// names match account roles so operators only learn one permission ladder:
// readonly reads, operator writes devices, admin also manages membership.
func ValidGroupRole(role string) bool {
	switch role {
	case RoleReadonly, RoleOperator, RoleAdmin:
		return true
	}
	return false
}

func (s *Store) CreateGroup(id, name, description string, memberQuota, deviceQuota int) (AccountGroup, error) {
	id = strings.TrimSpace(id)
	if !registry.ValidID(id) {
		return AccountGroup{}, ErrInvalidAccountGroupID
	}
	name = normalizeText(name, maxName)
	if name == "" {
		return AccountGroup{}, ErrInvalidAccountGroupName
	}
	description = normalizeText(description, maxGroupDescription)
	if memberQuota < 0 || deviceQuota < 0 {
		return AccountGroup{}, ErrInvalidGroupQuota
	}
	created := s.now().UTC()
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`INSERT INTO account_groups
			(id, name, description, member_quota, device_quota, created_at, updated_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		id, name, description, memberQuota, deviceQuota, unixMillis(created), unixMillis(created),
	); err != nil {
		if isUniqueViolation(err) {
			return AccountGroup{}, ErrAccountGroupExists
		}
		return AccountGroup{}, fmt.Errorf("account store %s: create group: %w", s.path, err)
	}
	return AccountGroup{
		ID: id, Name: name, Description: description,
		MemberQuota: memberQuota, DeviceQuota: deviceQuota,
		CreatedAt: created, UpdatedAt: created,
	}, nil
}

// Groups lists every group. Callers that hold only membership roles should
// use GroupsForUser instead.
func (s *Store) Groups() ([]AccountGroup, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.accountGroupsLocked("", nil)
}

// GroupsForUser lists the groups one account belongs to, each with the
// membership role that applies to it.
func (s *Store) GroupsForUser(userID string) ([]GroupAccess, error) {
	userID = strings.TrimSpace(userID)
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT g.id, g.name, g.description, g.member_quota, g.device_quota,
		        g.created_at, g.updated_at, m.role,
		        (SELECT COUNT(*) FROM account_group_members gm WHERE gm.group_id = g.id),
		        (SELECT COUNT(*) FROM account_group_devices gd WHERE gd.group_id = g.id)
		 FROM account_groups g
		 JOIN account_group_members m ON m.group_id = g.id
		 WHERE m.user_id = ?
		 ORDER BY g.name, g.id`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list user groups: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]GroupAccess, 0)
	for rows.Next() {
		var (
			group   GroupAccess
			created int64
			updated int64
		)
		if err := rows.Scan(&group.ID, &group.Name, &group.Description,
			&group.MemberQuota, &group.DeviceQuota, &created, &updated, &group.Role,
			&group.MemberCount, &group.DeviceCount); err != nil {
			return nil, fmt.Errorf("account store %s: scan user group: %w", s.path, err)
		}
		group.CreatedAt = timeFromMillis(created)
		group.UpdatedAt = timeFromMillis(updated)
		out = append(out, group)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list user groups: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) accountGroupsLocked(where string, args []any) ([]AccountGroup, error) {
	query := `SELECT g.id, g.name, g.description, g.member_quota, g.device_quota,
	        g.created_at, g.updated_at,
	        (SELECT COUNT(*) FROM account_group_members m WHERE m.group_id = g.id),
	        (SELECT COUNT(*) FROM account_group_devices d WHERE d.group_id = g.id)
	 FROM account_groups g`
	if where != "" {
		query += " WHERE " + where
	}
	query += " ORDER BY g.name, g.id"
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list groups: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]AccountGroup, 0)
	for rows.Next() {
		group, err := scanAccountGroup(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: scan group: %w", s.path, err)
		}
		out = append(out, group)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list groups: %w", s.path, err)
	}
	return out, nil
}

func scanAccountGroup(row rowScanner) (AccountGroup, error) {
	var (
		group   AccountGroup
		created int64
		updated int64
	)
	if err := row.Scan(&group.ID, &group.Name, &group.Description,
		&group.MemberQuota, &group.DeviceQuota, &created, &updated,
		&group.MemberCount, &group.DeviceCount); err != nil {
		return AccountGroup{}, err
	}
	group.CreatedAt = timeFromMillis(created)
	group.UpdatedAt = timeFromMillis(updated)
	return group, nil
}

func (s *Store) AccountGroupByID(groupID string) (AccountGroup, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return AccountGroup{}, ErrInvalidAccountGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.groupLocked(groupID)
}

func (s *Store) groupLocked(groupID string) (AccountGroup, error) {
	group, err := scanAccountGroup(s.db.QueryRow(
		`SELECT g.id, g.name, g.description, g.member_quota, g.device_quota,
		        g.created_at, g.updated_at,
		        (SELECT COUNT(*) FROM account_group_members m WHERE m.group_id = g.id),
		        (SELECT COUNT(*) FROM account_group_devices d WHERE d.group_id = g.id)
		 FROM account_groups g WHERE g.id = ?`,
		groupID,
	))
	if errors.Is(err, sql.ErrNoRows) {
		return AccountGroup{}, ErrUnknownAccountGroup
	}
	if err != nil {
		return AccountGroup{}, fmt.Errorf("account store %s: read group: %w", s.path, err)
	}
	return group, nil
}

// UpdateGroup applies the non-nil fields and rejects a quota that would leave
// the group over its own limit, so the invariant "count never exceeds quota"
// keeps holding after an edit.
func (s *Store) UpdateAccountGroup(groupID string, name, description *string, memberQuota, deviceQuota *int) (AccountGroup, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return AccountGroup{}, ErrInvalidAccountGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	group, err := s.groupLocked(groupID)
	if err != nil {
		return AccountGroup{}, err
	}
	changed := false
	if name != nil {
		clean := normalizeText(*name, maxName)
		if clean == "" {
			return AccountGroup{}, ErrInvalidAccountGroupName
		}
		group.Name = clean
		changed = true
	}
	if description != nil {
		group.Description = normalizeText(*description, maxGroupDescription)
		changed = true
	}
	if memberQuota != nil {
		if *memberQuota < 0 {
			return AccountGroup{}, ErrInvalidGroupQuota
		}
		if *memberQuota > 0 && *memberQuota < group.MemberCount {
			return AccountGroup{}, fmt.Errorf("%w: member quota %d is below the current count %d",
				ErrGroupQuotaTooSmall, *memberQuota, group.MemberCount)
		}
		group.MemberQuota = *memberQuota
		changed = true
	}
	if deviceQuota != nil {
		if *deviceQuota < 0 {
			return AccountGroup{}, ErrInvalidGroupQuota
		}
		if *deviceQuota > 0 && *deviceQuota < group.DeviceCount {
			return AccountGroup{}, fmt.Errorf("%w: device quota %d is below the current count %d",
				ErrGroupQuotaTooSmall, *deviceQuota, group.DeviceCount)
		}
		group.DeviceQuota = *deviceQuota
		changed = true
	}
	if !changed {
		return group, nil
	}
	updated := s.now().UTC()
	if _, err := s.db.Exec(
		`UPDATE account_groups
		 SET name = ?, description = ?, member_quota = ?, device_quota = ?, updated_at = ?
		 WHERE id = ?`,
		group.Name, group.Description, group.MemberQuota, group.DeviceQuota, unixMillis(updated), groupID,
	); err != nil {
		return AccountGroup{}, fmt.Errorf("account store %s: update group: %w", s.path, err)
	}
	group.UpdatedAt = updated
	return group, nil
}

// DeleteGroup removes the group and its membership and device links. It is
// allowed to remove the last group admin because the whole scope disappears.
func (s *Store) DeleteAccountGroup(groupID string) error {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return ErrInvalidAccountGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin group delete: %w", s.path, err)
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM account_group_members WHERE group_id = ?`, groupID); err != nil {
		return fmt.Errorf("account store %s: delete group members: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM account_group_devices WHERE group_id = ?`, groupID); err != nil {
		return fmt.Errorf("account store %s: delete group devices: %w", s.path, err)
	}
	// Address-book shares addressed to the group expire with it; a grant whose
	// target no longer exists must not linger in the owner's grant list.
	if _, err := tx.Exec(
		`DELETE FROM address_book_target_grants WHERE target_kind = 'group' AND target_id = ?`,
		groupID,
	); err != nil {
		return fmt.Errorf("account store %s: delete group address book grants: %w", s.path, err)
	}
	// A group-scoped policy assignment dies with its group; devices that were
	// covered by it are reconciled back to open/manual by the caller.
	if _, err := tx.Exec(
		`DELETE FROM policy_assignments WHERE scope_kind = ? AND scope_value = ?`,
		PolicyScopeGroup, groupID,
	); err != nil {
		return fmt.Errorf("account store %s: delete group policy assignments: %w", s.path, err)
	}
	res, err := tx.Exec(`DELETE FROM account_groups WHERE id = ?`, groupID)
	if err != nil {
		return fmt.Errorf("account store %s: delete group: %w", s.path, err)
	}
	affected, err := res.RowsAffected()
	if err != nil {
		return err
	}
	if affected == 0 {
		return ErrUnknownAccountGroup
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit group delete: %w", s.path, err)
	}
	return nil
}

// GroupRole resolves one account's membership in one group.
func (s *Store) GroupRole(groupID, userID string) (string, bool, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return "", false, ErrInvalidAccountGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var role string
	err := s.db.QueryRow(
		`SELECT role FROM account_group_members WHERE group_id = ? AND user_id = ?`,
		groupID, strings.TrimSpace(userID),
	).Scan(&role)
	if errors.Is(err, sql.ErrNoRows) {
		return "", false, nil
	}
	if err != nil {
		return "", false, fmt.Errorf("account store %s: read group role: %w", s.path, err)
	}
	return role, true, nil
}

func (s *Store) GroupMembers(groupID string) ([]GroupMember, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return nil, ErrInvalidAccountGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT m.user_id, u.username, m.role, m.created_at
		 FROM account_group_members m
		 JOIN users u ON u.id = m.user_id
		 WHERE m.group_id = ?
		 ORDER BY u.username`,
		groupID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list group members: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]GroupMember, 0)
	for rows.Next() {
		var (
			member  GroupMember
			created int64
		)
		if err := rows.Scan(&member.UserID, &member.Username, &member.Role, &created); err != nil {
			return nil, fmt.Errorf("account store %s: scan group member: %w", s.path, err)
		}
		member.CreatedAt = timeFromMillis(created)
		out = append(out, member)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list group members: %w", s.path, err)
	}
	return out, nil
}

func (s *Store) AddGroupMember(groupID, username, role string) (GroupMember, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return GroupMember{}, ErrInvalidAccountGroupID
	}
	role = strings.TrimSpace(role)
	if role == "" {
		role = RoleReadonly
	}
	if !ValidGroupRole(role) {
		return GroupMember{}, ErrInvalidGroupRole
	}
	username = strings.TrimSpace(username)
	if username == "" {
		return GroupMember{}, ErrUnknownUser
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		userID  string
		display string
	)
	err := s.db.QueryRow(`SELECT id, username FROM users WHERE username = ?`, username).
		Scan(&userID, &display)
	if errors.Is(err, sql.ErrNoRows) {
		return GroupMember{}, ErrUnknownUser
	}
	if err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: find group member: %w", s.path, err)
	}
	created := s.now().UTC()
	tx, err := s.db.Begin()
	if err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: begin group member add: %w", s.path, err)
	}
	defer tx.Rollback()
	quota, err := s.lockGroupQuota(tx, groupID)
	if err != nil {
		return GroupMember{}, err
	}
	var existing int
	if err := tx.QueryRow(
		`SELECT COUNT(*) FROM account_group_members WHERE group_id = ? AND user_id = ?`,
		groupID, userID,
	).Scan(&existing); err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: check group member: %w", s.path, err)
	}
	if existing > 0 {
		return GroupMember{}, ErrGroupMemberExists
	}
	var count int
	if err := tx.QueryRow(
		`SELECT COUNT(*) FROM account_group_members WHERE group_id = ?`, groupID,
	).Scan(&count); err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: count group members: %w", s.path, err)
	}
	if quota.MemberQuota > 0 && count >= quota.MemberQuota {
		return GroupMember{}, fmt.Errorf("%w: limit %d", ErrGroupMemberQuota, quota.MemberQuota)
	}
	if _, err := tx.Exec(
		`INSERT INTO account_group_members (group_id, user_id, role, created_at)
		 VALUES (?, ?, ?, ?)`,
		groupID, userID, role, unixMillis(created),
	); err != nil {
		if isUniqueViolation(err) {
			return GroupMember{}, ErrGroupMemberExists
		}
		return GroupMember{}, fmt.Errorf("account store %s: add group member: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: commit group member add: %w", s.path, err)
	}
	return GroupMember{UserID: userID, Username: display, Role: role, CreatedAt: created}, nil
}

// lockGroupQuota reads the quota columns and, on PostgreSQL, takes a row lock
// so two server instances cannot both pass the quota check and then insert.
func (s *Store) lockGroupQuota(tx *sql.Tx, groupID string) (AccountGroup, error) {
	query := `SELECT id, member_quota, device_quota FROM account_groups WHERE id = ?`
	if s.dialect == dialectPostgres {
		query += " FOR UPDATE"
	}
	var quota AccountGroup
	err := tx.QueryRow(query, groupID).Scan(&quota.ID, &quota.MemberQuota, &quota.DeviceQuota)
	if errors.Is(err, sql.ErrNoRows) {
		return AccountGroup{}, ErrUnknownAccountGroup
	}
	if err != nil {
		return AccountGroup{}, fmt.Errorf("account store %s: lock group: %w", s.path, err)
	}
	return quota, nil
}

func (s *Store) UpdateGroupMember(groupID, username, role string) (GroupMember, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return GroupMember{}, ErrInvalidAccountGroupID
	}
	role = strings.TrimSpace(role)
	if !ValidGroupRole(role) {
		return GroupMember{}, ErrInvalidGroupRole
	}
	username = strings.TrimSpace(username)
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: begin group member update: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		member  GroupMember
		created int64
	)
	err = tx.QueryRow(
		`SELECT m.user_id, u.username, m.role, m.created_at
		 FROM account_group_members m
		 JOIN users u ON u.id = m.user_id
		 WHERE m.group_id = ? AND u.username = ?`,
		groupID, username,
	).Scan(&member.UserID, &member.Username, &member.Role, &created)
	if errors.Is(err, sql.ErrNoRows) {
		return GroupMember{}, ErrUnknownGroupMember
	}
	if err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: read group member: %w", s.path, err)
	}
	if member.Role == RoleAdmin && role != RoleAdmin {
		admins, err := s.countGroupAdmins(tx, groupID)
		if err != nil {
			return GroupMember{}, err
		}
		if admins <= 1 {
			return GroupMember{}, ErrLastGroupAdmin
		}
	}
	if _, err := tx.Exec(
		`UPDATE account_group_members SET role = ? WHERE group_id = ? AND user_id = ?`,
		role, groupID, member.UserID,
	); err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: update group member: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return GroupMember{}, fmt.Errorf("account store %s: commit group member update: %w", s.path, err)
	}
	member.Role = role
	member.CreatedAt = timeFromMillis(created)
	return member, nil
}

func (s *Store) RemoveGroupMember(groupID, username string) error {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return ErrInvalidAccountGroupID
	}
	username = strings.TrimSpace(username)
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin group member remove: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		userID string
		role   string
	)
	err = tx.QueryRow(
		`SELECT m.user_id, m.role
		 FROM account_group_members m
		 JOIN users u ON u.id = m.user_id
		 WHERE m.group_id = ? AND u.username = ?`,
		groupID, username,
	).Scan(&userID, &role)
	if errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownGroupMember
	}
	if err != nil {
		return fmt.Errorf("account store %s: read group member: %w", s.path, err)
	}
	if role == RoleAdmin {
		admins, err := s.countGroupAdmins(tx, groupID)
		if err != nil {
			return err
		}
		if admins <= 1 {
			return ErrLastGroupAdmin
		}
	}
	if _, err := tx.Exec(
		`DELETE FROM account_group_members WHERE group_id = ? AND user_id = ?`,
		groupID, userID,
	); err != nil {
		return fmt.Errorf("account store %s: remove group member: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit group member remove: %w", s.path, err)
	}
	return nil
}

func (s *Store) countGroupAdmins(tx *sql.Tx, groupID string) (int, error) {
	var admins int
	if err := tx.QueryRow(
		`SELECT COUNT(*) FROM account_group_members WHERE group_id = ? AND role = ?`,
		groupID, RoleAdmin,
	).Scan(&admins); err != nil {
		return 0, fmt.Errorf("account store %s: count group admins: %w", s.path, err)
	}
	return admins, nil
}

func (s *Store) GroupDevices(groupID string) ([]GroupDevice, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return nil, ErrInvalidAccountGroupID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT peer_id, added_by, created_at FROM account_group_devices
		 WHERE group_id = ? ORDER BY peer_id`,
		groupID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list group devices: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]GroupDevice, 0)
	for rows.Next() {
		var (
			device  GroupDevice
			created int64
		)
		if err := rows.Scan(&device.PeerID, &device.AddedBy, &created); err != nil {
			return nil, fmt.Errorf("account store %s: scan group device: %w", s.path, err)
		}
		device.CreatedAt = timeFromMillis(created)
		out = append(out, device)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list group devices: %w", s.path, err)
	}
	return out, nil
}

// DeviceInGroup reports whether peerID is a member of the group. An empty
// group is the unscoped console view and admits every device.
func (s *Store) DeviceInGroup(groupID, peerID string) (bool, error) {
	groupID = strings.TrimSpace(groupID)
	if groupID == "" {
		return true, nil
	}
	if !registry.ValidID(groupID) {
		return false, ErrInvalidAccountGroupID
	}
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return false, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var exists int
	err := s.db.QueryRow(
		`SELECT 1 FROM account_group_devices WHERE group_id = ? AND peer_id = ?`,
		groupID, peerID,
	).Scan(&exists)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("account store %s: check group device: %w", s.path, err)
	}
	return true, nil
}

func (s *Store) AddGroupDevice(groupID, peerID, addedBy string) (GroupDevice, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return GroupDevice{}, ErrInvalidAccountGroupID
	}
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return GroupDevice{}, ErrInvalidPeerID
	}
	addedBy = normalizeText(addedBy, maxName)
	created := s.now().UTC()
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return GroupDevice{}, fmt.Errorf("account store %s: begin group device add: %w", s.path, err)
	}
	defer tx.Rollback()
	quota, err := s.lockGroupQuota(tx, groupID)
	if err != nil {
		return GroupDevice{}, err
	}
	var existing int
	if err := tx.QueryRow(
		`SELECT COUNT(*) FROM account_group_devices WHERE group_id = ? AND peer_id = ?`,
		groupID, peerID,
	).Scan(&existing); err != nil {
		return GroupDevice{}, fmt.Errorf("account store %s: check group device: %w", s.path, err)
	}
	if existing > 0 {
		return GroupDevice{}, ErrGroupDeviceExists
	}
	var count int
	if err := tx.QueryRow(
		`SELECT COUNT(*) FROM account_group_devices WHERE group_id = ?`, groupID,
	).Scan(&count); err != nil {
		return GroupDevice{}, fmt.Errorf("account store %s: count group devices: %w", s.path, err)
	}
	if quota.DeviceQuota > 0 && count >= quota.DeviceQuota {
		return GroupDevice{}, fmt.Errorf("%w: limit %d", ErrGroupDeviceQuota, quota.DeviceQuota)
	}
	if _, err := tx.Exec(
		`INSERT INTO account_group_devices (group_id, peer_id, added_by, created_at)
		 VALUES (?, ?, ?, ?)`,
		groupID, peerID, addedBy, unixMillis(created),
	); err != nil {
		if isUniqueViolation(err) {
			return GroupDevice{}, ErrGroupDeviceExists
		}
		return GroupDevice{}, fmt.Errorf("account store %s: add group device: %w", s.path, err)
	}
	if err := tx.Commit(); err != nil {
		return GroupDevice{}, fmt.Errorf("account store %s: commit group device add: %w", s.path, err)
	}
	return GroupDevice{PeerID: peerID, AddedBy: addedBy, CreatedAt: created}, nil
}

func (s *Store) RemoveGroupDevice(groupID, peerID string) error {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return ErrInvalidAccountGroupID
	}
	peerID = strings.TrimSpace(peerID)
	if !registry.ValidID(peerID) {
		return ErrInvalidPeerID
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(
		`DELETE FROM account_group_devices WHERE group_id = ? AND peer_id = ?`,
		groupID, peerID,
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

// groupEventFilter selects history rows that belong to a group: by member
// account or by a device the group owns. It is shared by connection and file
// transfer queries, which have the same user/peer/host columns.
const groupEventFilter = `(user_id IN (SELECT user_id FROM account_group_members WHERE group_id = ?)
	OR peer_id IN (SELECT peer_id FROM account_group_devices WHERE group_id = ?)
	OR host_id IN (SELECT peer_id FROM account_group_devices WHERE group_id = ?))`

// ConnectionsForGroupBefore pages a group's session history. Optional
// username and host filters narrow the group window further.
func (s *Store) ConnectionsForGroupBefore(groupID, username, hostID string, beforeID int64, limit int) ([]ConnectionEvent, error) {
	return s.ConnectionsForGroupBeforeRange(groupID, username, hostID, TimeBounds{}, beforeID, limit)
}

// ConnectionsForGroupBeforeRange is ConnectionsForGroupBefore narrowed to an
// inclusive window on started_at.
func (s *Store) ConnectionsForGroupBeforeRange(groupID, username, hostID string, bounds TimeBounds, beforeID int64, limit int) ([]ConnectionEvent, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return nil, ErrInvalidAccountGroupID
	}
	clauses := []string{groupEventFilter}
	args := []any{groupID, groupID, groupID}
	if username = strings.TrimSpace(username); username != "" {
		clauses = append(clauses, "username = ? COLLATE NOCASE")
		args = append(args, username)
	}
	if hostID = strings.TrimSpace(hostID); hostID != "" {
		clauses = append(clauses, "(host_id = ? OR peer_id = ? COLLATE NOCASE)")
		args = append(args, hostID, hostID)
	}
	clauses, args = addTimeBounds(clauses, args, "started_at", bounds)
	return s.connectionEvents(strings.Join(clauses, " AND "), args, beforeID, limit)
}

// FileTransfersForGroupBefore is the file-transfer counterpart of
// ConnectionsForGroupBefore.
func (s *Store) FileTransfersForGroupBefore(groupID, username, hostID string, beforeID int64, limit int) ([]FileEvent, error) {
	return s.FileTransfersForGroupBeforeRange(groupID, username, hostID, TimeBounds{}, beforeID, limit)
}

// FileTransfersForGroupBeforeRange is FileTransfersForGroupBefore narrowed to
// an inclusive window on occurred_at.
func (s *Store) FileTransfersForGroupBeforeRange(groupID, username, hostID string, bounds TimeBounds, beforeID int64, limit int) ([]FileEvent, error) {
	groupID = strings.TrimSpace(groupID)
	if !registry.ValidID(groupID) {
		return nil, ErrInvalidAccountGroupID
	}
	clauses := []string{groupEventFilter}
	args := []any{groupID, groupID, groupID}
	if username = strings.TrimSpace(username); username != "" {
		clauses = append(clauses, "username = ? COLLATE NOCASE")
		args = append(args, username)
	}
	if hostID = strings.TrimSpace(hostID); hostID != "" {
		clauses = append(clauses, "(host_id = ? OR peer_id = ? COLLATE NOCASE)")
		args = append(args, hostID, hostID)
	}
	clauses, args = addTimeBounds(clauses, args, "occurred_at", bounds)
	return s.fileEvents(strings.Join(clauses, " AND "), args, beforeID, limit)
}
