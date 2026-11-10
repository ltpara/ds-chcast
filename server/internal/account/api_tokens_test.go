package account

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func openTokenStore(t *testing.T) (*Store, User) {
	t.Helper()
	store, err := Open(filepath.Join(t.TempDir(), "tokens.db"))
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	alice, err := store.CreateUser("alice", "correct horse battery")
	if err != nil {
		t.Fatalf("create admin: %v", err)
	}
	return store, alice
}

func TestAPITokenLifecycle(t *testing.T) {
	store, alice := openTokenStore(t)

	token, secret, err := store.CreateAPIToken(alice.ID, "backup job", RoleOperator,
		[]string{TokenResourceDevices, TokenResourceAudit, TokenResourceDevices}, "", 24*time.Hour)
	if err != nil {
		t.Fatalf("create token: %v", err)
	}
	if !strings.HasPrefix(secret, APITokenPrefix) {
		t.Fatalf("secret %q lacks the API prefix", secret)
	}
	if token.Role != RoleOperator {
		t.Fatalf("role = %q", token.Role)
	}
	if strings.Join(token.Resources, ",") != "audit,devices" {
		t.Fatalf("resources = %v, want sorted and deduplicated", token.Resources)
	}

	var stored string
	if err := store.db.QueryRow(`SELECT token_hash FROM api_tokens WHERE id = ?`, token.ID).Scan(&stored); err != nil {
		t.Fatalf("read stored hash: %v", err)
	}
	if strings.Contains(stored, secret) || stored != tokenHash(secret) {
		t.Fatalf("stored token is not a plain SHA-256 hash")
	}

	principal, err := store.AuthenticateAPIToken(secret)
	if err != nil {
		t.Fatalf("authenticate: %v", err)
	}
	if principal.User.ID != alice.ID || principal.Token.ID != token.ID {
		t.Fatalf("principal = %+v", principal)
	}
	if principal.Token.LastUsedAt.IsZero() {
		t.Fatalf("last use was not recorded")
	}

	listed, err := store.APITokens(alice.ID)
	if err != nil {
		t.Fatalf("list tokens: %v", err)
	}
	if len(listed) != 1 || listed[0].ID != token.ID || listed[0].Revoked {
		t.Fatalf("listed = %+v", listed)
	}

	if err := store.RevokeAPIToken(alice.ID, token.ID); err != nil {
		t.Fatalf("revoke token: %v", err)
	}
	if _, err := store.AuthenticateAPIToken(secret); !errors.Is(err, ErrInvalidAPIToken) {
		t.Fatalf("revoked token authenticated: %v", err)
	}
	if err := store.RevokeAPIToken(alice.ID, token.ID); err != nil {
		t.Fatalf("revoke is not idempotent: %v", err)
	}
	if err := store.RevokeAPIToken(alice.ID, "missing"); !errors.Is(err, ErrUnknownAPIToken) {
		t.Fatalf("unknown revoke = %v", err)
	}

	listed, err = store.APITokens(alice.ID)
	if err != nil || len(listed) != 1 || !listed[0].Revoked {
		t.Fatalf("revoked listing = %+v, %v", listed, err)
	}
}

