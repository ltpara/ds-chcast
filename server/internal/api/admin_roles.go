package api

import (
	"errors"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type adminRoleListResponse struct {
	Roles       []account.AdminRole `json:"roles"`
	Permissions []string            `json:"permissions"`
}

type adminRoleRequest struct {
	ID          string   `json:"id"`
	Name        string   `json:"name"`
	Description string   `json:"description"`
	Permissions []string `json:"permissions"`
}

type roleBindingRequest struct {
	Username   string `json:"username"`
	RoleID     string `json:"role_id"`
	ScopeKind  string `json:"scope_kind"`
	ScopeValue string `json:"scope_value"`
	ObjectKind string `json:"object_kind"`
}

type roleBindingListResponse struct {
	Bindings []account.RoleBindingView `json:"bindings"`
}

func (s *server) rolePrincipal(w http.ResponseWriter, r *http.Request, permission string) (consolePrincipal, bool) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return consolePrincipal{}, false
	}
	principal, ok := s.consolePrincipalFor(w, r, permission, account.TokenResourceAccounts)
	if !ok {
		return consolePrincipal{}, false
	}
	return principal, true
}

func (s *server) roleActor(w http.ResponseWriter, r *http.Request, permission string) (account.User, string, bool) {
	principal, ok := s.rolePrincipal(w, r, permission)
	if !ok {
		return account.User{}, "", false
	}
	return principal.User, principal.Actor, true
}

func permissionSubset(candidate []string, held map[string]bool) bool {
	for _, key := range candidate {
		if !held[key] {
			return false
		}
	}
	return true
}

func (s *server) requireRolePermissionCeiling(w http.ResponseWriter, principal consolePrincipal, permissions []string) bool {
	if principal.Actor == "server-key" || principal.User.Role == account.RoleAdmin {
		return true
	}
	if !permissionSubset(permissions, principal.Permissions) {
		writeError(w, http.StatusForbidden, "role permissions exceed the caller's effective permissions")
		return false
	}
	return true
}

func (s *server) requireBindingPermissionCeiling(w http.ResponseWriter, principal consolePrincipal, role account.AdminRole) bool {
	if principal.Actor == "server-key" || principal.User.Role == account.RoleAdmin {
		return true
	}
	if err := s.cfg.Accounts.RoleBindingEscalation(principal.User.ID, role); err != nil {
		if errors.Is(err, account.ErrPermissionDenied) {
			writeError(w, http.StatusForbidden, "role binding would exceed the caller's effective permissions")
			return false
		}
		s.cfg.Logger.Error("role binding ceiling", "error", err)
		writeError(w, http.StatusInternalServerError, "could not check role bindings")
		return false
	}
	return true
}

func (s *server) listAdminRoles(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.roleActor(w, r, account.PermissionRolesRead); !ok {
		return
	}
	roles, err := s.cfg.Accounts.ListAdminRoles()
	if err != nil {
		s.cfg.Logger.Error("list admin roles", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list admin roles")
		return
	}
	writeJSON(w, http.StatusOK, adminRoleListResponse{
		Roles: roles, Permissions: account.AllPermissions(),
	})
}

func (s *server) getAdminRole(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.roleActor(w, r, account.PermissionRolesRead); !ok {
		return
	}
	role, err := s.cfg.Accounts.AdminRole(r.PathValue("id"))
	if err != nil {
		writeRoleError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, role)
}

func (s *server) createAdminRole(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.rolePrincipal(w, r, account.PermissionRolesWrite)
	if !ok {
		return
	}
	var req adminRoleRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !s.requireRolePermissionCeiling(w, principal, req.Permissions) {
		return
	}
	role, err := s.cfg.Accounts.CreateAdminRole(req.ID, req.Name, req.Description, req.Permissions)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditRoleCreate,
		"created role "+role.ID+" permissions="+strings.Join(role.Permissions, ","), true)
	writeJSON(w, http.StatusCreated, role)
}

func (s *server) updateAdminRole(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.rolePrincipal(w, r, account.PermissionRolesWrite)
	if !ok {
		return
	}
	existing, err := s.cfg.Accounts.AdminRole(r.PathValue("id"))
	if err != nil {
		writeRoleError(w, err)
		return
	}
	if !s.requireRolePermissionCeiling(w, principal, existing.Permissions) {
		return
	}
	var req adminRoleRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if !s.requireRolePermissionCeiling(w, principal, req.Permissions) {
		return
	}
	role, err := s.cfg.Accounts.UpdateAdminRole(existing.ID, req.Name, req.Description, req.Permissions)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditRoleUpdate,
		"updated role "+role.ID+" permissions="+strings.Join(role.Permissions, ","), true)
	writeJSON(w, http.StatusOK, role)
}

