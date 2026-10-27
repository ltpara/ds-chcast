package api_test

import (
	"encoding/base64"
	"encoding/json"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/webauthn"
	"github.com/ltpara/ds-chcast/server/internal/webauthn/webauthntest"
)

type webauthnTestServer struct {
	URL      string
	Origin   string
	RPID     string
	Accounts *account.Store
}

// startWebAuthnAPI runs the real handler on a fixed loopback listener, so the
// origin allowlist can name the exact port before the server starts.
func startWebAuthnAPI(t *testing.T, limiter *authlimit.Limiter) *webauthnTestServer {
	t.Helper()
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatalf("open accounts: %v", err)
	}
	t.Cleanup(func() { store.Close() })
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	origin := "http://" + listener.Addr().String()
	handler := api.New(api.Config{
		Accounts:        store,
		LoginLimiter:    limiter,
		WebAuthnRPID:    "127.0.0.1",
		WebAuthnOrigins: []string{origin},
		Logger:          slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	server := httptest.NewUnstartedServer(handler)
	server.Listener.Close()
	server.Listener = listener
	server.Start()
	t.Cleanup(server.Close)
	return &webauthnTestServer{URL: server.URL, Origin: origin, RPID: "127.0.0.1", Accounts: store}
}

type webauthnCredentialReply struct {
	ID          string `json:"id"`
	Alg         int    `json:"alg"`
	Name        string `json:"name"`
	CreatedAtMS int64  `json:"created_at_ms"`
	LastUsedMS  int64  `json:"last_used_at_ms"`
}

type webauthnBeginReply struct {
	Challenge          string                    `json:"challenge"`
	RPID               string                    `json:"rp_id"`
	UserID             string                    `json:"user_id"`
	Username           string                    `json:"username"`
	TimeoutMS          int                       `json:"timeout_ms"`
	ExcludeCredentials []webauthnCredentialReply `json:"exclude_credentials"`
	Credentials        []webauthnCredentialReply `json:"credentials"`
}

func webauthnFinishBody(fields map[string]string) string {
	body, err := json.Marshal(fields)
	if err != nil {
		panic(err)
	}
	return string(body)
}

func TestWebAuthnSecurityKeyFlow(t *testing.T) {
	s := startWebAuthnAPI(t, nil)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token

	status, data := request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/begin",
		`{"password":"wrong horse"}`, token)
	if status != http.StatusForbidden {
		t.Fatalf("register begin with wrong password: %d %s", status, data)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account/webauthn/credentials", "", "")
	if status != http.StatusUnauthorized {
		t.Fatalf("list without token: %d %s", status, data)
	}

	auth, err := webauthntest.New(webauthn.AlgES256, s.RPID, s.Origin)
	if err != nil {
		t.Fatalf("authenticator: %v", err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/begin",
		`{"password":"correct horse"}`, token)
	if status != http.StatusOK {
		t.Fatalf("register begin: %d %s", status, data)
	}
	var begin webauthnBeginReply
	if err := json.Unmarshal(data, &begin); err != nil {
		t.Fatalf("register begin reply: %v", err)
	}
	if begin.RPID != s.RPID || begin.TimeoutMS <= 0 || begin.UserID == "" || begin.Username != "alice" {
		t.Fatalf("register begin = %+v", begin)
	}
	if len(begin.ExcludeCredentials) != 0 {
		t.Fatalf("first registration exclude list = %+v", begin.ExcludeCredentials)
	}
	challenge, err := base64.RawURLEncoding.DecodeString(begin.Challenge)
	if err != nil {
		t.Fatalf("begin challenge: %v", err)
	}
	// A response from a different origin is rejected, and the challenge stays
	// usable for the correct authenticator.
	evil, err := webauthntest.New(webauthn.AlgES256, s.RPID, "http://evil.example")
	if err != nil {
		t.Fatalf("evil authenticator: %v", err)
	}
	evilClient, evilAttestation, err := evil.MakeCredential(challenge)
	if err != nil {
		t.Fatalf("evil credential: %v", err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/finish",
		webauthnFinishBody(map[string]string{
			"name":               "evil",
			"client_data_json":   base64.RawURLEncoding.EncodeToString(evilClient),
			"attestation_object": base64.RawURLEncoding.EncodeToString(evilAttestation),
		}), token)
	if status != http.StatusForbidden {
		t.Fatalf("foreign origin attestation: %d %s", status, data)
	}
	clientData, attestation, err := auth.MakeCredential(challenge)
	if err != nil {
		t.Fatalf("make credential: %v", err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/finish",
		webauthnFinishBody(map[string]string{
			"name":               "Office key",
			"client_data_json":   base64.RawURLEncoding.EncodeToString(clientData),
			"attestation_object": base64.RawURLEncoding.EncodeToString(attestation),
		}), token)
	if status != http.StatusOK {
		t.Fatalf("register finish: %d %s", status, data)
	}
	var stored webauthnCredentialReply
	if err := json.Unmarshal(data, &stored); err != nil {
		t.Fatalf("register finish reply: %v", err)
	}
	credential := auth.Credential()
	if stored.ID != base64.RawURLEncoding.EncodeToString(credential.ID) || stored.Alg != webauthn.AlgES256 ||
		stored.Name != "Office key" || stored.CreatedAtMS == 0 {
		t.Fatalf("stored credential = %+v", stored)
	}

	status, data = request(t, http.MethodGet, s.URL+"/v1/account/webauthn/credentials", "", token)
	if status != http.StatusOK {
		t.Fatalf("list credentials: %d %s", status, data)
	}
	var list struct {
		Credentials []webauthnCredentialReply `json:"credentials"`
	}
	if err := json.Unmarshal(data, &list); err != nil || len(list.Credentials) != 1 ||
		list.Credentials[0].ID != stored.ID {
		t.Fatalf("credentials = %s (%v)", data, err)
	}

	// A second registration ceremony lists the first key for exclusion.
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/begin",
		`{"password":"correct horse"}`, token)
	if status != http.StatusOK {
		t.Fatalf("second register begin: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &begin); err != nil {
		t.Fatalf("second register begin reply: %v", err)
	}
	if len(begin.ExcludeCredentials) != 1 || begin.ExcludeCredentials[0].ID != stored.ID {
		t.Fatalf("exclude list = %+v", begin.ExcludeCredentials)
	}

	// Security-key login: password first, then an assertion.
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
		`{"username":"alice","password":"wrong horse"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("login begin with wrong password: %d %s", status, data)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
		`{"username":"nobody","password":"wrong horse"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("login begin for unknown user: %d %s", status, data)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
		`{"username":"alice","password":"correct horse"}`, "")
	if status != http.StatusOK {
		t.Fatalf("login begin: %d %s", status, data)
	}
	var loginBegin webauthnBeginReply
	if err := json.Unmarshal(data, &loginBegin); err != nil || loginBegin.RPID != s.RPID ||
		len(loginBegin.Credentials) != 1 || loginBegin.Credentials[0].ID != stored.ID {
		t.Fatalf("login begin = %s (%v)", data, err)
	}
	loginChallenge, err := base64.RawURLEncoding.DecodeString(loginBegin.Challenge)
	if err != nil {
		t.Fatalf("login challenge: %v", err)
	}
	assertClientData, authenticatorData, signature, err := auth.Assert(loginChallenge)
	if err != nil {
		t.Fatalf("assert: %v", err)
	}
	assertionBody := webauthnFinishBody(map[string]string{
		"username":           "alice",
		"credential_id":      stored.ID,
		"client_data_json":   base64.RawURLEncoding.EncodeToString(assertClientData),
		"authenticator_data": base64.RawURLEncoding.EncodeToString(authenticatorData),
		"signature":          base64.RawURLEncoding.EncodeToString(signature),
		"device_name":        "Desk",
	})
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/finish", assertionBody, "")
	if status != http.StatusOK {
		t.Fatalf("login finish: %d %s", status, data)
	}
	var reply loginReply
	if err := json.Unmarshal(data, &reply); err != nil || reply.Token == "" || reply.RefreshToken == "" ||
		reply.User.Username != "alice" {
		t.Fatalf("login reply = %s (%v)", data, err)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account", "", reply.Token)
	if status != http.StatusOK {
		t.Fatalf("issued token: %d %s", status, data)
	}
	// The assertion challenge is single-use.
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/finish", assertionBody, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("replayed assertion: %d %s", status, data)
	}

	// Removal requires the password.
	removeBody := func(password string) string {
		return webauthnFinishBody(map[string]string{"password": password, "credential_id": stored.ID})
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/credentials/remove", removeBody("wrong horse"), token)
	if status != http.StatusForbidden {
		t.Fatalf("remove with wrong password: %d %s", status, data)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/credentials/remove", removeBody("correct horse"), token)
	if status != http.StatusNoContent {
		t.Fatalf("remove credential: %d %s", status, data)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account/webauthn/credentials", "", token)
	if status != http.StatusOK || !strings.Contains(string(data), `"credentials":[]`) {
		t.Fatalf("credentials after remove: %d %s", status, data)
	}
	// Without keys the login start is answered like an enabled account so it
	// cannot be used to enumerate who registered a security key.
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
		`{"username":"alice","password":"correct horse"}`, "")
	if status != http.StatusAccepted || !strings.Contains(string(data), "accepted") {
		t.Fatalf("login begin without keys: %d %s", status, data)
	}

	events, err := s.Accounts.AuditEventsFor(account.AuditWebAuthnRegister, 10)
	if err != nil || len(events) == 0 || !events[0].Success {
		t.Fatalf("register audit = %+v (%v)", events, err)
	}
	events, err = s.Accounts.AuditEventsFor(account.AuditLogin, 10)
	if err != nil || len(events) == 0 {
		t.Fatalf("login audit: %v", err)
	}
	found := false
	for _, event := range events {
		if event.Success && strings.Contains(event.Detail, "password+security-key") &&
			strings.Contains(event.Detail, "device=Desk") {
			found = true
		}
	}
	if !found {
		t.Fatalf("login audit has no security-key entry: %+v", events)
	}
}

func TestWebAuthnAssertionRejections(t *testing.T) {
	s := startWebAuthnAPI(t, nil)
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	auth, err := webauthntest.New(webauthn.AlgES256, s.RPID, s.Origin)
	if err != nil {
		t.Fatalf("authenticator: %v", err)
	}
	registrationChallenge := []byte("register-challenge")
	if _, err := s.Accounts.IssueWebAuthnChallenge(alice.ID, account.WebAuthnRegister, registrationChallenge, time.Minute); err != nil {
		t.Fatalf("issue registration challenge: %v", err)
	}
	credential := auth.Credential()
	if _, err := s.Accounts.FinishWebAuthnRegistration(alice.ID, registrationChallenge, account.WebAuthnCredential{
		ID:        credential.ID,
		PublicKey: credential.PublicKey,
		Alg:       credential.Alg,
		SignCount: credential.SignCount,
	}, "key"); err != nil {
		t.Fatalf("store credential: %v", err)
	}
	credentialID := base64.RawURLEncoding.EncodeToString(credential.ID)

	begin := func() []byte {
		t.Helper()
		status, data := request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
			`{"username":"alice","password":"correct horse"}`, "")
		if status != http.StatusOK {
			t.Fatalf("login begin: %d %s", status, data)
		}
		var reply webauthnBeginReply
		if err := json.Unmarshal(data, &reply); err != nil {
			t.Fatalf("login begin reply: %v", err)
		}
		challenge, err := base64.RawURLEncoding.DecodeString(reply.Challenge)
		if err != nil {
			t.Fatalf("login challenge: %v", err)
		}
		return challenge
	}
	finish := func(challenge []byte, mutate func(clientData, authData, signature []byte) ([]byte, []byte, []byte)) (int, []byte) {
		t.Helper()
		clientData, authData, signature, err := auth.Assert(challenge)
		if err != nil {
			t.Fatalf("assert: %v", err)
		}
		if mutate != nil {
			clientData, authData, signature = mutate(clientData, authData, signature)
		}
		return request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/finish",
			webauthnFinishBody(map[string]string{
				"username":           "alice",
				"credential_id":      credentialID,
				"client_data_json":   base64.RawURLEncoding.EncodeToString(clientData),
				"authenticator_data": base64.RawURLEncoding.EncodeToString(authData),
				"signature":          base64.RawURLEncoding.EncodeToString(signature),
			}), "")
	}

	// A tampered signature is rejected and the challenge survives for a retry.
	challenge := begin()
	status, data := finish(challenge, func(clientData, authData, signature []byte) ([]byte, []byte, []byte) {
		return clientData, authData, webauthntest.TamperSignature(signature)
	})
	if status != http.StatusUnauthorized {
		t.Fatalf("tampered signature: %d %s", status, data)
	}
	// Clearing the user-presence flag is rejected too.
	status, data = finish(challenge, func([]byte, []byte, []byte) ([]byte, []byte, []byte) {
		clientData, authData, signature, err := auth.AssertWithFlags(challenge, 0x00)
		if err != nil {
			t.Fatalf("assert without presence: %v", err)
		}
		return clientData, authData, signature
	})
	if status != http.StatusUnauthorized {
		t.Fatalf("missing user presence: %d %s", status, data)
	}
	// An assertion for a foreign credential ID is rejected.
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/finish",
		webauthnFinishBody(map[string]string{
			"username":           "alice",
			"credential_id":      base64.RawURLEncoding.EncodeToString([]byte("other")),
			"client_data_json":   base64.RawURLEncoding.EncodeToString([]byte("{}")),
			"authenticator_data": base64.RawURLEncoding.EncodeToString([]byte("{}")),
			"signature":          base64.RawURLEncoding.EncodeToString([]byte("{}")),
		}), "")
	if status != http.StatusUnauthorized {
		t.Fatalf("foreign credential: %d %s", status, data)
	}
	// The challenge is still valid after the failed attempts.
	if status, data := finish(challenge, nil); status != http.StatusOK {
		t.Fatalf("valid assertion after failures: %d %s", status, data)
	}

	// A banned account cannot start or finish a security-key login.
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.SetRole("bob", account.RoleAdmin); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	if _, err := s.Accounts.SetBanned("alice", true); err != nil {
		t.Fatalf("ban alice: %v", err)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
		`{"username":"alice","password":"correct horse"}`, "")
	if status != http.StatusForbidden {
		t.Fatalf("banned login begin: %d %s", status, data)
	}
}

func TestWebAuthnRoutesRequireConfiguration(t *testing.T) {
	s := startWebAuthnAPI(t, nil)
	// The same store with an unconfigured handler answers 503 on every route.
	handler := api.New(api.Config{
		Accounts: s.Accounts,
		Logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	})
	server := httptest.NewServer(handler)
	defer server.Close()
	status, data := request(t, http.MethodPost, server.URL+"/v1/account/login/webauthn/begin",
		`{"username":"alice","password":"correct horse"}`, "")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("unconfigured begin login: %d %s", status, data)
	}
	status, data = request(t, http.MethodGet, server.URL+"/v1/account/webauthn/credentials", "", "token")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("unconfigured list: %d %s", status, data)
	}

	// A missing registration challenge means the ceremony was never started.
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	token := login(t, s.URL, "alice", "correct horse").Token
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/finish",
		`{"client_data_json":"e30","attestation_object":"e30"}`, token)
	if status != http.StatusForbidden {
		t.Fatalf("finish without begin: %d %s", status, data)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/webauthn/register/begin",
		`{"password":""}`, token)
	if status != http.StatusForbidden {
		t.Fatalf("empty password: %d %s", status, data)
	}
}

