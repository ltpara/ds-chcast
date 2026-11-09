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
	Receipt           *devicePolicyReceiptView `json:"receipt,omitempty"`
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
	if _, _, ok := s.adminActor(w, r, account.TokenResourceDevices); !ok {
		return
	}
	policies, err := s.cfg.Accounts.DevicePolicies()
	if err != nil {
		s.cfg.Logger.Error("list device policies", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read device policies")
		return
	}
	out := make([]devicePolicyView, 0, len(policies))
	for _, policy := range policies {
		out = append(out, viewDevicePolicy(policy))
	}
	writeJSON(w, http.StatusOK, map[string]any{"policies": out})
}

func (s *server) devicePolicyDetail(w http.ResponseWriter, r *http.Request) {
	if _, _, ok := s.adminActor(w, r, account.TokenResourceDevices); !ok {
		return
	}
	peerID := r.PathValue("peer")
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
	_, actor, ok := s.adminActor(w, r, account.TokenResourceDevices)
	if !ok {
		return
	}
	var req devicePolicySetRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	peerID := r.PathValue("peer")
	policy, err := s.cfg.Accounts.SetDevicePolicy(
		peerID, req.Mode, req.PermissionCeiling, req.AllowedUsers, req.AllowedGroups, actor)
	if err != nil {
		writeDevicePolicyError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditDevicePolicyUpdate,
		"set "+policy.Mode+" policy version "+strconv.FormatUint(uint64(policy.PolicyVersion), 10)+" for "+policy.PeerID, true)
	view := viewDevicePolicy(policy)
	writeJSON(w, http.StatusOK, devicePolicyResponse{Managed: true, Policy: &view})
}

func (s *server) deleteDevicePolicy(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.adminActor(w, r, account.TokenResourceDevices)
	if !ok {
		return
	}
	peerID := r.PathValue("peer")
	if err := s.cfg.Accounts.DeleteDevicePolicy(peerID); err != nil {
		writeDevicePolicyError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditDevicePolicyDelete,
		"removed device policy for "+peerID, true)
	w.WriteHeader(http.StatusNoContent)
}

// hostDevicePolicy is the policy statement a host receives on registration.
// Unmanaged devices get nil so they keep the v61 local-authentication
// behavior; the account allow list stays server-side because ticket issuance
// already applies it.
func (s *server) hostDevicePolicy(peerID string) (*hostDevicePolicy, error) {
	if s.cfg.Accounts == nil {
		return nil, nil
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
