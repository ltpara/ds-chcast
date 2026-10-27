package api

import (
	"encoding/base64"
	"errors"
	"fmt"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/webauthn"
)

type webauthnCredentialView struct {
	ID           string `json:"id"`
	Alg          int    `json:"alg"`
	Name         string `json:"name"`
	CreatedAtMS  int64  `json:"created_at_ms"`
	LastUsedAtMS int64  `json:"last_used_at_ms,omitempty"`
}

type webauthnRegisterBeginRequest struct {
	Password string `json:"password"`
}

type webauthnRegisterBeginResponse struct {
	Challenge          string                   `json:"challenge"`
	RPID               string                   `json:"rp_id"`
	UserID             string                   `json:"user_id"`
	Username           string                   `json:"username"`
	DisplayName        string                   `json:"display_name"`
	TimeoutMS          int                      `json:"timeout_ms"`
	ExcludeCredentials []webauthnCredentialView `json:"exclude_credentials"`
}

type webauthnRegisterFinishRequest struct {
	Name              string `json:"name"`
	ClientDataJSON    string `json:"client_data_json"`
	AttestationObject string `json:"attestation_object"`
}

type webauthnCredentialsResponse struct {
	Credentials []webauthnCredentialView `json:"credentials"`
}

type webauthnRemoveRequest struct {
	Password     string `json:"password"`
	CredentialID string `json:"credential_id"`
}

type webauthnLoginBeginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type webauthnLoginBeginResponse struct {
	Challenge   string                   `json:"challenge"`
	RPID        string                   `json:"rp_id"`
	Username    string                   `json:"username"`
	TimeoutMS   int                      `json:"timeout_ms"`
	Credentials []webauthnCredentialView `json:"credentials"`
}

type webauthnLoginFinishRequest struct {
	Username          string `json:"username"`
	CredentialID      string `json:"credential_id"`
	ClientDataJSON    string `json:"client_data_json"`
	AuthenticatorData string `json:"authenticator_data"`
	Signature         string `json:"signature"`
	DeviceID          string `json:"device_id"`
	DeviceName        string `json:"device_name"`
}

func viewWebAuthnCredential(cred account.WebAuthnCredential) webauthnCredentialView {
	view := webauthnCredentialView{
		ID:          base64.RawURLEncoding.EncodeToString(cred.ID),
		Alg:         cred.Alg,
		Name:        cred.Name,
		CreatedAtMS: cred.CreatedAt.UnixMilli(),
	}
	if !cred.LastUsedAt.IsZero() {
		view.LastUsedAtMS = cred.LastUsedAt.UnixMilli()
	}
	return view
}

func viewWebAuthnCredentials(creds []account.WebAuthnCredential) []webauthnCredentialView {
	out := make([]webauthnCredentialView, 0, len(creds))
	for _, cred := range creds {
		out = append(out, viewWebAuthnCredential(cred))
	}
	return out
}

// decodeBase64URL accepts the unpadded base64url the API returns and the
// padded form some clients produce.
func decodeBase64URL(value, field string) ([]byte, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return nil, fmt.Errorf("%s is required", field)
	}
	if raw, err := base64.RawURLEncoding.DecodeString(value); err == nil {
		return raw, nil
	}
	raw, err := base64.URLEncoding.DecodeString(value)
	if err != nil {
		return nil, fmt.Errorf("%s must be base64url", field)
	}
	return raw, nil
}

// webauthnReady reports whether the security-key routes are usable. The
// feature stays off until both the relying party id and an origin allowlist
// are configured, because a wrong pairing would silently reject every key.
func (s *server) webauthnReady() bool {
	return s.cfg.Accounts != nil && s.cfg.WebAuthnRPID != "" && len(s.cfg.WebAuthnOrigins) > 0
}