func TestWebAuthnLoginLockout(t *testing.T) {
	limiter := authlimit.New(authlimit.Config{Window: time.Minute, MaxFailures: 2, Lockout: time.Minute})
	s := startWebAuthnAPI(t, limiter)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	auth, err := webauthntest.New(webauthn.AlgES256, s.RPID, s.Origin)
	if err != nil {
		t.Fatalf("authenticator: %v", err)
	}
	alice, err := s.Accounts.FindUser("alice")
	if err != nil {
		t.Fatalf("find alice: %v", err)
	}
	challenge := []byte("register")
	if _, err := s.Accounts.IssueWebAuthnChallenge(alice.ID, account.WebAuthnRegister, challenge, time.Minute); err != nil {
		t.Fatalf("issue challenge: %v", err)
	}
	credential := auth.Credential()
	if _, err := s.Accounts.FinishWebAuthnRegistration(alice.ID, challenge, account.WebAuthnCredential{
		ID: credential.ID, PublicKey: credential.PublicKey, Alg: credential.Alg,
	}, "key"); err != nil {
		t.Fatalf("store credential: %v", err)
	}
	bad := `{"username":"alice","password":"wrong horse"}`
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin", bad, ""); status != http.StatusUnauthorized {
		t.Fatalf("first failure: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin", bad, ""); status != http.StatusTooManyRequests {
		t.Fatalf("second failure: %d %s", status, data)
	}
	if status, data := request(t, http.MethodPost, s.URL+"/v1/account/login/webauthn/begin",
		`{"username":"alice","password":"correct horse"}`, ""); status != http.StatusTooManyRequests {
		t.Fatalf("locked correct password: %d %s", status, data)
	}
}
