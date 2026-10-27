package api

import (
	"errors"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// Mailer delivers one password-reset message. *mail.Sender implements it; the
// interface keeps API tests free of a real SMTP server.
type Mailer interface {
	SendPasswordReset(to, username, token, link string, ttl time.Duration) error
	SendLoginCode(to, username, code string, ttl time.Duration) error
}

// SMSer delivers one SMS password-reset code and one login second-factor code.
// *sms.Sender implements it.
type SMSer interface {
	SendPasswordReset(to, username, code string, ttl time.Duration) error
	SendLoginCode(to, username, code string, ttl time.Duration) error
}

// recoveryInterval throttles repeated self-service requests per address and
// username. The answer stays the same when throttled, so it cannot be used to
// probe which accounts exist.
const recoveryInterval = time.Minute

type emailChangeRequest struct {
	Email    string `json:"email"`
	Password string `json:"password"`
}

type emailChangeResponse struct {
	Email string `json:"email"`
}

type recoverRequest struct {
	Username string `json:"username"`
}

type recoverResponse struct {
	Status string `json:"status"`
}

type recoverCompleteRequest struct {
	Token    string `json:"token"`
	Password string `json:"password"`
}

type recoverCompleteResponse struct {
	Username string `json:"username"`
}

type phoneChangeRequest struct {
	Phone    string `json:"phone"`
	Password string `json:"password"`
}

type phoneChangeResponse struct {
	Phone string `json:"phone"`
}

type smsRecoverCompleteRequest struct {
	Username string `json:"username"`
	Code     string `json:"code"`
	Password string `json:"password"`
}

func (s *server) recoveryAllowed(key string) bool {
	now := time.Now()
	s.recoveryMu.Lock()
	defer s.recoveryMu.Unlock()
	if len(s.recoveryLast) > 8192 {
		for candidate, seen := range s.recoveryLast {
			if now.Sub(seen) > 10*time.Minute {
				delete(s.recoveryLast, candidate)
			}
		}
	}
	if last, ok := s.recoveryLast[key]; ok && now.Sub(last) < recoveryInterval {
		return false
	}
	return true
}

func (s *server) markRecovery(key string) {
	now := time.Now()
	s.recoveryMu.Lock()
	defer s.recoveryMu.Unlock()
	s.recoveryLast[key] = now
}

func (s *server) allowRecovery(key string) bool {
	if !s.recoveryAllowed(key) {
		return false
	}
	s.markRecovery(key)
	return true
}

// changeAccountEmail lets a signed-in user attach or clear the recovery address.
// The current password is required so a borrowed token cannot redirect future
// resets to an attacker's mailbox.
func (s *server) changeAccountEmail(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req emailChangeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := s.cfg.Accounts.CheckPassword(user.Username, req.Password); err != nil {
		s.recordAudit(r, user.Username, account.AuditAccountEmail, "password rejected", false)
		writeError(w, http.StatusForbidden, "current password was not accepted")
		return
	}
	updated, err := s.cfg.Accounts.SetEmail(user.Username, req.Email)
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditAccountEmail, "invalid change", false)
		writeAccountError(w, err)
		return
	}
	detail := "email set"
	if updated.Email == "" {
		detail = "email cleared"
	}
	s.recordAudit(r, user.Username, account.AuditAccountEmail, detail, true)
	s.cfg.Logger.Info("account recovery email changed", "user", updated.Username)
	writeJSON(w, http.StatusOK, emailChangeResponse{Email: updated.Email})
}

// changeAccountPhone lets a signed-in user attach or clear the SMS recovery
// number. Like the email change it requires the current password, and any code
// issued for the previous number is dropped by the store.
func (s *server) changeAccountPhone(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req phoneChangeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := s.cfg.Accounts.CheckPassword(user.Username, req.Password); err != nil {
		s.recordAudit(r, user.Username, account.AuditAccountPhone, "password rejected", false)
		writeError(w, http.StatusForbidden, "current password was not accepted")
		return
	}
	updated, err := s.cfg.Accounts.SetPhone(user.Username, req.Phone)
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditAccountPhone, "invalid change", false)
		writeAccountError(w, err)
		return
	}
	detail := "phone set"
	if updated.Phone == "" {
		detail = "phone cleared"
	}
	s.recordAudit(r, user.Username, account.AuditAccountPhone, detail, true)
	s.cfg.Logger.Info("account recovery phone changed", "user", updated.Username)
	writeJSON(w, http.StatusOK, phoneChangeResponse{Phone: updated.Phone})
}

