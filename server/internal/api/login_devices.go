package api

import (
	"errors"
	"net/http"
	"strconv"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type loginDeviceChallengeRequest struct {
	PublicKey string `json:"public_key"`
}

type loginDeviceChallengeResponse struct {
	ChallengeID string `json:"challenge_id"`
	Message     string `json:"message"`
	ExpiresInMS int64  `json:"expires_in_ms"`
}

type loginDeviceView struct {
	ID           string `json:"id"`
	DeviceName   string `json:"device_name"`
	CreatedAtMS  int64  `json:"created_at_ms"`
	LastSeenAtMS int64  `json:"last_seen_at_ms"`
	Current      bool   `json:"current,omitempty"`
}

type loginDevicesResponse struct {
	Enabled bool              `json:"enabled"`
	Devices []loginDeviceView `json:"devices"`
}

type loginDevicePolicyRequest struct {
	Enabled *bool `json:"enabled"`
}

type trustLoginDeviceRequest struct {
	PublicKey  string `json:"public_key"`
	DeviceName string `json:"device_name"`
}

func viewLoginDevices(state account.LoginDeviceState, currentID ...string) loginDevicesResponse {
	current := ""
	if len(currentID) > 0 {
		current = currentID[0]
	}
	response := loginDevicesResponse{
		Enabled: state.Enabled,
		Devices: make([]loginDeviceView, 0, len(state.Devices)),
	}
	for _, device := range state.Devices {
		response.Devices = append(response.Devices, loginDeviceView{
			ID: device.ID, DeviceName: device.DeviceName,
			CreatedAtMS:  device.CreatedAt.UnixMilli(),
			LastSeenAtMS: device.LastSeenAt.UnixMilli(),
			Current:      current != "" && device.ID == current,
		})
	}
	return response
}

func (s *server) createLoginDeviceChallenge(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req loginDeviceChallengeRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	id, message, err := s.cfg.Accounts.CreateLoginDeviceChallenge(req.PublicKey)
	if errors.Is(err, account.ErrInvalidLoginDeviceProof) {
		writeError(w, http.StatusBadRequest, "public key is not a valid Ed25519 installation key")
		return
	}
	if err != nil {
		s.cfg.Logger.Error("create login device challenge", "error", err)
		writeError(w, http.StatusInternalServerError, "could not create device verification challenge")
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, loginDeviceChallengeResponse{
		ChallengeID: id, Message: message, ExpiresInMS: account.LoginDeviceChallengeTTL.Milliseconds(),
	})
}

func (s *server) listLoginDevices(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	state, err := s.cfg.Accounts.LoginDevices(user.ID)
	if err != nil {
		s.cfg.Logger.Error("list login devices", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not list trusted login devices")
		return
	}
	token, _ := bearerToken(r)
	currentID, err := s.cfg.Accounts.LoginDeviceIDForToken(user.ID, token)
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, viewLoginDevices(state, currentID))
}

func (s *server) setLoginDevicePolicy(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	var req loginDevicePolicyRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Enabled == nil {
		writeError(w, http.StatusBadRequest, "enabled is required")
		return
	}
	token, _ := bearerToken(r)
	state, revoked, err := s.cfg.Accounts.SetLoginDevicePolicy(user.ID, token, *req.Enabled)
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	verb := "disabled"
	if *req.Enabled {
		verb = "enabled"
	}
	s.recordAudit(r, user.Username, account.AuditLoginDevicePolicy,
		"login device allowlist "+verb+"; revoked_sessions="+itoa(revoked), true)
	currentID, err := s.cfg.Accounts.LoginDeviceIDForToken(user.ID, token)
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, viewLoginDevices(state, currentID))
}

func (s *server) revokeLoginDevice(w http.ResponseWriter, r *http.Request) {
	user, ok := s.accountUser(w, r)
	if !ok {
		return
	}
	revoked, err := s.cfg.Accounts.RevokeLoginDevice(user.ID, r.PathValue("id"))
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	s.recordAudit(r, user.Username, account.AuditLoginDeviceRevoke,
		"login device revoked; sessions="+itoa(revoked), true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) adminListLoginDevices(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	principal, ok := s.consolePrincipalFor(w, r, account.PermissionAccountsRead, account.TokenResourceAccounts)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	state, err := s.cfg.Accounts.LoginDevices(user.ID)
	if err != nil {
		s.cfg.Logger.Error("admin list login devices", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "could not list trusted login devices")
		return
	}
	writeJSON(w, http.StatusOK, viewLoginDevices(state))
}

func (s *server) adminTrustLoginDevice(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	var req trustLoginDeviceRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	device, err := s.cfg.Accounts.AddLoginDevice(user.ID, req.PublicKey, req.DeviceName)
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditLoginDeviceTrust,
		"trusted login device for "+user.Username+" id="+device.ID, true)
	state, err := s.cfg.Accounts.LoginDevices(user.ID)
	if err != nil {
		s.cfg.Logger.Error("read trusted login devices after enrollment", "user", user.Username, "error", err)
		writeError(w, http.StatusInternalServerError, "device was trusted but the account list could not be read")
		return
	}
	writeJSON(w, http.StatusCreated, viewLoginDevices(state))
}

func (s *server) adminSetLoginDevicePolicy(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	var req loginDevicePolicyRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Enabled == nil {
		writeError(w, http.StatusBadRequest, "enabled is required")
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	if *req.Enabled {
		current, err := s.cfg.Accounts.LoginDevices(user.ID)
		if err != nil {
			s.cfg.Logger.Error("check trusted login devices before admin policy enable", "user", user.Username, "error", err)
			writeError(w, http.StatusInternalServerError, "could not check trusted login devices")
			return
		}
		if len(current.Devices) == 0 {
			writeError(w, http.StatusConflict, "trust at least one installation before enabling the login device allowlist")
			return
		}
	}
	state, revoked, err := s.cfg.Accounts.SetLoginDevicePolicy(user.ID, "", *req.Enabled)
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	verb := "disabled"
	if *req.Enabled {
		verb = "enabled"
	}
	s.recordAudit(r, principal.Actor, account.AuditLoginDevicePolicy,
		"login device allowlist "+verb+" for "+user.Username+"; revoked_sessions="+itoa(revoked), true)
	writeJSON(w, http.StatusOK, viewLoginDevices(state))
}

func (s *server) adminRevokeLoginDevice(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceAccounts)
	if !ok {
		return
	}
	user, err := s.cfg.Accounts.FindUser(r.PathValue("username"))
	if err != nil {
		writeAccountError(w, err)
		return
	}
	revoked, err := s.cfg.Accounts.RevokeLoginDevice(user.ID, r.PathValue("id"))
	if err != nil {
		writeLoginDeviceError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditLoginDeviceRevoke,
		"revoked login device for "+user.Username+" id="+r.PathValue("id")+"; sessions="+itoa(revoked), true)
	w.WriteHeader(http.StatusNoContent)
}

func writeLoginDeviceError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownUser):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrUnknownLoginDevice):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrInvalidToken):
		writeError(w, http.StatusUnauthorized, "account token was not accepted")
	case errors.Is(err, account.ErrInvalidLoginDeviceID), errors.Is(err, account.ErrInvalidLoginDeviceProof):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrLoginDeviceProofNeeded):
		writeError(w, http.StatusConflict, "sign in from a trusted installation before enabling the allowlist")
	default:
		writeError(w, http.StatusInternalServerError, "could not update trusted login devices")
	}
}

func writeLoginDeviceAuthError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, map[string]string{"error": message, "code": code})
}

func itoa(value int) string {
	return strconv.Itoa(value)
}
