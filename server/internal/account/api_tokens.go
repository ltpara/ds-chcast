package account

import (
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// APITokenPrefix marks automation credentials so authentication can route them
// without trying the interactive session table first. The rest of the string
// is a full-entropy random token; only its SHA-256 hash is stored.
const APITokenPrefix = "chast_api_"

// Token resources name the console surfaces a machine token may reach. The
// role (readonly/operator/admin) still orders read, write and admin actions
// inside each surface, so a token is always resource set × action ladder.
const (
	TokenResourceDevices    = "devices"
	TokenResourceAudit      = "audit"
	TokenResourceAccounts   = "accounts"
	TokenResourceGroups     = "groups"
	TokenResourceRecordings = "recordings"
	TokenResourceOps        = "ops"
	TokenResourceAll        = "all"
)

const (
	maxAPITokenName  = 64
	maxAPITokens     = 20
	minAPITokenTTL   = time.Minute
	maxAPITokenTTL   = 365 * 24 * time.Hour
	apiTokenUseWrite = time.Minute
)

var (
	ErrInvalidAPIToken      = errors.New("invalid API token")
	ErrInvalidAPITokenName  = errors.New("API token name is required")
	ErrInvalidAPITokenRole  = errors.New("API token role must be readonly, operator or admin")
	ErrInvalidTokenResource = errors.New("unknown API token resource")
	ErrEmptyTokenResources  = errors.New("API token needs at least one resource")
	ErrInvalidAPITokenTTL   = errors.New("API token lifetime must be between one minute and 365 days")
	ErrAPITokenOutranks     = errors.New("API token role exceeds the owner role")
	ErrAPITokenGroupScope   = errors.New("group-scoped tokens may only cover devices, recordings, audit and groups")
	ErrUnknownAPIToken      = errors.New("unknown API token")
	ErrAPITokenQuota        = errors.New("API token quota reached")
)

// APIToken is one automation credential. The secret is returned only at
// creation; list and audit paths see the metadata row.
type APIToken struct {
	ID         string    `json:"id"`
	Name       string    `json:"name"`
	Role       string    `json:"role"`
	Resources  []string  `json:"resources"`
	GroupID    string    `json:"group_id,omitempty"`
	CreatedAt  time.Time `json:"created_at"`
	ExpiresAt  time.Time `json:"expires_at"`
	LastUsedAt time.Time `json:"last_used_at,omitempty"`
	Revoked    bool      `json:"revoked,omitempty"`
}

// APITokenPrincipal is a successfully authenticated machine token plus its
// owner. The caller intersects the token role with the owner's current role.
type APITokenPrincipal struct {
	Token APIToken
	User  User
}

// ValidTokenResource reports whether resource names a console surface.
func ValidTokenResource(resource string) bool {
	switch resource {
	case TokenResourceDevices, TokenResourceAudit, TokenResourceAccounts,
		TokenResourceGroups, TokenResourceRecordings, TokenResourceOps,
		TokenResourceAll:
		return true
	}
	return false
}

func roleRank(role string) int {
	switch role {
	case RoleAdmin:
		return 3
	case RoleOperator:
		return 2
	case RoleReadonly:
		return 1
	}
	return 0
}

func normalizeTokenResources(resources []string) ([]string, error) {
	seen := make(map[string]bool, len(resources))
	out := make([]string, 0, len(resources))
	for _, raw := range resources {
		resource := strings.ToLower(strings.TrimSpace(raw))
		if !ValidTokenResource(resource) {
			return nil, ErrInvalidTokenResource
		}
		if seen[resource] {
			continue
		}
		seen[resource] = true
		out = append(out, resource)
	}
	if len(out) == 0 {
		return nil, ErrEmptyTokenResources
	}
	sort.Strings(out)
	return out, nil
}

func tokenGroupScopedResources(resources []string) bool {
	for _, resource := range resources {
		switch resource {
		case TokenResourceGroups, TokenResourceAudit, TokenResourceRecordings, TokenResourceDevices:
		default:
			return false
		}
	}
	return true
}

// CreateAPIToken mints one machine token for userID. The plaintext secret is
// returned once. The token role may not exceed the owner's current role, and a
// group-scoped token may only cover surfaces that have a group dimension.
func (s *Store) CreateAPIToken(userID, name, role string, resources []string, groupID string, ttl time.Duration) (APIToken, string, error) {
	name = normalizeText(name, maxAPITokenName)
	if name == "" {
		return APIToken{}, "", ErrInvalidAPITokenName
	}
	if roleRank(role) == 0 {
		return APIToken{}, "", ErrInvalidAPITokenRole
	}
	resources, err := normalizeTokenResources(resources)
	if err != nil {
		return APIToken{}, "", err
	}
	groupID = strings.TrimSpace(groupID)
	if groupID != "" {
		if !registry.ValidID(groupID) {
			return APIToken{}, "", ErrInvalidAccountGroupID
		}
		if !tokenGroupScopedResources(resources) {
			return APIToken{}, "", ErrAPITokenGroupScope
		}
	}
	if ttl < minAPITokenTTL || ttl > maxAPITokenTTL {
		return APIToken{}, "", ErrInvalidAPITokenTTL
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	var ownerRole string
	var banned bool
	if err := s.db.QueryRow(`SELECT role, banned FROM users WHERE id = ?`, userID).
		Scan(&ownerRole, &banned); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return APIToken{}, "", ErrUnknownUser
		}
		return APIToken{}, "", fmt.Errorf("account store %s: read token owner: %w", s.path, err)
	}
	if banned {
		return APIToken{}, "", ErrUnknownUser
	}
	if roleRank(role) > roleRank(ownerRole) {
		return APIToken{}, "", ErrAPITokenOutranks
	}
	if groupID != "" {
		var exists int
		if err := s.db.QueryRow(`SELECT 1 FROM account_groups WHERE id = ?`, groupID).Scan(&exists); err != nil {
			if errors.Is(err, sql.ErrNoRows) {
				return APIToken{}, "", ErrUnknownAccountGroup
			}
			return APIToken{}, "", fmt.Errorf("account store %s: read token group: %w", s.path, err)
		}
	}
	var count int
	if err := s.db.QueryRow(
		`SELECT COUNT(*) FROM api_tokens WHERE user_id = ? AND revoked_at = 0 AND expires_at > ?`,
		userID, unixMillis(s.now().UTC()),
	).Scan(&count); err != nil {
		return APIToken{}, "", fmt.Errorf("account store %s: count API tokens: %w", s.path, err)
	}
	if count >= maxAPITokens {
		return APIToken{}, "", ErrAPITokenQuota
	}
	secret, err := newToken()
	if err != nil {
		return APIToken{}, "", err
	}
	secret = APITokenPrefix + secret
	id, err := newRandomHex(sessionIDBytes)
	if err != nil {
		return APIToken{}, "", err
	}
	now := s.now().UTC()
	token := APIToken{
		ID:        id,
		Name:      name,
		Role:      role,
		Resources: resources,
		GroupID:   groupID,
		CreatedAt: now,
		ExpiresAt: now.Add(ttl),
	}
	if _, err := s.db.Exec(
		`INSERT INTO api_tokens
			(id, token_hash, user_id, name, role, resources, group_id, created_at, expires_at, revoked_at, last_used_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0)`,
		id, tokenHash(secret), userID, name, role, strings.Join(resources, ","), groupID,
		unixMillis(now), unixMillis(token.ExpiresAt),
	); err != nil {
		return APIToken{}, "", fmt.Errorf("account store %s: create API token: %w", s.path, err)
	}
	return token, secret, nil
}

