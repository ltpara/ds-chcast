package api_test

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"math/big"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/oidc"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type fakeIdentityProvider struct {
	server  *httptest.Server
	key     *rsa.PrivateKey
	nonce   string
	subject string
	expiry  time.Time
}

func newFakeIdentityProvider(t *testing.T) *fakeIdentityProvider {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	fake := &fakeIdentityProvider{
		key:     key,
		nonce:   "",
		subject: "oidc-subject-1",
		expiry:  time.Now().Add(5 * time.Minute),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/.well-known/openid-configuration", func(w http.ResponseWriter, r *http.Request) {
		writeJSONTest(w, map[string]any{
			"issuer":                 fake.server.URL,
			"authorization_endpoint": fake.server.URL + "/authorize",
			"token_endpoint":         fake.server.URL + "/token",
			"jwks_uri":               fake.server.URL + "/jwks",
		})
	})
	mux.HandleFunc("/jwks", func(w http.ResponseWriter, r *http.Request) {
		writeJSONTest(w, map[string]any{"keys": []map[string]any{{
			"kty": "RSA",
			"kid": "key-1",
			"alg": "RS256",
			"use": "sig",
			"n":   base64.RawURLEncoding.EncodeToString(key.N.Bytes()),
			"e":   base64.RawURLEncoding.EncodeToString(big.NewInt(int64(key.E)).Bytes()),
		}}})
	})
	mux.HandleFunc("/token", func(w http.ResponseWriter, r *http.Request) {
		_ = r.ParseForm()
		if r.Form.Get("grant_type") != "authorization_code" || r.Form.Get("code") != "test-code" {
			writeJSONTest(w, map[string]string{"error": "invalid_grant"})
			return
		}
		claims := map[string]any{
			"iss":                fake.server.URL,
			"sub":                fake.subject,
			"aud":                "chast-client",
			"exp":                fake.expiry.Unix(),
			"iat":                time.Now().Add(-time.Minute).Unix(),
			"nonce":              fake.nonce,
			"preferred_username": "sso-alice",
			"email":              "sso-alice@example.com",
			"email_verified":     true,
		}
		writeJSONTest(w, map[string]any{"id_token": signAPITestToken(t, key, claims)})
	})
	fake.server = httptest.NewServer(mux)
	t.Cleanup(fake.server.Close)
	return fake
}

func (f *fakeIdentityProvider) provider(t *testing.T, autoCreate bool) *oidc.Provider {
	t.Helper()
	provider, err := oidc.New(oidc.Config{
		Issuer:      f.server.URL,
		ClientID:    "chast-client",
		RedirectURL: "http://127.0.0.1/v1/account/oidc/callback",
		AutoCreate:  autoCreate,
		DisplayName: "Example SSO",
	})
	if err != nil {
		t.Fatal(err)
	}
	return provider
}

func writeJSONTest(w http.ResponseWriter, value any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(value)
}

func signAPITestToken(t *testing.T, key *rsa.PrivateKey, claims map[string]any) string {
	t.Helper()
	header, err := json.Marshal(map[string]string{"alg": "RS256", "kid": "key-1", "typ": "JWT"})
	if err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(claims)
	if err != nil {
		t.Fatal(err)
	}
	input := base64.RawURLEncoding.EncodeToString(header) + "." + base64.RawURLEncoding.EncodeToString(payload)
	digest := sha256.Sum256([]byte(input))
	signature, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, digest[:])
	if err != nil {
		t.Fatal(err)
	}
	return input + "." + base64.RawURLEncoding.EncodeToString(signature)
}

type oidcBeginReply struct {
	AuthURL     string `json:"auth_url"`
	State       string `json:"state"`
	ExpiresInMS int64  `json:"expires_in_ms"`
}

func beginOIDC(t *testing.T, base string) oidcBeginReply {
	t.Helper()
	status, data := request(t, http.MethodPost, base+"/v1/account/oidc/begin",
		`{"device_id":"dev-1","device_name":"workstation"}`, "")
	if status != http.StatusOK {
		t.Fatalf("oidc begin: %d %s", status, data)
	}
	var reply oidcBeginReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("begin reply: %v", err)
	}
	if reply.State == "" || reply.AuthURL == "" || reply.ExpiresInMS <= 0 {
		t.Fatalf("unexpected begin reply: %+v", reply)
	}
	return reply
}

