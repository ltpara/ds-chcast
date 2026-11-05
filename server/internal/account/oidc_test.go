package account

import (
	"errors"
	"testing"
)

func TestLoginOIDCAutoCreatesAndLinksAccount(t *testing.T) {
	store := openTemp(t, "state.db")
	user, token, accessExpiry, refresh, refreshExpiry, err := store.LoginOIDC(
		"https://idp.example", "subject-1", "alice", "alice@example.com", true,
		"device-1", "workstation", true,
	)
	if err != nil {
		t.Fatalf("first OIDC login: %v", err)
	}
	if user.Username != "alice" || user.Email != "alice@example.com" || token == "" || refresh == "" {
		t.Fatalf("created user = %+v", user)
	}
	if accessExpiry.IsZero() || refreshExpiry.IsZero() || !accessExpiry.Before(refreshExpiry) {
		t.Fatalf("expiry pair = %v / %v", accessExpiry, refreshExpiry)
	}
	authenticated, err := store.Authenticate(token)
	if err != nil || authenticated.ID != user.ID {
		t.Fatalf("token did not authenticate the OIDC user: %+v, %v", authenticated, err)
	}

	again, secondToken, _, _, _, err := store.LoginOIDC(
		"https://idp.example", "subject-1", "different-hint", "other@example.com", true,
		"", "", true,
	)
	if err != nil {
		t.Fatalf("second OIDC login: %v", err)
	}
	if again.ID != user.ID || again.Username != "alice" || secondToken == token {
		t.Fatalf("subject was not reused: %+v", again)
	}
}

func TestLoginOIDCDerivesUniqueUsernames(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatal(err)
	}
	user, _, _, _, _, err := store.LoginOIDC(
		"https://idp.example", "subject-2", "alice", "", false, "", "", true,
	)
	if err != nil {
		t.Fatalf("OIDC login with colliding name: %v", err)
	}
	if user.Username != "alice-2" {
		t.Fatalf("username = %q", user.Username)
	}
	if user.Email != "" {
		t.Fatalf("unverified email was stored: %q", user.Email)
	}

	digestUser, _, _, _, _, err := store.LoginOIDC(
		"https://idp.example", "subject-3", "", "", false, "", "", true,
	)
	if err != nil {
		t.Fatalf("OIDC login without a name hint: %v", err)
	}
	if digestUser.Username == "" || digestUser.Username == "alice-2" {
		t.Fatalf("derived username = %q", digestUser.Username)
	}
}

func TestLoginOIDCRequiresKnownIdentityWithoutAutoCreate(t *testing.T) {
	store := openTemp(t, "state.db")
	_, _, _, _, _, err := store.LoginOIDC(
		"https://idp.example", "subject-4", "bob", "", false, "", "", false,
	)
	if !errors.Is(err, ErrOIDCAccountNotFound) {
		t.Fatalf("error = %v", err)
	}
	if _, err := store.FindUser("bob"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("unexpected account after refused login: %v", err)
	}
}

func TestLoginOIDCRefusesBannedAccount(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("root", "correct horse"); err != nil {
		t.Fatal(err)
	}
	user, _, _, _, _, err := store.LoginOIDC(
		"https://idp.example", "subject-5", "carol", "", false, "", "", true,
	)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.SetBanned(user.Username, true); err != nil {
		t.Fatal(err)
	}
	_, _, _, _, _, err = store.LoginOIDC(
		"https://idp.example", "subject-5", "carol", "", false, "", "", true,
	)
	if !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("banned login error = %v", err)
	}
}

func TestLoginOIDCRejectsDuplicateEmail(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUserWithEmail("dave", "correct horse", "dave@example.com"); err != nil {
		t.Fatal(err)
	}
	_, _, _, _, _, err := store.LoginOIDC(
		"https://idp.example", "subject-6", "dave-oidc", "dave@example.com", true, "", "", true,
	)
	if !errors.Is(err, ErrEmailTaken) {
		t.Fatalf("duplicate email error = %v", err)
	}
	if _, err := store.FindUser("dave-oidc"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("half-created OIDC account: %v", err)
	}
}

func TestLoginOIDCScopesSubjectsByIssuer(t *testing.T) {
	store := openTemp(t, "state.db")
	first, _, _, _, _, err := store.LoginOIDC(
		"https://idp-one.example", "shared-subject", "user", "", false, "", "", true,
	)
	if err != nil {
		t.Fatal(err)
	}
	second, _, _, _, _, err := store.LoginOIDC(
		"https://idp-two.example", "shared-subject", "user", "", false, "", "", true,
	)
	if err != nil {
		t.Fatal(err)
	}
	if first.ID == second.ID || first.Username == second.Username {
		t.Fatalf("issuers shared an account: %+v / %+v", first, second)
	}
}

func TestLoginOIDCSessionRefreshes(t *testing.T) {
	store := openTemp(t, "state.db")
	_, _, _, refresh, _, err := store.LoginOIDC(
		"https://idp.example", "subject-7", "erin", "", false, "", "", true,
	)
	if err != nil {
		t.Fatal(err)
	}
	user, token, _, rotated, _, err := store.RefreshSession(refresh)
	if err != nil {
		t.Fatalf("refresh OIDC session: %v", err)
	}
	if user.Username != "erin" || token == "" || rotated == "" {
		t.Fatalf("refreshed session = %+v", user)
	}
}
