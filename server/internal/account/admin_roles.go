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

// PermissionKey names one stable console operation. Role definitions persist
// these keys, not HTTP routes or UI labels, so moving a handler does not
// silently grant or revoke an operation.
const (
	PermissionDevicesRead           = "devices.read"
	PermissionDevicesWrite          = "devices.write"
	PermissionDevicesDelete         = "devices.delete"
	PermissionDevicesPolicyRead     = "devices.policy.read"
	PermissionDevicesPolicyWrite    = "devices.policy.write"
	PermissionConnectionsRead       = "connections.read"
	PermissionConnectionsDisconnect = "connections.disconnect"
	PermissionAuditRead             = "audit.read"
	PermissionAccountsRead          = "accounts.read"
	PermissionAccountsWrite         = "accounts.write"
	PermissionGroupsRead            = "groups.read"
	PermissionGroupsWrite           = "groups.write"
	PermissionRecordingsRead        = "recordings.read"
	PermissionRecordingsDelete      = "recordings.delete"
	PermissionOpsRead               = "ops.read"
	PermissionRolesRead             = "roles.read"
	PermissionRolesWrite            = "roles.write"
)

// RoleScope restricts where a named role applies. Global roles apply to every
// object; group roles apply only to the named account group; object roles are
// checked against the exact kind and id bound in role_bindings.
const (
	RoleScopeGlobal = "global"
	RoleScopeGroup  = "group"
	RoleScopeObject = "object"
)

var (
	ErrInvalidAdminRoleID     = errors.New("admin role id must match [A-Za-z0-9][A-Za-z0-9_.-]{0,63}")
	ErrInvalidAdminRoleName   = errors.New("admin role name is required")
	ErrUnknownAdminRole       = errors.New("unknown admin role")
	ErrAdminRoleExists        = errors.New("admin role already exists")
	ErrInvalidAdminPermission = errors.New("unknown console permission")
	ErrBuiltinRoleImmutable   = errors.New("built-in roles cannot be changed")
	ErrRoleBindingExists      = errors.New("role binding already exists")
	ErrUnknownRoleBinding     = errors.New("unknown role binding")
	ErrInvalidRoleScope       = errors.New("role scope must be global, group or object")
	ErrLastManagementAdmin    = ErrLastAdmin
)

// AdminRole is a named permission set. Built-in roles are materialised by the
// migration and are not editable; named roles may be changed and bound.
type AdminRole struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Description string    `json:"description,omitempty"`
	Permissions []string  `json:"permissions"`
	Builtin     bool      `json:"builtin"`
	CreatedAt   time.Time `json:"created_at"`
	UpdatedAt   time.Time `json:"updated_at"`
}

// RoleBinding assigns one named role to one account. ScopeValue is empty for a
// global binding, a group id for group scope, and a stable object id for object
// scope. ObjectKind is required for object scope.
type RoleBinding struct {
	ID         string    `json:"id"`
	RoleID     string    `json:"role_id"`
	UserID     string    `json:"user_id"`
	Username   string    `json:"username,omitempty"`
	ScopeKind  string    `json:"scope_kind"`
	ScopeValue string    `json:"scope_value,omitempty"`
	ObjectKind string    `json:"object_kind,omitempty"`
	CreatedAt  time.Time `json:"created_at"`
}

// AllPermissions returns the stable permission catalogue.
func AllPermissions() []string {
	return []string{
		PermissionAccountsRead,
		PermissionAccountsWrite,
		PermissionAuditRead,
		PermissionConnectionsDisconnect,
		PermissionConnectionsRead,
		PermissionDevicesDelete,
		PermissionDevicesPolicyRead,
		PermissionDevicesPolicyWrite,
		PermissionDevicesRead,
		PermissionDevicesWrite,
		PermissionGroupsRead,
		PermissionGroupsWrite,
		PermissionOpsRead,
		PermissionRecordingsDelete,
		PermissionRecordingsRead,
		PermissionRolesRead,
		PermissionRolesWrite,
	}
}

// ValidPermissionKey reports whether key is a stable console permission.
func ValidPermissionKey(key string) bool {
	key = strings.TrimSpace(key)
	i := sort.SearchStrings(AllPermissions(), key)
	return i < len(AllPermissions()) && AllPermissions()[i] == key
}