// APITokens lists the caller's tokens, including expired and revoked rows so
// the console can explain why a credential stopped working.
func (s *Store) APITokens(userID string) ([]APIToken, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(
		`SELECT id, name, role, resources, group_id, created_at, expires_at, revoked_at, last_used_at
		 FROM api_tokens WHERE user_id = ? ORDER BY created_at DESC, id`,
		userID,
	)
	if err != nil {
		return nil, fmt.Errorf("account store %s: list API tokens: %w", s.path, err)
	}
	defer rows.Close()
	out := make([]APIToken, 0)
	for rows.Next() {
		token, err := scanAPIToken(rows)
		if err != nil {
			return nil, fmt.Errorf("account store %s: list API tokens: %w", s.path, err)
		}
		out = append(out, token)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("account store %s: list API tokens: %w", s.path, err)
	}
	return out, nil
}

func scanAPIToken(row rowScanner) (APIToken, error) {
	var (
		token                              APIToken
		resources                          string
		created, expires, revoked, lastUse int64
	)
	if err := row.Scan(
		&token.ID, &token.Name, &token.Role, &resources, &token.GroupID,
		&created, &expires, &revoked, &lastUse,
	); err != nil {
		return APIToken{}, err
	}
	for _, resource := range strings.Split(resources, ",") {
		if resource != "" {
			token.Resources = append(token.Resources, resource)
		}
	}
	token.CreatedAt = timeFromMillis(created)
	token.ExpiresAt = timeFromMillis(expires)
	token.Revoked = revoked != 0
	if lastUse > 0 {
		token.LastUsedAt = timeFromMillis(lastUse)
	}
	return token, nil
}

