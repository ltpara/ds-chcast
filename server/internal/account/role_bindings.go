package account

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// RoleBindingView is one binding plus the role and account names needed by the
// console without a second lookup per row.
type RoleBindingView struct {
	RoleBinding
	RoleName string `json:"role_name"`
}

// EffectiveConsolePermissions resolves a fixed role and every applicable named
// binding. Global bindings always apply. Group bindings apply when groupID is
// set. Object bindings apply only when both objectKind and objectID match.
func (s *Store) EffectiveConsolePermissions(userID, groupID, objectKind, objectID string) (map[string]bool, error) {
	userID = strings.TrimSpace(userID)
	if userID == "" {
		return nil, ErrUnknownUser
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var fixedRole string
	if err := s.db.QueryRow(`SELECT role FROM users WHERE id = ?`, userID).Scan(&fixedRole); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return nil, ErrUnknownUser
		}
		return nil, fmt.Errorf("account store %s: read binding subject: %w", s.path, err)
	}
	out := make(map[string]bool)
	if keys, ok := BuiltinRolePermissions(fixedRole); ok {
		for _, key := range keys {
			out[key] = true
		}
	}
	rows, err := s.db.Query(
		`SELECT r.permissions, b.scope_kind, b.scope_value, b.object_kind
		 FROM role_bindings b JOIN admin_roles r ON r.id = b.role_id
		 WHERE b.user_id = ?`, userID)
	if err != nil {
		return nil, fmt.Errorf("account store %s: read effective role bindings: %w", s.path, err)
	}
	defer rows.Close()
	for rows.Next() {
		var permissions, scopeKind, scopeValue, bindingKind string
		if err := rows.Scan(&permissions, &scopeKind, &scopeValue, &bindingKind); err != nil {
			return nil, fmt.Errorf("account store %s: scan effective role binding: %w", s.path, err)
		}
		if !bindingApplies(scopeKind, scopeValue, bindingKind, groupID, objectKind, objectID) {
			continue
		}
		for _, key := range strings.Split(permissions, ",") {
			if key != "" {
				out[key] = true
			}
		}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list effective role bindings: %w", s.path, err)
	}
	return out, nil
}

func bindingApplies(scopeKind, scopeValue, bindingKind, groupID, objectKind, objectID string) bool {
	switch scopeKind {
	case RoleScopeGlobal:
		return true
	case RoleScopeGroup:
		return groupID != "" && scopeValue == groupID
	case RoleScopeObject:
		return objectKind != "" && objectID != "" &&
			bindingKind == objectKind && scopeValue == objectID
	default:
		return false
	}
}

// RoleBindings lists bindings, optionally narrowed to one account. The list
// includes the role and username so the console can render the access matrix.
func (s *Store) RoleBindings(userID string) ([]RoleBindingView, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	query := `SELECT b.id, b.role_id, b.user_id, u.username, b.scope_kind,
	                 b.scope_value, b.object_kind, b.created_at, r.name
	          FROM role_bindings b
	          JOIN users u ON u.id = b.user_id
	          JOIN admin_roles r ON r.id = b.role_id`
	args := make([]any, 0, 1)
	if strings.TrimSpace(userID) != "" {
		query += ` WHERE b.user_id = ?`
		args = append(args, strings.TrimSpace(userID))
	}
	query += ` ORDER BY LOWER(u.username), b.scope_kind, b.object_kind, b.scope_value, b.id`
	rows, err := s.db.Query(query, args...)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list role bindings: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]RoleBindingView, 0)
	for rows.Next() {
		var (
			binding RoleBindingView
			created int64
		)
		if err := rows.Scan(&binding.ID, &binding.RoleID, &binding.UserID,
			&binding.Username, &binding.ScopeKind, &binding.ScopeValue,
			&binding.ObjectKind, &created, &binding.RoleName); err != nil {
			return nil, fmt.Errorf("account store %s: scan role binding: %w", s.path, err)
		}
		binding.CreatedAt = timeFromMillis(created)
		out = append(out, binding)
	}
	return out, rows.Err()
}

