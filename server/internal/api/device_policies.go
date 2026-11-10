package api

import (
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
)

type devicePolicyView struct {
	PeerID            string                   `json:"peer_id"`
	Mode              string                   `json:"mode"`
	PermissionCeiling uint32                   `json:"permission_ceiling"`
	PolicyVersion     uint32                   `json:"policy_version"`
	UpdatedAtMS       int64                    `json:"updated_at_ms"`
	UpdatedBy         string                   `json:"updated_by,omitempty"`
	AllowedUsers      []string                 `json:"allowed_users"`
	AllowedGroups     []string                 `json:"allowed_groups"`
	Source            *devicePolicySourceView  `json:"source,omitempty"`
	Receipt           *devicePolicyReceiptView `json:"receipt,omitempty"`
}

type devicePolicySourceView struct {
	Kind     string `json:"kind"`
	ID       string `json:"id"`
	Name     string `json:"name"`
	Version  uint32 `json:"version"`
	Priority int    `json:"priority"`
}

type devicePolicyReceiptView struct {
	PolicyVersion uint32 `json:"policy_version"`
	Applied       bool   `json:"applied"`
	Detail        string `json:"detail,omitempty"`
	ReportedAtMS  int64  `json:"reported_at_ms"`
}

type devicePolicyResponse struct {
	Managed bool              `json:"managed"`
	Policy  *devicePolicyView `json:"policy,omitempty"`
}

type devicePolicySetRequest struct {
	Mode              string   `json:"mode"`
	PermissionCeiling uint32   `json:"permission_ceiling"`
	AllowedUsers      []string `json:"allowed_users"`
	AllowedGroups     []string `json:"allowed_groups"`
}

type devicePolicyReceiptRequest struct {
	PolicyVersion uint32 `json:"policy_version"`
	Applied       bool   `json:"applied"`
	Detail        string `json:"detail"`
	Timestamp     int64  `json:"timestamp"`
	Signature     string `json:"signature"`
}

func viewDevicePolicy(policy account.DevicePolicy) devicePolicyView {
	view := devicePolicyView{
		PeerID:            policy.PeerID,
		Mode:              policy.Mode,
		PermissionCeiling: policy.PermissionCeiling,
		PolicyVersion:     policy.PolicyVersion,
		UpdatedAtMS:       policy.UpdatedAt.UnixMilli(),
		UpdatedBy:         policy.UpdatedBy,
		AllowedUsers:      policy.AllowedUsers,
		AllowedGroups:     policy.AllowedGroups,
	}
	if view.AllowedUsers == nil {
		view.AllowedUsers = []string{}
	}
	if view.AllowedGroups == nil {
		view.AllowedGroups = []string{}
	}
	if policy.Source != nil {
		view.Source = &devicePolicySourceView{
			Kind:     policy.Source.Kind,
			ID:       policy.Source.ID,
			Name:     policy.Source.Name,
			Version:  policy.Source.Version,
			Priority: policy.Source.Priority,
		}
	}
	if policy.Receipt != nil {
		view.Receipt = &devicePolicyReceiptView{
			PolicyVersion: policy.Receipt.PolicyVersion,
			Applied:       policy.Receipt.Applied,
			Detail:        policy.Receipt.Detail,
			ReportedAtMS:  policy.Receipt.ReportedAt.UnixMilli(),
		}
	}
	return view
}

