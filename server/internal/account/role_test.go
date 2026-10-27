package account

import (
	"database/sql"
	"errors"
	"path/filepath"
	"testing"
	"time"
)

func TestRolesDefaultAndProtectLastAdmin(t *testing.T) {
	store := openTemp(t, "roles.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if alice.Role != RoleAdmin {
		t.Fatalf("first account role = %q, want admin", alice.Role)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if bob.Role != RoleUser {
		t.Fatalf("second account role = %q, want user", bob.Role)
	}

	if _, err := store.SetRole("bob", "root"); !errors.Is(err, ErrInvalidRole) {
		t.Fatalf("invalid role = %v", err)
	}
	if _, err := store.SetRole("alice", RoleUser); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("demoting the last admin = %v", err)
	}
	if err := store.DeleteUser("alice"); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("deleting the last admin = %v", err)
	}

	if _, err := store.SetRole("bob", RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if _, err := store.SetRole("alice", RoleUser); err != nil {
		t.Fatalf("demote alice: %v", err)
	}
	user, token, _, _, _, err := store.LoginWithRefresh("alice", "correct horse", "", "", "", "")
	if err != nil {
		t.Fatalf("login alice: %v", err)
	}
	if user.Role != RoleUser {
		t.Fatalf("login role = %q, want user", user.Role)
	}
	authed, err := store.Authenticate(token)
	if err != nil || authed.Role != RoleUser {
		t.Fatalf("authenticate = %+v, %v", authed, err)
	}
	refreshed, _, _, _, _, err := store.RefreshSession(mustRefresh(t, store, "alice", "correct horse"))
	if err != nil || refreshed.Role != RoleUser {
		t.Fatalf("refresh role = %+v, %v", refreshed, err)
	}
	if err := store.DeleteUser("alice"); err != nil {
		t.Fatalf("delete demoted account: %v", err)
	}
	if _, err := store.FindUser("alice"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("deleted user found: %v", err)
	}
}

func mustRefresh(t *testing.T, store *Store, username, password string) string {
	t.Helper()
	_, _, _, refresh, _, err := store.LoginWithRefresh(username, password, "", "", "", "")
	if err != nil {
		t.Fatalf("login %s: %v", username, err)
	}
	return refresh
}

func TestRoleMigrationPromotesEarliestAccount(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatalf("open legacy database: %v", err)
	}
	for _, schema := range []string{
		`CREATE TABLE users (
			id            TEXT PRIMARY KEY,
			username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
			password_hash TEXT NOT NULL,
			created_at    INTEGER NOT NULL
		)`,
		`INSERT INTO users (id, username, password_hash, created_at)
		 VALUES ('b', 'bob', 'x', 20), ('a', 'alice', 'x', 10)`,
	} {
		if _, err := db.Exec(schema); err != nil {
			t.Fatalf("seed legacy schema: %v", err)
		}
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close legacy database: %v", err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	alice, err := store.FindUser("alice")
	if err != nil {
		t.Fatalf("find alice: %v", err)
	}
	if alice.Role != RoleAdmin {
		t.Fatalf("earliest account role = %q, want admin", alice.Role)
	}
	bob, err := store.FindUser("bob")
	if err != nil || bob.Role != RoleUser {
		t.Fatalf("second legacy account = %+v, %v", bob, err)
	}

	// Reopening must keep the promoted role rather than shuffling admins.
	store.Close()
	store, err = Open(path)
	if err != nil {
		t.Fatalf("reopen store: %v", err)
	}
	if user, err := store.FindUser("alice"); err != nil || user.Role != RoleAdmin {
		t.Fatalf("role after reopen = %+v, %v", user, err)
	}
}

func TestRoleVisibleInUserList(t *testing.T) {
	store := openTemp(t, "list.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	users, err := store.ListUsers()
	if err != nil {
		t.Fatalf("list users: %v", err)
	}
	if len(users) != 2 || users[0].Username != "alice" || users[0].Role != RoleAdmin ||
		users[1].Role != RoleUser {
		t.Fatalf("users = %+v", users)
	}
	if users[0].CreatedAt.IsZero() || time.Since(users[0].CreatedAt) > time.Minute {
		t.Fatalf("created_at = %v", users[0].CreatedAt)
	}
}
