package api

import (
	"context"
	"errors"
	"fmt"
	"html"
	"net/http"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/oidc"
)

const (
	// oidcPendingTTL bounds how long a started browser login may take before
	// the viewer's poll is told the attempt expired.
	oidcPendingTTL = 10 * time.Minute
	// oidcMaxPending caps memory used by abandoned browser logins.
	oidcMaxPending = 512
)

// oidcLoginState is one authorization-code attempt. The state itself is the
// only handle the viewer and the browser callback share; it is never logged.
type oidcLoginState struct {
	created    time.Time
	nonce      string
	verifier   string
	deviceID   string
	deviceName string
	// client is the optional caller hint ("console") that lets the browser
	// callback page word its "return to ..." line for the right app.
	client  string
	started bool
	done    bool
	errText string
	errCode string
	user    account.User
	token   string
	refresh string
}

type oidcStatusResponse struct {
	Enabled    bool   `json:"enabled"`
	Name       string `json:"name,omitempty"`
	AutoCreate bool   `json:"auto_create"`
}

type oidcBeginRequest struct {
	DeviceID   string `json:"device_id"`
	DeviceName string `json:"device_name"`
	// Client is an optional caller hint; only "console" is recognised and
	// makes the callback page send the user back to the admin console.
	Client string `json:"client"`
}

type oidcBeginResponse struct {
	AuthURL     string `json:"auth_url"`
	State       string `json:"state"`
	ExpiresInMS int64  `json:"expires_in_ms"`
}

type oidcPollRequest struct {
	State string `json:"state"`
}

// oidcStatus tells a viewer whether the sign-in dialog should offer SSO. An
// unconfigured server answers 200 with enabled=false instead of an error so
// older clients and unconfigured deployments need no special case.
func (s *server) oidcStatus(w http.ResponseWriter, r *http.Request) {
	if s.cfg.OIDC == nil {
		writeJSON(w, http.StatusOK, oidcStatusResponse{Enabled: false})
		return
	}
	writeJSON(w, http.StatusOK, oidcStatusResponse{
		Enabled:    true,
		Name:       s.cfg.OIDC.Name(),
		AutoCreate: s.cfg.OIDC.AutoCreate(),
	})
}

