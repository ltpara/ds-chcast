package account

import (
	"errors"
	"testing"
)

const testLDAPDN = "uid=alice,ou=people,dc=example,dc=com"

func TestLoginLDAPAutoCreatesAndLinksAccount(t *testing.T) {
	store := openTemp(t, "state.db")
	user, token, accessExpiry, refresh, refreshExpiry, err := store.LoginLDAP(
		testLDAPDN, "alice", "alice@example.com", "device-1", "workstation", true,
	)
	if err != nil {
		t.Fatalf("first directory login: %v", err)
	}
	if user.Username != "alice" || user.Email != "alice@example.com" || user.Role != RoleAdmin {
		t.Fatalf("created user = %+v", user)
	}
	if token == "" || refresh == "" || accessExpiry.IsZero() || refreshExpiry.IsZero() {
		t.Fatalf("session = %q / %q / %v / %v", token, refresh, accessExpiry, refreshExpiry)
	}
	if authenticated, err := store.Authenticate(token); err != nil || authenticated.ID != user.ID {
		t.Fatalf("token did not authenticate the directory user: %+v, %v", authenticated, err)
	}
	linked, err := store.LDAPLinked("alice")
	if err != nil || !linked {
		t.Fatalf("LDAPLinked = %v, %v", linked, err)
	}

	// The DN is the identity: a renamed directory account keeps its local
	// account even if the login name and mail no longer match.
	again, secondToken, _, _, _, err := store.LoginLDAP(
		testLDAPDN, "alice-renamed", "other@example.com", "", "", false,
	)
	if err != nil {
		t.Fatalf("second directory login: %v", err)
	}
	if again.ID != user.ID || again.Username != "alice" || secondToken == token {
		t.Fatalf("directory identity was not reused: %+v", again)
	}
}

func TestLoginLDAPRequiresAutoCreate(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, _, _, _, _, err := store.LoginLDAP(testLDAPDN, "alice", "", "", "", false); !errors.Is(err, ErrLDAPAccountNotFound) {
		t.Fatalf("err = %v, want ErrLDAPAccountNotFound", err)
	}
	if _, err := store.FindUser("alice"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("account was created without auto-create: %v", err)
	}
}

func TestLoginLDAPRefusesLocalNameCollision(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if _, _, _, _, _, err := store.LoginLDAP(testLDAPDN, "alice", "", "", "", true); !errors.Is(err, ErrLDAPNameTaken) {
		t.Fatalf("err = %v, want ErrLDAPNameTaken", err)
	}
	// The local account keeps working with its own password and stays
	// unlinked, so the directory never signs into it.
	if _, _, _, err := store.Login("alice", "correct horse"); err != nil {
		t.Fatalf("local login after refused directory login: %v", err)
	}
	linked, err := store.LDAPLinked("alice")
	if err != nil || linked {
		t.Fatalf("LDAPLinked = %v, %v", linked, err)
	}
}

func TestLoginLDAPRefusesBannedLinkedAccount(t *testing.T) {
	store := openTemp(t, "state.db")
	// A local admin keeps the deployment manageable, so the directory user
	// below is an ordinary account that can be banned.
	if _, err := store.CreateUser("root", "correct horse"); err != nil {
		t.Fatal(err)
	}
	user, _, _, _, _, err := store.LoginLDAP(testLDAPDN, "alice", "", "", "", true)
	if err != nil {
		t.Fatalf("first directory login: %v", err)
	}
	if _, err := store.SetBanned(user.Username, true); err != nil {
		t.Fatalf("ban: %v", err)
	}
	if _, _, _, _, _, err := store.LoginLDAP(testLDAPDN, "alice", "", "", "", true); !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("err = %v, want ErrAccountBanned", err)
	}
}

func TestLoginLDAPRejectsUnusableNamesAndDropsInvalidMail(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, _, _, _, _, err := store.LoginLDAP(testLDAPDN, "alice/bob", "", "", "", true); !errors.Is(err, ErrInvalidUsername) {
		t.Fatalf("err = %v, want ErrInvalidUsername", err)
	}
	user, _, _, _, _, err := store.LoginLDAP(testLDAPDN, "alice", "not a mailbox", "", "", true)
	if err != nil {
		t.Fatalf("directory login with invalid mail: %v", err)
	}
	if user.Email != "" {
		t.Fatalf("invalid mail was stored: %q", user.Email)
	}
}