// RevokeAPIToken disables one token immediately. Revoking an already revoked
// token is harmless; only an unknown id is an error.
func (s *Store) RevokeAPIToken(userID, id string) error {
	id = strings.TrimSpace(id)
	if id == "" {
		return ErrUnknownAPIToken
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := unixMillis(s.now().UTC())
	res, err := s.db.Exec(
		`UPDATE api_tokens SET revoked_at = ? WHERE id = ? AND user_id = ? AND revoked_at = 0`,
		now, id, userID,
	)
	if err != nil {
		return fmt.Errorf("account store %s: revoke API token: %w", s.path, err)
	}
	if rows, err := res.RowsAffected(); err != nil {
		return fmt.Errorf("account store %s: revoke API token: %w", s.path, err)
	} else if rows > 0 {
		return nil
	}
	var exists int
	if err := s.db.QueryRow(
		`SELECT 1 FROM api_tokens WHERE id = ? AND user_id = ?`, id, userID,
	).Scan(&exists); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return ErrUnknownAPIToken
		}
		return fmt.Errorf("account store %s: revoke API token: %w", s.path, err)
	}
	return nil
}

// AuthenticateAPIToken resolves a machine secret to its owner. It rejects
// revoked and expired rows and banned owners. A token minted at a stronger
// role than its owner now holds is narrowed to the owner's current role, so a
// demotion never leaves an elevated credential behind.
func (s *Store) AuthenticateAPIToken(secret string) (APITokenPrincipal, error) {
	secret = strings.TrimSpace(secret)
	if !strings.HasPrefix(secret, APITokenPrefix) {
		return APITokenPrincipal{}, ErrInvalidAPIToken
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now().UTC()
	var (
		token                              APIToken
		resources                          string
		created, expires, revoked, lastUse int64
		user                               User
		createdUser                        int64
	)
	err := s.db.QueryRow(
		`SELECT t.id, t.name, t.role, t.resources, t.group_id, t.created_at, t.expires_at,
		        t.revoked_at, t.last_used_at,
		        u.id, u.username, u.role, u.created_at, u.banned, u.email, u.phone
		 FROM api_tokens t JOIN users u ON u.id = t.user_id
		 WHERE t.token_hash = ?`,
		tokenHash(secret),
	).Scan(
		&token.ID, &token.Name, &token.Role, &resources, &token.GroupID,
		&created, &expires, &revoked, &lastUse,
		&user.ID, &user.Username, &user.Role, &createdUser, &user.Banned, &user.Email, &user.Phone,
	)
	if errors.Is(err, sql.ErrNoRows) {
		return APITokenPrincipal{}, ErrInvalidAPIToken
	}
	if err != nil {
		return APITokenPrincipal{}, fmt.Errorf("account store %s: authenticate API token: %w", s.path, err)
	}
	if revoked != 0 || now.UnixMilli() >= expires || user.Banned {
		return APITokenPrincipal{}, ErrInvalidAPIToken
	}
	if roleRank(user.Role) < roleRank(token.Role) {
		// A demoted owner silently narrows every token it minted.
		token.Role = user.Role
	}
	for _, resource := range strings.Split(resources, ",") {
		if resource != "" {
			token.Resources = append(token.Resources, resource)
		}
	}
	token.CreatedAt = timeFromMillis(created)
	token.ExpiresAt = timeFromMillis(expires)
	if lastUse > 0 {
		token.LastUsedAt = timeFromMillis(lastUse)
	}
	user.CreatedAt = timeFromMillis(createdUser)
	if lastUse == 0 || now.Sub(timeFromMillis(lastUse)) >= apiTokenUseWrite {
		if _, err := s.db.Exec(
			`UPDATE api_tokens SET last_used_at = ? WHERE id = ?`, unixMillis(now), token.ID,
		); err != nil {
			return APITokenPrincipal{}, fmt.Errorf("account store %s: record API token use: %w", s.path, err)
		}
		lastUse = unixMillis(now)
		token.LastUsedAt = now
	}
	return APITokenPrincipal{Token: token, User: user}, nil
}

// HasAPITokenPrefix reports whether secret is routed to the token table.
func HasAPITokenPrefix(secret string) bool {
	return strings.HasPrefix(strings.TrimSpace(secret), APITokenPrefix)
}
