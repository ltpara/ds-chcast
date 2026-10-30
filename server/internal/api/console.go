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

// consoleActor authorizes one console request against the minimum permission
// it needs. Operators sign in with a normal account token; role changes take
// effect on the next request because Authenticate re-reads the user row.
func (s *server) consoleActor(w http.ResponseWriter, r *http.Request, need consolePermission) (account.User, string, bool) {
	if s.authorized(r) {
		return account.User{}, "server-key", true
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusUnauthorized, "server key was not accepted")
		return account.User{}, "", false
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return account.User{}, "", false
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return account.User{}, "", false
	}
	perm, ok := rolePermission(user.Role)
	if !ok {
		writeError(w, http.StatusForbidden, "account has no console role")
		return account.User{}, "", false
	}
	if perm < need {
		writeError(w, http.StatusForbidden, "console role does not allow this action")
		return account.User{}, "", false
	}
	return user, user.Username, true
}
