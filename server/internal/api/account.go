package api

import (
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

const accountUnavailable = "accounts require persistent server state"

// twoFactorHeader tells a viewer why a password-correct login was refused.
// Its value is "required" for a missing code and "invalid" for a wrong or
// replayed one, without exposing anything before the password matched.
const twoFactorHeader = "X-Chast-2FA"

type loginRequest struct {
	Username     string `json:"username"`
	Password     string `json:"password"`
	DeviceID     string `json:"device_id"`
	DeviceName   string `json:"device_name"`
	TOTPCode     string `json:"totp_code"`
	RecoveryCode string `json:"recovery_code"`
	SMSCode      string `json:"sms_code"`
	EmailCode    string `json:"email_code"`
}

type accountView struct {
	ID       string `json:"id"`
	Username string `json:"username"`
	Role     string `json:"role"`
	Email    string `json:"email,omitempty"`
	Phone    string `json:"phone,omitempty"`
}

type loginResponse struct {
	Token              string      `json:"token"`
	ExpiresInMS        int64       `json:"expires_in_ms"`
	RefreshToken       string      `json:"refresh_token,omitempty"`
	RefreshExpiresInMS int64       `json:"refresh_expires_in_ms,omitempty"`
	User               accountView `json:"user"`
}

type refreshRequest struct {
	RefreshToken string `json:"refresh_token"`
}

type accountResponse struct {
	User          accountView `json:"user"`
	TwoFactor     bool        `json:"two_factor"`
	RecoveryCodes int         `json:"recovery_codes"`
}

type twoFactorStartResponse struct {
	Secret     string `json:"secret"`
	OTPAuthURI string `json:"otpauth_uri"`
}

type twoFactorCodeRequest struct {
	Code string `json:"code"`
}

type twoFactorDisableRequest struct {
	Password string `json:"password"`
	Code     string `json:"code"`
}

type twoFactorRecoveryResponse struct {
	Codes []string `json:"codes"`
}

type passwordChangeRequest struct {
	CurrentPassword string `json:"current_password"`
	NewPassword     string `json:"new_password"`
}

type accountSessionView struct {
	ID          string `json:"id"`
	DeviceID    string `json:"device_id,omitempty"`
	DeviceName  string `json:"device_name,omitempty"`
	CreatedAtMS int64  `json:"created_at_ms"`
	ExpiresAtMS int64  `json:"expires_at_ms"`
	Current     bool   `json:"current"`
	LoginIP     string `json:"login_ip,omitempty"`
	Location    string `json:"location,omitempty"`
}

type accountSessionsResponse struct {
	Sessions []accountSessionView `json:"sessions"`
}

type renameSessionRequest struct {
	DeviceName string `json:"device_name"`
}

type revokeOtherSessionsResponse struct {
	Revoked int `json:"revoked"`
}

type addressBookPeer struct {
	ID          string   `json:"id"`
	Name        string   `json:"name,omitempty"`
	Tags        []string `json:"tags,omitempty"`
	GroupID     string   `json:"group_id,omitempty"`
	Position    int64    `json:"position,omitempty"`
	UpdatedAtMS int64    `json:"updated_at_ms"`
	Revision    int64    `json:"revision,omitempty"`
	Deleted     bool     `json:"deleted,omitempty"`
}

type addressBookGroup struct {
	ID          string `json:"id"`
	Name        string `json:"name,omitempty"`
	ParentID    string `json:"parent_id,omitempty"`
	Position    int64  `json:"position,omitempty"`
	UpdatedAtMS int64  `json:"updated_at_ms"`
	Revision    int64  `json:"revision,omitempty"`
	Deleted     bool   `json:"deleted,omitempty"`
}

type addressBookResponse struct {
	Revision     int64              `json:"revision"`
	Peers        []addressBookPeer  `json:"peers"`
	Changes      []addressBookPeer  `json:"changes,omitempty"`
	Groups       []addressBookGroup `json:"groups,omitempty"`
	GroupChanges []addressBookGroup `json:"group_changes,omitempty"`
}

type addressBookPeerRequest struct {
	Name       *string   `json:"name"`
	Tags       *[]string `json:"tags"`
	GroupID    *string   `json:"group_id"`
	Position   *int64    `json:"position"`
	IfRevision *int64    `json:"if_revision"`
}

type addressBookGroupRequest struct {
	Name       *string `json:"name"`
	ParentID   *string `json:"parent_id"`
	Position   *int64  `json:"position"`
	IfRevision *int64  `json:"if_revision"`
}

type sharedAddressBookPeerRequest struct {
	ID      string   `json:"id"`
	Name    string   `json:"name"`
	Tags    []string `json:"tags"`
	GroupID string   `json:"group_id"`
}

type addressBookGrantView struct {
	Username    string `json:"username"`
	Permission  string `json:"permission"`
	GroupID     string `json:"group_id,omitempty"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type addressBookGrantsResponse struct {
	Outgoing []addressBookGrantView `json:"outgoing"`
	Incoming []addressBookGrantView `json:"incoming"`
}

type shareAddressBookRequest struct {
	Username   string `json:"username"`
	Permission string `json:"permission"`
	GroupID    string `json:"group_id"`
}

type sharedAddressBookScope struct {
	GroupID    string `json:"group_id,omitempty"`
	Permission string `json:"permission"`
}

type sharedAddressBookView struct {
	Owner      string                   `json:"owner"`
	Permission string                   `json:"permission"`
	Revision   int64                    `json:"revision"`
	Peers      []addressBookPeer        `json:"peers"`
	Groups     []addressBookGroup       `json:"groups"`
	Scopes     []sharedAddressBookScope `json:"scopes"`
}

type sharedAddressBooksResponse struct {
	Books []sharedAddressBookView `json:"books"`
}

func viewAccount(user account.User) accountView {
	return accountView{
		ID:       user.ID,
		Username: user.Username,
		Role:     user.Role,
		Email:    user.Email,
		Phone:    user.Phone,
	}
}

func viewAccountSession(session account.Session) accountSessionView {
	return accountSessionView{
		ID:          session.ID,
		DeviceID:    session.DeviceID,
		DeviceName:  session.DeviceName,
		CreatedAtMS: session.CreatedAt.UnixMilli(),
		ExpiresAtMS: session.ExpiresAt.UnixMilli(),
		Current:     session.Current,
		LoginIP:     session.LoginIP,
		Location:    session.Location,
	}
}

func viewAddressBookPeer(peer account.Peer) addressBookPeer {
	return addressBookPeer{
		ID:          peer.ID,
		Name:        peer.Name,
		Tags:        peer.Tags,
		GroupID:     peer.GroupID,
		Position:    peer.Position,
		UpdatedAtMS: peer.UpdatedAt.UnixMilli(),
		Revision:    peer.Revision,
		Deleted:     peer.Deleted,
	}
}

func viewAddressBookGroup(group account.Group) addressBookGroup {
	return addressBookGroup{
		ID:          group.ID,
		Name:        group.Name,
		ParentID:    group.ParentID,
		Position:    group.Position,
		UpdatedAtMS: group.UpdatedAt.UnixMilli(),
		Revision:    group.Revision,
		Deleted:     group.Deleted,
	}
}

func viewAddressBookGrant(grant account.Grant) addressBookGrantView {
	return addressBookGrantView{
		Username:    grant.Username,
		Permission:  grant.Permission,
		GroupID:     grant.GroupID,
		CreatedAtMS: grant.CreatedAt.UnixMilli(),
	}
}

// login issues a viewer token. Server-key operators and account users are
// separate identities: this token never unlocks the operator HTTP surface.
func (s *server) login(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req loginRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if strings.TrimSpace(req.Username) == "" || req.Password == "" {
		s.auditLogin(r, req.Username, "missing credentials", false)
		writeError(w, http.StatusUnauthorized, "username or password was not accepted")
		return
	}
	key := strings.ToLower(strings.TrimSpace(req.Username)) + "|" + s.clientIP(r)
	if retry, ok := s.cfg.LoginLimiter.Allow(key); !ok {
		s.auditLogin(r, req.Username, "locked out", false)
		writeLoginLocked(w, retry)
		return
	}
	user, token, _, refresh, _, err := s.cfg.Accounts.LoginWithRefreshFactors(
		req.Username, req.Password, req.DeviceID, req.DeviceName,
		req.TOTPCode, req.RecoveryCode, req.SMSCode, req.EmailCode,
	)
	if errors.Is(err, account.ErrTwoFactorRequired) {
		s.auditLogin(r, req.Username, "two-factor required", false)
		w.Header().Set(twoFactorHeader, "required")
		writeError(w, http.StatusUnauthorized, "two-factor code required")
		return
	}
	if errors.Is(err, account.ErrInvalidTwoFactorCode) {
		if retry, allowed := s.cfg.LoginLimiter.Failure(key); !allowed {
			s.auditLogin(r, req.Username, "two-factor invalid; locked out", false)
			writeLoginLocked(w, retry)
			return
		}
		s.auditLogin(r, req.Username, "two-factor invalid", false)
		w.Header().Set(twoFactorHeader, "invalid")
		writeError(w, http.StatusUnauthorized, "two-factor code was not accepted")
		return
	}
	if errors.Is(err, account.ErrAccountBanned) {
		s.auditLogin(r, req.Username, "account banned", false)
		writeError(w, http.StatusForbidden, "account is banned")
		return
	}
	if errors.Is(err, account.ErrInvalidCredentials) {
		if retry, allowed := s.cfg.LoginLimiter.Failure(key); !allowed {
			s.auditLogin(r, req.Username, "invalid credentials; locked out", false)
			writeLoginLocked(w, retry)
			return
		}
		s.auditLogin(r, req.Username, "invalid credentials", false)
		writeError(w, http.StatusUnauthorized, "username or password was not accepted")
		return
	}
	if err != nil {
		s.auditLogin(r, req.Username, "internal error", false)
		s.cfg.Logger.Error("account login", "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the login")
		return
	}
	s.cfg.LoginLimiter.Success(key)
	ip := s.clientIP(r)
	if err := s.cfg.Accounts.SetSessionOrigin(token, ip, s.clientLocation(ip)); err != nil {
		s.cfg.Logger.Error("account session origin", "error", err)
	}
	detail := "password"
	if strings.TrimSpace(req.TOTPCode) != "" {
		detail = "password+two-factor"
	} else if strings.TrimSpace(req.RecoveryCode) != "" {
		detail = "password+recovery-code"
	} else if strings.TrimSpace(req.SMSCode) != "" {
		detail = "password+sms-code"
	} else if strings.TrimSpace(req.EmailCode) != "" {
		detail = "password+email-code"
	}
	if device := strings.TrimSpace(req.DeviceName); device != "" {
		detail += " device=" + device
	}
	s.auditLogin(r, user.Username, detail, true)
	s.cfg.Logger.Info("account login", "user", user.Username)
	writeJSON(w, http.StatusOK, loginResponse{
		Token:              token,
		ExpiresInMS:        account.AccessTokenTTL.Milliseconds(),
		RefreshToken:       refresh,
		RefreshExpiresInMS: account.TokenTTL.Milliseconds(),
		User:               viewAccount(user),
	})
}

// refresh rotates a refresh token into a new access/refresh pair. A replayed
// token means the pair leaked or a previous rotation response was lost; the
// store revokes the session in both cases, so the client must log in again.
func (s *server) refresh(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req refreshRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if strings.TrimSpace(req.RefreshToken) == "" {
		s.recordAudit(r, "-", account.AuditRefresh, "missing token", false)
		writeError(w, http.StatusUnauthorized, "refresh token was not accepted")
		return
	}
	user, token, _, refresh, _, err := s.cfg.Accounts.RefreshSession(req.RefreshToken)
	if errors.Is(err, account.ErrRefreshReused) {
		s.recordAudit(r, "-", account.AuditRefresh, "replay; session revoked", false)
		writeError(w, http.StatusUnauthorized, "refresh token was not accepted")
		return
	}
	if errors.Is(err, account.ErrInvalidToken) {
		s.recordAudit(r, "-", account.AuditRefresh, "invalid token", false)
		writeError(w, http.StatusUnauthorized, "refresh token was not accepted")
		return
	}
	if errors.Is(err, account.ErrAccountBanned) {
		s.recordAudit(r, "-", account.AuditRefresh, "account banned", false)
		writeError(w, http.StatusForbidden, "account is banned")
		return
	}
	if err != nil {
		s.recordAudit(r, "-", account.AuditRefresh, "internal error", false)
		s.cfg.Logger.Error("account refresh", "error", err)
		writeError(w, http.StatusInternalServerError, "could not renew the session")
		return
	}
	s.recordAudit(r, user.Username, account.AuditRefresh, "rotated", true)
	writeJSON(w, http.StatusOK, loginResponse{
		Token:              token,
		ExpiresInMS:        account.AccessTokenTTL.Milliseconds(),
		RefreshToken:       refresh,
		RefreshExpiresInMS: account.TokenTTL.Milliseconds(),
		User:               viewAccount(user),
	})
}

// recordAudit persists an account-security outcome for operators. Audit write
// failures are logged but never change the response, so a busy state file
// cannot lock users out or hide an otherwise valid action.
func (s *server) recordAudit(r *http.Request, username, action, detail string, success bool) {
	if s.cfg.Accounts == nil {
		return
	}
	ip := s.clientIP(r)
	if err := s.cfg.Accounts.AppendAudit(account.AuditEvent{
		Username: username,
		Action:   action,
		Detail:   detail,
		ClientIP: ip,
		Location: s.clientLocation(ip),
		Success:  success,
	}); err != nil {
		s.cfg.Logger.Error("account audit", "error", err)
	}
}

// clientLocation resolves an address through the optional GeoIP table. An
// absent table or uncovered address yields an empty label; the IP is stored
// either way.
func (s *server) clientLocation(ip string) string {
	if s.cfg.GeoIP == nil {
		return ""
	}
	return s.cfg.GeoIP.Lookup(ip)
}

func (s *server) auditLogin(r *http.Request, username, detail string, success bool) {
	s.recordAudit(r, username, account.AuditLogin, detail, success)
}

func (s *server) logout(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "account token was not accepted")
		return
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		s.recordAudit(r, "-", account.AuditLogout, "invalid token", false)
		writeError(w, http.StatusUnauthorized, "account token was not accepted")
		return
	}
	if err := s.cfg.Accounts.Logout(token); err != nil {
		s.recordAudit(r, user.Username, account.AuditLogout, "internal error", false)
		s.cfg.Logger.Error("account logout", "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the logout")
		return
	}
	s.recordAudit(r, user.Username, account.AuditLogout, "", true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accountInfo(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	enabled, err := s.cfg.Accounts.TwoFactorEnabled(user.ID)
	if err != nil {
		s.cfg.Logger.Error("account two-factor state", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the account")
		return
	}
	remaining, err := s.cfg.Accounts.RecoveryCodeCount(user.ID)
	if err != nil {
		s.cfg.Logger.Error("account recovery code count", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the account")
		return
	}
	writeJSON(w, http.StatusOK, accountResponse{
		User:          viewAccount(user),
		TwoFactor:     enabled,
		RecoveryCodes: remaining,
	})
}

// changeAccountPassword lets a signed-in user rotate their own password. The
// current session survives so the viewer stays usable; other devices are
// revoked and must log in with the new password.
func (s *server) changeAccountPassword(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req passwordChangeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	token, _ := bearerToken(r)
	err := s.cfg.Accounts.ChangePassword(user.ID, token, req.CurrentPassword, req.NewPassword)
	if errors.Is(err, account.ErrInvalidCredentials) {
		s.recordAudit(r, user.Username, account.AuditPasswordChange, "current password rejected", false)
		writeError(w, http.StatusForbidden, "current password was not accepted")
		return
	}
	if errors.Is(err, account.ErrWeakPassword) {
		s.recordAudit(r, user.Username, account.AuditPasswordChange, "weak password", false)
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditPasswordChange, "internal error", false)
		s.cfg.Logger.Error("account password change", "error", err)
		writeError(w, http.StatusInternalServerError, "could not change the password")
		return
	}
	s.recordAudit(r, user.Username, account.AuditPasswordChange, "other sessions revoked", true)
	s.cfg.Logger.Info("account password changed", "user", user.Username)
	w.WriteHeader(http.StatusNoContent)
}

// startAccountTwoFactor creates a pending TOTP secret. It is not enforced
// until the caller proves possession with a code from the authenticator.
func (s *server) startAccountTwoFactor(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	secret, err := s.cfg.Accounts.StartTwoFactor(user.ID)
	if errors.Is(err, account.ErrTwoFactorEnabled) {
		writeError(w, http.StatusConflict, "two-factor is already enabled")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("account two-factor start", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start two-factor enrollment")
		return
	}
	writeJSON(w, http.StatusOK, twoFactorStartResponse{
		Secret:     secret,
		OTPAuthURI: account.TOTPURI("chast", user.Username, secret),
	})
}

func (s *server) enableAccountTwoFactor(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req twoFactorCodeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	err := s.cfg.Accounts.EnableTwoFactor(user.ID, req.Code)
	if errors.Is(err, account.ErrTwoFactorNotStarted) {
		s.recordAudit(r, user.Username, account.AuditTwoFactorEnable, "enrollment not started", false)
		writeError(w, http.StatusBadRequest, "two-factor enrollment has not started")
		return
	}
	if errors.Is(err, account.ErrInvalidTwoFactorCode) {
		s.recordAudit(r, user.Username, account.AuditTwoFactorEnable, "invalid code", false)
		writeError(w, http.StatusForbidden, "two-factor code was not accepted")
		return
	}
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditTwoFactorEnable, "internal error", false)
		s.cfg.Logger.Error("account two-factor enable", "error", err)
		writeError(w, http.StatusInternalServerError, "could not enable two-factor")
		return
	}
	s.recordAudit(r, user.Username, account.AuditTwoFactorEnable, "", true)
	s.cfg.Logger.Info("account two-factor enabled", "user", user.Username)
	w.WriteHeader(http.StatusNoContent)
}

// disableAccountTwoFactor requires the password as well as a current code, so
// a stolen token cannot remove the second factor by itself.
func (s *server) disableAccountTwoFactor(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req twoFactorDisableRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	err := s.cfg.Accounts.DisableTwoFactor(user.ID, req.Password, req.Code)
	if errors.Is(err, account.ErrTwoFactorNotEnabled) {
		s.recordAudit(r, user.Username, account.AuditTwoFactorDisable, "not enabled", false)
		writeError(w, http.StatusBadRequest, "two-factor is not enabled")
		return
	}
	if errors.Is(err, account.ErrInvalidCredentials) || errors.Is(err, account.ErrInvalidTwoFactorCode) {
		s.recordAudit(r, user.Username, account.AuditTwoFactorDisable, "invalid credentials or code", false)
		writeError(w, http.StatusForbidden, "password or two-factor code was not accepted")
		return
	}
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditTwoFactorDisable, "internal error", false)
		s.cfg.Logger.Error("account two-factor disable", "error", err)
		writeError(w, http.StatusInternalServerError, "could not disable two-factor")
		return
	}
	s.recordAudit(r, user.Username, account.AuditTwoFactorDisable, "", true)
	s.cfg.Logger.Info("account two-factor disabled", "user", user.Username)
	w.WriteHeader(http.StatusNoContent)
}

// generateAccountRecoveryCodes mints a fresh set of single-use backup codes.
// The password and a current TOTP code are required, and the plaintext is
// returned only this once; the store keeps hashes, so neither a stolen token
// nor a database copy can bypass the second factor.
func (s *server) generateAccountRecoveryCodes(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req twoFactorDisableRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	codes, err := s.cfg.Accounts.GenerateRecoveryCodes(user.ID, req.Password, req.Code)
	if errors.Is(err, account.ErrTwoFactorNotEnabled) {
		s.recordAudit(r, user.Username, account.AuditTwoFactorRecovery, "not enabled", false)
		writeError(w, http.StatusBadRequest, "two-factor is not enabled")
		return
	}
	if errors.Is(err, account.ErrInvalidCredentials) || errors.Is(err, account.ErrInvalidTwoFactorCode) {
		s.recordAudit(r, user.Username, account.AuditTwoFactorRecovery, "invalid credentials or code", false)
		writeError(w, http.StatusForbidden, "password or two-factor code was not accepted")
		return
	}
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditTwoFactorRecovery, "internal error", false)
		s.cfg.Logger.Error("account recovery codes", "error", err)
		writeError(w, http.StatusInternalServerError, "could not generate recovery codes")
		return
	}
	s.recordAudit(r, user.Username, account.AuditTwoFactorRecovery, "issued backup codes", true)
	s.cfg.Logger.Info("account recovery codes issued", "user", user.Username)
	writeJSON(w, http.StatusOK, twoFactorRecoveryResponse{Codes: codes})
}

// writeLoginLocked answers a locked username+IP pair. The Retry-After header
// lets a client wait instead of hammering; the body stays generic so it does
// not reveal whether the username exists.
func writeLoginLocked(w http.ResponseWriter, retry time.Duration) {
	seconds := int64((retry + time.Second - 1) / time.Second)
	if seconds < 1 {
		seconds = 1
	}
	w.Header().Set("Retry-After", strconv.FormatInt(seconds, 10))
	writeError(w, http.StatusTooManyRequests, "too many failed logins; retry later")
}

func (s *server) accountSessions(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	token, _ := bearerToken(r)
	sessions, err := s.cfg.Accounts.Sessions(user.ID, token)
	if err != nil {
		s.cfg.Logger.Error("account sessions", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the account sessions")
		return
	}
	out := accountSessionsResponse{Sessions: make([]accountSessionView, 0, len(sessions))}
	for _, session := range sessions {
		out.Sessions = append(out.Sessions, viewAccountSession(session))
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *server) revokeAccountSession(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	revoked, err := s.cfg.Accounts.RevokeSession(user.ID, r.PathValue("id"))
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditSessionRevoke, "internal error", false)
		s.cfg.Logger.Error("account session revoke", "error", err)
		writeError(w, http.StatusInternalServerError, "could not revoke the account session")
		return
	}
	if !revoked {
		s.recordAudit(r, user.Username, account.AuditSessionRevoke, "unknown session", false)
		writeError(w, http.StatusNotFound, "account session is unknown")
		return
	}
	s.recordAudit(r, user.Username, account.AuditSessionRevoke, "session="+r.PathValue("id"), true)
	w.WriteHeader(http.StatusNoContent)
}

// renameAccountSession lets the account label its logged-in devices so a user
// can tell an office desktop from a laptop in the session list. Any live
// session of the account may be renamed, including the caller's own.
func (s *server) renameAccountSession(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req renameSessionRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	renamed, err := s.cfg.Accounts.RenameSession(user.ID, r.PathValue("id"), req.DeviceName)
	if errors.Is(err, account.ErrInvalidDeviceName) {
		s.recordAudit(r, user.Username, account.AuditSessionRename, "invalid device name", false)
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditSessionRename, "internal error", false)
		s.cfg.Logger.Error("account session rename", "error", err)
		writeError(w, http.StatusInternalServerError, "could not rename the account session")
		return
	}
	if !renamed {
		s.recordAudit(r, user.Username, account.AuditSessionRename, "unknown session", false)
		writeError(w, http.StatusNotFound, "session is not live for this account")
		return
	}
	s.recordAudit(r, user.Username, account.AuditSessionRename, "session="+r.PathValue("id"), true)
	w.WriteHeader(http.StatusNoContent)
}

// revokeOtherAccountSessions keeps the caller signed in and removes every
// other device's token, matching the password-change side effect without
// rotating the password.
func (s *server) revokeOtherAccountSessions(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	token, _ := bearerToken(r)
	revoked, err := s.cfg.Accounts.RevokeOtherSessions(user.ID, token)
	if err != nil {
		s.recordAudit(r, user.Username, account.AuditSessionRevoke, "revoke others: internal error", false)
		s.cfg.Logger.Error("account other sessions revoke", "error", err)
		writeError(w, http.StatusInternalServerError, "could not revoke the other account sessions")
		return
	}
	detail := fmt.Sprintf("revoked %d other sessions", revoked)
	s.recordAudit(r, user.Username, account.AuditSessionRevoke, detail, true)
	s.cfg.Logger.Info("account other sessions revoked", "user", user.Username, "count", revoked)
	writeJSON(w, http.StatusOK, revokeOtherSessionsResponse{Revoked: revoked})
}

func (s *server) addressBook(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var since int64
	if raw := strings.TrimSpace(r.URL.Query().Get("since")); raw != "" {
		parsed, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || parsed < 0 {
			writeError(w, http.StatusBadRequest, "since must be a non-negative integer")
			return
		}
		since = parsed
	}
	snapshot, err := s.cfg.Accounts.Snapshot(user.ID, since)
	if err != nil {
		s.cfg.Logger.Error("address book read", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the address book")
		return
	}
	out := addressBookResponse{
		Revision: snapshot.Revision,
		Peers:    make([]addressBookPeer, 0, len(snapshot.Peers)),
		Groups:   make([]addressBookGroup, 0, len(snapshot.Groups)),
	}
	for _, peer := range snapshot.Peers {
		out.Peers = append(out.Peers, viewAddressBookPeer(peer))
	}
	for _, change := range snapshot.Changes {
		out.Changes = append(out.Changes, viewAddressBookPeer(change))
	}
	for _, group := range snapshot.Groups {
		out.Groups = append(out.Groups, viewAddressBookGroup(group))
	}
	for _, change := range snapshot.GroupChanges {
		out.GroupChanges = append(out.GroupChanges, viewAddressBookGroup(change))
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *server) addressBookGrants(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	outgoing, incoming, err := s.cfg.Accounts.Grants(user.ID)
	if err != nil {
		s.cfg.Logger.Error("address book grants", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read address book grants")
		return
	}
	out := addressBookGrantsResponse{
		Outgoing: make([]addressBookGrantView, 0, len(outgoing)),
		Incoming: make([]addressBookGrantView, 0, len(incoming)),
	}
	for _, grant := range outgoing {
		out.Outgoing = append(out.Outgoing, viewAddressBookGrant(grant))
	}
	for _, grant := range incoming {
		out.Incoming = append(out.Incoming, viewAddressBookGrant(grant))
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *server) shareAddressBook(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req shareAddressBookRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if strings.TrimSpace(req.Username) == "" {
		writeError(w, http.StatusBadRequest, "username is required")
		return
	}
	grant, err := s.cfg.Accounts.ShareBookScoped(user.ID, req.Username, req.Permission, req.GroupID)
	switch {
	case errors.Is(err, account.ErrInvalidPermission):
		writeError(w, http.StatusBadRequest, account.ErrInvalidPermission.Error())
		return
	case errors.Is(err, account.ErrInvalidGroupID):
		writeError(w, http.StatusBadRequest, account.ErrInvalidGroupID.Error())
		return
	case errors.Is(err, account.ErrUnknownGroup):
		writeError(w, http.StatusNotFound, "address book group is unknown")
		return
	case errors.Is(err, account.ErrUnknownUser):
		writeError(w, http.StatusNotFound, "user is unknown")
		return
	case errors.Is(err, account.ErrSelfShare):
		writeError(w, http.StatusBadRequest, "cannot share an address book with yourself")
		return
	case err != nil:
		s.cfg.Logger.Error("address book share", "error", err)
		writeError(w, http.StatusInternalServerError, "could not share the address book")
		return
	}
	writeJSON(w, http.StatusOK, viewAddressBookGrant(grant))
}

func (s *server) unshareAddressBook(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var err error
	if query := r.URL.Query(); query.Has("group_id") {
		err = s.cfg.Accounts.UnshareBookScope(user.ID, r.PathValue("username"), query.Get("group_id"))
	} else {
		err = s.cfg.Accounts.UnshareBook(user.ID, r.PathValue("username"))
	}
	switch {
	case errors.Is(err, account.ErrUnknownShare):
		writeError(w, http.StatusNotFound, "address book is not shared with that user")
		return
	case errors.Is(err, account.ErrInvalidGroupID):
		writeError(w, http.StatusBadRequest, account.ErrInvalidGroupID.Error())
		return
	case err != nil:
		s.cfg.Logger.Error("address book unshare", "error", err)
		writeError(w, http.StatusInternalServerError, "could not revoke the address book share")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) sharedAddressBooks(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	books, err := s.cfg.Accounts.SharedBooks(user.ID)
	if err != nil {
		s.cfg.Logger.Error("shared address books", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read shared address books")
		return
	}
	out := sharedAddressBooksResponse{
		Books: make([]sharedAddressBookView, 0, len(books)),
	}
	for _, book := range books {
		view := sharedAddressBookView{
			Owner:      book.Owner,
			Permission: book.Permission,
			Revision:   book.Revision,
			Peers:      make([]addressBookPeer, 0, len(book.Peers)),
			Groups:     make([]addressBookGroup, 0, len(book.Groups)),
			Scopes:     make([]sharedAddressBookScope, 0, len(book.Scopes)),
		}
		for _, peer := range book.Peers {
			view.Peers = append(view.Peers, viewAddressBookPeer(peer))
		}
		for _, group := range book.Groups {
			view.Groups = append(view.Groups, viewAddressBookGroup(group))
		}
		for _, scope := range book.Scopes {
			view.Scopes = append(view.Scopes, sharedAddressBookScope{
				GroupID:    scope.GroupID,
				Permission: scope.Permission,
			})
		}
		out.Books = append(out.Books, view)
	}
	writeJSON(w, http.StatusOK, out)
}

// sharedAddressBookError maps grant checks and validation failures shared by
// the grantee-facing write routes. It reports whether it wrote a response.
func (s *server) sharedAddressBookError(w http.ResponseWriter, action string, err error) bool {
	switch {
	case errors.Is(err, account.ErrUnknownShare):
		writeError(w, http.StatusNotFound, "address book is not shared with this account")
	case errors.Is(err, account.ErrPermissionDenied):
		writeError(w, http.StatusForbidden, "address book permission is insufficient")
	case errors.Is(err, account.ErrInvalidPeerID):
		writeError(w, http.StatusBadRequest, "peer id is invalid")
	case errors.Is(err, account.ErrInvalidGroupID):
		writeError(w, http.StatusBadRequest, account.ErrInvalidGroupID.Error())
	case errors.Is(err, account.ErrUnknownGroup):
		writeError(w, http.StatusNotFound, "address book group is unknown")
	case errors.Is(err, account.ErrInvalidPermission):
		writeError(w, http.StatusBadRequest, account.ErrInvalidPermission.Error())
	case errors.Is(err, account.ErrUnknownUser):
		writeError(w, http.StatusNotFound, "user is unknown")
	case errors.Is(err, account.ErrSelfShare):
		writeError(w, http.StatusBadRequest, "cannot share an address book with yourself")
	case errors.Is(err, account.ErrPeerConflict):
		writeError(w, http.StatusConflict, "address book peer changed elsewhere")
	case err != nil:
		s.cfg.Logger.Error(action, "error", err)
		writeError(w, http.StatusInternalServerError, "could not update the shared address book")
	default:
		return false
	}
	return true
}

func (s *server) addSharedAddressBookPeer(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req sharedAddressBookPeerRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	peer, err := s.cfg.Accounts.AddSharedPeerScoped(
		user.ID, r.PathValue("owner"), req.ID, req.GroupID, req.Name, req.Tags,
	)
	if s.sharedAddressBookError(w, "shared address book add", err) {
		return
	}
	writeJSON(w, http.StatusOK, viewAddressBookPeer(peer))
}

func (s *server) updateSharedAddressBookPeer(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req addressBookPeerRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Name == nil && req.Tags == nil {
		writeError(w, http.StatusBadRequest, "name or tags is required")
		return
	}
	if req.IfRevision != nil && *req.IfRevision <= 0 {
		writeError(w, http.StatusBadRequest, "if_revision must be positive")
		return
	}
	var ifRevision int64
	if req.IfRevision != nil {
		ifRevision = *req.IfRevision
	}
	peer, err := s.cfg.Accounts.UpdateSharedPeerAt(
		user.ID, r.PathValue("owner"), r.PathValue("id"), req.Name, req.Tags, ifRevision,
	)
	if s.sharedAddressBookError(w, "shared address book write", err) {
		return
	}
	writeJSON(w, http.StatusOK, viewAddressBookPeer(peer))
}

func (s *server) deleteSharedAddressBookPeer(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var ifRevision int64
	if raw := r.URL.Query().Get("if_revision"); raw != "" {
		parsed, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || parsed <= 0 {
			writeError(w, http.StatusBadRequest, "if_revision must be positive")
			return
		}
		ifRevision = parsed
	}
	removed, err := s.cfg.Accounts.DeleteSharedPeerAt(
		user.ID, r.PathValue("owner"), r.PathValue("id"), ifRevision,
	)
	if s.sharedAddressBookError(w, "shared address book delete", err) {
		return
	}
	if !removed {
		writeError(w, http.StatusNotFound, "address book peer is unknown")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) sharedAddressBookGrants(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	grants, err := s.cfg.Accounts.SharedBookGrants(user.ID, r.PathValue("owner"))
	if s.sharedAddressBookError(w, "shared address book grants", err) {
		return
	}
	out := addressBookGrantsResponse{
		Outgoing: make([]addressBookGrantView, 0, len(grants)),
		Incoming: make([]addressBookGrantView, 0),
	}
	for _, grant := range grants {
		out.Outgoing = append(out.Outgoing, viewAddressBookGrant(grant))
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *server) shareSharedAddressBook(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req shareAddressBookRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if strings.TrimSpace(req.Username) == "" {
		writeError(w, http.StatusBadRequest, "username is required")
		return
	}
	grant, err := s.cfg.Accounts.ShareBookScopedAs(
		user.ID, r.PathValue("owner"), req.Username, req.Permission, req.GroupID,
	)
	if s.sharedAddressBookError(w, "shared address book grant", err) {
		return
	}
	writeJSON(w, http.StatusOK, viewAddressBookGrant(grant))
}

func (s *server) unshareSharedAddressBook(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var err error
	if query := r.URL.Query(); query.Has("group_id") {
		err = s.cfg.Accounts.UnshareBookScopeAs(
			user.ID, r.PathValue("owner"), r.PathValue("username"), query.Get("group_id"),
		)
	} else {
		err = s.cfg.Accounts.UnshareBookAs(
			user.ID, r.PathValue("owner"), r.PathValue("username"),
		)
	}
	if s.sharedAddressBookError(w, "shared address book revoke", err) {
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) updateAddressBookPeer(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req addressBookPeerRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Name == nil && req.Tags == nil && req.GroupID == nil && req.Position == nil {
		writeError(w, http.StatusBadRequest, "name, tags, group_id or position is required")
		return
	}
	if req.Position != nil && *req.Position < 0 {
		writeError(w, http.StatusBadRequest, "position must not be negative")
		return
	}
	if req.IfRevision != nil && *req.IfRevision <= 0 {
		writeError(w, http.StatusBadRequest, "if_revision must be positive")
		return
	}
	var ifRevision int64
	if req.IfRevision != nil {
		ifRevision = *req.IfRevision
	}
	peerID := r.PathValue("id")
	var (
		peer account.Peer
		err  error
	)
	if req.Name == nil && req.Tags == nil && req.Position == nil {
		groupID := strings.TrimSpace(*req.GroupID)
		peer, err = s.cfg.Accounts.SetPeerGroupAt(user.ID, peerID, groupID, ifRevision)
	} else {
		var groupID *string
		if req.GroupID != nil {
			trimmed := strings.TrimSpace(*req.GroupID)
			groupID = &trimmed
		}
		peer, err = s.cfg.Accounts.UpdatePeerAt(user.ID, peerID, req.Name, req.Tags, groupID, req.Position, ifRevision)
	}
	switch {
	case errors.Is(err, account.ErrInvalidPeerID):
		writeError(w, http.StatusBadRequest, "peer id is invalid")
		return
	case errors.Is(err, account.ErrInvalidGroupID):
		writeError(w, http.StatusBadRequest, "group id is invalid")
		return
	case errors.Is(err, account.ErrInvalidPosition):
		writeError(w, http.StatusBadRequest, "position must not be negative")
		return
	case errors.Is(err, account.ErrUnknownGroup):
		writeError(w, http.StatusBadRequest, "address book group is unknown")
		return
	case errors.Is(err, account.ErrUnknownPeer):
		writeError(w, http.StatusNotFound, "address book peer is unknown")
		return
	case errors.Is(err, account.ErrPeerConflict):
		writeError(w, http.StatusConflict, "address book peer changed elsewhere")
		return
	case err != nil:
		s.cfg.Logger.Error("address book write", "error", err)
		writeError(w, http.StatusInternalServerError, "could not update the address book")
		return
	}
	writeJSON(w, http.StatusOK, viewAddressBookPeer(peer))
}

func (s *server) updateAddressBookGroup(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req addressBookGroupRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Name == nil && req.ParentID == nil && req.Position == nil {
		writeError(w, http.StatusBadRequest, "name, parent_id or position is required")
		return
	}
	if req.Position != nil && *req.Position < 0 {
		writeError(w, http.StatusBadRequest, "position must not be negative")
		return
	}
	if req.IfRevision != nil && *req.IfRevision <= 0 {
		writeError(w, http.StatusBadRequest, "if_revision must be positive")
		return
	}
	var ifRevision int64
	if req.IfRevision != nil {
		ifRevision = *req.IfRevision
	}
	group, err := s.cfg.Accounts.UpdateGroupAt(
		user.ID, r.PathValue("id"), req.Name, req.ParentID, req.Position, ifRevision,
	)
	switch {
	case errors.Is(err, account.ErrInvalidGroupID):
		writeError(w, http.StatusBadRequest, "group id is invalid")
		return
	case errors.Is(err, account.ErrInvalidGroupName):
		writeError(w, http.StatusBadRequest, account.ErrInvalidGroupName.Error())
		return
	case errors.Is(err, account.ErrUnknownGroup):
		writeError(w, http.StatusBadRequest, "parent group is unknown")
		return
	case errors.Is(err, account.ErrGroupCycle):
		writeError(w, http.StatusBadRequest, "group cannot contain itself")
		return
	case errors.Is(err, account.ErrGroupDepth):
		writeError(w, http.StatusBadRequest, "group nesting is too deep")
		return
	case errors.Is(err, account.ErrInvalidPosition):
		writeError(w, http.StatusBadRequest, "position must not be negative")
		return
	case errors.Is(err, account.ErrGroupConflict):
		writeError(w, http.StatusConflict, "address book group changed elsewhere")
		return
	case err != nil:
		s.cfg.Logger.Error("address book group write", "error", err)
		writeError(w, http.StatusInternalServerError, "could not update the address book group")
		return
	}
	writeJSON(w, http.StatusOK, viewAddressBookGroup(group))
}

func (s *server) deleteAddressBookGroup(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var ifRevision int64
	if raw := r.URL.Query().Get("if_revision"); raw != "" {
		parsed, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || parsed <= 0 {
			writeError(w, http.StatusBadRequest, "if_revision must be positive")
			return
		}
		ifRevision = parsed
	}
	removed, err := s.cfg.Accounts.DeleteGroupAt(user.ID, r.PathValue("id"), ifRevision)
	if errors.Is(err, account.ErrInvalidGroupID) {
		writeError(w, http.StatusBadRequest, "group id is invalid")
		return
	}
	if errors.Is(err, account.ErrGroupConflict) {
		writeError(w, http.StatusConflict, "address book group changed elsewhere")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("address book group delete", "error", err)
		writeError(w, http.StatusInternalServerError, "could not update the address book group")
		return
	}
	if !removed {
		writeError(w, http.StatusNotFound, "address book group is unknown")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) deleteAddressBookPeer(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	removed, err := s.cfg.Accounts.DeletePeer(user.ID, r.PathValue("id"))
	if errors.Is(err, account.ErrInvalidPeerID) {
		writeError(w, http.StatusBadRequest, "peer id is invalid")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("address book delete", "error", err)
		writeError(w, http.StatusInternalServerError, "could not update the address book")
		return
	}
	if !removed {
		writeError(w, http.StatusNotFound, "address book peer is unknown")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accountUser(w http.ResponseWriter, r *http.Request) (account.User, bool) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return account.User{}, false
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "account token was not accepted")
		return account.User{}, false
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "account token was not accepted")
		return account.User{}, false
	}
	return user, true
}

func bearerToken(r *http.Request) (string, bool) {
	const prefix = "Bearer "
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, prefix) {
		return "", false
	}
	token := strings.TrimSpace(header[len(prefix):])
	return token, token != ""
}
