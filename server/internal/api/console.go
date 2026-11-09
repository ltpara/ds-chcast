package api

import (
	"net/http"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// consolePermission orders the console surfaces: readonly < operator < admin.
// The server key outranks every account role because it is the deployment's
// root credential.
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

// consolePrincipal is one authenticated console caller. A machine token adds
// its resource set and optional group range to the account's role ladder.
type consolePrincipal struct {
	User       account.User
	Actor      string
	Permission consolePermission
	GroupID    string
	APIToken   bool
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

// consolePrincipalFor authorizes one console request against the minimum
// permission and resource it needs. Operators sign in with a normal account
// token; role changes take effect on the next request because Authenticate
// re-reads the user row. Machine tokens are checked against their resource set
// and group range on every request.
func (s *server) consolePrincipalFor(w http.ResponseWriter, r *http.Request, need consolePermission, resource string) (consolePrincipal, bool) {
	if s.serverKeyAuthorized(r) {
		return consolePrincipal{Actor: "server-key", Permission: consoleAdmin}, true
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
		principal, err := s.cfg.Accounts.AuthenticateAPIToken(token)
		if err != nil {
			writeError(w, http.StatusUnauthorized, "server key or console token required")
			return consolePrincipal{}, false
		}
		perm, ok := rolePermission(principal.Token.Role)
		if !ok {
			writeError(w, http.StatusForbidden, "API token has no console role")
			return consolePrincipal{}, false
		}
		if !tokenCoversResource(principal.Token.Resources, resource) {
			writeError(w, http.StatusForbidden, "API token does not cover this resource")
			return consolePrincipal{}, false
		}
		if perm < need {
			writeError(w, http.StatusForbidden, "console role does not allow this action")
			return consolePrincipal{}, false
		}
		return consolePrincipal{
			User:       principal.User,
			Actor:      principal.User.Username,
			Permission: perm,
			GroupID:    principal.Token.GroupID,
			APIToken:   true,
		}, true
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return consolePrincipal{}, false
	}
	perm, ok := rolePermission(user.Role)
	if !ok {
		writeError(w, http.StatusForbidden, "account has no console role")
		return consolePrincipal{}, false
	}
	if perm < need {
		writeError(w, http.StatusForbidden, "console role does not allow this action")
		return consolePrincipal{}, false
	}
	return consolePrincipal{User: user, Actor: user.Username, Permission: perm}, true
}

// consoleActor returns the account and audit name for call sites that do not
// need the group range. API tokens are still subject to their resource set.
func (s *server) consoleActor(w http.ResponseWriter, r *http.Request, need consolePermission, resource string) (account.User, string, bool) {
	principal, ok := s.consolePrincipalFor(w, r, need, resource)
	if !ok {
		return account.User{}, "", false
	}
	return principal.User, principal.Actor, true
}