// BindRole assigns a named role at global, group or object scope. The target
// account must exist. Group scope requires an account group; object scope
// requires a non-empty stable kind and id. Duplicate bindings are idempotent-
// free: the caller must remove the old binding first so audit is explicit.
func (s *Store) BindRole(username, roleID, scopeKind, scopeValue, objectKind string) (RoleBindingView, error) {
	roleID = strings.TrimSpace(roleID)
	scopeKind = strings.ToLower(strings.TrimSpace(scopeKind))
	scopeValue = strings.TrimSpace(scopeValue)
	objectKind = strings.ToLower(strings.TrimSpace(objectKind))
	if scopeKind == "" {
		scopeKind = RoleScopeGlobal
	}
	if scopeKind != RoleScopeGlobal && scopeKind != RoleScopeGroup && scopeKind != RoleScopeObject {
		return RoleBindingView{}, ErrInvalidRoleScope
	}
	// A group binding must retain its group id. Only global and object
	// bindings use scope_value directly; object ids are validated below.
	if scopeKind == RoleScopeGlobal {
		scopeValue = ""
		objectKind = ""
	} else if scopeKind == RoleScopeGroup {
		objectKind = ""
	}
	if scopeKind == RoleScopeGroup {
		if scopeValue == "" || !registry.ValidID(scopeValue) {
			return RoleBindingView{}, ErrInvalidRoleScope
		}
	}
	if scopeKind == RoleScopeObject {
		if objectKind == "" || scopeValue == "" || !registry.ValidID(scopeValue) {
			return RoleBindingView{}, ErrInvalidRoleScope
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var (
		userID   string
		roleName string
	)
	if err := s.db.QueryRow(`SELECT id FROM users WHERE username = ?`, strings.TrimSpace(username)).Scan(&userID); errors.Is(err, sql.ErrNoRows) {
		return RoleBindingView{}, ErrUnknownUser
	} else if err != nil {
		return RoleBindingView{}, fmt.Errorf("account store %s: read binding user: %w", s.path, err)
	}
	if err := s.db.QueryRow(`SELECT name FROM admin_roles WHERE id = ?`, roleID).Scan(&roleName); errors.Is(err, sql.ErrNoRows) {
		return RoleBindingView{}, ErrUnknownAdminRole
	} else if err != nil {
		return RoleBindingView{}, fmt.Errorf("account store %s: read binding role: %w", s.path, err)
	}
	if scopeKind == RoleScopeGroup {
		var exists int
		if err := s.db.QueryRow(`SELECT 1 FROM account_groups WHERE id = ?`, scopeValue).Scan(&exists); errors.Is(err, sql.ErrNoRows) {
			return RoleBindingView{}, ErrUnknownAccountGroup
		} else if err != nil {
			return RoleBindingView{}, fmt.Errorf("account store %s: read binding group: %w", s.path, err)
		}
	}
	id, err := newRandomHex(12)
	if err != nil {
		return RoleBindingView{}, err
	}
	now := s.now().UTC()
	if _, err := s.db.Exec(
		`INSERT INTO role_bindings
			(id, role_id, user_id, scope_kind, scope_value, object_kind, created_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		id, roleID, userID, scopeKind, scopeValue, objectKind, unixMillis(now)); err != nil {
		if isUniqueViolation(err) {
			return RoleBindingView{}, ErrRoleBindingExists
		}
		return RoleBindingView{}, fmt.Errorf("account store %s: bind role: %w", s.path, err)
	}
	return RoleBindingView{
		RoleBinding: RoleBinding{
			ID: id, RoleID: roleID, UserID: userID, Username: strings.TrimSpace(username),
			ScopeKind: scopeKind, ScopeValue: scopeValue, ObjectKind: objectKind, CreatedAt: now,
		},
		RoleName: roleName,
	}, nil
}

// UnbindRole removes one binding and returns it for the audit trail.
func (s *Store) UnbindRole(bindingID string) (RoleBindingView, error) {
	bindingID = strings.TrimSpace(bindingID)
	if bindingID == "" {
		return RoleBindingView{}, ErrUnknownRoleBinding
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return RoleBindingView{}, fmt.Errorf("account store %s: begin unbind role: %w", s.path, err)
	}
	defer tx.Rollback()
	var binding RoleBindingView
	var created int64
	err = tx.QueryRow(
		`SELECT b.id, b.role_id, b.user_id, u.username, b.scope_kind,
		        b.scope_value, b.object_kind, b.created_at, r.name
		 FROM role_bindings b
		 JOIN users u ON u.id = b.user_id
		 JOIN admin_roles r ON r.id = b.role_id
		 WHERE b.id = ?`, bindingID,
	).Scan(&binding.ID, &binding.RoleID, &binding.UserID, &binding.Username,
		&binding.ScopeKind, &binding.ScopeValue, &binding.ObjectKind, &created, &binding.RoleName)
	if errors.Is(err, sql.ErrNoRows) {
		return RoleBindingView{}, ErrUnknownRoleBinding
	}
	if err != nil {
		return RoleBindingView{}, fmt.Errorf("account store %s: read role binding: %w", s.path, err)
	}
	binding.CreatedAt = timeFromMillis(created)
	if _, err := tx.Exec(`DELETE FROM role_bindings WHERE id = ?`, bindingID); err != nil {
		return RoleBindingView{}, fmt.Errorf("account store %s: delete role binding: %w", s.path, err)
	}
	if err := ensureLastManagementAdmin(tx); err != nil {
		return RoleBindingView{}, err
	}
	if err := tx.Commit(); err != nil {
		return RoleBindingView{}, fmt.Errorf("account store %s: commit unbind role: %w", s.path, err)
	}
	return binding, nil
}