func normalizePermissionKeys(keys []string) ([]string, error) {
	seen := make(map[string]bool, len(keys))
	out := make([]string, 0, len(keys))
	for _, raw := range keys {
		key := strings.ToLower(strings.TrimSpace(raw))
		if !ValidPermissionKey(key) {
			return nil, ErrInvalidAdminPermission
		}
		if seen[key] {
			continue
		}
		seen[key] = true
		out = append(out, key)
	}
	sort.Strings(out)
	return out, nil
}

// PermissionAllowedForTokenResource reports whether a stable permission is
// reachable through one machine-token resource family. It prevents a token
// minted for devices from gaining role management or account access merely
// because its owner holds a named role with those permissions.
func PermissionAllowedForTokenResource(permission, resource string) bool {
	if resource == TokenResourceAll {
		return true
	}
	switch resource {
	case TokenResourceDevices:
		return strings.HasPrefix(permission, "devices.")
	case TokenResourceAudit:
		return permission == PermissionAuditRead
	case TokenResourceAccounts:
		return permission == PermissionAccountsRead || permission == PermissionAccountsWrite
	case TokenResourceGroups:
		return permission == PermissionGroupsRead || permission == PermissionGroupsWrite
	case TokenResourceRecordings:
		return permission == PermissionRecordingsRead || permission == PermissionRecordingsDelete
	case TokenResourceOps:
		return permission == PermissionOpsRead
	}
	return false
}

// BuiltinRolePermissions maps the existing account roles to permission keys.
// The fixed roles keep their prior ladder while all new authorization flows
// through the same catalogue as named roles.
func BuiltinRolePermissions(role string) ([]string, bool) {
	var keys []string
	switch role {
	case RoleAdmin:
		keys = AllPermissions()
	case RoleOperator:
		keys = []string{
			PermissionAuditRead, PermissionConnectionsDisconnect,
			PermissionConnectionsRead, PermissionDevicesDelete,
			PermissionDevicesPolicyRead, PermissionDevicesRead,
			PermissionDevicesWrite, PermissionGroupsRead,
			PermissionOpsRead, PermissionRecordingsDelete, PermissionRecordingsRead,
		}
	case RoleReadonly:
		keys = []string{
			PermissionAuditRead, PermissionConnectionsRead,
			PermissionDevicesPolicyRead, PermissionDevicesRead,
			PermissionGroupsRead, PermissionOpsRead,
			PermissionRecordingsRead,
		}
	default:
		return nil, false
	}
	out, _ := normalizePermissionKeys(keys)
	return out, true
}

func permissionSet(role AdminRole) map[string]bool {
	out := make(map[string]bool, len(role.Permissions))
	for _, key := range role.Permissions {
		out[key] = true
	}
	return out
}

func scanAdminRole(row rowScanner) (AdminRole, error) {
	var (
		role             AdminRole
		permissions      string
		created, updated int64
		builtin          int
	)
	if err := row.Scan(&role.ID, &role.Name, &role.Description, &permissions,
		&builtin, &created, &updated); err != nil {
		return AdminRole{}, err
	}
	role.Builtin = builtin != 0
	if permissions != "" {
		role.Permissions = strings.Split(permissions, ",")
	}
	role.CreatedAt = timeFromMillis(created)
	role.UpdatedAt = timeFromMillis(updated)
	return role, nil
}

func (s *Store) ListAdminRoles() ([]AdminRole, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT id, name, description, permissions, builtin, created_at, updated_at
		 FROM admin_roles ORDER BY builtin DESC, LOWER(name), id`)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list admin roles: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]AdminRole, 0)
	for rows.Next() {
		role, err := scanAdminRole(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: scan admin role: %w", s.path, err)
		}
		out = append(out, role)
	}
	return out, rows.Err()
}

func (s *Store) AdminRole(id string) (AdminRole, error) {
	id = strings.TrimSpace(id)
	if id == "" {
		return AdminRole{}, ErrUnknownAdminRole
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	role, err := scanAdminRole(s.db.QueryRow(
		`SELECT id, name, description, permissions, builtin, created_at, updated_at
		 FROM admin_roles WHERE id = ?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return AdminRole{}, ErrUnknownAdminRole
	}
	if err != nil {
		return AdminRole{}, fmt.Errorf("account store %s: read admin role: %w", s.path, err)
	}
	return role, nil
}