// requestPasswordRecovery accepts a username and always reports the same
// accepted status. A mail is sent only when the account exists, has an address
// and SMTP is configured; every other outcome is invisible to the caller.
func (s *server) requestPasswordRecovery(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	if s.cfg.Mailer == nil {
		writeError(w, http.StatusServiceUnavailable, "email recovery is not configured")
		return
	}
	var req recoverRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := strings.TrimSpace(req.Username)
	key := s.clientIP(r) + "|" + strings.ToLower(username)
	if !s.allowRecovery(key) {
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	}
	// Unknown names and accounts without an address stop here; the caller
	// still receives the generic accepted response below.
	token, user, _, err := s.cfg.Accounts.CreatePasswordReset(username, s.cfg.RecoveryTTL)
	if err != nil {
		if !errors.Is(err, account.ErrUnknownUser) && !errors.Is(err, account.ErrNoRecoveryEmail) {
			s.cfg.Logger.Error("create password reset", "user", username, "error", err)
		}
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	}
	link := s.recoveryLink(token)
	if err := s.cfg.Mailer.SendPasswordReset(
		user.Email, user.Username, token, link, s.cfg.RecoveryTTL,
	); err != nil {
		s.cfg.Logger.Warn("password reset email failed",
			"user", user.Username, "error", err)
		s.recordAudit(r, user.Username, account.AuditAccountRecover, "reset email failed", false)
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	}
	s.recordAudit(r, user.Username, account.AuditAccountRecover, "reset email sent", true)
	s.cfg.Logger.Info("password reset email sent", "user", user.Username)
	writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
}

func (s *server) recoveryLink(token string) string {
	base := strings.TrimSpace(s.cfg.RecoveryURL)
	if base == "" {
		return ""
	}
	target, err := url.Parse(base)
	if err != nil {
		return ""
	}
	query := target.Query()
	query.Set("token", token)
	target.RawQuery = query.Encode()
	return target.String()
}

// completePasswordRecovery spends the token and replaces the password. Every
// existing session is revoked by the store, so a stolen token from before the
// reset stops working immediately.
func (s *server) completePasswordRecovery(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req recoverCompleteRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.ConsumePasswordReset(req.Token, req.Password)
	switch {
	case errors.Is(err, account.ErrWeakPassword):
		writeError(w, http.StatusBadRequest, err.Error())
		return
	case errors.Is(err, account.ErrInvalidResetToken):
		writeError(w, http.StatusBadRequest, "password reset token is invalid or expired")
		return
	case err != nil:
		s.cfg.Logger.Error("complete password reset", "error", err)
		writeError(w, http.StatusInternalServerError, "could not reset the password")
		return
	}
	s.recordAudit(r, user.Username, account.AuditAccountRecover, "password reset with token", true)
	s.cfg.Logger.Info("password reset completed", "user", user.Username)
	writeJSON(w, http.StatusOK, recoverCompleteResponse{Username: user.Username})
}

// requestSMSPasswordRecovery mirrors the email flow: the same accepted answer
// is returned for unknown accounts, accounts without a number and gateway
// failures, so the endpoint cannot be used to enumerate users or numbers.
func (s *server) requestSMSPasswordRecovery(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	if s.cfg.SMS == nil {
		writeError(w, http.StatusServiceUnavailable, "SMS recovery is not configured")
		return
	}
	var req recoverRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := strings.TrimSpace(req.Username)
	key := "sms|" + s.clientIP(r) + "|" + strings.ToLower(username)
	if !s.allowRecovery(key) {
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	}
	code, user, _, err := s.cfg.Accounts.CreatePhoneReset(username, s.cfg.SMSTTL)
	if err != nil {
		if !errors.Is(err, account.ErrUnknownUser) && !errors.Is(err, account.ErrNoRecoveryPhone) {
			s.cfg.Logger.Error("create SMS reset", "user", username, "error", err)
		}
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	}
	if err := s.cfg.SMS.SendPasswordReset(
		user.Phone, user.Username, code, s.cfg.SMSTTL,
	); err != nil {
		s.cfg.Logger.Warn("password reset SMS failed",
			"user", user.Username, "error", err)
		s.recordAudit(r, user.Username, account.AuditAccountRecover, "reset SMS failed", false)
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	}
	s.recordAudit(r, user.Username, account.AuditAccountRecover, "reset SMS sent", true)
	s.cfg.Logger.Info("password reset SMS sent", "user", user.Username)
	writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
}

