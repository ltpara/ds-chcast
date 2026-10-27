package account

import (
	"errors"
	"testing"
)

func TestAccountBanRevokesSessionsAndBlocksLogin(t *testing.T) {
	store := openTemp(t, "ban.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	_, token, _, refresh, _, err := store.LoginWithRefresh("bob", "battery staple", "", "", "", "")
	if err != nil {
		t.Fatalf("login bob: %v", err)
	}

	user, err := store.SetBanned("bob", true)
	if err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	if user.Username != "bob" || !user.Banned {
		t.Fatalf("banned user = %+v", user)
	}
	if _, err := store.Authenticate(token); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("banned access token = %v", err)
	}
	if _, _, _, _, _, err := store.RefreshSession(refresh); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("banned refresh token = %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefresh("bob", "battery staple", "", "", "", ""); !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("banned login = %v", err)
	}
	// A wrong password must still look like any other failed login rather than
	// disclose the ban to someone who cannot authenticate anyway.
	if _, _, _, _, _, err := store.LoginWithRefresh("bob", "wrong password", "", "", "", ""); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("banned wrong password = %v", err)
	}
	users, err := store.ListUsers()
	if err != nil {
		t.Fatalf("list users: %v", err)
	}
	if len(users) != 2 || !users[1].Banned || users[0].Banned {
		t.Fatalf("users = %+v", users)
	}
	if sessions, err := store.Sessions(bob.ID, ""); err != nil || len(sessions) != 0 {
		t.Fatalf("banned sessions = %+v, %v", sessions, err)
	}

	if user, err = store.SetBanned("bob", false); err != nil || user.Banned {
		t.Fatalf("unban bob = %+v, %v", user, err)
	}
	if _, _, _, _, _, err := store.LoginWithRefresh("bob", "battery staple", "", "", "", ""); err != nil {
		t.Fatalf("login after unban: %v", err)
	}
	if _, err := store.SetBanned("nobody", true); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("ban unknown user = %v", err)
	}
}

func TestAccountBanProtectsLastUsableAdmin(t *testing.T) {
	store := openTemp(t, "ban-admin.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := store.SetBanned("alice", true); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("ban sole admin = %v", err)
	}
	if _, err := store.SetRole("bob", RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if _, err := store.SetBanned("alice", true); err != nil {
		t.Fatalf("ban alice: %v", err)
	}
	if _, err := store.SetBanned("bob", true); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("ban last usable admin = %v", err)
	}
	if _, err := store.SetRole("bob", RoleUser); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("demote last usable admin = %v", err)
	}
	// A banned admin no longer offers a way back in, so demoting or deleting
	// that account must not be what keeps the deployment locked out.
	if _, err := store.SetRole("alice", RoleUser); err != nil {
		t.Fatalf("demote banned admin: %v", err)
	}
	if err := store.DeleteUser("bob"); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("delete last usable admin = %v", err)
	}
	if err := store.DeleteUser("alice"); err != nil {
		t.Fatalf("delete demoted banned account: %v", err)
	}
	if err := store.DeleteUser("bob"); err != nil {
		t.Fatalf("delete sole account: %v", err)
	}
}
