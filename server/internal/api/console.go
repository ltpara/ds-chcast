package api

import (
	"net/http"
	"sort"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// consolePermission is retained for compatibility with the fixed API-token
// role ladder and older group-membership checks. Interactive account requests
// are authorized by stable permission keys in permissions.
type consolePermission int

const (
	consoleRead consolePermission = iota
	consoleWrite
	consoleAdmin
)

func rolePermission(role string) (consolePermission, bool) {
	switch role {
	case account.RoleAdmin:
		return consoleAdmin, true
	case account.RoleOperator:
		return consoleWrite, true
	case account.RoleReadonly:
		return consoleRead, true
	}
	return consoleRead, false
}

// consolePrincipal is one authenticated console caller. Permissions is the
// effective set for the target scope. GroupID is the token or request scope;
// ObjectKind/ObjectID identify an object-scoped authorization check.
type consolePrincipal struct {
	User        account.User
	Actor       string
	Permission  consolePermission
	Permissions map[string]bool
	GroupID     string
	ObjectKind  string
	ObjectID    string
	APIToken    bool
}

func permissionSet(keys []string) map[string]bool {
	out := make(map[string]bool, len(keys))
	for _, key := range keys {
		out[key] = true
	}
	return out
}

func permissionKeys(set map[string]bool) []string {
	out := make([]string, 0, len(set))
	for key, ok := range set {
		if ok {
			out = append(out, key)
		}
	}
	sort.Strings(out)
	return out
}

func tokenCoversResource(resources []string, resource string) bool {
	if resource == "" {
		return false
	}
	for _, held := range resources {
		if held == account.TokenResourceAll || held == resource {
			return true
		}
	}
	return false
}

// consolePermissionKey maps the legacy fixed-role threshold to the primary
// stable key for one resource family. New call sites should pass the stable
// key directly.
func consolePermissionKey(level consolePermission, resource string) string {
	switch level {
	case consoleAdmin:
		switch resource {
		case account.TokenResourceAccounts:
			return account.PermissionAccountsWrite
		case account.TokenResourceGroups:
			return account.PermissionGroupsWrite
		case account.TokenResourceRecordings:
			return account.PermissionRecordingsDelete
		case account.TokenResourceDevices:
			return account.PermissionDevicesPolicyWrite
		case account.TokenResourceAudit:
			return account.PermissionAuditRead
		case account.TokenResourceOps:
			return account.PermissionOpsRead
		}
		return account.PermissionAccountsWrite
	case consoleWrite:
		switch resource {
		case account.TokenResourceAccounts:
			return account.PermissionAccountsWrite
		case account.TokenResourceGroups:
			return account.PermissionGroupsWrite
		case account.TokenResourceRecordings:
			return account.PermissionRecordingsDelete
		case account.TokenResourceDevices:
			return account.PermissionDevicesWrite
		case account.TokenResourceAudit:
			return account.PermissionAuditRead
		case account.TokenResourceOps:
			return account.PermissionOpsRead
		}
		return account.PermissionDevicesWrite
	default:
		switch resource {
		case account.TokenResourceAccounts:
			return account.PermissionAccountsRead
		case account.TokenResourceGroups:
			return account.PermissionGroupsRead
		case account.TokenResourceRecordings:
			return account.PermissionRecordingsRead
		case account.TokenResourceDevices:
			return account.PermissionDevicesRead
		case account.TokenResourceAudit:
			return account.PermissionAuditRead
		case account.TokenResourceOps:
			return account.PermissionOpsRead
		}
		return account.PermissionDevicesRead
	}
}
func (s *server) consolePrincipalFor(w http.ResponseWriter, r *http.Request, need any, resource string) (consolePrincipal, bool) {
	permission := ""
	switch value := need.(type) {
	case string:
		permission = value
	case consolePermission:
		permission = consolePermissionKey(value, resource)
	default:
		writeError(w, http.StatusInternalServerError, "invalid console permission")
		return consolePrincipal{}, false
	}
	if s.serverKeyAuthorized(r) {
		return consolePrincipal{
			Actor:       "server-key",
			Permission:  consoleAdmin,
			Permissions: permissionSet(account.AllPermissions()),
		}, true
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return consolePrincipal{}, false
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return consolePrincipal{}, false
	}
	if account.HasAPITokenPrefix(token) {
		return s.apiTokenPrincipal(w, token, permission, resource)
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return consolePrincipal{}, false
	}
	perms, err := s.cfg.Accounts.EffectiveConsolePermissions(user.ID, "", "", "")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "could not resolve console permissions")
		return consolePrincipal{}, false
	}
	if permission != "" && !perms[permission] {
		writeError(w, http.StatusForbidden, "console permission does not allow this action")
		return consolePrincipal{}, false
	}
	return accountConsolePrincipal(user, perms), true
}

