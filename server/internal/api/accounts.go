package api

import (
	"crypto/rand"
	"encoding/base64"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// adminAccounts exposes the loopback console's user commands over HTTP so a
// future web console or automation can manage accounts remotely. Every route
// accepts either the server key or an account token whose role is admin;
// ordinary users cannot see or change other accounts.
type adminAccountEntry struct {
	ID       string `json:"id"`
	Username string `json:"username"`
	Role     string `json:"role"`
	Email    string `json:"email,omitempty"`
	Phone    string `json:"phone,omitempty"`
	// PermissionCeiling caps every session this account opens; zero is
	// unlimited. It intersects with each device policy's own ceiling.
	PermissionCeiling uint32 `json:"permission_ceiling"`
	CreatedAtMS       int64  `json:"created_at_ms"`
	Banned            bool   `json:"banned,omitempty"`
}

type adminAccountListResponse struct {
	Users []adminAccountEntry `json:"users"`
	// Total counts every account matching the filter, not just this page.
	Total int `json:"total"`
}

type adminAccountCreateRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Role     string `json:"role"`
	Email    string `json:"email"`
	Phone    string `json:"phone"`
}

type adminAccountCreateResponse struct {
	adminAccountEntry
	// Password is set only when the server generated one because the request
	// left it empty. It is the single chance to read it.
	Password string `json:"password,omitempty"`
}

type adminAccountPatchRequest struct {
	Role string `json:"role"`
}

type adminAccountPasswordRequest struct {
	Password string `json:"password"`
}

type adminAccountSessionCapRequest struct {
	PermissionCeiling uint32 `json:"permission_ceiling"`
}

type adminAccountPasswordResponse struct {
	Password string `json:"password,omitempty"`
}

func viewAdminAccount(user account.User) adminAccountEntry {
	return adminAccountEntry{
		ID:          user.ID,
		Username:    user.Username,
		Role:        user.Role,
		Email:       user.Email,
		Phone:       user.Phone,
		CreatedAtMS: user.CreatedAt.UnixMilli(),
		Banned:      user.Banned,
	}
}

// adminActor authorizes an administrative request for one resource and names
// the caller in the audit detail. The server key is not an account, so it
// reports no user.
func (s *server) adminPrincipal(w http.ResponseWriter, r *http.Request, resource string) (consolePrincipal, bool) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return consolePrincipal{}, false
	}
	return s.consolePrincipalFor(w, r, consoleAdmin, resource)
}

func (s *server) adminActor(w http.ResponseWriter, r *http.Request, resource string) (account.User, string, bool) {
	principal, ok := s.adminPrincipal(w, r, resource)
	if !ok {
		return account.User{}, "", false
	}
	return principal.User, principal.Actor, true
}