// CreateAdminRole creates a reusable named permission set. Built-in ids are
// reserved by the primary key, so a custom role cannot shadow compatibility.
func (s *Store) CreateAdminRole(id, name, description string, permissions []string) (AdminRole, error) {
	id = strings.TrimSpace(id)
	if !registry.ValidID(id) {
		return AdminRole{}, ErrInvalidAdminRoleID
	}
	name = normalizeText(name, maxName)
	if name == "" {
		return AdminRole{}, ErrInvalidAdminRoleName
	}
	perms, err := normalizePermissionKeys(permissions)
	if err != nil {
		return AdminRole{}, err
	}
	description = normalizeText(description, maxGroupDescription)
	now := s.now().UTC()
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, err := s.db.Exec(
		`INSERT INTO admin_roles (id, name, description, permissions, builtin, created_at, updated_at)
		 VALUES (?, ?, ?, ?, 0, ?, ?)`,
		id, name, description, strings.Join(perms, ","), unixMillis(now), unixMillis(now),
	); err != nil {
		if isUniqueViolation(err) {
			return AdminRole{}, ErrAdminRoleExists
		}
		return AdminRole{}, fmt.Errorf("account store %s: create admin role: %w", s.path, err)
	}
	return AdminRole{ID: id, Name: name, Description: description, Permissions: perms,
		CreatedAt: now, UpdatedAt: now}, nil
}

func (s *Store) UpdateAdminRole(id, name, description string, permissions []string) (AdminRole, error) {
	id = strings.TrimSpace(id)
	perms, err := normalizePermissionKeys(permissions)
	if err != nil {
		return AdminRole{}, err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return AdminRole{}, fmt.Errorf("account store %s: begin update admin role: %w", s.path, err)
	}
	defer tx.Rollback()
	var (
		builtin int
		created int64
	)
	if err := tx.QueryRow(`SELECT builtin, created_at FROM admin_roles WHERE id = ?`, id).Scan(&builtin, &created); errors.Is(err, sql.ErrNoRows) {
		return AdminRole{}, ErrUnknownAdminRole
	} else if err != nil {
		return AdminRole{}, fmt.Errorf("account store %s: read admin role: %w", s.path, err)
	}
	if builtin != 0 {
		return AdminRole{}, ErrBuiltinRoleImmutable
	}
	name = normalizeText(name, maxName)
	if name == "" {
		return AdminRole{}, ErrInvalidAdminRoleName
	}
	description = normalizeText(description, maxGroupDescription)
	now := s.now().UTC()
	if _, err := tx.Exec(
		`UPDATE admin_roles SET name = ?, description = ?, permissions = ?, updated_at = ? WHERE id = ?`,
		name, description, strings.Join(perms, ","), unixMillis(now), id); err != nil {
		return AdminRole{}, fmt.Errorf("account store %s: update admin role: %w", s.path, err)
	}
	if err := ensureLastManagementAdmin(tx); err != nil {
		return AdminRole{}, err
	}
	if err := tx.Commit(); err != nil {
		return AdminRole{}, fmt.Errorf("account store %s: commit admin role: %w", s.path, err)
	}
	return AdminRole{ID: id, Name: name, Description: description, Permissions: perms,
		CreatedAt: timeFromMillis(created), UpdatedAt: now}, nil
}

// DeleteAdminRole removes a custom role and every binding that references it.
func (s *Store) DeleteAdminRole(id string) error {
	id = strings.TrimSpace(id)
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return fmt.Errorf("account store %s: begin delete admin role: %w", s.path, err)
	}
	defer tx.Rollback()
	var builtin int
	if err := tx.QueryRow(`SELECT builtin FROM admin_roles WHERE id = ?`, id).Scan(&builtin); errors.Is(err, sql.ErrNoRows) {
		return ErrUnknownAdminRole
	} else if err != nil {
		return fmt.Errorf("account store %s: read admin role: %w", s.path, err)
	}
	if builtin != 0 {
		return ErrBuiltinRoleImmutable
	}
	if _, err := tx.Exec(`DELETE FROM role_bindings WHERE role_id = ?`, id); err != nil {
		return fmt.Errorf("account store %s: delete role bindings: %w", s.path, err)
	}
	if _, err := tx.Exec(`DELETE FROM admin_roles WHERE id = ?`, id); err != nil {
		return fmt.Errorf("account store %s: delete admin role: %w", s.path, err)
	}
	if err := ensureLastManagementAdmin(tx); err != nil {
		return err
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("account store %s: commit delete admin role: %w", s.path, err)
	}
	return nil
}