func (s *server) apiTokenPrincipal(w http.ResponseWriter, token, permission, resource string) (consolePrincipal, bool) {
	principal, err := s.cfg.Accounts.AuthenticateAPIToken(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or console token required")
		return consolePrincipal{}, false
	}
	if !tokenCoversResource(principal.Token.Resources, resource) {
		writeError(w, http.StatusForbidden, "API token does not cover this resource")
		return consolePrincipal{}, false
	}
	perms, err := s.cfg.Accounts.EffectiveConsolePermissions(
		principal.User.ID, principal.Token.GroupID, "", "")
	if err != nil {
		writeError(w, http.StatusInternalServerError, "could not resolve console permissions")
		return consolePrincipal{}, false
	}
	rolePerm, ok := rolePermission(principal.Token.Role)
	if ok {
		keys, _ := account.BuiltinRolePermissions(principal.Token.Role)
		ceiling := permissionSet(keys)
		for key := range perms {
			covered := false
			for _, held := range principal.Token.Resources {
				covered = covered || account.PermissionAllowedForTokenResource(key, held)
			}
			if !ceiling[key] || !covered {
				delete(perms, key)
			}
		}
	} else {
		perms = map[string]bool{}
	}
	if permission != "" && !perms[permission] {
		writeError(w, http.StatusForbidden, "API token role does not allow this action")
		return consolePrincipal{}, false
	}
	return consolePrincipal{
		User:        principal.User,
		Actor:       principal.User.Username,
		Permission:  rolePerm,
		Permissions: perms,
		GroupID:     principal.Token.GroupID,
		APIToken:    true,
	}, true
}

func accountConsolePrincipal(user account.User, perms map[string]bool) consolePrincipal {
	perm := consoleRead
	for _, candidate := range []struct {
		need consolePermission
		key  string
	}{
		{consoleAdmin, account.PermissionAccountsWrite},
		{consoleWrite, account.PermissionDevicesWrite},
		{consoleRead, account.PermissionDevicesRead},
	} {
		if perms[candidate.key] {
			perm = candidate.need
			break
		}
	}
	return consolePrincipal{User: user, Actor: user.Username, Permission: perm, Permissions: perms}
}

// permissionActor is the stable-key authorization entry point for surfaces
// that are not tied to a group or object.
func (s *server) permissionActor(w http.ResponseWriter, r *http.Request, permission, resource string) (account.User, string, bool) {
	principal, ok := s.consolePrincipalFor(w, r, permission, resource)
	if !ok {
		return account.User{}, "", false
	}
	return principal.User, principal.Actor, true
}

// consoleActor remains for call sites that need the legacy role ladder while
// they are being migrated. It maps the old threshold to its primary key.
func (s *server) consoleActor(w http.ResponseWriter, r *http.Request, need consolePermission, resource string) (account.User, string, bool) {
	principal, ok := s.consolePrincipalFor(w, r, consolePermissionKey(need, resource), resource)
	if !ok {
		return account.User{}, "", false
	}
	return principal.User, principal.Actor, true
}