func (s *server) adminAccounts(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.adminActor(w, r, account.TokenResourceAccounts); !ok {
		return
	}
	query := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("q")))
	role := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("role")))
	email := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("email")))
	phone := strings.TrimSpace(r.URL.Query().Get("phone"))
	sortBy := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("sort")))
	if sortBy == "" {
		sortBy = account.UserSortUsername
	}
	if !account.ValidUserSort(sortBy) {
		writeError(w, http.StatusBadRequest, account.ErrInvalidUserSort.Error())
		return
	}
	order := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("order")))
	if order == "" {
		order = "asc"
	}
	if order != "asc" && order != "desc" {
		writeError(w, http.StatusBadRequest, "order must be asc or desc")
		return
	}
	csv, ok := exportRequest(w, r)
	if !ok {
		return
	}
	// Without limit the listing keeps returning every account, which is what
	// the older API and its clients expect; the console always sends one.
	limit, offset := 0, 0
	if !csv {
		if limit, ok = intQuery(w, r, "limit", 0, 0, 1000); !ok {
			return
		}
		if offset, ok = intQuery(w, r, "offset", 0, 0, 1_000_000); !ok {
			return
		}
	}
	users, total, err := s.cfg.Accounts.ListUsersPage(account.UserListOptions{
		Query: query, Email: email, Phone: phone, Role: role,
		Sort: sortBy, Desc: order == "desc", Limit: limit, Offset: offset,
	})
	if err != nil {
		if errors.Is(err, account.ErrInvalidRole) || errors.Is(err, account.ErrInvalidUserSort) {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		s.cfg.Logger.Error("list accounts", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list accounts")
		return
	}
	caps, err := s.cfg.Accounts.SessionPermissionCaps()
	if err != nil {
		s.cfg.Logger.Error("list account session caps", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list accounts")
		return
	}
	if csv {
		s.exportAccounts(w, users, caps)
		return
	}
	resp := adminAccountListResponse{Users: make([]adminAccountEntry, 0, len(users)), Total: total}
	for _, user := range users {
		entry := viewAdminAccount(user)
		entry.PermissionCeiling = caps[user.Username]
		resp.Users = append(resp.Users, entry)
	}
	writeJSON(w, http.StatusOK, resp)
}

var accountExportHeader = []string{
	"id", "username", "role", "email", "phone", "permission_ceiling", "banned", "created_at",
}

// exportAccounts streams the filtered account list. Unlike the JSON listing it
// ignores limit/offset so a download always covers the whole filter.
func (s *server) exportAccounts(w http.ResponseWriter, users []account.User, caps map[string]uint32) {
	export := s.newCSVExport(w, "chast-accounts", "account", accountExportHeader)
	rows := make([][]string, 0, exportEventBatch)
	for _, user := range users {
		rows = append(rows, []string{
			user.ID,
			csvSafe(user.Username),
			user.Role,
			csvSafe(user.Email),
			csvSafe(user.Phone),
			strconv.FormatUint(uint64(caps[user.Username]), 10),
			strconv.FormatBool(user.Banned),
			user.CreatedAt.UTC().Format(time.RFC3339),
		})
		if len(rows) >= exportEventBatch {
			if !export.write(rows) {
				return
			}
			rows = rows[:0]
		}
	}
	if len(rows) > 0 && !export.write(rows) {
		return
	}
	export.done()
}

// adminSetAccountSessionCap limits every session an account opens, independent
// of any single device policy. The next ticket issuance applies the new mask;
// the device policy ceiling still tightens it further where configured.
func (s *server) adminSetAccountSessionCap(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	var req adminAccountSessionCapRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := r.PathValue("username")
	if err := s.cfg.Accounts.SetSessionPermissionCap(username, req.PermissionCeiling); err != nil {
		writeAccountError(w, err)
		return
	}
	user, err := s.cfg.Accounts.FindUser(username)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccountSessionCap,
		"session permission ceiling "+strconv.FormatUint(uint64(req.PermissionCeiling), 10)+
			" for "+user.Username, true)
	entry := viewAdminAccount(user)
	entry.PermissionCeiling = req.PermissionCeiling
	writeJSON(w, http.StatusOK, entry)
}

func (s *server) adminCreateAccount(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	var req adminAccountCreateRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	role := strings.TrimSpace(req.Role)
	if role == "" {
		role = account.RoleUser
	}
	password := req.Password
	generated := false
	if password == "" {
		var err error
		if password, err = randomAccountPassword(); err != nil {
			writeError(w, http.StatusInternalServerError, "could not generate a password")
			return
		}
		generated = true
	}
	user, err := s.cfg.Accounts.CreateUserWithRole(req.Username, password, req.Email, req.Phone, role)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccountCreate,
		"created "+user.Username+" role="+user.Role, true)
	resp := adminAccountCreateResponse{adminAccountEntry: viewAdminAccount(user)}
	if generated {
		resp.Password = password
	}
	writeJSON(w, http.StatusCreated, resp)
}

func (s *server) adminUpdateAccount(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	var req adminAccountPatchRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.SetRole(r.PathValue("username"), req.Role)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditRoleChange,
		"role="+user.Role+" for "+user.Username, true)
	writeJSON(w, http.StatusOK, viewAdminAccount(user))
}

func (s *server) adminDeleteAccount(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	username := r.PathValue("username")
	if err := s.cfg.Accounts.DeleteUser(username); err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccountDelete, "deleted "+username, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) adminResetAccountPassword(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	var req adminAccountPasswordRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := r.PathValue("username")
	password := req.Password
	generated := false
	if password == "" {
		var err error
		if password, err = randomAccountPassword(); err != nil {
			writeError(w, http.StatusInternalServerError, "could not generate a password")
			return
		}
		generated = true
	}
	if err := s.cfg.Accounts.SetPassword(username, password); err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccountPassword, "reset "+username, true)
	var resp adminAccountPasswordResponse
	if generated {
		resp.Password = password
	}
	writeJSON(w, http.StatusOK, resp)
}

// adminSetAccountBan blocks or restores an account. A ban revokes every live
// session in the store; lifting it lets the next login through again.
func (s *server) adminSetAccountBan(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	banned := r.Method == http.MethodPost
	username := r.PathValue("username")
	user, err := s.cfg.Accounts.SetBanned(username, banned)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	action, verb := account.AuditAccountUnban, "unbanned "
	if banned {
		action, verb = account.AuditAccountBan, "banned "
	}
	s.recordAudit(r, actor, action, verb+user.Username, true)
	writeJSON(w, http.StatusOK, viewAdminAccount(user))
}