func (s *server) listDevicePolicies(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok {
		return
	}
	var groupDevices map[string]bool
	if principal.GroupID != "" {
		devices, err := s.cfg.Accounts.GroupDevices(principal.GroupID)
		if err != nil {
			s.cfg.Logger.Error("device policy group scope", "group", principal.GroupID, "error", err)
			writeError(w, http.StatusInternalServerError, "could not read the device group")
			return
		}
		groupDevices = make(map[string]bool, len(devices))
		for _, device := range devices {
			groupDevices[device.PeerID] = true
		}
	}
	policies, err := s.cfg.Accounts.DevicePolicies()
	if err != nil {
		s.cfg.Logger.Error("list device policies", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read device policies")
		return
	}
	out := make([]devicePolicyView, 0, len(policies))
	for _, policy := range policies {
		if groupDevices != nil && !groupDevices[policy.PeerID] {
			continue
		}
		out = append(out, viewDevicePolicy(policy))
	}
	writeJSON(w, http.StatusOK, map[string]any{"policies": out})
}

func (s *server) devicePolicyDetail(w http.ResponseWriter, r *http.Request) {
	peerID := r.PathValue("peer")
	if _, ok := s.deviceActor(w, r, consoleAdmin, peerID); !ok {
		return
	}
	policy, managed, err := s.cfg.Accounts.DevicePolicy(peerID)
	if err != nil {
		writeDevicePolicyError(w, err)
		return
	}
	if !managed {
		writeJSON(w, http.StatusOK, devicePolicyResponse{Managed: false})
		return
	}
	view := viewDevicePolicy(policy)
	writeJSON(w, http.StatusOK, devicePolicyResponse{Managed: true, Policy: &view})
}

func (s *server) setDevicePolicy(w http.ResponseWriter, r *http.Request) {
	peerID := r.PathValue("peer")
	principal, ok := s.deviceActor(w, r, consoleAdmin, peerID)
	if !ok {
		return
	}
	actor := principal.Actor
	var req devicePolicySetRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	policy, err := s.cfg.Accounts.SetDevicePolicy(
		peerID, req.Mode, req.PermissionCeiling, req.AllowedUsers, req.AllowedGroups, actor)
	if err != nil {
		writeDevicePolicyError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditDevicePolicyUpdate,
		"set "+policy.Mode+" policy version "+strconv.FormatUint(uint64(policy.PolicyVersion), 10)+" for "+policy.PeerID, true)
	s.syncRegistryPolicy(policy.PeerID)
	if s.eventHub != nil {
		s.eventHub.publishPolicy(policy.PeerID, &policy)
	}
	view := viewDevicePolicy(policy)
	writeJSON(w, http.StatusOK, devicePolicyResponse{Managed: true, Policy: &view})
}

func (s *server) deleteDevicePolicy(w http.ResponseWriter, r *http.Request) {
	peerID := r.PathValue("peer")
	principal, ok := s.deviceActor(w, r, consoleAdmin, peerID)
	if !ok {
		return
	}
	actor := principal.Actor
	if err := s.cfg.Accounts.DeleteDevicePolicy(peerID); err != nil {
		writeDevicePolicyError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditDevicePolicyDelete,
		"removed device policy for "+peerID, true)
	// Deleting the manual override can reveal an assignment underneath it, so
	// the device is not necessarily unmanaged afterwards.
	changed, _, err := s.cfg.Accounts.ReconcileDevicePolicies([]string{peerID})
	if err != nil {
		s.cfg.Logger.Error("reconcile device policy after delete", "id", peerID, "error", err)
	}
	s.syncRegistryPolicy(peerID)
	if s.eventHub != nil {
		if len(changed) > 0 {
			s.eventHub.publishPolicy(peerID, &changed[0])
		} else {
			// Managed=false tells the host to drop its policy file. Existing
			// sessions are not terminated: deleting a policy relaxes admission.
			s.eventHub.publishPolicy(peerID, nil)
		}
	}
	w.WriteHeader(http.StatusNoContent)
}

// syncRegistryPolicy mirrors the durable policy state into the registry, so
// punch admission stops trusting the link key as soon as a device becomes
// managed instead of at the host's next registration.
func (s *server) syncRegistryPolicy(peerID string) {
	if s.cfg.Accounts == nil || s.cfg.Registry == nil {
		return
	}
	policy, managed, err := s.cfg.Accounts.DevicePolicy(peerID)
	if err != nil {
		s.cfg.Logger.Error("sync registry device policy", "id", peerID, "error", err)
		return
	}
	if !managed {
		s.cfg.Registry.SetPolicy(peerID, false, 0)
		return
	}
	s.cfg.Registry.SetPolicy(peerID, true, policy.PolicyVersion)
}

// hostDevicePolicy is the policy statement a host receives on registration.
// Unmanaged devices get nil so they keep the v61 local-authentication
// behavior; the account allow list stays server-side because ticket issuance
// already applies it.
func (s *server) hostDevicePolicy(peerID string) (*hostDevicePolicy, error) {
	if s.cfg.Accounts == nil {
		return nil, nil
	}
	// A device that registers after an assignment was created has no
	// materialised row yet; reconcile before reading so registration carries
	// the assigned policy instead of the unmanaged state.
	if _, _, err := s.cfg.Accounts.ReconcileDevicePolicies([]string{peerID}); err != nil {
		return nil, err
	}
	policy, managed, err := s.cfg.Accounts.DevicePolicy(peerID)
	if err != nil {
		return nil, err
	}
	if !managed {
		return nil, nil
	}
	return &hostDevicePolicy{
		Mode:              policy.Mode,
		PermissionCeiling: policy.PermissionCeiling,
		PolicyVersion:     policy.PolicyVersion,
		UpdatedAtMS:       policy.UpdatedAt.UnixMilli(),
	}, nil
}

// reportDevicePolicyReceipt is called by a controlled host after it has
// applied a policy version. The host key that owns the device id signs the
// receipt, so the shared host credential alone cannot acknowledge another
// machine's policy.
func (s *server) reportDevicePolicyReceipt(w http.ResponseWriter, r *http.Request) {
	if !s.hostAuthorized(r) {
		writeError(w, http.StatusUnauthorized, "host credential required")
		return
	}
	peerID := strings.TrimSpace(r.PathValue("peer"))
	if !registry.ValidID(peerID) {
		writeError(w, http.StatusBadRequest, "host id must be 1..64 characters of [A-Za-z0-9_.-]")
		return
	}
	var req devicePolicyReceiptRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	record, ok := s.cfg.Registry.Identity(peerID)
	if !ok {
		writeError(w, http.StatusNotFound, "unknown host")
		return
	}
	pub, err := identity.ParsePublicKey(record.PublicKey)
	if err != nil {
		s.cfg.Logger.Error("device policy receipt identity", "id", peerID, "error", err)
		writeError(w, http.StatusInternalServerError, "host identity is unreadable")
		return
	}
	now := time.Now()
	signedAt := time.Unix(req.Timestamp, 0)
	if req.Timestamp == 0 || now.Sub(signedAt) > maxSkew || signedAt.Sub(now) > maxSkew {
		writeError(w, http.StatusUnauthorized, "receipt timestamp is outside the allowed window")
		return
	}
	msg := identity.HostDevicePolicyReceiptMessage(
		peerID, req.PolicyVersion, req.Applied, req.Detail, req.Timestamp)
	if !identity.Verify(pub, msg, req.Signature) {
		writeError(w, http.StatusUnauthorized, "receipt signature was not accepted")
		return
	}
	receipt, err := s.cfg.Accounts.RecordDevicePolicyReceipt(
		peerID, req.PolicyVersion, req.Applied, req.Detail)
	if err != nil {
		writeDevicePolicyError(w, err)
		return
	}
	detail := "policy version " + strconv.FormatUint(uint64(receipt.PolicyVersion), 10) + " rejected"
	if receipt.Applied {
		detail = "applied policy version " + strconv.FormatUint(uint64(receipt.PolicyVersion), 10)
	}
	s.recordAudit(r, "host", account.AuditDevicePolicyReceipt,
		detail+" for "+peerID+": "+receipt.Detail, receipt.Applied)
	writeJSON(w, http.StatusOK, devicePolicyReceiptView{
		PolicyVersion: receipt.PolicyVersion,
		Applied:       receipt.Applied,
		Detail:        receipt.Detail,
		ReportedAtMS:  receipt.ReportedAt.UnixMilli(),
	})
}

func writeDevicePolicyError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownDevicePolicy):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrInvalidPeerID):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrInvalidDevicePolicyMode),
		errors.Is(err, account.ErrInvalidPermissionCeiling),
		errors.Is(err, account.ErrDevicePolicyUser),
		errors.Is(err, account.ErrDevicePolicyGroup),
		errors.Is(err, account.ErrDevicePolicyTooMany):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrStaleDevicePolicyVersion):
		writeError(w, http.StatusConflict, err.Error())
	default:
		writeError(w, http.StatusInternalServerError, "could not update device policy")
	}
}