func TestOIDCLoginEndToEnd(t *testing.T) {
	idp := newFakeIdentityProvider(t)
	server := testutil.StartWith(t, testutil.Options{OIDC: idp.provider(t, true)})

	status, data := request(t, http.MethodGet, server.URL+"/v1/account/oidc", "", "")
	if status != http.StatusOK {
		t.Fatalf("oidc status: %d %s", status, data)
	}
	var info struct {
		Enabled bool   `json:"enabled"`
		Name    string `json:"name"`
	}
	if err := json.Unmarshal(data, &info); err != nil {
		t.Fatal(err)
	}
	if !info.Enabled || info.Name != "Example SSO" {
		t.Fatalf("oidc status = %+v", info)
	}

	begin := beginOIDC(t, server.URL)
	authURL, err := url.Parse(begin.AuthURL)
	if err != nil {
		t.Fatal(err)
	}
	if authURL.Query().Get("state") != begin.State ||
		authURL.Query().Get("code_challenge") == "" ||
		authURL.Query().Get("code_challenge_method") != "S256" {
		t.Fatalf("authorization URL = %s", begin.AuthURL)
	}
	idp.nonce = authURL.Query().Get("nonce")

	status, data = request(t, http.MethodGet,
		server.URL+"/v1/account/oidc/callback?state="+url.QueryEscape(begin.State)+"&code=test-code", "", "")
	if status != http.StatusOK {
		t.Fatalf("callback: %d %s", status, data)
	}

	status, data = request(t, http.MethodPost, server.URL+"/v1/account/oidc/poll",
		`{"state":"`+begin.State+`"}`, "")
	if status != http.StatusOK {
		t.Fatalf("poll: %d %s", status, data)
	}
	var reply loginReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatal(err)
	}
	if reply.Token == "" || reply.RefreshToken == "" ||
		reply.User.Username != "sso-alice" || reply.User.Role != "admin" {
		t.Fatalf("unexpected login reply: %+v", reply)
	}
	if status, data := request(t, http.MethodGet, server.URL+"/v1/account", "", reply.Token); status != http.StatusOK {
		t.Fatalf("OIDC token did not authenticate: %d %s", status, data)
	}
	if status, _ := request(t, http.MethodPost, server.URL+"/v1/account/oidc/poll",
		`{"state":"`+begin.State+`"}`, ""); status != http.StatusNotFound {
		t.Fatalf("state was reusable: %d", status)
	}
}

func TestOIDCCallbackRejectsReusedState(t *testing.T) {
	idp := newFakeIdentityProvider(t)
	server := testutil.StartWith(t, testutil.Options{OIDC: idp.provider(t, true)})
	begin := beginOIDC(t, server.URL)
	authURL, _ := url.Parse(begin.AuthURL)
	idp.nonce = authURL.Query().Get("nonce")
	callback := server.URL + "/v1/account/oidc/callback?state=" + url.QueryEscape(begin.State) + "&code=test-code"
	if status, data := request(t, http.MethodGet, callback, "", ""); status != http.StatusOK {
		t.Fatalf("first callback: %d %s", status, data)
	}
	if status, _ := request(t, http.MethodGet, callback, "", ""); status != http.StatusConflict {
		t.Fatalf("second callback status = %d", status)
	}
}

func TestOIDCPollReportsProviderRejection(t *testing.T) {
	idp := newFakeIdentityProvider(t)
	server := testutil.StartWith(t, testutil.Options{OIDC: idp.provider(t, true)})
	begin := beginOIDC(t, server.URL)
	status, data := request(t, http.MethodGet,
		server.URL+"/v1/account/oidc/callback?state="+url.QueryEscape(begin.State)+"&error=access_denied", "", "")
	if status != http.StatusUnauthorized {
		t.Fatalf("rejected callback: %d %s", status, data)
	}
	status, data = request(t, http.MethodPost, server.URL+"/v1/account/oidc/poll",
		`{"state":"`+begin.State+`"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("poll after rejection: %d %s", status, data)
	}
	var failure struct {
		Code string `json:"code"`
	}
	if err := json.Unmarshal(data, &failure); err != nil || failure.Code != "oidc_failed" {
		t.Fatalf("poll failure = %s", data)
	}
}

func TestOIDCPollReportsUnknownState(t *testing.T) {
	idp := newFakeIdentityProvider(t)
	server := testutil.StartWith(t, testutil.Options{OIDC: idp.provider(t, true)})
	status, data := request(t, http.MethodPost, server.URL+"/v1/account/oidc/poll",
		`{"state":"not-issued"}`, "")
	if status != http.StatusNotFound {
		t.Fatalf("unknown state: %d %s", status, data)
	}
}

func TestOIDCDisabledWithoutConfiguration(t *testing.T) {
	server := testutil.Start(t)
	status, data := request(t, http.MethodGet, server.URL+"/v1/account/oidc", "", "")
	if status != http.StatusOK {
		t.Fatalf("status route: %d %s", status, data)
	}
	var info struct {
		Enabled bool `json:"enabled"`
	}
	if err := json.Unmarshal(data, &info); err != nil || info.Enabled {
		t.Fatalf("disabled status = %s", data)
	}
	status, _ = request(t, http.MethodPost, server.URL+"/v1/account/oidc/begin", `{}`, "")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("begin on unconfigured server = %d", status)
	}
}
