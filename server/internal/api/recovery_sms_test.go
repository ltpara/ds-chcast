package api_test

import (
	"encoding/json"
	"errors"
	"net/http"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type fakeSMS struct {
	mu     sync.Mutex
	sent   []resetMail
	logins []resetMail
	err    error
}

func (f *fakeSMS) SendPasswordReset(to, username, code string, ttl time.Duration) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.err != nil {
		return f.err
	}
	f.sent = append(f.sent, resetMail{To: to, Username: username, Token: code, TTL: ttl})
	return nil
}

func (f *fakeSMS) SendLoginCode(to, username, code string, ttl time.Duration) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.err != nil {
		return f.err
	}
	f.logins = append(f.logins, resetMail{To: to, Username: username, Token: code, TTL: ttl})
	return nil
}

func (f *fakeSMS) messages() []resetMail {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]resetMail(nil), f.sent...)
}

func (f *fakeSMS) loginMessages() []resetMail {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]resetMail(nil), f.logins...)
}

func TestAccountPhoneChange(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUserWithRecovery(
		"alice", "correct horse", "", "+1 (555) 010-0001",
	); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUserWithRecovery(
		"bob", "battery staple", "", "15550100002",
	); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token
	change := func(phone, password, auth string) (int, []byte) {
		body, err := json.Marshal(map[string]string{"phone": phone, "password": password})
		if err != nil {
			t.Fatal(err)
		}
		return request(t, http.MethodPost, s.URL+"/v1/account/phone", string(body), auth)
	}
	if status, data := change("15550100003", "wrong password", token); status != http.StatusForbidden {
		t.Fatalf("wrong password: %d %s", status, data)
	}
	status, data := change("+1 555 010 0003", "correct horse", token)
	if status != http.StatusOK {
		t.Fatalf("set phone: %d %s", status, data)
	}
	var changed struct {
		Phone string `json:"phone"`
	}
	if err := json.Unmarshal(data, &changed); err != nil || changed.Phone != "15550100003" {
		t.Fatalf("set phone reply = %s, err %v", data, err)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account", "", token)
	if status != http.StatusOK {
		t.Fatalf("account info: %d %s", status, data)
	}
	var info struct {
		User struct {
			Phone string `json:"phone"`
		} `json:"user"`
	}
	if err := json.Unmarshal(data, &info); err != nil || info.User.Phone != "15550100003" {
		t.Fatalf("account info phone = %s, err %v", data, err)
	}
	if status, data := change("15550100002", "correct horse", token); status != http.StatusConflict {
		t.Fatalf("duplicate phone: %d %s", status, data)
	}
	if status, data := change("12345", "correct horse", token); status != http.StatusBadRequest {
		t.Fatalf("invalid phone: %d %s", status, data)
	}
	if status, data := change("", "correct horse", ""); status != http.StatusUnauthorized {
		t.Fatalf("change without token: %d %s", status, data)
	}
	status, data = change("", "correct horse", token)
	if status != http.StatusOK {
		t.Fatalf("clear phone: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &changed); err != nil || changed.Phone != "" {
		t.Fatalf("clear phone reply = %s, err %v", data, err)
	}
	user, err := s.Accounts.FindUser("alice")
	if err != nil || user.Phone != "" {
		t.Fatalf("stored phone after clear = %q, err %v", user.Phone, err)
	}
}

func TestAdminAccountPhoneAndSMS(t *testing.T) {
	smsSender := &fakeSMS{}
	s := testutil.StartWith(t, testutil.Options{SMS: smsSender})
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token

	status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"bob","password":"battery staple","phone":"+1 555 010 0002"}`, admin)
	if status != http.StatusCreated {
		t.Fatalf("create bob: %d %s", status, data)
	}
	var bob struct {
		Username string `json:"username"`
		Phone    string `json:"phone"`
	}
	if err := json.Unmarshal(data, &bob); err != nil || bob.Phone != "15550100002" {
		t.Fatalf("create reply = %s, err %v", data, err)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","phone":"15550100002"}`, admin); status != http.StatusConflict {
		t.Fatalf("create with duplicate phone: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","phone":"bad"}`, admin); status != http.StatusBadRequest {
		t.Fatalf("create with invalid phone: %d %s", status, data)
	}

	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/phone",
		`{"phone":"15550100009"}`, admin); status != http.StatusOK {
		t.Fatalf("admin set phone: %d %s", status, data)
	} else if err := json.Unmarshal(data, &bob); err != nil || bob.Phone != "15550100009" {
		t.Fatalf("admin set phone reply = %s, err %v", data, err)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/ghost/phone",
		`{"phone":"15550100009"}`, admin); status != http.StatusNotFound {
		t.Fatalf("unknown account phone: %d %s", status, data)
	}

	status, data = request(t, http.MethodPost, s.URL+"/v1/accounts/bob/sms", "", admin)
	if status != http.StatusAccepted {
		t.Fatalf("admin send SMS: %d %s", status, data)
	}
	msgs := smsSender.messages()
	if len(msgs) != 1 || msgs[0].To != "15550100009" || msgs[0].Username != "bob" {
		t.Fatalf("admin SMS = %+v", msgs)
	}
	code := msgs[0].Token
	if len(code) != 8 {
		t.Fatalf("SMS code = %q", code)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/recover/sms/complete",
		`{"username":"bob","code":"`+code+`","password":"new battery staple"}`, "")
	if status != http.StatusOK {
		t.Fatalf("complete admin SMS reset: %d %s", status, data)
	}

	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/sms", "", admin); status != http.StatusBadRequest {
		t.Fatalf("SMS for account without phone: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/ghost/sms", "", admin); status != http.StatusNotFound {
		t.Fatalf("SMS for unknown account: %d %s", status, data)
	}

	userToken := login(t, s.URL, "bob", "new battery staple").Token
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/phone",
		`{"phone":"15550100010"}`, userToken); status != http.StatusForbidden {
		t.Fatalf("phone change as ordinary user: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/sms", "", userToken); status != http.StatusForbidden {
		t.Fatalf("SMS as ordinary user: %d %s", status, data)
	}
}

func TestAdminSMSUnavailable(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUserWithRecovery("alice", "correct horse", "", "15550100001"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/sms", "", admin); status != http.StatusServiceUnavailable {
		t.Fatalf("admin SMS without gateway: %d %s", status, data)
	}
}

func TestSMSPasswordRecoveryFlow(t *testing.T) {
	smsSender := &fakeSMS{}
	s := testutil.StartWith(t, testutil.Options{SMS: smsSender, SMSTTL: 5 * time.Minute})
	if _, err := s.Accounts.CreateUserWithRecovery("alice", "correct horse", "", "15550100001"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	session := login(t, s.URL, "alice", "correct horse")

	post := func(path, body string) (int, []byte) {
		return request(t, http.MethodPost, s.URL+path, body, "")
	}
	status, data := post("/v1/account/recover/sms", `{"username":"alice"}`)
	if status != http.StatusAccepted {
		t.Fatalf("recover alice: %d %s", status, data)
	}
	var accepted struct {
		Status string `json:"status"`
	}
	if err := json.Unmarshal(data, &accepted); err != nil || accepted.Status != "accepted" {
		t.Fatalf("recover reply = %s, err %v", data, err)
	}
	msgs := smsSender.messages()
	if len(msgs) != 1 {
		t.Fatalf("reset SMS = %d", len(msgs))
	}
	msg := msgs[0]
	if msg.To != "15550100001" || msg.Username != "alice" || len(msg.Token) != 8 || msg.TTL != 5*time.Minute {
		t.Fatalf("reset SMS = %+v", msg)
	}
	if status, data := post("/v1/account/recover/sms", `{"username":"ghost"}`); status != http.StatusAccepted {
		t.Fatalf("recover unknown: %d %s", status, data)
	}
	if status, data := post("/v1/account/recover/sms", `{"username":"bob"}`); status != http.StatusAccepted {
		t.Fatalf("recover without phone: %d %s", status, data)
	}
	if status, data := post("/v1/account/recover/sms", `{"username":"alice"}`); status != http.StatusAccepted {
		t.Fatalf("throttled recover: %d %s", status, data)
	}
	if got := len(smsSender.messages()); got != 1 {
		t.Fatalf("SMS after unknown and throttled requests = %d", got)
	}

	if status, data := post("/v1/account/recover/sms/complete",
		`{"username":"alice","code":"`+msg.Token+`","password":"short"}`); status != http.StatusBadRequest {
		t.Fatalf("weak reset password: %d %s", status, data)
	}
	wrong := "00000000"
	if msg.Token == wrong {
		wrong = "11111111"
	}
	if status, data := post("/v1/account/recover/sms/complete",
		`{"username":"alice","code":"`+wrong+`","password":"correct horse battery"}`); status != http.StatusBadRequest {
		t.Fatalf("wrong SMS code: %d %s", status, data)
	}
	status, data = post("/v1/account/recover/sms/complete",
		`{"username":"alice","code":"`+msg.Token+`","password":"correct horse battery"}`)
	if status != http.StatusOK {
		t.Fatalf("complete SMS reset: %d %s", status, data)
	}
	var completed struct {
		Username string `json:"username"`
	}
	if err := json.Unmarshal(data, &completed); err != nil || completed.Username != "alice" {
		t.Fatalf("complete reply = %s, err %v", data, err)
	}
	if status, data := post("/v1/account/recover/sms/complete",
		`{"username":"alice","code":"`+msg.Token+`","password":"another secret phrase"}`); status != http.StatusBadRequest {
		t.Fatalf("reused SMS code: %d %s", status, data)
	}
	if status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", session.Token); status != http.StatusUnauthorized {
		t.Fatalf("session survived SMS reset: %d %s", status, data)
	}
	if status, data := post("/v1/account/login",
		`{"username":"alice","password":"correct horse"}`); status != http.StatusUnauthorized {
		t.Fatalf("old password after SMS reset: %d %s", status, data)
	}
	if login(t, s.URL, "alice", "correct horse battery").User.Username != "alice" {
		t.Fatal("new password does not log in")
	}
}

func TestSMSPasswordRecoveryFailureStaysGeneric(t *testing.T) {
	smsSender := &fakeSMS{}
	s := testutil.StartWith(t, testutil.Options{SMS: smsSender})
	if _, err := s.Accounts.CreateUserWithRecovery("alice", "correct horse", "", "15550100001"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	smsSender.mu.Lock()
	smsSender.err = errors.New("gateway is down")
	smsSender.mu.Unlock()
	status, data := request(t, http.MethodPost, s.URL+"/v1/account/recover/sms",
		`{"username":"alice"}`, "")
	if status != http.StatusAccepted {
		t.Fatalf("recover with failing gateway: %d %s", status, data)
	}
	if got := len(smsSender.messages()); got != 0 {
		t.Fatalf("failed gateway recorded %d messages", got)
	}
}

func TestSMSPasswordRecoveryUnavailable(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUserWithRecovery("alice", "correct horse", "", "15550100001"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/recover/sms",
		`{"username":"alice"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("SMS recover without gateway: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/recover/sms/complete",
		`{"username":"alice","code":"12345678","password":"correct horse battery"}`, ""); status != http.StatusBadRequest {
		t.Fatalf("complete without outstanding code: %d %s", status, data)
	}

	noStore := testutil.StartWith(t, testutil.Options{NoAccounts: true, SMS: &fakeSMS{}})
	if status, data := request(t, http.MethodPost, noStore.URL+"/v1/account/recover/sms",
		`{"username":"alice"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("SMS recover without store: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, noStore.URL+"/v1/account/recover/sms/complete",
		`{"username":"alice","code":"12345678","password":"correct horse battery"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("SMS complete without store: %d %s", status, data)
	}
}