func (s *server) deleteAdminRole(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.rolePrincipal(w, r, account.PermissionRolesWrite)
	if !ok {
		return
	}
	existing, err := s.cfg.Accounts.AdminRole(r.PathValue("id"))
	if err != nil {
		writeRoleError(w, err)
		return
	}
	if !s.requireRolePermissionCeiling(w, principal, existing.Permissions) {
		return
	}
	if err := s.cfg.Accounts.DeleteAdminRole(existing.ID); err != nil {
		writeRoleError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditRoleDelete, "deleted role "+existing.ID, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) listRoleBindings(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.rolePrincipal(w, r, account.PermissionRolesRead)
	if !ok {
		return
	}
	userID := strings.TrimSpace(r.URL.Query().Get("user_id"))
	username := strings.TrimSpace(r.URL.Query().Get("username"))
	if userID == "" && username != "" {
		user, err := s.cfg.Accounts.FindUser(username)
		if err != nil {
			writeAccountError(w, err)
			return
		}
		userID = user.ID
	}
	bindings, err := s.cfg.Accounts.RoleBindings(userID)
	if err != nil {
		s.cfg.Logger.Error("list role bindings", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list role bindings")
		return
	}
	if !s.canReadAllBindings(principal) {
		filtered := bindings[:0]
		for _, binding := range bindings {
			if binding.UserID == principal.User.ID {
				filtered = append(filtered, binding)
			}
		}
		bindings = filtered
	}
	writeJSON(w, http.StatusOK, roleBindingListResponse{Bindings: bindings})
}

func (s *server) canReadAllBindings(principal consolePrincipal) bool {
	return principal.Actor == "server-key" || principal.User.Role == account.RoleAdmin
}

// roleBindingByID is used before an unbind so the caller's permission ceiling
// can be applied to the role that is about to be removed.
func (s *server) roleBindingByID(id string) (account.RoleBindingView, error) {
	bindings, err := s.cfg.Accounts.RoleBindings("")
	if err != nil {
		return account.RoleBindingView{}, err
	}
	for _, binding := range bindings {
		if binding.ID == id {
			return binding, nil
		}
	}
	return account.RoleBindingView{}, account.ErrUnknownRoleBinding
}

func (s *server) createRoleBinding(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.rolePrincipal(w, r, account.PermissionRolesWrite)
	if !ok {
		return
	}
	var req roleBindingRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	role, err := s.cfg.Accounts.AdminRole(req.RoleID)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	if !s.requireBindingPermissionCeiling(w, principal, role) {
		return
	}
	binding, err := s.cfg.Accounts.BindRole(req.Username, req.RoleID, req.ScopeKind, req.ScopeValue, req.ObjectKind)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditRoleBind,
		"bound role "+binding.RoleID+" to "+binding.Username+" scope="+binding.ScopeKind+
			" value="+binding.ScopeValue+" object="+binding.ObjectKind, true)
	writeJSON(w, http.StatusCreated, binding)
}

func (s *server) deleteRoleBinding(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.rolePrincipal(w, r, account.PermissionRolesWrite)
	if !ok {
		return
	}
	bindingID := r.PathValue("id")
	binding, err := s.roleBindingByID(bindingID)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	role, err := s.cfg.Accounts.AdminRole(binding.RoleID)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	if !s.requireBindingPermissionCeiling(w, principal, role) {
		return
	}
	binding, err = s.cfg.Accounts.UnbindRole(bindingID)
	if err != nil {
		writeRoleError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditRoleUnbind,
		"unbound role "+binding.RoleID+" from "+binding.Username, true)
	w.WriteHeader(http.StatusNoContent)
}

func writeRoleError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownAdminRole),
		errors.Is(err, account.ErrUnknownRoleBinding),
		errors.Is(err, account.ErrUnknownUser),
		errors.Is(err, account.ErrUnknownAccountGroup):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrInvalidAdminRoleID),
		errors.Is(err, account.ErrInvalidAdminRoleName),
		errors.Is(err, account.ErrInvalidAdminPermission),
		errors.Is(err, account.ErrInvalidRoleScope):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrAdminRoleExists),
		errors.Is(err, account.ErrRoleBindingExists),
		errors.Is(err, account.ErrBuiltinRoleImmutable),
		errors.Is(err, account.ErrLastManagementAdmin):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrPermissionDenied):
		writeError(w, http.StatusForbidden, err.Error())
	default:
		writeError(w, http.StatusInternalServerError, "could not complete the role change")
	}
}
