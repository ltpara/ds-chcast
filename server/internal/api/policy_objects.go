package api

import (
	"errors"
	"net/http"
	"strconv"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type policyObjectView struct {
	ID                string   `json:"id"`
	Name              string   `json:"name"`
	Mode              string   `json:"mode"`
	PermissionCeiling uint32   `json:"permission_ceiling"`
	PolicyVersion     uint32   `json:"policy_version"`
	AllowedUsers      []string `json:"allowed_users"`
	AllowedGroups     []string `json:"allowed_groups"`
	UpdatedAtMS       int64    `json:"updated_at_ms"`
	UpdatedBy         string   `json:"updated_by,omitempty"`
}

type policyObjectRequest struct {
	Name              string   `json:"name"`
	Mode              string   `json:"mode"`
	PermissionCeiling uint32   `json:"permission_ceiling"`
	AllowedUsers      []string `json:"allowed_users"`
	AllowedGroups     []string `json:"allowed_groups"`
}

type policyAssignmentView struct {
	ID          string `json:"id"`
	PolicyID    string `json:"policy_id"`
	PolicyName  string `json:"policy_name"`
	ScopeKind   string `json:"scope_kind"`
	ScopeValue  string `json:"scope_value"`
	Priority    int    `json:"priority"`
	CreatedAtMS int64  `json:"created_at_ms"`
	CreatedBy   string `json:"created_by,omitempty"`
}

type policyAssignmentRequest struct {
	PolicyID   string `json:"policy_id"`
	ScopeKind  string `json:"scope_kind"`
	ScopeValue string `json:"scope_value"`
	Priority   int    `json:"priority"`
}

func viewPolicyObject(policy account.PolicyObject) policyObjectView {
	view := policyObjectView{
		ID:                policy.ID,
		Name:              policy.Name,
		Mode:              policy.Mode,
		PermissionCeiling: policy.PermissionCeiling,
		PolicyVersion:     policy.PolicyVersion,
		AllowedUsers:      policy.AllowedUsers,
		AllowedGroups:     policy.AllowedGroups,
		UpdatedAtMS:       policy.UpdatedAt.UnixMilli(),
		UpdatedBy:         policy.UpdatedBy,
	}
	if view.AllowedUsers == nil {
		view.AllowedUsers = []string{}
	}
	if view.AllowedGroups == nil {
		view.AllowedGroups = []string{}
	}
	return view
}

func viewPolicyAssignment(assignment account.PolicyAssignment) policyAssignmentView {
	return policyAssignmentView{
		ID:          assignment.ID,
		PolicyID:    assignment.PolicyID,
		PolicyName:  assignment.PolicyName,
		ScopeKind:   assignment.ScopeKind,
		ScopeValue:  assignment.ScopeValue,
		Priority:    assignment.Priority,
		CreatedAtMS: assignment.CreatedAt.UnixMilli(),
		CreatedBy:   assignment.CreatedBy,
	}
}

func (s *server) listPolicyObjects(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	policies, err := s.cfg.Accounts.PolicyObjects()
	if err != nil {
		s.cfg.Logger.Error("list policy objects", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read policy objects")
		return
	}
	out := make([]policyObjectView, 0, len(policies))
	for _, policy := range policies {
		out = append(out, viewPolicyObject(policy))
	}
	writeJSON(w, http.StatusOK, map[string]any{"policies": out})
}

func (s *server) createPolicyObject(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	actor := principal.Actor
	var req policyObjectRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	policy, err := s.cfg.Accounts.CreatePolicyObject(
		req.Name, req.Mode, req.PermissionCeiling, req.AllowedUsers, req.AllowedGroups, actor)
	if err != nil {
		writePolicyObjectError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditPolicyObjectCreate,
		"created policy object "+policy.Name+" ("+policy.ID+")", true)
	writeJSON(w, http.StatusCreated, viewPolicyObject(policy))
}

func (s *server) policyObjectDetail(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	policy, err := s.cfg.Accounts.PolicyObject(r.PathValue("id"))
	if err != nil {
		writePolicyObjectError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, viewPolicyObject(policy))
}

func (s *server) updatePolicyObject(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	actor := principal.Actor
	var req policyObjectRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	policy, err := s.cfg.Accounts.UpdatePolicyObject(
		r.PathValue("id"), req.Name, req.Mode, req.PermissionCeiling,
		req.AllowedUsers, req.AllowedGroups, actor)
	if err != nil {
		writePolicyObjectError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditPolicyObjectUpdate,
		"updated policy object "+policy.Name+" to version "+
			strconv.FormatUint(uint64(policy.PolicyVersion), 10), true)
	s.reconcileAllPolicies()
	writeJSON(w, http.StatusOK, viewPolicyObject(policy))
}

func (s *server) deletePolicyObject(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	actor := principal.Actor
	id := r.PathValue("id")
	if err := s.cfg.Accounts.DeletePolicyObject(id); err != nil {
		writePolicyObjectError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditPolicyObjectDelete, "deleted policy object "+id, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) listPolicyAssignments(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	assignments, err := s.cfg.Accounts.PolicyAssignments()
	if err != nil {
		s.cfg.Logger.Error("list policy assignments", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read policy assignments")
		return
	}
	out := make([]policyAssignmentView, 0, len(assignments))
	for _, assignment := range assignments {
		out = append(out, viewPolicyAssignment(assignment))
	}
	writeJSON(w, http.StatusOK, map[string]any{"assignments": out})
}

func (s *server) createPolicyAssignment(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	actor := principal.Actor
	var req policyAssignmentRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	assignment, err := s.cfg.Accounts.CreatePolicyAssignment(
		req.PolicyID, req.ScopeKind, req.ScopeValue, req.Priority, actor)
	if err != nil {
		writePolicyObjectError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditPolicyAssignmentCreate,
		"assigned policy "+assignment.PolicyName+" to "+policyScopeLabel(assignment), true)
	s.reconcileAllPolicies()
	writeJSON(w, http.StatusCreated, viewPolicyAssignment(assignment))
}

func (s *server) deletePolicyAssignment(w http.ResponseWriter, r *http.Request) {
	principal, ok := s.adminPrincipal(w, r, account.TokenResourceDevices)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	actor := principal.Actor
	id := r.PathValue("id")
	if err := s.cfg.Accounts.DeletePolicyAssignment(id); err != nil {
		writePolicyObjectError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditPolicyAssignmentDelete, "deleted policy assignment "+id, true)
	s.reconcileAllPolicies()
	w.WriteHeader(http.StatusNoContent)
}

func policyScopeLabel(assignment account.PolicyAssignment) string {
	if assignment.ScopeKind == account.PolicyScopeAll {
		return "all devices"
	}
	return assignment.ScopeKind + " " + assignment.ScopeValue
}

// reconcilePolicies materialises assignment changes for the listed devices and
// pushes exactly the rows that changed. A failure is logged: the mutation
// already committed, and the next registration or reconciliation retries it.
func (s *server) reconcilePolicies(peers []string) {
	if s.cfg.Accounts == nil {
		return
	}
	changed, unmanaged, err := s.cfg.Accounts.ReconcileDevicePolicies(peers)
	if err != nil {
		s.cfg.Logger.Error("reconcile device policies", "devices", peers, "error", err)
		return
	}
	s.publishReconciledPolicies(changed, unmanaged)
}

func (s *server) reconcileAllPolicies() {
	if s.cfg.Accounts == nil {
		return
	}
	changed, unmanaged, err := s.cfg.Accounts.ReconcileAllDevicePolicies()
	if err != nil {
		s.cfg.Logger.Error("reconcile all device policies", "error", err)
		return
	}
	s.publishReconciledPolicies(changed, unmanaged)
}

func (s *server) publishReconciledPolicies(changed []account.DevicePolicy, unmanaged []string) {
	for i := range changed {
		s.syncRegistryPolicy(changed[i].PeerID)
	}
	for _, peerID := range unmanaged {
		s.syncRegistryPolicy(peerID)
	}
	if s.eventHub == nil {
		return
	}
	for i := range changed {
		policy := changed[i]
		s.eventHub.publishPolicy(policy.PeerID, &policy)
	}
	for _, peerID := range unmanaged {
		s.eventHub.publishPolicy(peerID, nil)
	}
}

func writePolicyObjectError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownPolicyObject),
		errors.Is(err, account.ErrUnknownPolicyAssignment),
		errors.Is(err, account.ErrUnknownPolicyScope):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrPolicyObjectExists),
		errors.Is(err, account.ErrPolicyAssignmentExists),
		errors.Is(err, account.ErrPolicyObjectInUse):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrInvalidPolicyObjectName),
		errors.Is(err, account.ErrInvalidPolicyScope),
		errors.Is(err, account.ErrInvalidPolicyPriority),
		errors.Is(err, account.ErrInvalidDevicePolicyMode),
		errors.Is(err, account.ErrDevicePolicyUser),
		errors.Is(err, account.ErrDevicePolicyGroup),
		errors.Is(err, account.ErrDevicePolicyTooMany):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrPolicyObjectQuota),
		errors.Is(err, account.ErrPolicyAssignmentQuota):
		writeError(w, http.StatusBadRequest, err.Error())
	default:
		writeError(w, http.StatusInternalServerError, "could not update policy objects")
	}
}
