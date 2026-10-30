package api

import (
	"crypto/rand"
	"encoding/base64"
	"errors"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// adminAccounts exposes the loopback console's user commands over HTTP so a
// future web console or automation can manage accounts remotely. Every route
// accepts either the server key or an account token whose role is admin;
// ordinary users cannot see or change other accounts.
type adminAccountEntry struct {
	ID          string `json:"id"`
	Username    string `json:"username"`
	Role        string `json:"role"`
	Email       string `json:"email,omitempty"`
	Phone       string `json:"phone,omitempty"`
	CreatedAtMS int64  `json:"created_at_ms"`
	Banned      bool   `json:"banned,omitempty"`
}

type adminAccountListResponse struct {
	Users []adminAccountEntry `json:"users"`
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

// adminActor authorizes an account-management request and names the caller in
// the audit detail. The server key is not an account, so it reports no user.
func (s *server) adminActor(w http.ResponseWriter, r *http.Request) (account.User, string, bool) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return account.User{}, "", false
	}
	return s.consoleActor(w, r, consoleAdmin)
}

func (s *server) adminAccounts(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.adminActor(w, r); !ok {
		return
	}
	users, err := s.cfg.Accounts.ListUsers()
	if err != nil {
		s.cfg.Logger.Error("list accounts", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list accounts")
		return
	}
	resp := adminAccountListResponse{Users: make([]adminAccountEntry, 0, len(users))}
	for _, user := range users {
		resp.Users = append(resp.Users, viewAdminAccount(user))
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) adminCreateAccount(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r)
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
	_, actor, ok := s.adminActor(w, r)
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
	_, actor, ok := s.adminActor(w, r)
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
	_, actor, ok := s.adminActor(w, r)
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
	_, actor, ok := s.adminActor(w, r)
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

func (s *server) adminResetAccountTwoFactor(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r)
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