// deviceActor remains for call sites that still use the fixed role ladder.
// New device call sites pass a stable permission to devicePermissionActor.
func (s *server) deviceActor(w http.ResponseWriter, r *http.Request, need consolePermission, peerID string) (consolePrincipal, bool) {
	return s.devicePermissionActor(w, r, consolePermissionKey(need, account.TokenResourceDevices), peerID)
}

// devicePermissionActor authorizes one stable permission against one device.
// Token and named-role bindings are resolved for the device object so group
// and exact-object scopes cannot leak into other devices.
func (s *server) devicePermissionActor(w http.ResponseWriter, r *http.Request, permission, peerID string) (consolePrincipal, bool) {
	if s.serverKeyAuthorized(r) {
		return consolePrincipal{
			Actor:       "server-key",
			Permission:  consoleAdmin,
			Permissions: permissionSet(account.AllPermissions()),
		}, true
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return consolePrincipal{}, false
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "server key or account token required")
		return consolePrincipal{}, false
	}
	if account.HasAPITokenPrefix(token) {
		principal, ok := s.apiTokenPrincipal(w, token, permission, account.TokenResourceDevices)
		if !ok {
			return consolePrincipal{}, false
		}
		if !s.deviceInScope(w, principal, peerID) {
			return consolePrincipal{}, false
		}
		return principal, true
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or account token required")
		return consolePrincipal{}, false
	}
	perms, err := s.effectiveDevicePermissions(user.ID, peerID)
	if err != nil {
		s.cfg.Logger.Error("device permission check", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not check the device permission")
		return consolePrincipal{}, false
	}
	if !perms[permission] {
		writeError(w, http.StatusForbidden, "console permission does not allow this device")
		return consolePrincipal{}, false
	}
	principal := accountConsolePrincipal(user, perms)
	principal.ObjectKind = "device"
	principal.ObjectID = peerID
	return principal, true
}

// effectiveDevicePermissions combines global and exact-device bindings with
// group bindings for every group that currently contains the device. Device
// routes carry a device id rather than a group id, so resolving the device's
// groups here is what keeps a group-scoped role useful without letting it
// leak to another group.
func (s *server) effectiveDevicePermissions(userID, peerID string) (map[string]bool, error) {
	perms, err := s.cfg.Accounts.EffectiveConsolePermissions(userID, "", "device", peerID)
	if err != nil {
		return nil, err
	}
	bindings, err := s.cfg.Accounts.RoleBindings(userID)
	if err != nil {
		return nil, err
	}
	seen := make(map[string]bool)
	for _, binding := range bindings {
		if binding.ScopeKind != account.RoleScopeGroup || seen[binding.ScopeValue] {
			continue
		}
		seen[binding.ScopeValue] = true
		contained, err := s.cfg.Accounts.DeviceInGroup(binding.ScopeValue, peerID)
		if err != nil {
			return nil, err
		}
		if !contained {
			continue
		}
		scoped, err := s.cfg.Accounts.EffectiveConsolePermissions(
			userID, binding.ScopeValue, "device", peerID)
		if err != nil {
			return nil, err
		}
		for key, allowed := range scoped {
			if allowed {
				perms[key] = true
			}
		}
	}
	return perms, nil
}

// deviceInScope applies the token group range to one device id.
func (s *server) deviceInScope(w http.ResponseWriter, principal consolePrincipal, peerID string) bool {
	if principal.GroupID == "" {
		return true
	}
	allowed, err := s.cfg.Accounts.DeviceInGroup(principal.GroupID, peerID)
	if err != nil {
		s.cfg.Logger.Error("device group check", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not check the device group")
		return false
	}
	if !allowed {
		writeError(w, http.StatusForbidden, "device is outside the token group")
		return false
	}
	return true
}

// requireUnscoped refuses global administration through a group-scoped token.
func requireUnscoped(w http.ResponseWriter, principal consolePrincipal) bool {
	if principal.GroupID != "" {
		writeError(w, http.StatusForbidden, "group-scoped token cannot manage global state")
		return false
	}
	return true
}