func TestAPITokenRejectsOutOfScopeRequests(t *testing.T) {
	store, alice := openTokenStore(t)

	if _, _, err := store.CreateAPIToken(alice.ID, " ", RoleReadonly, []string{TokenResourceDevices}, "", time.Hour); !errors.Is(err, ErrInvalidAPITokenName) {
		t.Fatalf("empty name = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "user role", RoleUser, []string{TokenResourceDevices}, "", time.Hour); !errors.Is(err, ErrInvalidAPITokenRole) {
		t.Fatalf("user role = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "bad resource", RoleReadonly, []string{"payroll"}, "", time.Hour); !errors.Is(err, ErrInvalidTokenResource) {
		t.Fatalf("unknown resource = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "no resources", RoleReadonly, nil, "", time.Hour); !errors.Is(err, ErrEmptyTokenResources) {
		t.Fatalf("empty resources = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "short", RoleReadonly, []string{TokenResourceDevices}, "", time.Second); !errors.Is(err, ErrInvalidAPITokenTTL) {
		t.Fatalf("short ttl = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "long", RoleReadonly, []string{TokenResourceDevices}, "", 366*24*time.Hour); !errors.Is(err, ErrInvalidAPITokenTTL) {
		t.Fatalf("long ttl = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "group accounts", RoleAdmin,
		[]string{TokenResourceDevices, TokenResourceAccounts}, "east", time.Hour); !errors.Is(err, ErrAPITokenGroupScope) {
		t.Fatalf("group with accounts = %v", err)
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "missing group", RoleReadonly,
		[]string{TokenResourceGroups}, "east", time.Hour); !errors.Is(err, ErrUnknownAccountGroup) {
		t.Fatalf("unknown group = %v", err)
	}

	bob, err := store.CreateUserWithRole("bob", "battery staple", "", "", RoleOperator)
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, _, err := store.CreateAPIToken(bob.ID, "too strong", RoleAdmin,
		[]string{TokenResourceDevices}, "", time.Hour); !errors.Is(err, ErrAPITokenOutranks) {
		t.Fatalf("outranking token = %v", err)
	}
}

func TestAPITokenExpiryDemotionAndBan(t *testing.T) {
	store, alice := openTokenStore(t)
	now := time.Date(2026, 11, 10, 6, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })

	_, secret, err := store.CreateAPIToken(alice.ID, "nightly", RoleAdmin,
		[]string{TokenResourceAll}, "", 2*time.Hour)
	if err != nil {
		t.Fatalf("create token: %v", err)
	}
	if _, err := store.AuthenticateAPIToken(secret); err != nil {
		t.Fatalf("authenticate fresh token: %v", err)
	}

	store.SetClock(func() time.Time { return now.Add(3 * time.Hour) })
	if _, err := store.AuthenticateAPIToken(secret); !errors.Is(err, ErrInvalidAPIToken) {
		t.Fatalf("expired token authenticated: %v", err)
	}

	store.SetClock(func() time.Time { return now })
	_, otherSecret, err := store.CreateAPIToken(alice.ID, "read only", RoleReadonly,
		[]string{TokenResourceAudit}, "", 2*time.Hour)
	if err != nil {
		t.Fatalf("create second token: %v", err)
	}
	if _, err := store.CreateUserWithRole("carol", "carol password", "", "", RoleAdmin); err != nil {
		t.Fatalf("create second admin: %v", err)
	}
	if _, err := store.SetRole("alice", RoleReadonly); err != nil {
		t.Fatalf("demote alice: %v", err)
	}
	principal, err := store.AuthenticateAPIToken(otherSecret)
	if err != nil {
		t.Fatalf("authenticate after demotion: %v", err)
	}
	if principal.Token.Role != RoleReadonly {
		t.Fatalf("demoted token role = %q", principal.Token.Role)
	}
	narrowed, err := store.AuthenticateAPIToken(secret)
	if err != nil {
		t.Fatalf("authenticate narrowed token: %v", err)
	}
	if narrowed.Token.Role != RoleReadonly {
		t.Fatalf("token above demoted owner kept role %q", narrowed.Token.Role)
	}
	if _, err := store.SetRole("alice", RoleAdmin); err != nil {
		t.Fatalf("restore alice: %v", err)
	}

	if _, err := store.SetBanned("alice", true); err != nil {
		t.Fatalf("ban alice: %v", err)
	}
	if _, err := store.AuthenticateAPIToken(otherSecret); !errors.Is(err, ErrInvalidAPIToken) {
		t.Fatalf("banned owner token authenticated: %v", err)
	}
}

func TestAPITokenQuotaAndGroups(t *testing.T) {
	store, alice := openTokenStore(t)
	group, err := store.CreateGroup("east", "East", "", 0, 0)
	if err != nil {
		t.Fatalf("create group: %v", err)
	}
	token, _, err := store.CreateAPIToken(alice.ID, "east ops", RoleOperator,
		[]string{TokenResourceGroups, TokenResourceAudit}, group.ID, time.Hour)
	if err != nil {
		t.Fatalf("create group token: %v", err)
	}
	if token.GroupID != group.ID {
		t.Fatalf("group = %q, want %q", token.GroupID, group.ID)
	}
	for index := 0; index < maxAPITokens-1; index++ {
		if _, _, err := store.CreateAPIToken(alice.ID, "bulk", RoleReadonly,
			[]string{TokenResourceDevices}, "", time.Hour); err != nil {
			t.Fatalf("create token %d: %v", index, err)
		}
	}
	if _, _, err := store.CreateAPIToken(alice.ID, "over quota", RoleReadonly,
		[]string{TokenResourceDevices}, "", time.Hour); !errors.Is(err, ErrAPITokenQuota) {
		t.Fatalf("quota error = %v", err)
	}
}