func (s *server) beginWebAuthnRegistration(w http.ResponseWriter, r *http.Request) {
	if !s.webauthnReady() {
		writeError(w, http.StatusServiceUnavailable, "security keys are not configured")
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req webauthnRegisterBeginRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := s.cfg.Accounts.CheckPassword(user.Username, req.Password); err != nil {
		if errors.Is(err, account.ErrInvalidCredentials) {
			s.recordAudit(r, user.Username, account.AuditWebAuthnRegister, "password rejected", false)
			writeError(w, http.StatusForbidden, "password was not accepted")
			return
		}
		s.cfg.Logger.Error("webauthn password check", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start security key registration")
		return
	}
	credentials, err := s.cfg.Accounts.WebAuthnCredentials(user.ID)
	if err != nil {
		s.cfg.Logger.Error("webauthn list credentials", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start security key registration")
		return
	}
	challenge, err := webauthn.NewChallenge()
	if err != nil {
		s.cfg.Logger.Error("webauthn challenge", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start security key registration")
		return
	}
	if _, err := s.cfg.Accounts.IssueWebAuthnChallenge(user.ID, account.WebAuthnRegister, challenge, webauthn.ChallengeTTL); err != nil {
		s.cfg.Logger.Error("webauthn store challenge", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start security key registration")
		return
	}
	writeJSON(w, http.StatusOK, webauthnRegisterBeginResponse{
		Challenge:          base64.RawURLEncoding.EncodeToString(challenge),
		RPID:               s.cfg.WebAuthnRPID,
		UserID:             base64.RawURLEncoding.EncodeToString([]byte(user.ID)),
		Username:           user.Username,
		DisplayName:        user.Username,
		TimeoutMS:          int(webauthn.TimeoutMS),
		ExcludeCredentials: viewWebAuthnCredentials(credentials),
	})
}

func (s *server) finishWebAuthnRegistration(w http.ResponseWriter, r *http.Request) {
	if !s.webauthnReady() {
		writeError(w, http.StatusServiceUnavailable, "security keys are not configured")
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req webauthnRegisterFinishRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	expected, err := s.cfg.Accounts.WebAuthnChallenge(user.ID, account.WebAuthnRegister)
	if err != nil {
		if errors.Is(err, account.ErrWebAuthnChallenge) {
			s.recordAudit(r, user.Username, account.AuditWebAuthnRegister, "no pending challenge", false)
			writeError(w, http.StatusForbidden, "start security key registration first")
			return
		}
		s.cfg.Logger.Error("webauthn registration challenge", "error", err)
		writeError(w, http.StatusInternalServerError, "could not register the security key")
		return
	}
	clientData, err := decodeBase64URL(req.ClientDataJSON, "client_data_json")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	attestation, err := decodeBase64URL(req.AttestationObject, "attestation_object")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	verified, err := webauthn.VerifyRegistration(
		s.cfg.WebAuthnRPID, s.cfg.WebAuthnOrigins, expected, clientData, attestation)
	if err != nil {
		if errors.Is(err, webauthn.ErrVerification) {
			s.recordAudit(r, user.Username, account.AuditWebAuthnRegister, "attestation rejected", false)
			s.cfg.Logger.Warn("webauthn attestation rejected", "user", user.Username, "error", err)
			writeError(w, http.StatusForbidden, "security key attestation was not accepted")
			return
		}
		s.cfg.Logger.Error("webauthn verify registration", "error", err)
		writeError(w, http.StatusInternalServerError, "could not register the security key")
		return
	}
	stored, err := s.cfg.Accounts.FinishWebAuthnRegistration(user.ID, expected, account.WebAuthnCredential{
		ID:        verified.ID,
		PublicKey: verified.PublicKey,
		Alg:       verified.Alg,
		SignCount: verified.SignCount,
	}, req.Name)
	switch {
	case errors.Is(err, account.ErrWebAuthnCredentialExists):
		s.recordAudit(r, user.Username, account.AuditWebAuthnRegister, "credential already registered", false)
		writeError(w, http.StatusConflict, "this security key is already registered")
		return
	case errors.Is(err, account.ErrWebAuthnChallenge):
		s.recordAudit(r, user.Username, account.AuditWebAuthnRegister, "challenge expired", false)
		writeError(w, http.StatusForbidden, "start security key registration first")
		return
	case err != nil:
		s.recordAudit(r, user.Username, account.AuditWebAuthnRegister, "internal error", false)
		s.cfg.Logger.Error("webauthn save credential", "error", err)
		writeError(w, http.StatusInternalServerError, "could not register the security key")
		return
	}
	s.recordAudit(r, user.Username, account.AuditWebAuthnRegister,
		fmt.Sprintf("name=%s alg=%d", stored.Name, stored.Alg), true)
	s.cfg.Logger.Info("security key registered", "user", user.Username, "name", stored.Name)
	writeJSON(w, http.StatusOK, viewWebAuthnCredential(stored))
}

func (s *server) listWebAuthnCredentials(w http.ResponseWriter, r *http.Request) {
	if !s.webauthnReady() {
		writeError(w, http.StatusServiceUnavailable, "security keys are not configured")
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	credentials, err := s.cfg.Accounts.WebAuthnCredentials(user.ID)
	if err != nil {
		s.cfg.Logger.Error("webauthn list credentials", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the security keys")
		return
	}
	writeJSON(w, http.StatusOK, webauthnCredentialsResponse{Credentials: viewWebAuthnCredentials(credentials)})
}

func (s *server) removeWebAuthnCredential(w http.ResponseWriter, r *http.Request) {
	if !s.webauthnReady() {
		writeError(w, http.StatusServiceUnavailable, "security keys are not configured")
		return
	}
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req webauthnRemoveRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	credentialID, err := decodeBase64URL(req.CredentialID, "credential_id")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	err = s.cfg.Accounts.RemoveWebAuthnCredential(user.ID, credentialID, req.Password)
	switch {
	case errors.Is(err, account.ErrInvalidCredentials):
		s.recordAudit(r, user.Username, account.AuditWebAuthnRemove, "password rejected", false)
		writeError(w, http.StatusForbidden, "password was not accepted")
		return
	case errors.Is(err, account.ErrWebAuthnCredential):
		s.recordAudit(r, user.Username, account.AuditWebAuthnRemove, "unknown credential", false)
		writeError(w, http.StatusNotFound, "security key is not registered")
		return
	case err != nil:
		s.recordAudit(r, user.Username, account.AuditWebAuthnRemove, "internal error", false)
		s.cfg.Logger.Error("webauthn remove credential", "error", err)
		writeError(w, http.StatusInternalServerError, "could not remove the security key")
		return
	}
	s.recordAudit(r, user.Username, account.AuditWebAuthnRemove, "removed", true)
	s.cfg.Logger.Info("security key removed", "user", user.Username)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) beginWebAuthnLogin(w http.ResponseWriter, r *http.Request) {
	if !s.webauthnReady() {
		writeError(w, http.StatusServiceUnavailable, "security keys are not configured")
		return
	}
	var req webauthnLoginBeginRequest
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
	challenge, err := webauthn.NewChallenge()
	if err != nil {
		s.cfg.Logger.Error("webauthn challenge", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start security key login")
		return
	}
	user, credentials, err := s.cfg.Accounts.BeginWebAuthnLogin(username, req.Password, challenge, webauthn.ChallengeTTL)
	switch {
	case errors.Is(err, account.ErrInvalidCredentials):
		if retry, allowed := s.cfg.LoginLimiter.Failure(key); !allowed {
			s.auditLogin(r, username, "security key invalid credentials; locked out", false)
			writeLoginLocked(w, retry)
			return
		}
		s.auditLogin(r, username, "security key invalid credentials", false)
		writeError(w, http.StatusUnauthorized, "username or password was not accepted")
		return
	case errors.Is(err, account.ErrAccountBanned):
		s.auditLogin(r, username, "account banned", false)
		writeError(w, http.StatusForbidden, "account is banned")
		return
	case errors.Is(err, account.ErrWebAuthnNotEnabled):
		// The password matched but no key is registered. Answer like a started
		// ceremony so the route cannot map accounts.
		s.auditLogin(r, username, "security key unavailable", false)
		writeJSON(w, http.StatusAccepted, recoverResponse{Status: "accepted"})
		return
	case err != nil:
		s.auditLogin(r, username, "security key internal error", false)
		s.cfg.Logger.Error("webauthn begin login", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not start security key login")
		return
	}
	writeJSON(w, http.StatusOK, webauthnLoginBeginResponse{
		Challenge:   base64.RawURLEncoding.EncodeToString(challenge),
		RPID:        s.cfg.WebAuthnRPID,
		Username:    user.Username,
		TimeoutMS:   int(webauthn.TimeoutMS),
		Credentials: viewWebAuthnCredentials(credentials),
	})
}

func (s *server) finishWebAuthnLogin(w http.ResponseWriter, r *http.Request) {
	if !s.webauthnReady() {
		writeError(w, http.StatusServiceUnavailable, "security keys are not configured")
		return
	}
	var req webauthnLoginFinishRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := strings.TrimSpace(req.Username)
	if username == "" {
		s.auditLogin(r, username, "missing credentials", false)
		writeError(w, http.StatusUnauthorized, "security key assertion was not accepted")
		return
	}
	key := strings.ToLower(username) + "|" + s.clientIP(r)
	if retry, ok := s.cfg.LoginLimiter.Allow(key); !ok {
		s.auditLogin(r, username, "locked out", false)
		writeLoginLocked(w, retry)
		return
	}
	credentialID, err := decodeBase64URL(req.CredentialID, "credential_id")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	clientData, err := decodeBase64URL(req.ClientDataJSON, "client_data_json")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	authenticatorData, err := decodeBase64URL(req.AuthenticatorData, "authenticator_data")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	signature, err := decodeBase64URL(req.Signature, "signature")
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	reject := func(detail string, status int, message string) {
		if retry, allowed := s.cfg.LoginLimiter.Failure(key); !allowed {
			s.auditLogin(r, username, detail+"; locked out", false)
			writeLoginLocked(w, retry)
			return
		}
		s.auditLogin(r, username, detail, false)
		writeError(w, status, message)
	}
	user, err := s.cfg.Accounts.FindUser(username)
	if errors.Is(err, account.ErrUnknownUser) {
		reject("security key assertion for unknown user", http.StatusUnauthorized,
			"security key assertion was not accepted")
		return
	}
	if err != nil {
		s.auditLogin(r, username, "security key internal error", false)
		s.cfg.Logger.Error("webauthn login lookup", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the login")
		return
	}
	credential, err := s.cfg.Accounts.WebAuthnCredentialForUser(user.ID, credentialID)
	if err != nil {
		if errors.Is(err, account.ErrWebAuthnCredential) {
			reject("security key assertion rejected", http.StatusUnauthorized,
				"security key assertion was not accepted")
			return
		}
		s.auditLogin(r, username, "security key internal error", false)
		s.cfg.Logger.Error("webauthn login credential", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the login")
		return
	}
	expected, err := s.cfg.Accounts.WebAuthnChallenge(user.ID, account.WebAuthnLogin)
	if err != nil {
		if errors.Is(err, account.ErrWebAuthnChallenge) {
			reject("security key challenge missing", http.StatusUnauthorized,
				"security key assertion was not accepted")
			return
		}
		s.auditLogin(r, username, "security key internal error", false)
		s.cfg.Logger.Error("webauthn login challenge", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the login")
		return
	}
	counter, err := webauthn.VerifyAssertion(
		s.cfg.WebAuthnRPID, s.cfg.WebAuthnOrigins, expected,
		webauthn.Credential{
			ID:        credential.ID,
			PublicKey: credential.PublicKey,
			Alg:       credential.Alg,
			SignCount: credential.SignCount,
		},
		clientData, authenticatorData, signature,
	)
	if err != nil {
		if errors.Is(err, webauthn.ErrVerification) {
			s.cfg.Logger.Warn("webauthn assertion rejected", "user", username, "error", err)
			reject("security key assertion rejected", http.StatusUnauthorized,
				"security key assertion was not accepted")
			return
		}
		s.auditLogin(r, username, "security key internal error", false)
		s.cfg.Logger.Error("webauthn verify assertion", "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the login")
		return
	}
	user, token, _, refresh, _, err := s.cfg.Accounts.CompleteWebAuthnLogin(
		user.Username, credential.ID, expected, counter, req.DeviceID, req.DeviceName)
	switch {
	case errors.Is(err, account.ErrAccountBanned):
		s.auditLogin(r, username, "account banned", false)
		writeError(w, http.StatusForbidden, "account is banned")
		return
	case errors.Is(err, account.ErrWebAuthnChallenge), errors.Is(err, account.ErrWebAuthnCredential):
		reject("security key assertion replayed", http.StatusUnauthorized,
			"security key assertion was not accepted")
		return
	case err != nil:
		s.auditLogin(r, username, "security key internal error", false)
		s.cfg.Logger.Error("webauthn complete login", "user", username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not complete the login")
		return
	}
	s.cfg.LoginLimiter.Success(key)
	ip := s.clientIP(r)
	if err := s.cfg.Accounts.SetSessionOrigin(token, ip, s.clientLocation(ip)); err != nil {
		s.cfg.Logger.Error("account session origin", "error", err)
	}
	detail := "password+security-key"
	if device := strings.TrimSpace(req.DeviceName); device != "" {
		detail += " device=" + device
	}
	s.auditLogin(r, user.Username, detail, true)
	s.cfg.Logger.Info("account login", "user", user.Username, "factor", "security-key")
	writeJSON(w, http.StatusOK, loginResponse{
		Token:              token,
		ExpiresInMS:        account.AccessTokenTTL.Milliseconds(),
		RefreshToken:       refresh,
		RefreshExpiresInMS: account.TokenTTL.Milliseconds(),
		User:               viewAccount(user),
	})
}
