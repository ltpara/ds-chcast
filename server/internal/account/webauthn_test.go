package account

import (
	"bytes"
	"errors"
	"testing"
	"time"
)

func registerTestKey(t *testing.T, store *Store, userID string, id []byte, name string) WebAuthnCredential {
	t.Helper()
	challenge := []byte("register-challenge-" + string(id))
	if _, err := store.IssueWebAuthnChallenge(userID, WebAuthnRegister, challenge, 5*time.Minute); err != nil {
		t.Fatalf("issue registration challenge: %v", err)
	}
	cred, err := store.FinishWebAuthnRegistration(userID, challenge, WebAuthnCredential{
		ID:        id,
		PublicKey: []byte{0xa5, 0x01, 0x02},
		Alg:       -7,
	}, name)
	if err != nil {
		t.Fatalf("finish registration: %v", err)
	}
	return cred
}

func TestWebAuthnRegistrationAndLogin(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	cred := registerTestKey(t, store, alice.ID, []byte{1, 2, 3, 4}, "  ")
	if cred.Name != "Security key" {
		t.Fatalf("default name = %q", cred.Name)
	}
	if cred.CreatedAt.IsZero() {
		t.Fatal("created_at not set")
	}
	// The registration challenge is single-use.
	if _, err := store.FinishWebAuthnRegistration(alice.ID, []byte("register-challenge-\x01\x02\x03\x04"), WebAuthnCredential{
		ID: []byte{9}, PublicKey: []byte{1}, Alg: -7,
	}, "second"); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("reused registration challenge = %v", err)
	}
	// A duplicate credential ID is rejected and the pending challenge survives
	// so the caller can retry with a different authenticator.
	if _, err := store.IssueWebAuthnChallenge(alice.ID, WebAuthnRegister, []byte("dup"), 5*time.Minute); err != nil {
		t.Fatalf("issue duplicate challenge: %v", err)
	}
	if _, err := store.FinishWebAuthnRegistration(alice.ID, []byte("dup"), WebAuthnCredential{
		ID: []byte{1, 2, 3, 4}, PublicKey: []byte{1}, Alg: -7,
	}, "dup"); !errors.Is(err, ErrWebAuthnCredentialExists) {
		t.Fatalf("duplicate credential = %v", err)
	}
	if _, err := store.WebAuthnChallenge(alice.ID, WebAuthnRegister); err != nil {
		t.Fatalf("challenge should survive a failed insert: %v", err)
	}

	loginChallenge := []byte("login-challenge")
	user, creds, err := store.BeginWebAuthnLogin("alice", "correct horse", loginChallenge, 5*time.Minute)
	if err != nil {
		t.Fatalf("begin login: %v", err)
	}
	if user.ID != alice.ID || len(creds) != 1 || !bytes.Equal(creds[0].ID, []byte{1, 2, 3, 4}) {
		t.Fatalf("begin login user=%v creds=%v", user.ID, creds)
	}
	user, token, accessExpires, refresh, sessionExpires, err := store.CompleteWebAuthnLogin(
		"alice", []byte{1, 2, 3, 4}, loginChallenge, 7, "dev-1", "Desk")
	if err != nil {
		t.Fatalf("complete login: %v", err)
	}
	if user.ID != alice.ID || token == "" || refresh == "" {
		t.Fatalf("complete login user=%v token=%q refresh=%q", user.ID, token, refresh)
	}
	if !accessExpires.Before(sessionExpires) {
		t.Fatalf("access expiry %v not before session expiry %v", accessExpires, sessionExpires)
	}
	if _, err := store.Authenticate(token); err != nil {
		t.Fatalf("authenticate issued token: %v", err)
	}
	stored, err := store.WebAuthnCredentialForUser(alice.ID, []byte{1, 2, 3, 4})
	if err != nil {
		t.Fatalf("read credential: %v", err)
	}
	if stored.SignCount != 7 || stored.LastUsedAt.IsZero() {
		t.Fatalf("credential after login = %+v", stored)
	}
	// Replaying the spent challenge must fail even with the same credential.
	if _, _, _, _, _, err := store.CompleteWebAuthnLogin(
		"alice", []byte{1, 2, 3, 4}, loginChallenge, 8, "dev-1", "Desk"); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("replayed login challenge = %v", err)
	}

	if err := store.RemoveWebAuthnCredential(alice.ID, []byte{1, 2, 3, 4}, "wrong horse"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("remove with wrong password = %v", err)
	}
	if err := store.RemoveWebAuthnCredential(alice.ID, []byte{1, 2, 3, 4}, "correct horse"); err != nil {
		t.Fatalf("remove credential: %v", err)
	}
	if creds, err := store.WebAuthnCredentials(alice.ID); err != nil || len(creds) != 0 {
		t.Fatalf("credentials after remove = %v, %v", creds, err)
	}
	if err := store.RemoveWebAuthnCredential(alice.ID, []byte{1, 2, 3, 4}, "correct horse"); !errors.Is(err, ErrWebAuthnCredential) {
		t.Fatalf("remove missing credential = %v", err)
	}
}

