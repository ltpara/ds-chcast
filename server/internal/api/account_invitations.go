package api

import (
	"net/http"
	"net/url"
	"strconv"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type createAccountInvitationRequest struct {
	Email string `json:"email"`
}

type acceptAccountInvitationRequest struct {
	Token    string `json:"token"`
	Username string `json:"username"`
	Password string `json:"password"`
}

type accountInvitationReply struct {
	ID               int64  `json:"id"`
	Email            string `json:"email"`
	InvitedBy        string `json:"invited_by"`
	Status           string `json:"status"`
	CreatedAtMS      int64  `json:"created_at_ms"`
	ExpiresAtMS      int64  `json:"expires_at_ms"`
	AcceptedAtMS     int64  `json:"accepted_at_ms,omitempty"`
	AcceptedUsername string `json:"accepted_username,omitempty"`
}

type accountInvitationsReply struct {
	Invitations []accountInvitationReply `json:"invitations"`
}

type acceptedAccountInvitationReply struct {
	Username      string `json:"username"`
	Email         string `json:"email"`
	EmailVerified bool   `json:"email_verified"`
}

func viewAccountInvitation(invitation account.AccountInvitation) accountInvitationReply {
	reply := accountInvitationReply{
		ID: invitation.ID, Email: invitation.Email, InvitedBy: invitation.InvitedBy,
		Status: invitation.Status, CreatedAtMS: invitation.CreatedAt.UnixMilli(),
		ExpiresAtMS:      invitation.ExpiresAt.UnixMilli(),
		AcceptedUsername: invitation.AcceptedUsername,
	}
	if !invitation.AcceptedAt.IsZero() {
		reply.AcceptedAtMS = invitation.AcceptedAt.UnixMilli()
	}
	return reply
}

func (s *server) adminListAccountInvitations(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	principal, ok := s.consolePrincipalFor(w, r, account.PermissionAccountsRead, account.TokenResourceAccounts)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	invitations, err := s.cfg.Accounts.ListAccountInvitations(200)
	if err != nil {
		s.cfg.Logger.Error("list account invitations", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list account invitations")
		return
	}
	reply := accountInvitationsReply{Invitations: make([]accountInvitationReply, 0, len(invitations))}
	for _, invitation := range invitations {
		reply.Invitations = append(reply.Invitations, viewAccountInvitation(invitation))
	}
	writeJSON(w, http.StatusOK, reply)
}

func (s *server) adminCreateAccountInvitation(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	principal, ok := s.consolePrincipalFor(w, r, account.PermissionAccountsWrite, account.TokenResourceAccounts)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	if s.cfg.Mailer == nil {
		writeError(w, http.StatusServiceUnavailable, "email delivery is not configured")
		return
	}
	var req createAccountInvitationRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	issued, err := s.cfg.Accounts.CreateAccountInvitation(req.Email, principal.Actor, s.cfg.InvitationTTL)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	link := ""
	if strings.TrimSpace(s.cfg.InvitationURL) != "" {
		link = accountInvitationLink(s.cfg.InvitationURL, issued.Token)
	}
	if err := s.cfg.Mailer.SendAccountInvitation(issued.Invitation.Email, issued.Token, link, s.cfg.InvitationTTL); err != nil {
		_ = s.cfg.Accounts.CancelAccountInvitation(issued.Invitation.ID)
		s.cfg.Logger.Warn("account invitation email failed", "invitation_id", issued.Invitation.ID, "error", err)
		s.recordAudit(r, principal.Actor, account.AuditAccountInvite, "invitation email failed", false)
		writeError(w, http.StatusBadGateway, "could not send the account invitation")
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditAccountInvite, "invitation created", true)
	writeJSON(w, http.StatusAccepted, viewAccountInvitation(issued.Invitation))
}

func (s *server) adminCancelAccountInvitation(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	principal, ok := s.consolePrincipalFor(w, r, account.PermissionAccountsWrite, account.TokenResourceAccounts)
	if !ok || !requireUnscoped(w, principal) {
		return
	}
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err != nil || id <= 0 {
		writeError(w, http.StatusBadRequest, "invitation id must be a positive integer")
		return
	}
	if err := s.cfg.Accounts.CancelAccountInvitation(id); err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, principal.Actor, account.AuditAccountInvite, "invitation cancelled", true)
	w.WriteHeader(http.StatusNoContent)
}

func (s *server) acceptAccountInvitation(w http.ResponseWriter, r *http.Request) {
	if s.cfg.Accounts == nil {
		writeError(w, http.StatusServiceUnavailable, accountUnavailable)
		return
	}
	var req acceptAccountInvitationRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	user, err := s.cfg.Accounts.AcceptAccountInvitation(req.Token, req.Username, req.Password)
	if err != nil {
		writeAccountError(w, err)
		return
	}
	s.recordAudit(r, user.Username, account.AuditAccountInvite, "invitation accepted", true)
	writeJSON(w, http.StatusCreated, acceptedAccountInvitationReply{
		Username: user.Username, Email: user.Email, EmailVerified: true,
	})
}

func accountInvitationLink(base, token string) string {
	page, err := url.Parse(strings.TrimSpace(base))
	if err != nil {
		return ""
	}
	fragment := url.Values{}
	fragment.Set("token", token)
	page.Fragment = fragment.Encode()
	return page.String()
}
