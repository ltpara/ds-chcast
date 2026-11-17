package api

import (
	"net/http"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// Scoped grants stay separate from global permissions in session payloads.
type consoleGrant struct {
	ScopeKind   string   `json:"scope_kind"`
	ScopeValue  string   `json:"scope_value"`
	ObjectKind  string   `json:"object_kind,omitempty"`
	Permissions []string `json:"permissions"`
}

func (s *server) consoleGrants(userID string) ([]consoleGrant, error) {
	bindings, err := s.cfg.Accounts.RoleBindings(userID)
	if err != nil {
		return nil, err
	}
	grants := make([]consoleGrant, 0)
	for _, binding := range bindings {
		if binding.ScopeKind != account.RoleScopeGlobal {
			grants = append(grants, consoleGrant{binding.ScopeKind, binding.ScopeValue, binding.ObjectKind, binding.Permissions})
		}
	}
	return grants, nil
}

type deviceCollectionScope struct {
	principal consolePrincipal
	// nil admits every device; an empty non-nil map admits none.
	allowed map[string]bool
	grants  map[string]map[string]bool
}

func (scope deviceCollectionScope) permissions(peerID string) []string {
	perms := permissionSet(permissionKeys(scope.principal.Permissions))
	for key := range scope.grants[peerID] {
		perms[key] = true
	}
	return permissionKeys(perms)
}

// Resolve scope before pagination, counts or exports. A binding for another
// object kind cannot admit a device, and global permissions are never inferred
// from a group or an object grant.
func (s *server) deviceCollectionActor(w http.ResponseWriter, r *http.Request, permission string) (deviceCollectionScope, bool) {
	principal, ok := s.consolePrincipalFor(w, r, "", account.TokenResourceDevices)
	if !ok {
		return deviceCollectionScope{}, false
	}
	scope := deviceCollectionScope{principal: principal, grants: map[string]map[string]bool{}}
	add := func(peerID string, keys []string) {
		if scope.grants[peerID] == nil {
			scope.grants[peerID] = map[string]bool{}
		}
		for _, key := range keys {
			scope.grants[peerID][key] = true
		}
	}
	hasGrant := principal.Permissions[permission]
	if principal.APIToken {
		if principal.GroupID != "" {
			devices, err := s.cfg.Accounts.GroupDevices(principal.GroupID)
			if err != nil {
				writeError(w, http.StatusInternalServerError, "could not resolve token group")
				return scope, false
			}
			scope.allowed = map[string]bool{}
			for _, device := range devices {
				scope.allowed[device.PeerID] = true
			}
		}
	} else if principal.User.ID != "" {
		grants, err := s.consoleGrants(principal.User.ID)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "could not resolve console scope")
			return scope, false
		}
		for _, grant := range grants {
			switch grant.ScopeKind {
			case account.RoleScopeGroup:
				devices, err := s.cfg.Accounts.GroupDevices(grant.ScopeValue)
				if err != nil {
					writeError(w, http.StatusInternalServerError, "could not resolve device group")
					return scope, false
				}
				for _, device := range devices {
					add(device.PeerID, grant.Permissions)
				}
			case account.RoleScopeObject:
				if grant.ObjectKind != "device" {
					continue
				}
				add(grant.ScopeValue, grant.Permissions)
			default:
				continue
			}
			hasGrant = hasGrant || permissionSet(grant.Permissions)[permission]
		}
		if !principal.Permissions[permission] {
			scope.allowed = map[string]bool{}
			for peerID, perms := range scope.grants {
				if perms[permission] {
					scope.allowed[peerID] = true
				}
			}
		}
	}
	if !hasGrant {
		writeError(w, http.StatusForbidden, "console permission does not allow this collection")
		return scope, false
	}
	return scope, true
}
