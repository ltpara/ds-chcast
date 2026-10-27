package api_test

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type resetMail struct {
	To       string
	Username string
	Token    string
	Link     string
	TTL      time.Duration
}

type fakeMailer struct {
	mu     sync.Mutex
	sent   []resetMail
	logins []resetMail
	err    error
}

func (f *fakeMailer) SendPasswordReset(to, username, token, link string, ttl time.Duration) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.err != nil {
		return f.err
	}
	f.sent = append(f.sent, resetMail{To: to, Username: username, Token: token, Link: link, TTL: ttl})
	return nil
}

func (f *fakeMailer) SendLoginCode(to, username, code string, ttl time.Duration) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.err != nil {
		return f.err
	}
	f.logins = append(f.logins, resetMail{To: to, Username: username, Token: code, TTL: ttl})
	return nil
}

func (f *fakeMailer) messages() []resetMail {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]resetMail(nil), f.sent...)
}

func (f *fakeMailer) loginMessages() []resetMail {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]resetMail(nil), f.logins...)
}

func TestAccountEmailChange(t *testing.T) {
	s := testutil.Start(t)
	alice, err := s.Accounts.CreateUserWithEmail("alice", "correct horse", "alice@example.com")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if alice.Email != "alice@example.com" {
		t.Fatalf("alice email = %q", alice.Email)
	}
	if _, err := s.Accounts.CreateUserWithEmail("bob", "battery staple", "bob@example.com"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token

	change := func(email, password, auth string) (int, []byte) {
		body, err := json.Marshal(map[string]string{"email": email, "password": password})
		if err != nil {
			t.Fatal(err)
		}
		return request(t, http.MethodPost, s.URL+"/v1/account/email", string(body), auth)
	}

	if status, data := change("new@example.com", "wrong password", token); status != http.StatusForbidden {
		t.Fatalf("wrong password: %d %s", status, data)
	}
	status, data := change("new@example.com", "correct horse", token)
	if status != http.StatusOK {
		t.Fatalf("set email: %d %s", status, data)
	}
	var changed struct {
		Email string `json:"email"`
	}
	if err := json.Unmarshal(data, &changed); err != nil {
		t.Fatalf("set email reply: %v", err)
	}
	if changed.Email != "new@example.com" {
		t.Fatalf("set email reply = %+v", changed)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account", "", token)
	if status != http.StatusOK {
		t.Fatalf("account info: %d %s", status, data)
	}
	var info struct {
		User struct {
			Email string `json:"email"`
		} `json:"user"`
	}
	if err := json.Unmarshal(data, &info); err != nil {
		t.Fatalf("account info reply: %v", err)
	}
	if info.User.Email != "new@example.com" {
		t.Fatalf("account info email = %q", info.User.Email)
	}

	if status, data := change("BOB@example.com", "correct horse", token); status != http.StatusConflict {
		t.Fatalf("duplicate email: %d %s", status, data)
	}
	if status, data := change("not-an-address", "correct horse", token); status != http.StatusBadRequest {
		t.Fatalf("invalid email: %d %s", status, data)
	}
	status, data = change("", "correct horse", token)
	if status != http.StatusOK {
		t.Fatalf("clear email: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &changed); err != nil {
		t.Fatalf("clear email reply: %v", err)
	}
	if changed.Email != "" {
		t.Fatalf("cleared email reply = %+v", changed)
	}
	user, err := s.Accounts.FindUser("alice")
	if err != nil || user.Email != "" {
		t.Fatalf("stored email after clear = %q, err %v", user.Email, err)
	}
	if status, data := change("new@example.com", "correct horse", ""); status != http.StatusUnauthorized {
		t.Fatalf("change without token: %d %s", status, data)
	}
}

func TestAdminAccountEmail(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token

	status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"bob","password":"battery staple","email":"bob@example.com"}`, admin)
	if status != http.StatusCreated {
		t.Fatalf("create bob: %d %s", status, data)
	}
	var bob struct {
		Username string `json:"username"`
		Email    string `json:"email"`
	}
	if err := json.Unmarshal(data, &bob); err != nil {
		t.Fatalf("create reply: %v", err)
	}
	if bob.Username != "bob" || bob.Email != "bob@example.com" {
		t.Fatalf("created bob = %+v", bob)
	}

	status, data = request(t, http.MethodPost, s.URL+"/v1/accounts/bob/email",
		`{"email":"robert@example.com"}`, admin)
	if status != http.StatusOK {
		t.Fatalf("admin set email: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &bob); err != nil {
		t.Fatalf("set email reply: %v", err)
	}
	if bob.Email != "robert@example.com" {
		t.Fatalf("set email reply = %+v", bob)
	}

	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/alice/email",
		`{"email":"ROBERT@example.com"}`, admin); status != http.StatusConflict {
		t.Fatalf("duplicate admin email: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/email",
		`{"email":"not-an-address"}`, admin); status != http.StatusBadRequest {
		t.Fatalf("invalid admin email: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/ghost/email",
		`{"email":"ghost@example.com"}`, admin); status != http.StatusNotFound {
		t.Fatalf("unknown account email: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","email":"Robert@example.com"}`, admin); status != http.StatusConflict {
		t.Fatalf("create with duplicate email: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts",
		`{"username":"carol","email":"bad"}`, admin); status != http.StatusBadRequest {
		t.Fatalf("create with invalid email: %d %s", status, data)
	}

	userToken := login(t, s.URL, "bob", "battery staple").Token
	if status, data := request(t, http.MethodPost, s.URL+"/v1/accounts/bob/email",
		`{"email":"bob@example.com"}`, userToken); status != http.StatusForbidden {
		t.Fatalf("email change as ordinary user: %d %s", status, data)
	}
}

func TestPasswordRecoveryFlow(t *testing.T) {
	mailer := &fakeMailer{}
	s := testutil.StartWith(t, testutil.Options{
		Mailer:      mailer,
		RecoveryURL: "https://chast.example/reset?lang=en",
		RecoveryTTL: 15 * time.Minute,
	})
	if _, err := s.Accounts.CreateUserWithEmail("alice", "correct horse", "alice@example.com"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	session := login(t, s.URL, "alice", "correct horse")

	post := func(path, body string) (int, []byte) {
		return request(t, http.MethodPost, s.URL+path, body, "")
	}
	status, data := post("/v1/account/recover", `{"username":"alice"}`)
	if status != http.StatusAccepted {
		t.Fatalf("recover alice: %d %s", status, data)
	}
	var accepted struct {
		Status string `json:"status"`
	}
	if err := json.Unmarshal(data, &accepted); err != nil || accepted.Status != "accepted" {
		t.Fatalf("recover reply = %s, err %v", data, err)
	}
	msgs := mailer.messages()
	if len(msgs) != 1 {
		t.Fatalf("reset mails = %d", len(msgs))
	}
	msg := msgs[0]
	if msg.To != "alice@example.com" || msg.Username != "alice" || msg.Token == "" || msg.TTL != 15*time.Minute {
		t.Fatalf("reset mail = %+v", msg)
	}
	link, err := url.Parse(msg.Link)
	if err != nil {
		t.Fatalf("reset link %q: %v", msg.Link, err)
	}
	if link.Scheme != "https" || link.Host != "chast.example" || link.Path != "/reset" ||
		link.Query().Get("lang") != "en" || link.Query().Get("token") != msg.Token {
		t.Fatalf("reset link = %q", msg.Link)
	}

	if status, data := post("/v1/account/recover", `{"username":"ghost"}`); status != http.StatusAccepted {
		t.Fatalf("recover unknown: %d %s", status, data)
	}
	if status, data := post("/v1/account/recover", `{"username":"bob"}`); status != http.StatusAccepted {
		t.Fatalf("recover without email: %d %s", status, data)
	}
	if status, data := post("/v1/account/recover", `{"username":"alice"}`); status != http.StatusAccepted {
		t.Fatalf("throttled recover: %d %s", status, data)
	}
	if got := len(mailer.messages()); got != 1 {
		t.Fatalf("mails after unknown and throttled requests = %d", got)
	}

	if status, data := post("/v1/account/recover/complete",
		`{"token":"`+msg.Token+`","password":"short"}`); status != http.StatusBadRequest {
		t.Fatalf("weak reset password: %d %s", status, data)
	}
	status, data = post("/v1/account/recover/complete",
		`{"token":"`+msg.Token+`","password":"correct horse battery"}`)
	if status != http.StatusOK {
		t.Fatalf("complete reset: %d %s", status, data)
	}
	var completed struct {
		Username string `json:"username"`
	}
	if err := json.Unmarshal(data, &completed); err != nil || completed.Username != "alice" {
		t.Fatalf("complete reply = %s, err %v", data, err)
	}
	if status, data := post("/v1/account/recover/complete",
		`{"token":"`+msg.Token+`","password":"another secret phrase"}`); status != http.StatusBadRequest {
		t.Fatalf("reused reset token: %d %s", status, data)
	}
	if status, data := request(t, http.MethodGet, s.URL+"/v1/account", "", session.Token); status != http.StatusUnauthorized {
		t.Fatalf("session survived password reset: %d %s", status, data)
	}
	if status, data := post("/v1/account/login",
		`{"username":"alice","password":"correct horse"}`); status != http.StatusUnauthorized {
		t.Fatalf("old password after reset: %d %s", status, data)
	}
	if login(t, s.URL, "alice", "correct horse battery").User.Username != "alice" {
		t.Fatal("new password does not log in")
	}

	expired, _, _, err := s.Accounts.CreatePasswordReset("alice", time.Millisecond)
	if err != nil {
		t.Fatalf("create expiring reset: %v", err)
	}
	time.Sleep(10 * time.Millisecond)
	if status, data := post("/v1/account/recover/complete",
		`{"token":"`+expired+`","password":"yetanother phrase"}`); status != http.StatusBadRequest {
		t.Fatalf("expired reset token: %d %s", status, data)
	}

	first, _, _, err := s.Accounts.CreatePasswordReset("alice", time.Minute)
	if err != nil {
		t.Fatalf("create first reset: %v", err)
	}
	second, _, _, err := s.Accounts.CreatePasswordReset("alice", time.Minute)
	if err != nil {
		t.Fatalf("create second reset: %v", err)
	}
	if status, data := post("/v1/account/recover/complete",
		`{"token":"`+first+`","password":"replaced reset phrase"}`); status != http.StatusBadRequest {
		t.Fatalf("replaced reset token: %d %s", status, data)
	}
	if status, data := post("/v1/account/recover/complete",
		`{"token":"`+second+`","password":"replaced reset phrase"}`); status != http.StatusOK {
		t.Fatalf("replacement reset token: %d %s", status, data)
	}
}

func TestPasswordRecoveryMailFailureStaysGeneric(t *testing.T) {
	mailer := &fakeMailer{}
	s := testutil.StartWith(t, testutil.Options{Mailer: mailer})
	if _, err := s.Accounts.CreateUserWithEmail("alice", "correct horse", "alice@example.com"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	mailer.mu.Lock()
	mailer.err = errors.New("smtp is down")
	mailer.mu.Unlock()
	status, data := request(t, http.MethodPost, s.URL+"/v1/account/recover",
		`{"username":"alice"}`, "")
	if status != http.StatusAccepted {
		t.Fatalf("recover with failing mailer: %d %s", status, data)
	}
	if got := len(mailer.messages()); got != 0 {
		t.Fatalf("failed mailer recorded %d messages", got)
	}
}

func TestPasswordRecoveryUnavailable(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUserWithEmail("alice", "correct horse", "alice@example.com"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/recover",
		`{"username":"alice"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("recover without mailer: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/recover/complete",
		`{"token":"whatever","password":"correct horse battery"}`, ""); status != http.StatusBadRequest {
		t.Fatalf("complete without mailer: %d %s", status, data)
	}

	noStore := testutil.StartWith(t, testutil.Options{NoAccounts: true, Mailer: &fakeMailer{}})
	if status, data := request(t, http.MethodPost, noStore.URL+"/v1/account/recover",
		`{"username":"alice"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("recover without store: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, noStore.URL+"/v1/account/recover/complete",
		`{"token":"whatever","password":"correct horse battery"}`, ""); status != http.StatusServiceUnavailable {
		t.Fatalf("complete without store: %d %s", status, data)
	}
}
