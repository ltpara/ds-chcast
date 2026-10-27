package account

import (
	"errors"
	"testing"
	"time"
)

func TestEmailStoredAndUnique(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUserWithEmail("alice", "secret123", "Alice@Example.com")
	if err != nil {
		t.Fatalf("create user with email: %v", err)
	}
	if user.Email != "Alice@Example.com" {
		t.Fatalf("created email = %q", user.Email)
	}
	found, err := store.FindUser("alice")
	if err != nil || found.Email != "Alice@Example.com" {
		t.Fatalf("find user email = %q, %v", found.Email, err)
	}
	if _, err := store.CreateUserWithEmail("bob", "secret123", "alice@example.com"); !errors.Is(err, ErrEmailTaken) {
		t.Fatalf("duplicate email error = %v", err)
	}
	if _, err := store.FindUser("bob"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("duplicate email left a user behind: %v", err)
	}
	for _, bad := range []string{"not-an-email", "a@b@c", "Alice <a@b.com>"} {
		if _, err := store.CreateUserWithEmail("carol", "secret123", bad); !errors.Is(err, ErrInvalidEmail) {
			t.Fatalf("email %q error = %v", bad, err)
		}
	}
	if _, err := store.SetEmail("bob", "bob@example.com"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("set email for unknown user: %v", err)
	}
	bob, err := store.CreateUser("bob", "secret123")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	bob, err = store.SetEmail("bob", "bob@example.com")
	if err != nil || bob.Email != "bob@example.com" {
		t.Fatalf("set email: %+v, %v", bob, err)
	}
	users, err := store.ListUsers()
	if err != nil || len(users) != 2 || users[1].Email != "bob@example.com" {
		t.Fatalf("list users = %+v, %v", users, err)
	}
	if bob, err = store.SetEmail("bob", ""); err != nil || bob.Email != "" {
		t.Fatalf("clear email: %+v, %v", bob, err)
	}
}

func TestCheckPassword(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "secret123"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	if err := store.CheckPassword("alice", "secret123"); err != nil {
		t.Fatalf("check correct password: %v", err)
	}
	if err := store.CheckPassword("alice", "wrongpass"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("check wrong password: %v", err)
	}
	if err := store.CheckPassword("nobody", "secret123"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("check unknown user: %v", err)
	}
}

func TestPasswordResetLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUserWithEmail("alice", "secret123", "alice@example.com"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "secret123"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, _, _, err := store.CreatePasswordReset("nobody", time.Minute); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("reset unknown user: %v", err)
	}
	if _, _, _, err := store.CreatePasswordReset("bob", time.Minute); !errors.Is(err, ErrNoRecoveryEmail) {
		t.Fatalf("reset without email: %v", err)
	}
	_, token, _, err := store.Login("alice", "secret123")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	first, user, expires, err := store.CreatePasswordReset("alice", 30*time.Minute)
	if err != nil {
		t.Fatalf("create reset: %v", err)
	}
	if first == "" || user.Email != "alice@example.com" || !expires.After(time.Now()) {
		t.Fatalf("reset = %q, %+v, %v", first, user, expires)
	}
	var stored string
	if err := store.db.QueryRow(`SELECT token_hash FROM password_resets`).Scan(&stored); err != nil {
		t.Fatalf("read reset row: %v", err)
	}
	if stored != tokenHash(first) || stored == first {
		t.Fatalf("stored reset hash = %q", stored)
	}
	second, _, _, err := store.CreatePasswordReset("alice", 30*time.Minute)
	if err != nil {
		t.Fatalf("replace reset: %v", err)
	}
	if _, err := store.ConsumePasswordReset(first, "newsecret123"); !errors.Is(err, ErrInvalidResetToken) {
		t.Fatalf("superseded token: %v", err)
	}
	if _, err := store.ConsumePasswordReset(second, "short"); !errors.Is(err, ErrWeakPassword) {
		t.Fatalf("weak password: %v", err)
	}
	consumed, err := store.ConsumePasswordReset(second, "newsecret123")
	if err != nil || consumed.Username != "alice" {
		t.Fatalf("consume reset: %+v, %v", consumed, err)
	}
	if _, err := store.ConsumePasswordReset(second, "another123"); !errors.Is(err, ErrInvalidResetToken) {
		t.Fatalf("reused token: %v", err)
	}
	if _, _, _, err := store.Login("alice", "secret123"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("old password still works: %v", err)
	}
	if _, _, _, err := store.Login("alice", "newsecret123"); err != nil {
		t.Fatalf("new password rejected: %v", err)
	}
	if _, err := store.Authenticate(token); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("session survived reset: %v", err)
	}
}

func TestPasswordResetExpiry(t *testing.T) {
	store := openTemp(t, "state.db")
	base := time.Now().UTC()
	store.SetClock(func() time.Time { return base })
	if _, err := store.CreateUserWithEmail("alice", "secret123", "alice@example.com"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	token, _, _, err := store.CreatePasswordReset("alice", 10*time.Minute)
	if err != nil {
		t.Fatalf("create reset: %v", err)
	}
	store.SetClock(func() time.Time { return base.Add(11 * time.Minute) })
	if _, err := store.ConsumePasswordReset(token, "newsecret123"); !errors.Is(err, ErrInvalidResetToken) {
		t.Fatalf("expired token: %v", err)
	}
}
