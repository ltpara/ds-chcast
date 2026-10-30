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

func TestOperatorAndReadonlyRoles(t *testing.T) {
	store := openTemp(t, "console-roles.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := store.CreateUser("carol", "carol pass"); err != nil {
		t.Fatalf("create carol: %v", err)
	}
	for role, want := range map[string]bool{
		RoleAdmin: true, RoleOperator: true, RoleReadonly: true, RoleUser: true,
		"root": false, "": false,
	} {
		if ValidRole(role) != want {
			t.Fatalf("ValidRole(%q) = %v, want %v", role, ValidRole(role), want)
		}
	}

	if _, err := store.SetRole("bob", RoleOperator); err != nil {
		t.Fatalf("set operator: %v", err)
	}
	if _, err := store.SetRole("carol", RoleReadonly); err != nil {
		t.Fatalf("set readonly: %v", err)
	}
	user, token, _, _, _, err := store.LoginWithRefresh("carol", "carol pass", "", "", "", "")
	if err != nil || user.Role != RoleReadonly {
		t.Fatalf("readonly login = %+v, %v", user, err)
	}
	if authed, err := store.Authenticate(token); err != nil || authed.Role != RoleReadonly {
		t.Fatalf("readonly authenticate = %+v, %v", authed, err)
	}

	// Operators and readonly accounts are console roles, not admins: the last
	// admin still cannot step down to either of them.
	for _, role := range []string{RoleOperator, RoleReadonly} {
		if _, err := store.SetRole("alice", role); !errors.Is(err, ErrLastAdmin) {
			t.Fatalf("last admin to %s = %v", role, err)
		}
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

func TestCreateUserWithRole(t *testing.T) {
	store := openTemp(t, "create-role.db")
	// The bootstrap account stays admin even when the request asks for less,
	// instead of being created and then rejected by the last-admin guard.
	bootstrap, err := store.CreateUserWithRole("alice", "correct horse", "", "", RoleUser)
	if err != nil || bootstrap.Role != RoleAdmin {
		t.Fatalf("bootstrap account = %+v, %v", bootstrap, err)
	}
	if _, err := store.CreateUserWithRole("bob", "battery staple", "", "", "root"); !errors.Is(err, ErrInvalidRole) {
		t.Fatalf("invalid role = %v", err)
	}
	if _, err := store.FindUser("bob"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("invalid role created an account: %v", err)
	}
	if user, err := store.CreateUserWithRole("bob", "battery staple", "", "", RoleOperator); err != nil ||
		user.Role != RoleOperator {
		t.Fatalf("operator account = %+v, %v", user, err)
	}
}
