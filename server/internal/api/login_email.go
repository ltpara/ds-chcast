package api

import (
	"errors"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type loginEmailRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

// requestLoginEmailCode sends a single-use second-factor code to the account's
// recovery email. The password is verified first, so only a caller who already
// holds it can trigger a message; wrong passwords count toward the same
// username+IP lockout as a normal login. Accounts without two-factor or an
// address still receive the generic accepted answer, and the send itself is
// throttled per address and username.
func (s *server) requestLoginEmailCode(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	if s.cfg.Mailer == nil {
		writeError(w, http.StatusServiceUnavailable, "email two-factor is not configured")
		return
	}
	var req loginEmailRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := strings.TrimSpace(req.Username)
	if username == "" || req.Password == "" {
		s.auditLogin(r, username, "missing credentials", false)
		writeError(w, http.StatusUnauthorized, "username or password was not accepted")
		return
	}
	key := strings.ToLower(username) + "|" + s.clientIP(r)
	if retry, ok := s.cfg.LoginLimiter.Allow(key); !ok {
		s.auditLogin(r, username, "locked out", false)
		writeLoginLocked(w, retry)
		return
	}
	// The throttle is checked before minting: a rejected request must not
	// replace the code that was already sent.
	throttleKey := "login-email|" + s.clientIP(r) + "|" + strings.ToLower(username)
	if !s.recoveryAllowed(throttleKey) {
		s.auditLogin(r, username, "login email throttled", false)
		writeError(w, http.StatusTooManyRequests, "an email code was requested too recently")
		return
	}
	code, user, _, err := s.cfg.Accounts.CreateLoginEmailCode(username, req.Password, s.cfg.RecoveryTTL)
	switch {
	case errors.Is(err, account.ErrInvalidCredentials):
		if retry, allowed := s.cfg.LoginLimiter.Failure(key); !allowed {
			s.auditLogin(r, username, "login email invalid credentials; locked out", false)
			writeLoginLocked(w, retry)
			return
		}
		s.auditLogin(r, username, "login email invalid credentials", false)
		writeError(w, http.StatusUnauthorized, "username or password was not accepted")
		return
	case errors.Is(err, account.ErrAccountBanned):
		s.auditLogin(r, username, "account banned", false)
		writeError(w, http.StatusForbidden, "account is banned")
		return
	case errors.Is(err, account.ErrTwoFactorNotEnabled), errors.Is(err, account.ErrNoRecoveryEmail):
		// The password matched, but this account has no email second factor.
		// Answer like a sent message so the route cannot map accounts.
		s.auditLogin(r, username, "login email unavailable", false)
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	case err != nil:
		s.auditLogin(r, username, "login email internal error", false)
		s.cfg.Logger.Error("request login email", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not send the email code")
		return
	}
	if err := s.cfg.Mailer.SendLoginCode(user.Email, user.Username, code, s.cfg.RecoveryTTL); err != nil {
		s.cfg.Logger.Warn("login email failed", "user", user.Username, "error", err)
		s.auditLogin(r, user.Username, "login email failed", false)
		writeError(w, http.StatusBadGateway, "could not send the email code")
		return
	}
	s.markRecovery(throttleKey)
	s.auditLogin(r, user.Username, "login email sent", true)
	s.cfg.Logger.Info("login email sent", "user", user.Username)
	writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
}
