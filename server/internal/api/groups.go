package api

import (
	"errors"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type accountGroupView struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Description string `json:"description,omitempty"`
	MemberQuota int    `json:"member_quota"`
	DeviceQuota int    `json:"device_quota"`
	MemberCount int    `json:"member_count"`
	DeviceCount int    `json:"device_count"`
	Role        string `json:"role"`
	CreatedAtMS int64  `json:"created_at_ms"`
	UpdatedAtMS int64  `json:"updated_at_ms"`
}

type accountGroupListResponse struct {
	Groups []accountGroupView `json:"groups"`
	// GlobalRole is the caller's console role, empty for an account that only
	// reaches the console through group memberships.
	GlobalRole string `json:"global_role,omitempty"`
}

type accountGroupMemberView struct {
	UserID      string `json:"user_id"`
	Username    string `json:"username"`
	Role        string `json:"role"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type accountGroupDeviceView struct {
	PeerID      string `json:"peer_id"`
	AddedBy     string `json:"added_by,omitempty"`
	CreatedAtMS int64  `json:"created_at_ms"`
}

type accountGroupDetailResponse struct {
	Group   accountGroupView         `json:"group"`
	Members []accountGroupMemberView `json:"members"`
	Devices []accountGroupDeviceView `json:"devices"`
}

type accountGroupCreateRequest struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	Description string `json:"description"`
	MemberQuota int    `json:"member_quota"`
	DeviceQuota int    `json:"device_quota"`
}

type accountGroupUpdateRequest struct {
	Name        *string `json:"name"`
	Description *string `json:"description"`
	MemberQuota *int    `json:"member_quota"`
	DeviceQuota *int    `json:"device_quota"`
}

type accountGroupMemberRequest struct {
	Username string `json:"username"`
	Role     string `json:"role"`
}

type accountGroupMemberRoleRequest struct {
	Role string `json:"role"`
}

type accountGroupDeviceRequest struct {
	PeerID string `json:"peer_id"`
}

func permissionRole(perm consolePermission) string {
	switch {
	case perm >= consoleAdmin:
		return account.RoleAdmin
	case perm >= consoleWrite:
		return account.RoleOperator
	}
	return account.RoleReadonly
}

func maxPermission(a, b consolePermission) consolePermission {
	if a > b {
		return a
	}
	return b
}

func groupView(group account.AccountGroup, role string) accountGroupView {
	return accountGroupView{
		ID:          group.ID,
		Name:        group.Name,
		Description: group.Description,
		MemberQuota: group.MemberQuota,
		DeviceQuota: group.DeviceQuota,
		MemberCount: group.MemberCount,
		DeviceCount: group.DeviceCount,
		Role:        role,
		CreatedAtMS: group.CreatedAt.UnixMilli(),
		UpdatedAtMS: group.UpdatedAt.UnixMilli(),
	}
}

// groupActor authorizes one action against one group. The server key outranks
// everything; a console account uses the stronger of its global role and its
// membership role in this group, so a global operator may write any group and
// a viewer account promoted to group admin may manage that group alone. A
// machine token uses its own role, must cover the resource, and a group-scoped
// token only reaches its own group.
func (s *server) groupActor(w http.ResponseWriter, r *http.Request, groupID string, need consolePermission, resource string) (account.User, string, bool) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return account.User{}, "", false
	}
	if s.serverKeyAuthorized(r) {
		return account.User{}, "server-key", true
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "server key or account token required")
		return account.User{}, "", false
	}
	if account.HasAPITokenPrefix(token) {
		principal, err := s.cfg.Accounts.AuthenticateAPIToken(token)
		if err != nil {
			writeError(w, http.StatusUnauthorized, "server key or account token required")
			return account.User{}, "", false
		}
		if principal.Token.GroupID != "" && principal.Token.GroupID != groupID {
			writeError(w, http.StatusForbidden, "API token is limited to another group")
			return account.User{}, "", false
		}
		perm, ok := rolePermission(principal.Token.Role)
		if !ok || perm < need || !tokenCoversResource(principal.Token.Resources, resource) {
			writeError(w, http.StatusForbidden, "API token does not allow this action")
			return account.User{}, "", false
		}
		return principal.User, principal.User.Username, true
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or account token required")
		return account.User{}, "", false
	}
	effective := consolePermission(-1)
	if perm, ok := rolePermission(user.Role); ok {
		effective = perm
	}
	role, member, err := s.cfg.Accounts.GroupRole(groupID, user.ID)
	if err != nil {
		writeGroupError(w, err)
		return account.User{}, "", false
	}
	if member {
		if perm, ok := rolePermission(role); ok {
			effective = maxPermission(effective, perm)
		}
	}
	if effective < need {
		writeError(w, http.StatusForbidden, "group role does not allow this action")
		return account.User{}, "", false
	}
	return user, user.Username, true
}

// auditGroupFilter authorizes the optional group query parameter used by the
// audit endpoints. An account with no console role may still read the trail of
// a group it belongs to; the unfiltered endpoint keeps its stricter rule.
func (s *server) auditGroupFilter(w http.ResponseWriter, r *http.Request) (string, bool) {
	groupID := strings.TrimSpace(r.URL.Query().Get("group"))
	if groupID == "" {
		return "", true
	}
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return "", false
	}
	if s.serverKeyAuthorized(r) {
		return groupID, true
	}
	token, ok := bearerToken(r)
	if !ok {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return "", false
	}
	if account.HasAPITokenPrefix(token) {
		principal, err := s.cfg.Accounts.AuthenticateAPIToken(token)
		if err != nil {
			writeError(w, http.StatusUnauthorized, "server key or operator account token required")
			return "", false
		}
		if !tokenCoversResource(principal.Token.Resources, account.TokenResourceAudit) {
			writeError(w, http.StatusForbidden, "API token does not cover this resource")
			return "", false
		}
		if principal.Token.GroupID != "" {
			if groupID != "" && groupID != principal.Token.GroupID {
				writeError(w, http.StatusForbidden, "API token is limited to another group")
				return "", false
			}
			// A group-scoped token never reads the unfiltered trail.
			return principal.Token.GroupID, true
		}
		return groupID, true
	}
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or operator account token required")
		return "", false
	}
	if perm, ok := rolePermission(user.Role); ok && perm >= consoleRead {
		return groupID, true
	}
	if _, member, err := s.cfg.Accounts.GroupRole(groupID, user.ID); err != nil {
		writeGroupError(w, err)
		return "", false
	} else if member {
		return groupID, true
	}
	writeError(w, http.StatusForbidden, "group membership is required")
	return "", false
}

// listAccountGroups shows every group to a console role and only the caller's
// memberships to an account without one.
func (s *server) listAccountGroups(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var (
		user        account.User
		globalPerm  = consolePermission(-1)
		groupFilter string
		apiToken    bool
	)
	if s.serverKeyAuthorized(r) {
		globalPerm = consoleAdmin
	} else {
		token, ok := bearerToken(r)
		if !ok {
			writeError(w, http.StatusUnauthorized, "server key or account token required")
			return
		}
		if account.HasAPITokenPrefix(token) {
			principal, err := s.cfg.Accounts.AuthenticateAPIToken(token)
			if err != nil {
				writeError(w, http.StatusUnauthorized, "server key or account token required")
				return
			}
			if !tokenCoversResource(principal.Token.Resources, account.TokenResourceGroups) {
				writeError(w, http.StatusForbidden, "API token does not cover this resource")
				return
			}
			user = principal.User
			groupFilter = principal.Token.GroupID
			apiToken = true
			if perm, ok := rolePermission(principal.Token.Role); ok {
				globalPerm = perm
			}
		} else {
			var err error
			user, err = s.cfg.Accounts.Authenticate(token)
			if err != nil {
				writeError(w, http.StatusUnauthorized, "server key or account token required")
				return
			}
			if perm, ok := rolePermission(user.Role); ok {
				globalPerm = perm
			}
		}
	}
	memberships := make(map[string]string)
	if user.ID != "" && !apiToken {
		access, err := s.cfg.Accounts.GroupsForUser(user.ID)
		if err != nil {
			s.cfg.Logger.Error("list account groups", "error", err)
			writeError(w, http.StatusInternalServerError, "could not list groups")
			return
		}
		for _, group := range access {
			memberships[group.ID] = group.Role
		}
	}
	var groups []account.AccountGroup
	if globalPerm >= consoleRead {
		var err error
		groups, err = s.cfg.Accounts.Groups()
		if err != nil {
			s.cfg.Logger.Error("list account groups", "error", err)
			writeError(w, http.StatusInternalServerError, "could not list groups")
			return
		}
	} else {
		access, err := s.cfg.Accounts.GroupsForUser(user.ID)
		if err != nil {
			s.cfg.Logger.Error("list account groups", "error", err)
			writeError(w, http.StatusInternalServerError, "could not list groups")
			return
		}
		groups = make([]account.AccountGroup, 0, len(access))
		for _, entry := range access {
			groups = append(groups, entry.AccountGroup)
		}
	}
	resp := accountGroupListResponse{Groups: make([]accountGroupView, 0, len(groups))}
	if globalPerm >= consoleRead {
		resp.GlobalRole = permissionRole(globalPerm)
	}
	for _, group := range groups {
		if groupFilter != "" && group.ID != groupFilter {
			continue
		}
		effective := globalPerm
		if role, ok := memberships[group.ID]; ok {
			if perm, ok := rolePermission(role); ok {
				effective = maxPermission(effective, perm)
			}
		}
		if effective < consoleRead {
			continue
		}
		resp.Groups = append(resp.Groups, groupView(group, permissionRole(effective)))
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) createAccountGroup(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.consoleActor(w, r, consoleAdmin, account.TokenResourceGroups)
	if !ok {
		return
	}
	var req accountGroupCreateRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := s.cfg.Accounts.CreateGroup(req.ID, req.Name, req.Description,
		req.MemberQuota, req.DeviceQuota)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupCreate,
		"created group "+group.ID+" name="+group.Name, true)
	writeJSON(w, http.StatusCreated, groupView(group, account.RoleAdmin))
}

func (s *server) accountGroupDetail(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.groupActor(w, r, groupID, consoleRead, account.TokenResourceGroups); !ok {
		return
	}
	s.writeAccountGroupDetail(w, groupID)
}

func (s *server) writeAccountGroupDetail(w http.ResponseWriter, groupID string) {
	group, err := s.cfg.Accounts.AccountGroupByID(groupID)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	members, err := s.cfg.Accounts.GroupMembers(groupID)
	if err != nil {
		s.cfg.Logger.Error("list group members", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read group members")
		return
	}
	devices, err := s.cfg.Accounts.GroupDevices(groupID)
	if err != nil {
		s.cfg.Logger.Error("list group devices", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read group devices")
		return
	}
	resp := accountGroupDetailResponse{
		Group:   groupView(group, ""),
		Members: make([]accountGroupMemberView, 0, len(members)),
		Devices: make([]accountGroupDeviceView, 0, len(devices)),
	}
	for _, member := range members {
		resp.Members = append(resp.Members, accountGroupMemberView{
			UserID:      member.UserID,
			Username:    member.Username,
			Role:        member.Role,
			CreatedAtMS: member.CreatedAt.UnixMilli(),
		})
	}
	for _, device := range devices {
		resp.Devices = append(resp.Devices, accountGroupDeviceView{
			PeerID:      device.PeerID,
			AddedBy:     device.AddedBy,
			CreatedAtMS: device.CreatedAt.UnixMilli(),
		})
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) updateAccountGroup(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.groupActor(w, r, groupID, consoleAdmin, account.TokenResourceGroups)
	if !ok {
		return
	}
	var req accountGroupUpdateRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := s.cfg.Accounts.UpdateAccountGroup(groupID,
		req.Name, req.Description, req.MemberQuota, req.DeviceQuota)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupUpdate, "updated group "+group.ID, true)
	writeJSON(w, http.StatusOK, groupView(group, account.RoleAdmin))
}

func (s *server) deleteAccountGroup(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.groupActor(w, r, groupID, consoleAdmin, account.TokenResourceGroups)
	if !ok {
		return
	}
	if err := s.cfg.Accounts.DeleteAccountGroup(groupID); err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupDelete, "deleted group "+groupID, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accountGroupMembers(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.groupActor(w, r, groupID, consoleRead, account.TokenResourceGroups); !ok {
		return
	}
	members, err := s.cfg.Accounts.GroupMembers(groupID)
	if err != nil {
		s.cfg.Logger.Error("list group members", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read group members")
		return
	}
	out := make([]accountGroupMemberView, 0, len(members))
	for _, member := range members {
		out = append(out, accountGroupMemberView{
			UserID:      member.UserID,
			Username:    member.Username,
			Role:        member.Role,
			CreatedAtMS: member.CreatedAt.UnixMilli(),
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"members": out})
}

func (s *server) addAccountGroupMember(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.groupActor(w, r, groupID, consoleAdmin, account.TokenResourceGroups)
	if !ok {
		return
	}
	var req accountGroupMemberRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	member, err := s.cfg.Accounts.AddGroupMember(groupID, req.Username, req.Role)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupMemberAdd,
		"added "+member.Username+" role="+member.Role+" to "+groupID, true)
	writeJSON(w, http.StatusCreated, accountGroupMemberView{
		UserID:      member.UserID,
		Username:    member.Username,
		Role:        member.Role,
		CreatedAtMS: member.CreatedAt.UnixMilli(),
	})
}

func (s *server) updateAccountGroupMember(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.groupActor(w, r, groupID, consoleAdmin, account.TokenResourceGroups)
	if !ok {
		return
	}
	var req accountGroupMemberRoleRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := r.PathValue("username")
	member, err := s.cfg.Accounts.UpdateGroupMember(groupID, username, req.Role)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupMemberUpdate,
		"set "+member.Username+" role="+member.Role+" in "+groupID, true)
	writeJSON(w, http.StatusOK, accountGroupMemberView{
		UserID:      member.UserID,
		Username:    member.Username,
		Role:        member.Role,
		CreatedAtMS: member.CreatedAt.UnixMilli(),
	})
}

func (s *server) removeAccountGroupMember(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	username := r.PathValue("username")
	_, actor, ok := s.groupActor(w, r, groupID, consoleAdmin, account.TokenResourceGroups)
	if !ok {
		return
	}
	if err := s.cfg.Accounts.RemoveGroupMember(groupID, username); err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupMemberRemove,
		"removed "+username+" from "+groupID, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accountGroupDevices(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.groupActor(w, r, groupID, consoleRead, account.TokenResourceDevices); !ok {
		return
	}
	devices, err := s.cfg.Accounts.GroupDevices(groupID)
	if err != nil {
		s.cfg.Logger.Error("list group devices", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read group devices")
		return
	}
	out := make([]accountGroupDeviceView, 0, len(devices))
	for _, device := range devices {
		out = append(out, accountGroupDeviceView{
			PeerID:      device.PeerID,
			AddedBy:     device.AddedBy,
			CreatedAtMS: device.CreatedAt.UnixMilli(),
		})
	}
	writeJSON(w, http.StatusOK, map[string]any{"devices": out})
}

func (s *server) addAccountGroupDevice(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.groupActor(w, r, groupID, consoleWrite, account.TokenResourceDevices)
	if !ok {
		return
	}
	var req accountGroupDeviceRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	device, err := s.cfg.Accounts.AddGroupDevice(groupID, req.PeerID, actor)
	if err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupDeviceAdd,
		"added device "+device.PeerID+" to "+groupID, true)
	writeJSON(w, http.StatusCreated, accountGroupDeviceView{
		PeerID:      device.PeerID,
		AddedBy:     device.AddedBy,
		CreatedAtMS: device.CreatedAt.UnixMilli(),
	})
}

func (s *server) removeAccountGroupDevice(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	peerID := r.PathValue("peer")
	_, actor, ok := s.groupActor(w, r, groupID, consoleWrite, account.TokenResourceDevices)
	if !ok {
		return
	}
	if err := s.cfg.Accounts.RemoveGroupDevice(groupID, peerID); err != nil {
		writeGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditGroupDeviceRemove,
		"removed device "+peerID+" from "+groupID, true)
	w.WriteHeader(http.StatusNoContent)
}

func writeGroupError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownAccountGroup),
		errors.Is(err, account.ErrUnknownGroupMember),
		errors.Is(err, account.ErrUnknownGroupDevice),
		errors.Is(err, account.ErrUnknownUser):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrInvalidAccountGroupID),
		errors.Is(err, account.ErrInvalidAccountGroupName),
		errors.Is(err, account.ErrInvalidGroupRole),
		errors.Is(err, account.ErrInvalidGroupQuota),
		errors.Is(err, account.ErrInvalidPeerID):
		writeError(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, account.ErrAccountGroupExists),
		errors.Is(err, account.ErrGroupMemberExists),
		errors.Is(err, account.ErrGroupDeviceExists),
		errors.Is(err, account.ErrGroupMemberQuota),
		errors.Is(err, account.ErrGroupDeviceQuota),
		errors.Is(err, account.ErrGroupQuotaTooSmall),
		errors.Is(err, account.ErrLastGroupAdmin):
		writeError(w, http.StatusConflict, err.Error())
	default:
		writeError(w, http.StatusInternalServerError, "could not complete the group change")
	}
}