// completeSMSPasswordRecovery spends the code and replaces the password. The
// store retires the code after five wrong attempts and revokes every session.
func (s *server) completeSMSPasswordRecovery(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req smsRecoverCompleteRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.ConsumePhoneReset(req.Username, req.Code, req.Password)
	switch {
	case errors.Is(err, account.ErrWeakPassword):
		writeError(w, http.StatusBadRequest, err.Error())
		return
	case errors.Is(err, account.ErrInvalidResetCode):
		writeError(w, http.StatusBadRequest, "SMS reset code is invalid or expired")
		return
	case err != nil:
		s.cfg.Logger.Error("complete SMS password reset", "error", err)
		writeError(w, http.StatusInternalServerError, "could not reset the password")
		return
	}
	s.recordAudit(r, user.Username, account.AuditAccountRecover, "password reset with SMS code", true)
	s.cfg.Logger.Info("password reset with SMS completed", "user", user.Username)
	writeJSON(w, http.StatusOK, recoverCompleteResponse{Username: user.Username})
}

// adminSetAccountPhone attaches or clears a recovery number without the user's
// password. Any code issued for the previous number is retired by the store.
func (s *server) adminSetAccountPhone(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r)
	if !ok {
		return
	}
	var req struct {
		Phone string `json:"phone"`
	}
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.SetPhone(r.PathValue("username"), req.Phone)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	detail := "phone set"
	if user.Phone == "" {
		detail = "phone cleared"
	}
	s.recordAudit(r, actor, account.AuditAccountPhone, detail+" for "+user.Username, true)
	writeJSON(w, http.StatusOK, viewAdminAccount(user))
}

// adminSendAccountSMSCode mints a reset code and hands it to the gateway. The
// code itself never appears in the response or the audit log.
func (s *server) adminSendAccountSMSCode(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r)
	if !ok {
		return
	}
	if s.cfg.SMS == nil {
		writeError(w, http.StatusServiceUnavailable, "SMS recovery is not configured")
		return
	}
	code, user, _, err := s.cfg.Accounts.CreatePhoneReset(r.PathValue("username"), s.cfg.SMSTTL)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	if err := s.cfg.SMS.SendPasswordReset(user.Phone, user.Username, code, s.cfg.SMSTTL); err != nil {
		s.cfg.Logger.Warn("admin password reset SMS failed",
			"user", user.Username, "error", err)
		s.recordAudit(r, actor, account.AuditAccountRecover,
			"reset SMS failed for "+user.Username, false)
		writeError(w, http.StatusBadGateway, "could not send the SMS code")
		return
	}
	s.recordAudit(r, actor, account.AuditAccountRecover, "reset SMS sent for "+user.Username, true)
	writeJSON(w, http.StatusAccepted, recoverResponse{Status: "sent"})
}

// adminSetAccountEmail lets an operator attach a recovery address while
// creating the account or later without the user's password. A failed set
// leaves the previous address untouched.
func (s *server) adminSetAccountEmail(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r)
	if !ok {
		return
	}
	var req struct {
		Email string `json:"email"`
	}
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.SetEmail(r.PathValue("username"), req.Email)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	detail := "email set"
	if user.Email == "" {
		detail = "email cleared"
	}
	s.recordAudit(r, actor, account.AuditAccountEmail, detail+" for "+user.Username, true)
	writeJSON(w, http.StatusOK, viewAdminAccount(user))
}
