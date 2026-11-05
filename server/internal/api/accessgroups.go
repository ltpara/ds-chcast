package api

import (
	"errors"
	"net/http"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// accessGroupPermission orders the group-scoped surfaces. The global account
// roles do not grant group access by themselves: a member's group role is the
// scoped permission, while server-key and global admin callers may manage any
// group. This keeps device groups independent of the address-book ACL.
type accessGroupPermission int

const (
	accessGroupRead accessGroupPermission = iota
	accessGroupWrite
	accessGroupAdmin
)

func accessGroupRolePermission(role string) (accessGroupPermission, bool) {
	switch role {
	case account.GroupRoleAdmin:
		return accessGroupAdmin, true
	case account.GroupRoleWrite:
		return accessGroupWrite, true
	case account.GroupRoleReadonly:
		return accessGroupRead, true
	}
	return accessGroupRead, false
}

// accessGroupActor authenticates one group request. Server key and global
// admins bypass membership; every other caller needs a membership with at
// least the requested group role.
func (s *server) accessGroupActor(w http.ResponseWriter, r *http.Request, groupID string, need accessGroupPermission) (account.User, string, bool) {
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
	user, err := s.cfg.Accounts.Authenticate(token)
	if err != nil {
		writeError(w, http.StatusUnauthorized, "server key or account token required")
		return account.User{}, "", false
	}
	if perm, ok := rolePermission(user.Role); ok && perm >= consoleAdmin {
		return user, user.Username, true
	}
	role, member, err := s.cfg.Accounts.AccessGroupRole(groupID, user.ID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return account.User{}, "", false
	}
	if !member {
		writeError(w, http.StatusForbidden, "account is not a member of this access group")
		return account.User{}, "", false
	}
	perm, ok := accessGroupRolePermission(role)
	if !ok || perm < need {
		writeError(w, http.StatusForbidden, "group role does not allow this action")
		return account.User{}, "", false
	}
	return user, user.Username, true
}

func (s *server) writeAccessGroupError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrUnknownAccessGroup),
		errors.Is(err, account.ErrUnknownUser),
		errors.Is(err, account.ErrUnknownGroupDevice):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrAccessGroupExists),
		errors.Is(err, account.ErrLastGroupAdmin),
		errors.Is(err, account.ErrAccessGroupDeviceQuota),
		errors.Is(err, account.ErrAccessGroupMemberQuota),
		errors.Is(err, account.ErrAccessGroupQuotaBelow):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrInvalidAccessGroupName),
		errors.Is(err, account.ErrInvalidAccessGroupDescription),
		errors.Is(err, account.ErrInvalidAccessGroupQuota),
		errors.Is(err, account.ErrInvalidGroupRole),
		errors.Is(err, account.ErrInvalidGroupDeviceID):
		writeError(w, http.StatusBadRequest, err.Error())
	default:
		s.cfg.Logger.Error("access group", "error", err)
		writeError(w, http.StatusInternalServerError, "access group operation failed")
	}
}

type accessGroupListResponse struct {
	Groups []account.AccessGroup `json:"groups"`
}

type accessGroupCreateRequest struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	MaxDevices  int    `json:"max_devices"`
	MaxMembers  int    `json:"max_members"`
	// Admin optionally seeds another group admin. An account caller is always
	// added as admin itself; the server key has no account, so this is how it
	// hands a new group to its first operator.
	Admin string `json:"admin,omitempty"`
}

type accessGroupUpdateRequest struct {
	Name        *string `json:"name,omitempty"`
	Description *string `json:"description,omitempty"`
	MaxDevices  *int    `json:"max_devices,omitempty"`
	MaxMembers  *int    `json:"max_members,omitempty"`
}

type accessGroupMemberRequest struct {
	Role string `json:"role"`
}

type accessGroupMemberListResponse struct {
	Members []account.AccessGroupMember `json:"members"`
}

type accessGroupDeviceRequest struct {
	DeviceID string `json:"device_id"`
}

type accessGroupDeviceListResponse struct {
	Devices []account.AccessGroupDevice `json:"devices"`
}

type accessGroupDeviceResponse struct {
	account.AccessGroupDevice
	Added bool `json:"added"`
}