// applyNamedConsoleRoles creates the durable role catalogue and assignments.
// The DDL is intentionally portable across SQLite and PostgreSQL.
func applyNamedConsoleRoles(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS admin_roles (
			id          TEXT PRIMARY KEY,
			name        TEXT NOT NULL,
			description TEXT NOT NULL DEFAULT '',
			permissions TEXT NOT NULL DEFAULT '',
			builtin     INTEGER NOT NULL DEFAULT 0,
			created_at  BIGINT NOT NULL,
			updated_at  BIGINT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS role_bindings (
			id          TEXT PRIMARY KEY,
			role_id     TEXT NOT NULL REFERENCES admin_roles(id) ON DELETE CASCADE,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			scope_kind  TEXT NOT NULL,
			scope_value TEXT NOT NULL DEFAULT '',
			object_kind TEXT NOT NULL DEFAULT '',
			created_at  BIGINT NOT NULL,
			UNIQUE (role_id, user_id, scope_kind, scope_value, object_kind)
		)`,
		`CREATE INDEX IF NOT EXISTS role_bindings_user
			ON role_bindings(user_id)`,
		`CREATE INDEX IF NOT EXISTS role_bindings_scope
			ON role_bindings(scope_kind, scope_value, object_kind)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create named console role schema: %w", err)
		}
	}
	now := unixMillis(time.Now().UTC())
	seeds := []struct {
		id, name, description string
		role                  string
	}{
		{RoleAdmin, "Administrator", "Full console access", RoleAdmin},
		{RoleOperator, "Operator", "Device operations and live connections", RoleOperator},
		{RoleReadonly, "Read only", "Read-only console access", RoleReadonly},
	}
	for _, seed := range seeds {
		permissions, _ := BuiltinRolePermissions(seed.role)
		if _, err := db.Exec(
			`INSERT INTO admin_roles
				(id, name, description, permissions, builtin, created_at, updated_at)
			 VALUES (?, ?, ?, ?, 1, ?, ?)
			 ON CONFLICT(id) DO UPDATE SET
				name = excluded.name,
				description = excluded.description,
				permissions = excluded.permissions,
				builtin = 1,
				updated_at = excluded.updated_at`,
			seed.id, seed.name, seed.description, strings.Join(permissions, ","), now, now,
		); err != nil {
			return fmt.Errorf("seed built-in role %s: %w", seed.id, err)
		}
	}
	return nil
}

func permissionSetContains(keys []string, want string) bool {
	for _, key := range keys {
		if key == want {
			return true
		}
	}
	return false
}

func effectiveGlobalManagementPermissions(fixedRole, permissions string) bool {
	if keys, ok := BuiltinRolePermissions(fixedRole); ok &&
		permissionSetContains(keys, PermissionAccountsWrite) {
		return true
	}
	for _, key := range strings.Split(permissions, ",") {
		if strings.TrimSpace(key) == PermissionAccountsWrite {
			return true
		}
	}
	return false
}

// ensureLastManagementAdmin checks the post-mutation state of the current
// transaction. A fixed admin and a global named role carrying accounts.write
// are equivalent; group/object-scoped assignments do not protect the
// deployment because they cannot manage every account. No users is allowed so
// a fresh deployment can bootstrap its first account as admin.
func ensureLastManagementAdmin(tx *sql.Tx) error {
	var total int
	if err := tx.QueryRow(`SELECT COUNT(*) FROM users`).Scan(&total); err != nil {
		return fmt.Errorf("count accounts after console role change: %w", err)
	}
	if total == 0 {
		return nil
	}
	rows, err := tx.Query(
		`SELECT u.id, u.role, COALESCE(r.permissions, '')
		 FROM users u
		 LEFT JOIN role_bindings b
			ON b.user_id = u.id AND b.scope_kind = ?
		 LEFT JOIN admin_roles r ON r.id = b.role_id
		 WHERE u.banned = 0`,
		RoleScopeGlobal,
	)
	if err != nil {
		return fmt.Errorf("read global management assignments: %w", err)
	}
	defer rows.Close()
	managers := make(map[string]bool)
	for rows.Next() {
		var id, fixedRole, permissions string
		if err := rows.Scan(&id, &fixedRole, &permissions); err != nil {
			return fmt.Errorf("scan global management assignment: %w", err)
		}
		if effectiveGlobalManagementPermissions(fixedRole, permissions) {
			managers[id] = true
		}
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("list global management assignments: %w", err)
	}
	if len(managers) == 0 {
		return ErrLastManagementAdmin
	}
	return nil
}
