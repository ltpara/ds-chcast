package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func enableAccountTwoFactor(t *testing.T, s *testutil.Server, username, password string, at time.Time) string {
	t.Helper()
	token := login(t, s.URL, username, password).Token
	status, data := request(t, http.MethodPost, s.URL+"/v1/account/2fa/start", "", token)
	if status != http.StatusOK {
		t.Fatalf("start two-factor for %s: %d %s", username, status, data)
	}
	var start twoFactorStartReply
	if err := json.Unmarshal(data, &start); err != nil || start.Secret == "" {
		t.Fatalf("start reply for %s: %s (%v)", username, data, err)
	}
	code, err := account.TOTPCode(start.Secret, at)
	if err != nil {
		t.Fatal(err)
	}
	body, err := json.Marshal(map[string]string{"code": code})
	if err != nil {
		t.Fatal(err)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/2fa/enable",
		string(body), token); status != http.StatusNoContent {
		t.Fatalf("enable two-factor for %s: %d %s", username, status, data)
	}
	return token
}

func TestLoginSMSCodeFlow(t *testing.T) {
	smsSender := &fakeSMS{}
	s := testutil.StartWith(t, testutil.Options{SMS: smsSender})
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	s.Accounts.SetClock(func() time.Time { return now })
	if _, err := s.Accounts.CreateUserWithRecovery(
		"alice", "correct horse", "", "+1 (555) 010-0001",
	); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	enableAccountTwoFactor(t, s, "alice", "correct horse", now)

	send := func(username, password string) (int, []byte) {
		body, err := json.Marshal(map[string]string{"username": username, "password": password})
		if err != nil {
			t.Fatal(err)
		}
		return request(t, http.MethodPost, s.URL+"/v1/account/login/sms", string(body), "")
	}
	loginBody := func(username, password, smsCode string) string {
		payload := map[string]string{"username": username, "password": password}
		if smsCode != "" {
			payload["sms_code"] = smsCode
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
	if got := len(smsSender.loginMessages()); got != 0 {
		t.Fatalf("failed requests sent %d SMS", got)
	}

	status, data := send("alice", "correct horse")
	if status != http.StatusAccepted {
		t.Fatalf("request login SMS: %d %s", status, data)
	}
	messages := smsSender.loginMessages()
	if len(messages) != 1 {
		t.Fatalf("login SMS messages = %d", len(messages))
	}
	message := messages[0]
	if message.To != "15550100001" || message.Username != "alice" || len(message.Token) != 8 {
		t.Fatalf("login SMS = %+v", message)
	}
	if status, data := send("alice", "correct horse"); status != http.StatusTooManyRequests {
		t.Fatalf("repeated request: %d %s", status, data)
	}
	if got := len(smsSender.loginMessages()); got != 1 {
		t.Fatalf("throttled request sent %d SMS", got)
	}

	status, body, headers := requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", ""))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "required" {
		t.Fatalf("login without code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", "00000000"))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("login with wrong SMS code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	spaced := message.Token[:4] + " " + message.Token[4:]
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", spaced), "")
	if status != http.StatusOK {
		t.Fatalf("login with SMS code: %d %s", status, data)
	}
	var reply loginReply
	if err := json.Unmarshal(data, &reply); err != nil || reply.Token == "" {
		t.Fatalf("login reply = %s (%v)", data, err)
	}
	status, body, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("alice", "correct horse", message.Token))
	if status != http.StatusUnauthorized || headers.Get("X-Chast-2FA") != "invalid" {
		t.Fatalf("replayed SMS code: %d %s second-factor=%q", status, body, headers.Get("X-Chast-2FA"))
	}

	// An account with two-factor but no recovery phone cannot use SMS, yet the
	// request is answered exactly like a sent code.
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	enableAccountTwoFactor(t, s, "bob", "battery staple", now)
	if status, data := send("bob", "battery staple"); status != http.StatusAccepted {
		t.Fatalf("account without phone: %d %s", status, data)
	}
	if got := len(smsSender.loginMessages()); got != 1 {
		t.Fatalf("account without phone sent %d SMS", got)
	}
	if _, _, headers = requestWithHeaders(t, http.MethodPost, s.URL+"/v1/account/login",
		loginBody("bob", "battery staple", "")); headers.Get("X-Chast-2FA") != "required" {
		t.Fatalf("phone-less login second-factor=%q", headers.Get("X-Chast-2FA"))
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
		if event.Username == "alice" && event.Detail == "login SMS sent" && event.Success {
			sent = true
		}
	}
	if !sent {
		t.Fatal("successful login SMS send is missing from the audit log")
	}
}

func TestLoginSMSCodeRequiresConfiguration(t *testing.T) {
	body := `{"username":"alice","password":"correct horse"}`
	noSMS := testutil.Start(t)
	if status, data := request(t, http.MethodPost, noSMS.URL+"/v1/account/login/sms", body, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("server without SMS: %d %s", status, data)
	}
	noAccounts := testutil.StartWith(t, testutil.Options{NoAccounts: true, SMS: &fakeSMS{}})
	if status, data := request(t, http.MethodPost, noAccounts.URL+"/v1/account/login/sms", body, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("server without accounts: %d %s", status, data)
	}
	withSMS := testutil.StartWith(t, testutil.Options{SMS: &fakeSMS{}})
	if status, data := request(t, http.MethodPost, withSMS.URL+"/v1/account/login/sms", body, ""); status != http.StatusUnauthorized {
		t.Fatalf("unknown account with SMS: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, withSMS.URL+"/v1/account/login/sms", `{`, ""); status != http.StatusBadRequest {
		t.Fatalf("malformed body: %d %s", status, data)
	}
}