// oidcBegin creates one pending attempt and returns the provider URL the
// viewer must open in a browser.
func (s *server) oidcBegin(w http.ResponseWriter, r *http.Request) {
	if s.cfg.OIDC == nil || s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, "single sign-on is not configured")
		return
	}
	var req oidcBeginRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	state, err := oidc.RandomURLToken()
	if err != nil {
		s.cfg.Logger.Error("oidc state", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start single sign-on")
		return
	}
	nonce, err := oidc.RandomURLToken()
	if err != nil {
		s.cfg.Logger.Error("oidc nonce", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start single sign-on")
		return
	}
	verifier, err := oidc.NewVerifier()
	if err != nil {
		s.cfg.Logger.Error("oidc verifier", "error", err)
		writeError(w, http.StatusInternalServerError, "could not start single sign-on")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	authURL, err := s.cfg.OIDC.AuthorizeURL(ctx, state, nonce, verifier)
	cancel()
	if err != nil {
		s.cfg.Logger.Warn("oidc discovery failed", "error", err)
		writeError(w, http.StatusServiceUnavailable, "identity provider is unavailable")
		return
	}
	now := time.Now()
	client := ""
	if strings.EqualFold(strings.TrimSpace(req.Client), "console") {
		client = "console"
	}
	s.oidcMu.Lock()
	s.pruneOIDCPendingLocked(now)
	s.oidcPending[state] = &oidcLoginState{
		created:    now,
		nonce:      nonce,
		verifier:   verifier,
		deviceID:   strings.TrimSpace(req.DeviceID),
		deviceName: strings.TrimSpace(req.DeviceName),
		client:     client,
	}
	s.oidcMu.Unlock()
	writeJSON(w, http.StatusOK, oidcBeginResponse{
		AuthURL:     authURL,
		State:       state,
		ExpiresInMS: oidcPendingTTL.Milliseconds(),
	})
}

// oidcCallback receives the provider redirect in the browser. It stores the
// issued session for the polling viewer; tokens never travel through the
// browser URL, history or referrer.
func (s *server) oidcCallback(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Referrer-Policy", "no-referrer")
	if s.cfg.OIDC == nil || s.cfg.Accounts == nil {
		writeOIDCPage(w, http.StatusServiceUnavailable, "Single sign-on unavailable",
			"This server has no identity provider configured.", "")
		return
	}
	stateKey := strings.TrimSpace(r.URL.Query().Get("state"))
	if stateKey == "" {
		writeOIDCPage(w, http.StatusBadRequest, "Login failed", "The identity provider returned no state.", "")
		return
	}
	now := time.Now()
	s.oidcMu.Lock()
	s.pruneOIDCPendingLocked(now)
	entry := s.oidcPending[stateKey]
	if entry == nil || now.Sub(entry.created) > oidcPendingTTL {
		delete(s.oidcPending, stateKey)
		s.oidcMu.Unlock()
		writeOIDCPage(w, http.StatusBadRequest, "Login expired",
			"Start the sign-in again from the chast viewer or admin console.", "")
		return
	}
	if entry.started || entry.done {
		s.oidcMu.Unlock()
		writeOIDCPage(w, http.StatusConflict, "Login already used",
			"This sign-in link has already been processed.", "")
		return
	}
	entry.started = true
	nonce, verifier := entry.nonce, entry.verifier
	deviceID, deviceName := entry.deviceID, entry.deviceName
	client := entry.client
	s.oidcMu.Unlock()
	returnTo := oidcReturnTarget(client)

	if providerError := strings.TrimSpace(r.URL.Query().Get("error")); providerError != "" {
		if len(providerError) > 64 {
			providerError = providerError[:64]
		}
		s.cfg.Logger.Warn("oidc provider rejected the login", "error", providerError)
		s.finishOIDC(stateKey, "oidc_failed", "identity provider rejected the login")
		writeOIDCPage(w, http.StatusUnauthorized, "Login rejected",
			"The identity provider did not approve this sign-in.", returnTo)
		return
	}
	code := strings.TrimSpace(r.URL.Query().Get("code"))
	if code == "" {
		s.finishOIDC(stateKey, "oidc_failed", "identity provider returned no code")
		writeOIDCPage(w, http.StatusBadRequest, "Login failed",
			"The identity provider returned no authorization code.", returnTo)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	identity, err := s.cfg.OIDC.Exchange(ctx, code, verifier, nonce)
	cancel()
	if err != nil {
		s.cfg.Logger.Warn("oidc token exchange failed", "error", err)
		s.finishOIDC(stateKey, "oidc_failed", "token exchange failed")
		writeOIDCPage(w, http.StatusBadGateway, "Login failed",
			"The identity provider response could not be verified.", returnTo)
		return
	}
	user, token, _, refresh, _, err := s.cfg.Accounts.LoginOIDC(
		s.cfg.OIDC.Issuer(), identity.Subject, identity.PreferredUsername,
		identity.Email, identity.EmailVerified, deviceID, deviceName,
		s.cfg.OIDC.AutoCreate(),
	)
	if err != nil {
		status, code, message := oidcLoginError(err)
		s.cfg.Logger.Warn("oidc account login failed", "error", err)
		s.finishOIDC(stateKey, code, message)
		writeOIDCPage(w, status, "Login failed", message, returnTo)
		return
	}
	ip := s.clientIP(r)
	if err := s.cfg.Accounts.SetSessionOrigin(token, ip, s.clientLocation(ip)); err != nil {
		s.cfg.Logger.Error("oidc session origin", "error", err)
	}
	s.auditLogin(r, user.Username, "oidc", true)
	s.cfg.Logger.Info("account login", "user", user.Username, "method", "oidc")
	s.completeOIDC(stateKey, user, token, refresh)
	message := fmt.Sprintf("Return to the chast viewer to finish signing in as %s.", user.Username)
	if client == "console" {
		// The console polls in its own tab and closes this one itself.
		message = fmt.Sprintf("Signed in as %s. Switch back to the chast admin console, which completes the sign-in on its own.", user.Username)
	}
	writeOIDCPage(w, http.StatusOK, "Signed in", message, returnTo)
}

// oidcPoll lets the viewer collect the session the browser callback created.
// A 202 answer means the browser step is still pending.
func (s *server) oidcPoll(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	if s.cfg.OIDC == nil || s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, "single sign-on is not configured")
		return
	}
	var req oidcPollRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	stateKey := strings.TrimSpace(req.State)
	if stateKey == "" {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "unknown login", "code": "oidc_expired"})
		return
	}
	now := time.Now()
	s.oidcMu.Lock()
	s.pruneOIDCPendingLocked(now)
	entry := s.oidcPending[stateKey]
	if entry == nil || now.Sub(entry.created) > oidcPendingTTL {
		delete(s.oidcPending, stateKey)
		s.oidcMu.Unlock()
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "unknown or expired login", "code": "oidc_expired"})
		return
	}
	if !entry.done {
		s.oidcMu.Unlock()
		writeJSON(w, http.StatusAccepted, map[string]string{"status": "pending"})
		return
	}
	delete(s.oidcPending, stateKey)
	entryCopy := *entry
	s.oidcMu.Unlock()
	if entryCopy.errText != "" {
		writeJSON(w, http.StatusUnauthorized, map[string]string{
			"error": entryCopy.errText,
			"code":  entryCopy.errCode,
		})
		return
	}
	writeJSON(w, http.StatusOK, loginResponse{
		Token:              entryCopy.token,
		ExpiresInMS:        account.AccessTokenTTL.Milliseconds(),
		RefreshToken:       entryCopy.refresh,
		RefreshExpiresInMS: account.TokenTTL.Milliseconds(),
		User:               viewAccount(entryCopy.user),
	})
}

