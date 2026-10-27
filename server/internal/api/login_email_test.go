package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestLoginEmailCodeFlow(t *testing.T) {
	mailer := &fakeMailer{}
	s := testutil.StartWith(t, testutil.Options{Mailer: mailer})
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	s.Accounts.SetClock(func() time.Time { return now })
	if _, err := s.Accounts.CreateUserWithRecovery(
		"alice", "correct horse", "alice@example.com", "",
	); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	enableAccountTwoFactor(t, s, "alice", "correct horse", now)

	send := func(username, password string) (int, []byte) {
		body, err := json.Marshal(map[string]string{"username": username, "password": password})
		if err != nil {
			t.Fatal(err)
		}
		return request(t, http.MethodPost, s.URL+"/v1/account/login/email", string(body), "")
	}
	loginBody := func(username, password, emailCode string) string {
		payload := map[string]string{"username": username, "password": password}
		if emailCode != "" {
			payload["email_code"] = emailCode
		}
		body, err := json.Marshal(payload)
		if err != nil {
			t.Fatal(err)
		}
		return string(body)
	}

	if status, data := send("alice", "wrong horse"); status != http.StatusUnauthorized {
		t.Fatalf("wrong password: %d %s", status, data)
	}
	if status, _ := send("nobody", "wrong horse"); status != http.StatusUnauthorized {
		t.Fatalf("unknown account: %d", status)
	}
	if got := len(mailer.loginMessages()); got != 0 {
		t.Fatalf("failed requests sent %d messages", got)
	}

	status, data := send("alice", "correct horse")
	if status != http.StatusAccepted {
		t.Fatalf("request login email: %d %s", status, data)
	}
	messages := mailer.loginMessages()
	if len(messages) != 1 {
		t.Fatalf("login email messages = %d", len(messages))
	}
	message := messages[0]
	if message.To != "alice@example.com" || message.Username != "alice" ||
		len(message.Token) != 8 || message.TTL != 30*time.Minute {
		t.Fatalf("login email = %+v", message)
	}
	if status, data := send("alice", "correct horse"); status != http.StatusTooManyRequests {
		t.Fatalf("repeated request: %d %s", status, data)
	}
	if got := len(mailer.loginMessages()); got != 1 {
		t.Fatalf("throttled request sent %d messages", got)
	}

	status, body, headers := requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", ""))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "required" {
		t.Fatalf("login without code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", "00000000"))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("login with wrong email code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	spaced := message.Token[:4] + " " + message.Token[4:]
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", spaced), "")
	if status != http.StatusOK {
		t.Fatalf("login with email code: %d %s", status, data)
	}
	var reply loginReply
	if err := json.Unmarshal(data, &reply); err != nil || reply.Token == "" {
		t.Fatalf("login reply = %s (%v)", data, err)
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", message.Token))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("replayed email code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	// An account with two-factor but no recovery email cannot use the channel,
	// yet the request is answered exactly like a sent code.
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	enableAccountTwoFactor(t, s, "bob", "battery staple", now)
	if status, data := send("bob", "battery staple"); status != http.StatusAccepted {
		t.Fatalf("account without email: %d %s", status, data)
	}
	if got := len(mailer.loginMessages()); got != 1 {
		t.Fatalf("account without email sent %d messages", got)
	}
	if _, _, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("bob", "battery staple", "")); headers.Get("X-Chast-2FA") != "required" {
		t.Fatalf("email-less login second-factor=%q", headers.Get("X-Chast-2FA"))
	}

	if _, err := s.Accounts.SetBanned("bob", true); err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	if status, data := send("bob", "battery staple"); status != http.StatusForbidden {
		t.Fatalf("banned account: %d %s", status, data)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditLogin, 50)
	if err != nil {
		t.Fatalf("audit events: %v", err)
	}
	sent := false
	for _, event := range events {
		if event.Username == "alice" && event.Detail == "login email sent" && event.Success {
			sent = true
		}
	}
	if !sent {
		t.Fatal("successful login email send is missing from the audit log")
	}
}

func TestLoginEmailCodeRequiresConfiguration(t *testing.T) {
	body := `{"username":"alice","password":"correct horse"}`
	noMail := testutil.Start(t)
	if status, data := request(t, http.MethodPost, noMail.URL+"/v1/account/login/email", body, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("server without mail: %d %s", status, data)
	}
	noAccounts := testutil.StartWith(t, testutil.Options{NoAccounts: true, Mailer: &fakeMailer{}})
	if status, data := request(t, http.MethodPost, noAccounts.URL+"/v1/account/login/email", body, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("server without accounts: %d %s", status, data)
	}
	withMail := testutil.StartWith(t, testutil.Options{Mailer: &fakeMailer{}})
	if status, data := request(t, http.MethodPost, withMail.URL+"/v1/account/login/email", body, ""); status != http.StatusUnauthorized {
		t.Fatalf("unknown account with mail: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, withMail.URL+"/v1/account/login/email", `{`, ""); status != http.StatusBadRequest {
		t.Fatalf("malformed body: %d %s", status, data)
	}
}
