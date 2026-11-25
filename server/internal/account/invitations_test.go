package account

import (
	"errors"
	"testing"
	"time"
)

func TestAccountInvitationIsSingleUseAndVerifiesInvitedEmail(t *testing.T) {
	store := openTemp(t, "invitations.db")
	admin, err := store.CreateUser("admin", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 11, 24, 8, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })

	issued, err := store.CreateAccountInvitation("new.user@example.com", admin.Username, 24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	var stored string
	if err := store.db.QueryRow(`SELECT token_hash FROM account_invitations WHERE id = ?`, issued.Invitation.ID).Scan(&stored); err != nil {
		t.Fatal(err)
	}
	if stored != tokenHash(issued.Token) || stored == issued.Token {
		t.Fatalf("stored invitation credential is not a token hash: %q", stored)
	}

	user, err := store.AcceptAccountInvitation(issued.Token, "new.viewer", "strong password")
	if err != nil {
		t.Fatal(err)
	}
	if user.Username != "new.viewer" || user.Role != RoleUser || user.Email != "new.user@example.com" {
		t.Fatalf("unexpected invited account: %+v", user)
	}
	verified, err := store.EmailVerified(user.Username)
	if err != nil || !verified {
		t.Fatalf("invited email verified = %v, err = %v", verified, err)
	}
	if _, err := store.AcceptAccountInvitation(issued.Token, "another.viewer", "strong password"); !errors.Is(err, ErrInvalidAccountInvitation) {
		t.Fatalf("replayed invitation error = %v", err)
	}
	items, err := store.ListAccountInvitations(0)
	if err != nil || len(items) != 1 || items[0].Status != "accepted" || items[0].AcceptedUsername != user.Username {
		t.Fatalf("invitation history = %+v, err = %v", items, err)
	}
}

func TestAccountInvitationExpiresCancelsAndCanBeReissued(t *testing.T) {
	store := openTemp(t, "invitations.db")
	if _, err := store.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 11, 24, 8, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })

	first, err := store.CreateAccountInvitation("person@example.com", "admin", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	second, err := store.CreateAccountInvitation("PERSON@example.com", "admin", 2*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	items, err := store.ListAccountInvitations(0)
	if err != nil || len(items) != 2 || items[0].ID != second.Invitation.ID || items[0].Status != "pending" || items[1].Status != "cancelled" {
		t.Fatalf("reissued invitations = %+v, err = %v", items, err)
	}
	if _, err := store.AcceptAccountInvitation(first.Token, "expired.user", "strong password"); !errors.Is(err, ErrInvalidAccountInvitation) {
		t.Fatalf("replaced invitation error = %v", err)
	}
	if err := store.CancelAccountInvitation(second.Invitation.ID); err != nil {
		t.Fatal(err)
	}
	if _, err := store.AcceptAccountInvitation(second.Token, "cancelled.user", "strong password"); !errors.Is(err, ErrInvalidAccountInvitation) {
		t.Fatalf("cancelled invitation error = %v", err)
	}

	third, err := store.CreateAccountInvitation("later@example.com", "admin", time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	now = now.Add(2 * time.Minute)
	items, err = store.ListAccountInvitations(0)
	if err != nil || len(items) != 3 || items[0].Status != "expired" {
		t.Fatalf("expired invitation history = %+v, err = %v", items, err)
	}
	if _, err := store.AcceptAccountInvitation(third.Token, "late.user", "strong password"); !errors.Is(err, ErrInvalidAccountInvitation) {
		t.Fatalf("expired invitation error = %v", err)
	}
}

func TestAccountInvitationDoesNotConsumeWhenAccountCreationFails(t *testing.T) {
	store := openTemp(t, "invitations.db")
	if _, err := store.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if _, err := store.CreateUser("taken", "correct horse"); err != nil {
		t.Fatal(err)
	}
	issued, err := store.CreateAccountInvitation("new@example.com", "admin", time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.AcceptAccountInvitation(issued.Token, "taken", "strong password"); !errors.Is(err, ErrUserExists) {
		t.Fatalf("duplicate username error = %v", err)
	}
	if _, err := store.AcceptAccountInvitation(issued.Token, "available", "strong password"); err != nil {
		t.Fatalf("invite after rolled-back attempt: %v", err)
	}
}
