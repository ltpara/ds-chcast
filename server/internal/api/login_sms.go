package api

import (
	"errors"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type loginSMSRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

// requestLoginSMSCode sends a single-use second-factor code to the account's
// recovery phone. The password is verified first, so only a caller who already
// holds it can trigger a message; wrong passwords count toward the same
// username+IP lockout as a normal login. Accounts without two-factor or a
// number still receive the generic accepted answer, and the send itself is
// throttled per address and username.
func (s *server) requestLoginSMSCode(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	if s.cfg.SMS == nil {
		writeError(w, http.StatusServiceUnavailable, "SMS two-factor is not configured")
		return
	}
	var req loginSMSRequest
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
	throttleKey := "login-sms|" + s.clientIP(r) + "|" + strings.ToLower(username)
	if !s.recoveryAllowed(throttleKey) {
		s.auditLogin(r, username, "login SMS throttled", false)
		writeError(w, http.StatusTooManyRequests, "an SMS code was requested too recently")
		return
	}
	code, user, _, err := s.cfg.Accounts.CreateLoginSMSCode(username, req.Password, s.cfg.SMSTTL)
	switch {
	case errors.Is(err, account.ErrInvalidCredentials):
		if retry, allowed := s.cfg.LoginLimiter.Failure(key); !allowed {
			s.auditLogin(r, username, "login SMS invalid credentials; locked out", false)
			writeLoginLocked(w, retry)
			return
		}
		s.auditLogin(r, username, "login SMS invalid credentials", false)
		writeError(w, http.StatusUnauthorized, "username or password was not accepted")
		return
	case errors.Is(err, account.ErrAccountBanned):
		s.auditLogin(r, username, "account banned", false)
		writeError(w, http.StatusForbidden, "account is banned")
		return
	case errors.Is(err, account.ErrTwoFactorNotEnabled), errors.Is(err, account.ErrNoRecoveryPhone):
		// The password matched, but this account has no SMS second factor.
		// Answer like a sent message so the route cannot map accounts.
		s.auditLogin(r, username, "login SMS unavailable", false)
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	case err != nil:
		s.auditLogin(r, username, "login SMS internal error", false)
		s.cfg.Logger.Error("request login SMS", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not send the SMS code")
		return
	}
	if err := s.cfg.SMS.SendLoginCode(user.Phone, user.Username, code, s.cfg.SMSTTL); err != nil {
		s.cfg.Logger.Warn("login SMS failed", "user", user.Username, "error", err)
		s.auditLogin(r, user.Username, "login SMS failed", false)
		writeError(w, http.StatusBadGateway, "could not send the SMS code")
		return
	}
	s.markRecovery(throttleKey)
	s.auditLogin(r, user.Username, "login SMS sent", true)
	s.cfg.Logger.Info("login SMS sent", "user", user.Username)
	writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
}
