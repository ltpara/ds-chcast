package api_test

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type invitationJSON struct {
	ID               int64  `json:"id"`
	Email            string `json:"email"`
	Status           string `json:"status"`
	CreatedAtMS      int64  `json:"created_at_ms"`
	ExpiresAtMS      int64  `json:"expires_at_ms"`
	AcceptedUsername string `json:"accepted_username"`
}

type invitationListJSON struct {
	Invitations []invitationJSON `json:"invitations"`
}

type acceptedInvitationJSON struct {
	Username      string `json:"username"`
	Email         string `json:"email"`
	EmailVerified bool   `json:"email_verified"`
}

func TestAccountInvitationCreateAcceptAndReplay(t *testing.T) {
	mailer := &fakeMailer{}
	s := testutil.StartWith(t, testutil.Options{
		Mailer: mailer, InvitationURL: "https://console.example.com/admin/invite.html?from=mail",
		InvitationTTL: 48 * time.Hour,
	})
	if _, err := s.Accounts.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "admin", "correct horse").Token
	if _, err := s.Accounts.CreateUser("ordinary", "correct horse"); err != nil {
		t.Fatal(err)
	}
	ordinary := login(t, s.URL, "ordinary", "correct horse").Token

	if status, body := request(t, http.MethodPost, s.URL+"/v1/account-invitations",
		`{"email":"new.person@example.com"}`, ordinary); status != http.StatusForbidden {
		t.Fatalf("ordinary user created an invitation: %d %s", status, body)
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/account-invitations",
		`{"email":"new.person@example.com"}`, admin)
	if status != http.StatusAccepted {
		t.Fatalf("create invitation: %d %s", status, body)
	}
	var invitation invitationJSON
	if err := json.Unmarshal(body, &invitation); err != nil {
		t.Fatal(err)
	}
	if invitation.ID == 0 || invitation.Email != "new.person@example.com" ||
		invitation.Status != "pending" || invitation.ExpiresAtMS <= invitation.CreatedAtMS {
		t.Fatalf("invitation response = %+v", invitation)
	}
	messages := mailer.invitationMessages()
	if len(messages) != 1 || messages[0].To != invitation.Email || messages[0].Token == "" ||
		messages[0].TTL != 48*time.Hour {
		t.Fatalf("invitation messages = %+v", messages)
	}
	link, err := url.Parse(messages[0].Link)
	fragment, fragmentErr := url.ParseQuery(link.Fragment)
	if err != nil || fragmentErr != nil || link.Query().Get("from") != "mail" || fragment.Get("token") != messages[0].Token {
		t.Fatalf("invitation link = %q (%v)", messages[0].Link, err)
	}

	status, body = request(t, http.MethodPost, s.URL+"/v1/account-invitations/accept",
		`{"token":"`+messages[0].Token+`","username":"invited","password":"a strong password"}`, "")
	if status != http.StatusCreated {
		t.Fatalf("accept invitation: %d %s", status, body)
	}
	var accepted acceptedInvitationJSON
	if err := json.Unmarshal(body, &accepted); err != nil || !accepted.EmailVerified ||
		accepted.Username != "invited" || accepted.Email != invitation.Email {
		t.Fatalf("accepted account = %+v (%v)", accepted, err)
	}
	if login(t, s.URL, accepted.Username, "a strong password").User.Role != account.RoleUser {
		t.Fatal("invited account did not start as a viewer")
	}
	verified, err := s.Accounts.EmailVerified(accepted.Username)
	if err != nil || !verified {
		t.Fatalf("invited email verification = %v (%v)", verified, err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account-invitations/accept",
		`{"token":"`+messages[0].Token+`","username":"replay","password":"a strong password"}`, ""); status != http.StatusBadRequest {
		t.Fatalf("replayed invitation: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/account-invitations", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list invitations: %d %s", status, body)
	}
	var list invitationListJSON
	if err := json.Unmarshal(body, &list); err != nil || len(list.Invitations) != 1 ||
		list.Invitations[0].Status != "accepted" || list.Invitations[0].AcceptedUsername != accepted.Username {
		t.Fatalf("invitation list = %+v (%v)", list, err)
	}
}

func TestAccountInvitationCanBeCancelledAndMailFailureCancels(t *testing.T) {
	mailer := &fakeMailer{}
	s := testutil.StartWith(t, testutil.Options{Mailer: mailer})
	if _, err := s.Accounts.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "admin", "correct horse").Token

	status, body := request(t, http.MethodPost, s.URL+"/v1/account-invitations",
		`{"email":"first@example.com"}`, admin)
	if status != http.StatusAccepted {
		t.Fatalf("create invitation: %d %s", status, body)
	}
	var first invitationJSON
	if err := json.Unmarshal(body, &first); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodDelete,
		s.URL+"/v1/account-invitations/"+strconv.FormatInt(first.ID, 10), "", admin); status != http.StatusNoContent {
		t.Fatalf("cancel invitation: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account-invitations/accept",
		`{"token":"`+mailer.invitationMessages()[0].Token+`","username":"cancelled","password":"a strong password"}`, ""); status != http.StatusBadRequest {
		t.Fatalf("accepted cancelled invitation: %d %s", status, body)
	}

	mailer.mu.Lock()
	mailer.err = errors.New("SMTP relay unavailable")
	mailer.mu.Unlock()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account-invitations",
		`{"email":"failed@example.com"}`, admin); status != http.StatusBadGateway {
		t.Fatalf("mail failure response: %d %s", status, body)
	}
	invitations, err := s.Accounts.ListAccountInvitations(0)
	if err != nil || len(invitations) != 2 || invitations[0].Status != "cancelled" {
		t.Fatalf("failed email left a usable invitation: %+v (%v)", invitations, err)
	}
}
