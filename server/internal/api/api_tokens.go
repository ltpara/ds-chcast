package api

import (
	"errors"
	"fmt"
	"net/http"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

const defaultAPITokenTTL = 90 * 24 * time.Hour

type apiTokenView struct {
	ID           string   `json:"id"`
	Name         string   `json:"name"`
	Role         string   `json:"role"`
	Resources    []string `json:"resources"`
	GroupID      string   `json:"group_id,omitempty"`
	CreatedAtMS  int64    `json:"created_at_ms"`
	ExpiresAtMS  int64    `json:"expires_at_ms"`
	LastUsedAtMS int64    `json:"last_used_at_ms,omitempty"`
	Revoked      bool     `json:"revoked,omitempty"`
}

type apiTokenListResponse struct {
	Tokens []apiTokenView `json:"tokens"`
}

type apiTokenCreateRequest struct {
	Name             string   `json:"name"`
	Role             string   `json:"role"`
	Resources        []string `json:"resources"`
	GroupID          string   `json:"group_id"`
	ExpiresInSeconds int64    `json:"expires_in_seconds"`
}

type apiTokenCreateResponse struct {
	Token  apiTokenView `json:"token"`
	Secret string       `json:"secret"`
}

func apiTokenViewOf(token account.APIToken) apiTokenView {
	view := apiTokenView{
		ID:          token.ID,
		Name:        token.Name,
		Role:        token.Role,
		Resources:   token.Resources,
		GroupID:     token.GroupID,
		CreatedAtMS: token.CreatedAt.UnixMilli(),
		ExpiresAtMS: token.ExpiresAt.UnixMilli(),
		Revoked:     token.Revoked,
	}
	if !token.LastUsedAt.IsZero() {
		view.LastUsedAtMS = token.LastUsedAt.UnixMilli()
	}
	return view
}

// tokenOwner rejects the server key, which has no account to own a token.
func tokenOwner(w http.ResponseWriter, user account.User) bool {
	if user.ID == "" {
		writeError(w, http.StatusBadRequest, "API tokens must be owned by an account")
		return false
	}
	return true
}

func writeAPITokenError(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, account.ErrAPITokenOutranks):
		writeError(w, http.StatusForbidden, err.Error())
	case errors.Is(err, account.ErrAPITokenQuota):
		writeError(w, http.StatusConflict, err.Error())
	case errors.Is(err, account.ErrUnknownAPIToken):
		writeError(w, http.StatusNotFound, err.Error())
	case errors.Is(err, account.ErrInvalidAPITokenName),
		errors.Is(err, account.ErrInvalidAPITokenRole),
		errors.Is(err, account.ErrInvalidTokenResource),
		errors.Is(err, account.ErrEmptyTokenResources),
		errors.Is(err, account.ErrInvalidAPITokenTTL),
		errors.Is(err, account.ErrAPITokenGroupScope),
		errors.Is(err, account.ErrInvalidAccountGroupID),
		errors.Is(err, account.ErrUnknownAccountGroup),
		errors.Is(err, account.ErrUnknownUser):
		writeError(w, http.StatusBadRequest, err.Error())
	default:
		writeError(w, http.StatusInternalServerError, "could not manage API tokens")
	}
}

func (s *server) listAPITokens(w http.ResponseWriter, r *http.Request) {
	user, _, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok || !tokenOwner(w, user) {
		return
	}
	tokens, err := s.cfg.Accounts.APITokens(user.ID)
	if err != nil {
		s.cfg.Logger.Error("list API tokens", "error", err)
		writeError(w, http.StatusInternalServerError, "could not list API tokens")
		return
	}
	resp := apiTokenListResponse{Tokens: make([]apiTokenView, 0, len(tokens))}
	for _, token := range tokens {
		resp.Tokens = append(resp.Tokens, apiTokenViewOf(token))
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *server) createAPIToken(w http.ResponseWriter, r *http.Request) {
	user, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok || !tokenOwner(w, user) {
		return
	}
	var req apiTokenCreateRequest
	if err := decodeBody(w, r, &req); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	ttl := defaultAPITokenTTL
	if req.ExpiresInSeconds != 0 {
		ttl = time.Duration(req.ExpiresInSeconds) * time.Second
	}
	token, secret, err := s.cfg.Accounts.CreateAPIToken(
		user.ID, req.Name, req.Role, req.Resources, req.GroupID, ttl,
	)
	if err != nil {
		writeAPITokenError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAPITokenCreate, fmt.Sprintf(
		"created token %s name=%s role=%s resources=%v group=%s expires=%s",
		token.ID, token.Name, token.Role, token.Resources, token.GroupID,
		token.ExpiresAt.UTC().Format(time.RFC3339),
	), true)
	writeJSON(w, http.StatusCreated, apiTokenCreateResponse{
		Token:  apiTokenViewOf(token),
		Secret: secret,
	})
}

func (s *server) revokeAPIToken(w http.ResponseWriter, r *http.Request) {
	user, actor, ok := s.adminActor(w, r, account.TokenResourceAccounts)
	if !ok || !tokenOwner(w, user) {
		return
	}
	id := r.PathValue("id")
	if err := s.cfg.Accounts.RevokeAPIToken(user.ID, id); err != nil {
		writeAPITokenError(w, err)
		return
	}
	s.recordAudit(r, actor, account.AuditAPITokenRevoke, "revoked token "+id, true)
	w.WriteHeader(http.StatusNoContent)
}
