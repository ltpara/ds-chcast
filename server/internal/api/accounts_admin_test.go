package api_test

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type adminAccountReply struct {
	ID          string `json:"id"`
	Username    string `json:"username"`
	Role        string `json:"role"`
	CreatedAtMS int64  `json:"created_at_ms"`
	Password    string `json:"password"`
	Banned      bool   `json:"banned"`
}

type adminAccountListReply struct {
	Users []adminAccountReply `json:"users"`
}

func TestAdminAccountsAuthorization(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if alice.Role != account.RoleAdmin {
		t.Fatalf("first account role = %q", alice.Role)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	adminToken := login(t, s.URL, "alice", "correct horse").Token
	userToken := login(t, s.URL, "bob", "battery staple").Token

	if status, body := request(t, http.MethodGet, s.URL+"/v1/accounts", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("list without credentials: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/accounts", "", userToken); status != http.StatusForbidden {
		t.Fatalf("list as ordinary user: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol"}`, userToken); status != http.StatusForbidden {
		t.Fatalf("create as ordinary user: %d %s", status, body)
	}
	for _, token := range []string{adminToken, testutil.ServerKey} {
		status, data := request(t, http.MethodGet, s.URL+"/v1/accounts", "", token)
		if status != http.StatusOK {
			t.Fatalf("list with %q: %d %s", token, status, data)
		}
		var reply adminAccountListReply
		if err := json.Unmarshal(data, &reply); err != nil {
			t.Fatalf("list reply: %v", err)
		}
		if len(reply.Users) != 2 || reply.Users[0].Username != "alice" ||
			reply.Users[0].Role != account.RoleAdmin || reply.Users[0].ID == "" ||
			reply.Users[0].CreatedAtMS == 0 || reply.Users[0].Password != "" ||
			reply.Users[1].Username != "bob" || reply.Users[1].Role != account.RoleUser {
			t.Fatalf("accounts = %+v", reply.Users)
		}
	}

	noStore := testutil.StartWith(t, testutil.Options{NoAccounts: true})
	if status, body := request(t, http.MethodGet, noStore.URL+"/v1/accounts", "", testutil.ServerKey); status != http.StatusServiceUnavailable {
		t.Fatalf("list without account store: %d %s", status, body)
	}
}

func TestAdminAccountsCreateRoleAndDelete(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse")
	if admin.User.Role != account.RoleAdmin {
		t.Fatalf("login role = %q", admin.User.Role)
	}

	status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"bob","password":"battery staple"}`, admin.Token)
	if status != http.StatusCreated {
		t.Fatalf("create bob: %d %s", status, data)
	}
	var bob adminAccountReply
	if err := json.Unmarshal(data, &bob); err != nil {
		t.Fatalf("create reply: %v", err)
	}
	if bob.Username != "bob" || bob.Role != account.RoleUser || bob.ID == "" || bob.Password != "" {
		t.Fatalf("created bob = %+v", bob)
	}
	bobLogin := login(t, s.URL, "bob", "battery staple")
	if bobLogin.User.Role != account.RoleUser {
		t.Fatalf("bob login role = %q", bobLogin.User.Role)
	}

	status, data = request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","role":"admin"}`, admin.Token)
	if status != http.StatusCreated {
		t.Fatalf("create carol: %d %s", status, data)
	}
	var carol adminAccountReply
	if err := json.Unmarshal(data, &carol); err != nil {
		t.Fatalf("carol reply: %v", err)
	}
	if carol.Role != account.RoleAdmin || carol.Password == "" {
		t.Fatalf("created carol = %+v", carol)
	}
	if login(t, s.URL, "carol", carol.Password).User.Role != account.RoleAdmin {
		t.Fatal("generated password does not log in")
	}

	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"dave","role":"owner"}`, admin.Token); status != http.StatusBadRequest {
		t.Fatalf("invalid create role: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"bob","password":"another password"}`, admin.Token); status != http.StatusConflict {
		t.Fatalf("duplicate create: %d %s", status, body)
	}

	if status, body := request(t, http.MethodPatch, s.URL+"/v1/accounts/bob",
		`{"role":"admin"}`, bobLogin.Token); status != http.StatusForbidden {
		t.Fatalf("self-promotion: %d %s", status, body)
	}
	status, data = request(t, http.MethodPatch, s.URL+"/v1/accounts/bob", `{"role":"admin"}`, admin.Token)
	if status != http.StatusOK {
		t.Fatalf("promote bob: %d %s", status, data)
	}
	var promoted adminAccountReply
	if err := json.Unmarshal(data, &promoted); err != nil || promoted.Role != account.RoleAdmin {
		t.Fatalf("promoted bob = %+v, %v", promoted, err)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/accounts/nobody",
		`{"role":"user"}`, admin.Token); status != http.StatusNotFound {
		t.Fatalf("patch unknown: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/accounts/bob",
		`{"role":"owner"}`, admin.Token); status != http.StatusBadRequest {
		t.Fatalf("invalid role: %d %s", status, body)
	}

	bobAdmin := login(t, s.URL, "bob", "battery staple").Token
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/accounts/carol",
		`{"role":"user"}`, bobAdmin); status != http.StatusOK {
		t.Fatalf("demote carol: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/accounts/alice",
		`{"role":"user"}`, bobAdmin); status != http.StatusOK {
		t.Fatalf("demote alice: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/accounts/bob",
		`{"role":"user"}`, bobAdmin); status != http.StatusConflict {
		t.Fatalf("demote last admin: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/accounts/bob", "", bobAdmin); status != http.StatusConflict {
		t.Fatalf("delete last admin: %d %s", status, body)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/accounts/alice", "", bobAdmin); status != http.StatusNoContent {
		t.Fatalf("delete demoted alice: %d %s", status, body)
	}
	if status, _ := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"alice","password":"correct horse"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("deleted account login = %d", status)
	}
	if status, _ := request(t, http.MethodDelete, s.URL+"/v1/accounts/nobody", "", bobAdmin); status != http.StatusNotFound {
		t.Fatalf("delete unknown = %d", status)
	}

	wantAudit := func(actor, action, detail string) {
		t.Helper()
		events, err := s.Accounts.AuditEventsFor(action, 20)
		if err != nil {
			t.Fatalf("audit %s: %v", action, err)
		}
		for _, event := range events {
			if event.Username == actor && event.Success &&
				strings.Contains(event.Detail, detail) {
				return
			}
		}
		t.Fatalf("no %s audit by %s containing %q: %+v", action, actor, detail, events)
	}
	wantAudit("alice", account.AuditAccountCreate, "created bob role=user")
	wantAudit("alice", account.AuditRoleChange, "role=admin for bob")
	wantAudit("bob", account.AuditAccountDelete, "deleted alice")

	// carol and bob are left; removing carol first makes bob the sole account,
	// which the store lets go so the next CreateUser becomes admin again.
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/accounts/carol", "", bobAdmin); status != http.StatusNoContent {
		t.Fatalf("delete carol: %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/accounts/bob", "", bobAdmin); status != http.StatusNoContent {
		t.Fatalf("delete sole account: %d %s", status, body)
	}
	if users, err := s.Accounts.ListUsers(); err != nil || len(users) != 0 {
		t.Fatalf("users after cleanup = %+v, %v", users, err)
	}
	recreated, err := s.Accounts.CreateUser("eve", "battery staple")
	if err != nil || recreated.Role != account.RoleAdmin {
		t.Fatalf("recreated account = %+v, %v", recreated, err)
	}
}

func TestAdminAccountBanAndUnban(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse")
	bob := login(t, s.URL, "bob", "battery staple")

	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/ban", "", bob.Token); status != http.StatusForbidden {
		t.Fatalf("ban as ordinary user: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/ban", "", admin.Token); status != http.StatusConflict {
		t.Fatalf("ban last usable admin: %d %s", status, body)
	}
	status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/ban", "", admin.Token)
	if status != http.StatusOK {
		t.Fatalf("ban bob: %d %s", status, data)
	}
	var banned adminAccountReply
	if err := json.Unmarshal(data, &banned); err != nil {
		t.Fatalf("ban reply: %v", err)
	}
	if banned.Username != "bob" || !banned.Banned {
		t.Fatalf("banned bob = %+v", banned)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/account", "", bob.Token); status != http.StatusUnauthorized {
		t.Fatalf("banned token: %d %s", status, body)
	}
	refreshBody, err := json.Marshal(map[string]string{"refresh_token": bob.RefreshToken})
	if err != nil {
		t.Fatal(err)
	}
	// The ban revoked the refresh token, so rotation fails like any other
	// retired token instead of revealing the account state.
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/refresh", string(refreshBody), ""); status != http.StatusUnauthorized {
		t.Fatalf("banned refresh: %d %s", status, body)
	}
	bobLogin, err := json.Marshal(map[string]string{"username": "bob", "password": "battery staple"})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", string(bobLogin), ""); status != http.StatusForbidden {
		t.Fatalf("banned login: %d %s", status, body)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/accounts", "", admin.Token)
	if status != http.StatusOK {
		t.Fatalf("list accounts: %d %s", status, data)
	}
	var list adminAccountListReply
	if err := json.Unmarshal(data, &list); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	if len(list.Users) != 2 || list.Users[0].Banned || !list.Users[1].Banned {
		t.Fatalf("accounts after ban = %+v", list.Users)
	}

	status, data = request(t, http.MethodDelete, s.URL+"/v1/accounts/bob/ban", "", admin.Token)
	if status != http.StatusOK {
		t.Fatalf("unban bob: %d %s", status, data)
	}
	var unbanned adminAccountReply
	if err := json.Unmarshal(data, &unbanned); err != nil || unbanned.Banned {
		t.Fatalf("unbanned bob = %+v, %v", unbanned, err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", string(bobLogin), ""); status != http.StatusOK {
		t.Fatalf("login after unban: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/nobody/ban", "", admin.Token); status != http.StatusNotFound {
		t.Fatalf("ban unknown: %d %s", status, body)
	}

	for action, detail := range map[string]string{
		account.AuditAccountBan:   "banned bob",
		account.AuditAccountUnban: "unbanned bob",
	} {
		events, err := s.Accounts.AuditEventsFor(action, 10)
		if err != nil || len(events) != 1 || events[0].Username != "alice" ||
			!events[0].Success || events[0].Detail != detail {
			t.Fatalf("%s audit = %+v, %v", action, events, err)
		}
	}
}

func TestAdminAccountPasswordAndTwoFactorReset(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := s.Accounts.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	now := time.Date(2026, 10, 27, 12, 0, 0, 0, time.UTC)
	s.Accounts.SetClock(func() time.Time { return now })
	secret, err := s.Accounts.StartTwoFactor(bob.ID)
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	code, err := account.TOTPCode(secret, now)
	if err != nil {
		t.Fatalf("totp code: %v", err)
	}
	if err := s.Accounts.EnableTwoFactor(bob.ID, code); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token

	status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/password", `{}`, admin)
	if status != http.StatusOK {
		t.Fatalf("reset password: %d %s", status, data)
	}
	var reset adminAccountReply
	if err := json.Unmarshal(data, &reset); err != nil {
		t.Fatalf("reset reply: %v", err)
	}
	if reset.Password == "" || len(reset.Password) < 8 {
		t.Fatalf("generated password = %q", reset.Password)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login",
		`{"username":"bob","password":"battery staple"}`, ""); status != http.StatusUnauthorized {
		t.Fatalf("old password after reset: %d %s", status, body)
	}
	// TOTP codes are single use, so log in one window later with a fresh code.
	next := now.Add(30 * time.Second)
	s.Accounts.SetClock(func() time.Time { return next })
	loginCode, err := account.TOTPCode(secret, next)
	if err != nil {
		t.Fatalf("next totp code: %v", err)
	}
	body, err := json.Marshal(map[string]string{
		"username": "bob", "password": reset.Password, "totp_code": loginCode,
	})
	if err != nil {
		t.Fatal(err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", string(body), "")
	if status != http.StatusOK {
		t.Fatalf("login with reset password: %d %s", status, data)
	}
	var logged loginReply
	if err := json.Unmarshal(data, &logged); err != nil {
		t.Fatalf("login reply: %v", err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/password",
		`{"password":"new battery staple"}`, admin); status != http.StatusOK || strings.Contains(string(body), "password") {
		t.Fatalf("explicit reset: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/nobody/password", `{}`, admin); status != http.StatusNotFound {
		t.Fatalf("reset unknown password: %d %s", status, body)
	}

	if enabled, err := s.Accounts.TwoFactorEnabled(bob.ID); err != nil || !enabled {
		t.Fatalf("two-factor before reset = %v, %v", enabled, err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/2fa/reset", "", admin); status != http.StatusNoContent {
		t.Fatalf("reset two-factor: %d %s", status, body)
	}
	if enabled, err := s.Accounts.TwoFactorEnabled(bob.ID); err != nil || enabled {
		t.Fatalf("two-factor after reset = %v, %v", enabled, err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/accounts/nobody/2fa/reset", "", admin); status != http.StatusNotFound {
		t.Fatalf("reset unknown two-factor: %d %s", status, body)
	}
	plain, err := json.Marshal(map[string]string{"username": "bob", "password": "new battery staple"})
	if err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/account/login", string(plain), ""); status != http.StatusOK {
		t.Fatalf("login after two-factor reset: %d %s", status, body)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditAccountPassword, 10)
	if err != nil {
		t.Fatalf("password audit: %v", err)
	}
	found := false
	for _, event := range events {
		if event.Username == alice.Username && event.Detail == "reset bob" && event.Success {
			found = true
		}
	}
	if !found {
		t.Fatalf("password audit = %+v", events)
	}
	twoFactorEvents, err := s.Accounts.AuditEventsFor(account.AuditTwoFactorReset, 10)
	if err != nil || len(twoFactorEvents) != 1 ||
		twoFactorEvents[0].Username != "alice" || !twoFactorEvents[0].Success ||
		twoFactorEvents[0].Detail != "reset bob" {
		t.Fatalf("two-factor audit = %+v, %v", twoFactorEvents, err)
	}
}