// listAccessGroups returns every group for server-key or global-admin callers
// and only the caller's memberships for everyone else.
func (s *server) listAccessGroups(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var (
		groups []account.AccessGroup
		err    error
	)
	if s.serverKeyAuthorized(r) {
		groups, err = s.cfg.Accounts.ListAccessGroups()
	} else {
		user, ok := s.accountUser(w, r)
		if !ok {
			return
		}
		if perm, ok := rolePermission(user.Role); ok && perm >= consoleAdmin {
			groups, err = s.cfg.Accounts.ListAccessGroups()
		} else {
			groups, err = s.cfg.Accounts.AccessGroupsForUser(user.ID)
		}
	}
	if err != nil {
		s.cfg.Logger.Error("list access groups", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list access groups")
		return
	}
	if groups == nil {
		groups = []account.AccessGroup{}
	}
	writeJSON(w, http.StatusOK, accessGroupListResponse{Groups: groups})
}

func (s *server) createAccessGroup(w http.ResponseWriter, r *http.Request) {
	user, actor, ok := s.consoleActor(w, r, consoleAdmin)
	if !ok {
		return
	}
	var req accessGroupCreateRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := s.cfg.Accounts.CreateAccessGroup(
		req.Name, req.Description, req.MaxDevices, req.MaxMembers, user.ID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	if admin := strings.TrimSpace(req.Admin); admin != "" && !strings.EqualFold(admin, actor) {
		if _, err := s.cfg.Accounts.SetAccessGroupMember(group.ID, admin, account.GroupRoleAdmin); err != nil {
			// The group was created; report the failed seed so the operator can
			// retry the membership instead of guessing whether the group exists.
			s.recordAudit(r, actor, account.AuditAccessGroupMember,
				"seed admin "+admin+" failed for "+group.Name, false)
			s.writeAccessGroupError(w, err)
			return
		}
		group, err = s.cfg.Accounts.AccessGroup(group.ID)
		if err != nil {
			s.writeAccessGroupError(w, err)
			return
		}
	}
	if user.ID != "" {
		group.Role = account.GroupRoleAdmin
	}
	s.recordAudit(r, actor, account.AuditAccessGroupCreate, "created "+group.Name, true)
	writeJSON(w, http.StatusCreated, group)
}

func (s *server) accessGroup(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	user, _, ok := s.accessGroupActor(w, r, groupID, accessGroupRead)
	if !ok {
		return
	}
	group, err := s.cfg.Accounts.AccessGroup(groupID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	s.annotateAccessGroupRole(&group, user)
	writeJSON(w, http.StatusOK, group)
}

func (s *server) annotateAccessGroupRole(group *account.AccessGroup, user account.User) {
	if group == nil || user.ID == "" {
		return
	}
	if role, member, err := s.cfg.Accounts.AccessGroupRole(group.ID, user.ID); err == nil && member {
		group.Role = role
	}
}

func (s *server) updateAccessGroup(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	user, actor, ok := s.accessGroupActor(w, r, groupID, accessGroupAdmin)
	if !ok {
		return
	}
	var req accessGroupUpdateRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	group, err := s.cfg.Accounts.UpdateAccessGroup(groupID, account.AccessGroupUpdate{
		Name:        req.Name,
		Description: req.Description,
		MaxDevices:  req.MaxDevices,
		MaxMembers:  req.MaxMembers,
	})
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	s.annotateAccessGroupRole(&group, user)
	s.recordAudit(r, actor, account.AuditAccessGroupUpdate, "updated "+group.Name, true)
	writeJSON(w, http.StatusOK, group)
}

func (s *server) deleteAccessGroup(w http.ResponseWriter, r *http.Request) {
	_, actor, ok := s.consoleActor(w, r, consoleAdmin)
	if !ok {
		return
	}
	groupID := r.PathValue("id")
	group, err := s.cfg.Accounts.AccessGroup(groupID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	if err := s.cfg.Accounts.DeleteAccessGroup(groupID); err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccessGroupDelete, "deleted "+group.Name, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accessGroupMembers(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.accessGroupActor(w, r, groupID, accessGroupRead); !ok {
		return
	}
	members, err := s.cfg.Accounts.ListAccessGroupMembers(groupID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	if members == nil {
		members = []account.AccessGroupMember{}
	}
	writeJSON(w, http.StatusOK, accessGroupMemberListResponse{Members: members})
}

func (s *server) setAccessGroupMember(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.accessGroupActor(w, r, groupID, accessGroupAdmin)
	if !ok {
		return
	}
	var req accessGroupMemberRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	username := strings.TrimSpace(r.PathValue("username"))
	member, err := s.cfg.Accounts.SetAccessGroupMember(groupID, username, req.Role)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccessGroupMember,
		"role="+member.Role+" for "+member.Username+" in group "+groupID, true)
	writeJSON(w, http.StatusOK, member)
}

func (s *server) removeAccessGroupMember(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.accessGroupActor(w, r, groupID, accessGroupAdmin)
	if !ok {
		return
	}
	username := strings.TrimSpace(r.PathValue("username"))
	if err := s.cfg.Accounts.RemoveAccessGroupMember(groupID, username); err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccessGroupMember,
		"removed "+username+" from group "+groupID, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accessGroupDevices(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.accessGroupActor(w, r, groupID, accessGroupRead); !ok {
		return
	}
	devices, err := s.cfg.Accounts.ListAccessGroupDevices(groupID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	if devices == nil {
		devices = []account.AccessGroupDevice{}
	}
	writeJSON(w, http.StatusOK, accessGroupDeviceListResponse{Devices: devices})
}

func (s *server) addAccessGroupDevice(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.accessGroupActor(w, r, groupID, accessGroupWrite)
	if !ok {
		return
	}
	var req accessGroupDeviceRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	device, added, err := s.cfg.Accounts.AddAccessGroupDevice(groupID, req.DeviceID)
	if err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	status := http.StatusOK
	if added {
		status = http.StatusCreated
		s.recordAudit(r, actor, account.AuditAccessGroupDevice,
			"added "+device.DeviceID+" to group "+groupID, true)
	}
	writeJSON(w, status, accessGroupDeviceResponse{AccessGroupDevice: device, Added: added})
}

func (s *server) removeAccessGroupDevice(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	_, actor, ok := s.accessGroupActor(w, r, groupID, accessGroupWrite)
	if !ok {
		return
	}
	deviceID := r.PathValue("device")
	if err := s.cfg.Accounts.RemoveAccessGroupDevice(groupID, deviceID); err != nil {
		s.writeAccessGroupError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAccessGroupDevice,
		"removed "+deviceID+" from group "+groupID, true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) accessGroupConnections(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.accessGroupActor(w, r, groupID, accessGroupRead); !ok {
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	beforeID, ok := s.listBeforeID(w, r)
	if !ok {
		return
	}
	events, err := s.cfg.Accounts.ConnectionsForGroupBefore(groupID, beforeID, limit)
	if err != nil {
		s.cfg.Logger.Error("access group connection query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the connection log")
		return
	}
	out := connectionsResponse{Connections: make([]connectionView, 0, len(events))}
	for _, event := range events {
		out.Connections = append(out.Connections, connectionToView(event))
	}
	if len(out.Connections) > 0 {
		out.NextBeforeID = nextBeforeID(out.Connections[len(out.Connections)-1].ID,
			len(out.Connections), limit)
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *server) accessGroupFileTransfers(w http.ResponseWriter, r *http.Request) {
	groupID := r.PathValue("id")
	if _, _, ok := s.accessGroupActor(w, r, groupID, accessGroupRead); !ok {
		return
	}
	limit, ok := s.connectionLimit(w, r)
	if !ok {
		return
	}
	beforeID, ok := s.listBeforeID(w, r)
	if !ok {
		return
	}
	events, err := s.cfg.Accounts.FileTransfersForGroupBefore(groupID, beforeID, limit)
	if err != nil {
		s.cfg.Logger.Error("access group file query", "error", err)
		writeError(w, http.StatusInternalServerError, "could not read the file transfer log")
		return
	}
	out := fileTransfersResponse{Transfers: make([]fileTransferView, 0, len(events))}
	for _, event := range events {
		out.Transfers = append(out.Transfers, fileTransferToView(event))
	}
	if len(out.Transfers) > 0 {
		out.NextBeforeID = nextBeforeID(out.Transfers[len(out.Transfers)-1].ID,
			len(out.Transfers), limit)
	}
	writeJSON(w, http.StatusOK, out)
}