func (s *server) finishOIDC(stateKey, code, message string) {
	s.oidcMu.Lock()
	defer s.oidcMu.Unlock()
	if entry := s.oidcPending[stateKey]; entry != nil {
		entry.done = true
		entry.errCode = code
		entry.errText = message
	}
}

func (s *server) completeOIDC(stateKey string, user account.User, token, refresh string) {
	s.oidcMu.Lock()
	defer s.oidcMu.Unlock()
	if entry := s.oidcPending[stateKey]; entry != nil {
		entry.done = true
		entry.user = user
		entry.token = token
		entry.refresh = refresh
	}
}

func (s *server) pruneOIDCPendingLocked(now time.Time) {
	for state, entry := range s.oidcPending {
		if now.Sub(entry.created) > oidcPendingTTL {
			delete(s.oidcPending, state)
		}
	}
	if len(s.oidcPending) < oidcMaxPending {
		return
	}
	var oldestState string
	var oldest time.Time
	for state, entry := range s.oidcPending {
		if oldestState == "" || entry.created.Before(oldest) {
			oldestState, oldest = state, entry.created
		}
	}
	if oldestState != "" {
		delete(s.oidcPending, oldestState)
	}
}

func oidcLoginError(err error) (status int, code, message string) {
	switch {
	case errors.Is(err, account.ErrAccountBanned):
		return http.StatusForbidden, "account_banned", "this account is banned"
	case errors.Is(err, account.ErrEmailTaken):
		return http.StatusConflict, "email_taken", "the email address is already used by another account"
	case errors.Is(err, account.ErrOIDCAccountNotFound):
		return http.StatusUnauthorized, "no_account", "no account is linked to this identity"
	default:
		return http.StatusInternalServerError, "oidc_failed", "single sign-on could not complete the login"
	}
}

// oidcReturnTarget names the app whose sign-in started the browser step so the
// page footer can send the user back there. Unknown or missing hints keep the
// neutral "close this tab" wording.
func oidcReturnTarget(client string) string {
	if client == "console" {
		return "the chast admin console"
	}
	return ""
}

func writeOIDCPage(w http.ResponseWriter, status int, title, message, returnTo string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(status)
	footer := "You may close this tab."
	if returnTo != "" {
		footer = "You may close this tab and return to " + returnTo + "."
	}
	_, _ = fmt.Fprintf(w, `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>%s</title>
<style>body{font-family:system-ui,sans-serif;max-width:34rem;margin:15vh auto;padding:0 1.5rem;color:#1f2328}
h1{font-size:1.4rem}p{line-height:1.6}</style></head>
<body><h1>%s</h1><p>%s</p><p>%s</p></body></html>`,
		html.EscapeString(title), html.EscapeString(title), html.EscapeString(message), html.EscapeString(footer))
}
