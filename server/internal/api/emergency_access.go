package api

import (
	"encoding/base64"
	"errors"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const maxEmergencyReason = 512

type emergencyAccessRequest struct {
	PeerID             string `json:"peer_id"`
	ClientPublicKey    string `json:"client_public_key"`
	TransportPublicKey string `json:"transport_public_key"`
	Permissions        uint32 `json:"permissions"`
	Reason             string `json:"reason"`
}

// emergencyAccessResponse is a complete connection description: the ordinary
// lookup fields (host identity, punch/relay/carrier endpoints) plus the
// short-lived access and transport tickets. A ticket alone is unusable when
// the allow list refuses the caller's lookup, so the override must hand the
// caller the same rendezvous data an admitted lookup would.
type emergencyAccessResponse struct {
	lookupResponse
}

// issueEmergencyAccess grants one explicit management override for a managed
// device. It bypasses the account allow list but not the permission ceiling:
// the ticket is still short-lived, still passes host local authentication, and
// the actor and stated reason are recorded as their own audit action.
func (s *server) issueEmergencyAccess(w http.ResponseWriter, r *http.Request) {
	if s.accessIssuer == nil {
		writeError(w, http.StatusServiceUnavailable, "access tickets are not configured")
		return
	}
	if s.accessIssuerRevoked() {
		writeError(w, http.StatusServiceUnavailable, "the access ticket issuer key has been revoked")
		return
	}
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok {
		return
	}
	actor := principal.Actor
	var req emergencyAccessRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	peerID := strings.TrimSpace(req.PeerID)
	if !registry.ValidID(peerID) {
		writeError(w, http.StatusBadRequest, "host id must be 1..64 characters of [A-Za-z0-9_.-]")
		return
	}
	if !s.deviceInScope(w, principal, peerID) {
		return
	}
	reason := strings.TrimSpace(req.Reason)
	if reason == "" {
		writeError(w, http.StatusBadRequest, "reason is required for emergency access")
		return
	}
	if len(reason) > maxEmergencyReason {
		writeError(w, http.StatusBadRequest, "reason is too long")
		return
	}
	clientKey, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(req.ClientPublicKey))
	if err != nil || len(clientKey) != 32 {
		writeError(w, http.StatusBadRequest, "client_public_key must be 32 base64url bytes")
		return
	}
	transportKey, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(req.TransportPublicKey))
	if err != nil || len(transportKey) != 32 {
		writeError(w, http.StatusBadRequest, "transport_public_key must be 32 base64url bytes")
		return
	}
	record, exists := s.cfg.Registry.Identity(peerID)
	if !exists {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	if record.Fingerprint == "" {
		writeError(w, http.StatusConflict, "host fingerprint is not available")
		return
	}
	policy, managed, err := s.cfg.Accounts.DevicePolicy(peerID)
	if err != nil {
		s.cfg.Logger.Error("emergency access policy", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the device policy")
		return
	}
	if !managed {
		writeError(w, http.StatusConflict, "emergency access requires a managed device")
		return
	}
	// The allow list is deliberately ignored; the ceiling still applies because
	// an override may reach the host but must not widen its published limits.
	admission := account.DeviceAdmission{
		Managed:           true,
		Allowed:           true,
		Reason:            "emergency access",
		PolicyVersion:     policy.PolicyVersion,
		PermissionCeiling: policy.PermissionCeiling,
	}
	requested := req.Permissions
	if requested == 0 {
		requested = ^uint32(0)
	}
	subject := "emergency:" + actor
	now := time.Now().UTC()
	resp, ok := s.hostLookup(peerID, now, s.relayOnly(), net.ParseIP(s.clientIP(r)))
	if !ok {
		writeError(w, http.StatusNotFound, "host is not registered")
		return
	}
	resp.PunchV6 = s.punchV6(r)
	resp.Software = s.software()
	resp.ConfigSerial, resp.RendezvousServers = s.rendezvousConfig()
	grant, err := s.mintAccessTicket(peerID, subject, record.Fingerprint, clientKey,
		requested, admission, now)
	switch {
	case errors.Is(err, errTicketNotConfigured):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	case errors.Is(err, errTicketReplayFull):
		writeError(w, http.StatusServiceUnavailable, err.Error())
		return
	case err != nil:
		s.cfg.Logger.Error("emergency access ticket sign", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not sign the access ticket")
		return
	}
	credentials, err := s.mintTransportCredentials(peerID, subject, admission, transportKey, now)
	if err != nil {
		s.cfg.Logger.Error("emergency access transport ticket", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "could not mint the transport ticket")
		return
	}
	s.recordAudit(r, actor, account.AuditEmergencyAccess,
		"granted emergency access to "+peerID+" policy "+
			strconv.FormatUint(uint64(grant.policyVersion), 10)+" permissions "+
			strconv.FormatUint(uint64(grant.permissions), 10)+" reason: "+reason, true)
	resp.AccessTicket = s.accessTicketView(grant)
	resp.TransportPunch = credentials.punch
	resp.TransportRelay = credentials.relay
	resp.TransportCarrier = credentials.carrier
	writeJSON(w, http.StatusCreated, emergencyAccessResponse{lookupResponse: resp})
}