// adminAccountSessions lists one account's live logins so an operator can see
// where it is signed in and cut a stale device without banning the account.
// The caller's own session is flagged only when the target account is theirs.
func (s *server) adminAccountSessions(w http.ResponseWriter, r *http.Request) {
	_, _, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	limit, offset, ok := sessionPageParams(w, r)
	if !ok {
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	token, _ := bearerToken(r)
	sessions, total, err := s.cfg.Accounts.SessionsPage(user.ID, token, limit, offset)
	if err != nil {
		s.cfg.Logger.Error("list account sessions", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not list the account sessions")
		return
	}
	resp := accountSessionsResponse{
		Sessions: make([]accountSessionView, 0, len(sessions)),
		Total:    total,
	}
	for _, session := range sessions {
		resp.Sessions = append(resp.Sessions, viewAccountSession(session))
	}
	writeJSON(w, http.StatusOK, resp)
}

// adminRevokeAccountSession ends one login of another account. The id must
// belong to the named account, and the caller cannot cut the session it is
// itself signed in with; that account keeps RevokeOtherSessions for its own
// sign-outs.
func (s *server) adminRevokeAccountSession(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	sessionID := r.PathValue("id")
	token, _ := bearerToken(r)
	sessions, err := s.cfg.Accounts.Sessions(user.ID, token)
	if err != nil {
		s.cfg.Logger.Error("list account sessions", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not list the account sessions")
		return
	}
	var found, current bool
	for _, session := range sessions {
		if session.ID == sessionID {
			found, current = true, session.Current
			break
		}
	}
	if !found {
		writeError(w, http.StatusNotFound, "session not found")
		return
	}
	if current {
		writeError(w, http.StatusConflict, "cannot revoke the session you are signed in with")
		return
	}
	if _, err := s.cfg.Accounts.RevokeSession(user.ID, sessionID); err != nil {
		s.cfg.Logger.Error("revoke account session", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not revoke the account session")
		return
	}
	s.recordAudit(r, actor, account.AuditSessionRevoke,
		"revoked session "+sessionID+" for "+user.Username, true)
	w.WriteHeader(http.StatusNoContent)
}

// adminRevokeAccountSessions ends every login of another account. An operator
// signed in as that same account keeps the session it is calling with, so
// "sign out everywhere" cannot lock the caller out of the console.
func (s *server) adminRevokeAccountSessions(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	token, _ := bearerToken(r)
	var revoked int
	if token != "" && principal.User.ID == user.ID {
		revoked, err = s.cfg.Accounts.RevokeOtherSessions(user.ID, token)
	} else {
		revoked, err = s.cfg.Accounts.RevokeAllSessions(user.ID)
	}
	if err != nil {
		s.cfg.Logger.Error("revoke account sessions", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not revoke the account sessions")
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditSessionRevoke,
		"revoked "+strconv.Itoa(revoked)+" sessions for "+user.Username, true)
	writeJSON(w, http.StatusOK, revokeOtherSessionsResponse{Revoked: revoked})
}

func (s *server) adminResetAccountTwoFactor(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	username := r.PathValue("username")
	if err := s.cfg.Accounts.ResetTwoFactor(username); err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditTwoFactorReset, "reset "+username, true)
	w.WriteHeader(http.StatusNoContent)
}

func writeAccountError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownUser):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrUserExists), errors.Is(err, account.ErrLastAdmin),
		errors.Is(err, account.ErrEmailTaken), errors.Is(err, account.ErrPhoneTaken):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrInvalidUsername), errors.Is(err, account.ErrWeakPassword),
		errors.Is(err, account.ErrInvalidRole), errors.Is(err, account.ErrInvalidEmail),
		errors.Is(err, account.ErrInvalidPhone), errors.Is(err, account.ErrNoRecoveryEmail),
		errors.Is(err, account.ErrNoRecoveryPhone), errors.Is(err, account.ErrInvalidResetToken),
		errors.Is(err, account.ErrInvalidResetCode):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrAccountBanned):
		writeError(w, http.StatusForbidden, err.Error())
	default:
		writeError(w, http.StatusInternalServerError, "could not complete the account change")
	}
}

func randomAccountPassword() (string, error) {
	buf := make([]byte, 12)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buf), nil
}