func TestWebAuthnLoginRequirements(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, _, err := store.BeginWebAuthnLogin("nobody", "wrong horse", []byte("c"), time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("unknown user = %v", err)
	}
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	if _, _, err := store.BeginWebAuthnLogin("alice", "wrong horse", []byte("c"), time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password = %v", err)
	}
	if _, _, err := store.BeginWebAuthnLogin("alice", "correct horse", []byte("c"), time.Minute); !errors.Is(err, ErrWebAuthnNotEnabled) {
		t.Fatalf("account without keys = %v", err)
	}
	if _, err := store.IssueWebAuthnChallenge(alice.ID, "bogus", []byte("c"), time.Minute); err == nil {
		t.Fatal("invalid challenge purpose accepted")
	}
	if _, err := store.IssueWebAuthnChallenge(alice.ID, WebAuthnRegister, nil, time.Minute); err == nil {
		t.Fatal("empty challenge accepted")
	}

	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	registerTestKey(t, store, bob.ID, []byte{5, 6}, "YubiKey")
	if _, err := store.IssueWebAuthnChallenge(bob.ID, WebAuthnLogin, []byte("pre-ban"), time.Minute); err != nil {
		t.Fatalf("issue pre-ban challenge: %v", err)
	}
	if _, err := store.SetBanned("bob", true); err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	// Banning drops a pending assertion so it cannot be finished after the
	// account was locked.
	if _, err := store.WebAuthnChallenge(bob.ID, WebAuthnLogin); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("pending challenge after ban = %v", err)
	}
	if _, _, err := store.BeginWebAuthnLogin("bob", "correct horse", []byte("c"), time.Minute); !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("banned login begin = %v", err)
	}
	if _, _, _, _, _, err := store.CompleteWebAuthnLogin(
		"bob", []byte{5, 6}, []byte("pre-ban"), 1, "", ""); !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("banned login complete = %v", err)
	}
	if creds, err := store.WebAuthnCredentials(bob.ID); err != nil || len(creds) != 1 {
		t.Fatalf("ban should keep keys: %v, %v", creds, err)
	}
	if _, err := store.SetBanned("bob", false); err != nil {
		t.Fatalf("unban bob: %v", err)
	}
	if _, _, err := store.BeginWebAuthnLogin("bob", "correct horse", []byte("c2"), time.Minute); err != nil {
		t.Fatalf("unbanned login begin: %v", err)
	}
}

func TestWebAuthnChallengeExpiry(t *testing.T) {
	store := openTemp(t, "state.db")
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	if _, err := store.IssueWebAuthnChallenge(alice.ID, WebAuthnRegister, []byte("c"), time.Minute); err != nil {
		t.Fatalf("issue challenge: %v", err)
	}
	now = now.Add(2 * time.Minute)
	if _, err := store.WebAuthnChallenge(alice.ID, WebAuthnRegister); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("expired challenge = %v", err)
	}
	if _, err := store.FinishWebAuthnRegistration(alice.ID, []byte("c"), WebAuthnCredential{
		ID: []byte{1}, PublicKey: []byte{1}, Alg: -7,
	}, "key"); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("finish with expired challenge = %v", err)
	}
}

func TestWebAuthnResetTwoFactorClearsKeys(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create user: %v", err)
	}
	registerTestKey(t, store, alice.ID, []byte{1, 2, 3}, "key")
	if _, err := store.IssueWebAuthnChallenge(alice.ID, WebAuthnLogin, []byte("c"), time.Minute); err != nil {
		t.Fatalf("issue challenge: %v", err)
	}
	if err := store.ResetTwoFactor("alice"); err != nil {
		t.Fatalf("reset two-factor: %v", err)
	}
	if creds, err := store.WebAuthnCredentials(alice.ID); err != nil || len(creds) != 0 {
		t.Fatalf("keys after reset = %v, %v", creds, err)
	}
	if _, err := store.WebAuthnChallenge(alice.ID, WebAuthnLogin); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("challenge after reset = %v", err)
	}
}

func TestWebAuthnCredentialScoping(t *testing.T) {
	store := openTemp(t, "state.db")
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "correct horse")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	registerTestKey(t, store, alice.ID, []byte{7, 8, 9}, "alice key")
	if _, err := store.WebAuthnCredentialForUser(bob.ID, []byte{7, 8, 9}); !errors.Is(err, ErrWebAuthnCredential) {
		t.Fatalf("cross-account credential lookup = %v", err)
	}
	if _, _, _, _, _, err := store.CompleteWebAuthnLogin(
		"bob", []byte{7, 8, 9}, []byte("c"), 1, "", ""); !errors.Is(err, ErrWebAuthnChallenge) {
		t.Fatalf("bob finishing alice's assertion = %v", err)
	}
}
