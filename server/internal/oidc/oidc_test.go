package oidc

import (
	"context"
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
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

type fakeProvider struct {
	t         *testing.T
	server    *httptest.Server
	key       *rsa.PrivateKey
	kid       string
	audience  string
	nonce     string
	issuer    string
	expiry    time.Time
	alg       string
	badKey    bool
	tokenErr  string
	tokenHits atomic.Int64
}

func newFakeProvider(t *testing.T) *fakeProvider {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	fake := &fakeProvider{
		t:        t,
		key:      key,
		kid:      "test-key",
		audience: "chast-client",
		nonce:    "test-nonce",
		alg:      "RS256",
		expiry:   time.Now().Add(5 * time.Minute),
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/.well-known/openid-configuration", func(w http.ResponseWriter, r *http.Request) {
		writeTestJSON(w, map[string]any{
			"issuer":                                fake.issuer,
			"authorization_endpoint":                fake.issuer + "/authorize",
			"token_endpoint":                        fake.issuer + "/token",
			"jwks_uri":                              fake.issuer + "/jwks",
			"id_token_signing_alg_values_supported": []string{"RS256"},
		})
	})
	mux.HandleFunc("/jwks", func(w http.ResponseWriter, r *http.Request) {
		writeTestJSON(w, map[string]any{"keys": []map[string]any{{
			"kty": "RSA",
			"kid": fake.kid,
			"alg": "RS256",
			"use": "sig",
			"n":   base64.RawURLEncoding.EncodeToString(key.N.Bytes()),
			"e":   base64.RawURLEncoding.EncodeToString(big.NewInt(int64(key.E)).Bytes()),
		}}})
	})
	mux.HandleFunc("/token", func(w http.ResponseWriter, r *http.Request) {
		fake.tokenHits.Add(1)
		if err := r.ParseForm(); err != nil {
			http.Error(w, "bad form", http.StatusBadRequest)
			return
		}
		if fake.tokenErr != "" {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusBadRequest)
			_ = json.NewEncoder(w).Encode(map[string]string{"error": fake.tokenErr})
			return
		}
		if r.Form.Get("grant_type") != "authorization_code" ||
			r.Form.Get("code") != "good-code" ||
			r.Form.Get("redirect_uri") != "https://chast.example/v1/account/oidc/callback" ||
			r.Form.Get("client_id") != "chast-client" ||
			len(r.Form.Get("code_verifier")) < 43 {
			http.Error(w, "bad code", http.StatusBadRequest)
			return
		}
		signingKey := fake.key
		if fake.badKey {
			other, err := rsa.GenerateKey(rand.Reader, 2048)
			if err != nil {
				t.Error(err)
				return
			}
			signingKey = other
		}
		token := signIDToken(fake.t, signingKey, fake.kid, fake.alg, map[string]any{
			"iss":                fake.issuer,
			"sub":                "user-123",
			"aud":                fake.audience,
			"exp":                fake.expiry.Unix(),
			"iat":                time.Now().Add(-time.Minute).Unix(),
			"nonce":              fake.nonce,
			"preferred_username": "alice",
			"email":              "alice@example.com",
			"email_verified":     true,
			"name":               "Alice",
		})
		writeTestJSON(w, map[string]any{"id_token": token, "token_type": "Bearer"})
	})
	fake.server = httptest.NewServer(mux)
	fake.issuer = fake.server.URL
	t.Cleanup(fake.server.Close)
	return fake
}

func (f *fakeProvider) provider(t *testing.T) *Provider {
	t.Helper()
	provider, err := New(Config{
		Issuer:       f.issuer,
		ClientID:     "chast-client",
		ClientSecret: "secret",
		RedirectURL:  "https://chast.example/v1/account/oidc/callback",
	})
	if err != nil {
		t.Fatal(err)
	}
	return provider
}

func writeTestJSON(w http.ResponseWriter, value any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(value)
}

func signIDToken(t *testing.T, key *rsa.PrivateKey, kid, alg string, claims map[string]any) string {
	t.Helper()
	header, err := json.Marshal(map[string]string{"alg": alg, "kid": kid, "typ": "JWT"})
	if err != nil {
		t.Fatal(err)
	}
	payload, err := json.Marshal(claims)
	if err != nil {
		t.Fatal(err)
	}
	signingInput := base64.RawURLEncoding.EncodeToString(header) + "." + base64.RawURLEncoding.EncodeToString(payload)
	digest := sha256.Sum256([]byte(signingInput))
	signature, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, digest[:])
	if err != nil {
		t.Fatal(err)
	}
	return signingInput + "." + base64.RawURLEncoding.EncodeToString(signature)
}

func TestNewValidatesConfiguration(t *testing.T) {
	base := Config{
		Issuer:      "https://idp.example",
		ClientID:    "client",
		RedirectURL: "https://chast.example/v1/account/oidc/callback",
	}
	if _, err := New(base); err != nil {
		t.Fatalf("valid config rejected: %v", err)
	}
	for name, mutate := range map[string]func(*Config){
		"missing issuer":   func(c *Config) { c.Issuer = "" },
		"missing client":   func(c *Config) { c.ClientID = "" },
		"missing redirect": func(c *Config) { c.RedirectURL = "" },
		"plain http":       func(c *Config) { c.Issuer = "http://idp.example" },
		"no openid scope":  func(c *Config) { c.Scopes = []string{"profile"} },
	} {
		t.Run(name, func(t *testing.T) {
			cfg := base
			mutate(&cfg)
			if _, err := New(cfg); err == nil {
				t.Fatalf("%s was accepted", name)
			}
		})
	}
}

func TestAuthorizeURLIncludesPKCEStateAndNonce(t *testing.T) {
	fake := newFakeProvider(t)
	provider := fake.provider(t)
	verifier, err := NewVerifier()
	if err != nil {
		t.Fatal(err)
	}
	authURL, err := provider.AuthorizeURL(context.Background(), "state-1", "nonce-1", verifier)
	if err != nil {
		t.Fatal(err)
	}
	parsed, err := url.Parse(authURL)
	if err != nil {
		t.Fatal(err)
	}
	if parsed.Path != "/authorize" {
		t.Fatalf("path = %q", parsed.Path)
	}
	query := parsed.Query()
	if query.Get("client_id") != "chast-client" ||
		query.Get("state") != "state-1" ||
		query.Get("nonce") != "nonce-1" ||
		query.Get("response_type") != "code" ||
		query.Get("redirect_uri") != "https://chast.example/v1/account/oidc/callback" ||
		query.Get("scope") != "openid profile email" {
		t.Fatalf("unexpected query: %v", query)
	}
	digest := sha256.Sum256([]byte(verifier))
	if query.Get("code_challenge") != base64.RawURLEncoding.EncodeToString(digest[:]) ||
		query.Get("code_challenge_method") != "S256" {
		t.Fatalf("PKCE parameters missing: %v", query)
	}
}

func TestExchangeReturnsVerifiedIdentity(t *testing.T) {
	fake := newFakeProvider(t)
	provider := fake.provider(t)
	verifier, _ := NewVerifier()
	identity, err := provider.Exchange(context.Background(), "good-code", verifier, "test-nonce")
	if err != nil {
		t.Fatal(err)
	}
	if identity.Subject != "user-123" || identity.PreferredUsername != "alice" ||
		identity.Email != "alice@example.com" || !identity.EmailVerified {
		t.Fatalf("identity = %+v", identity)
	}
	if fake.tokenHits.Load() != 1 {
		t.Fatalf("token endpoint hits = %d", fake.tokenHits.Load())
	}
}

func TestExchangeRejectsBadTokens(t *testing.T) {
	verifier, _ := NewVerifier()
	cases := map[string]func(*fakeProvider){
		"wrong nonce":    func(f *fakeProvider) { f.nonce = "other" },
		"wrong audience": func(f *fakeProvider) { f.audience = "other-client" },
		"expired":        func(f *fakeProvider) { f.expiry = time.Now().Add(-10 * time.Minute) },
		"bad signature":  func(f *fakeProvider) { f.badKey = true },
		"token error":    func(f *fakeProvider) { f.tokenErr = "invalid_grant" },
	}
	for name, mutate := range cases {
		t.Run(name, func(t *testing.T) {
			fake := newFakeProvider(t)
			mutate(fake)
			provider := fake.provider(t)
			if _, err := provider.Exchange(context.Background(), "good-code", verifier, "test-nonce"); err == nil {
				t.Fatalf("%s was accepted", name)
			}
		})
	}
	if _, err := providerForTest(t).Exchange(context.Background(), "bad-code", verifier, "test-nonce"); err == nil {
		t.Fatal("an unknown code was accepted")
	}
}

func providerForTest(t *testing.T) *Provider {
	t.Helper()
	return newFakeProvider(t).provider(t)
}

func TestDiscoveryIssuerMismatchIsRejected(t *testing.T) {
	fake := newFakeProvider(t)
	fake.issuer = fake.server.URL + "/tenant"
	provider, err := New(Config{
		Issuer:      fake.server.URL,
		ClientID:    "chast-client",
		RedirectURL: "https://chast.example/v1/account/oidc/callback",
	})
	if err != nil {
		t.Fatal(err)
	}
	verifier, _ := NewVerifier()
	if _, err := provider.AuthorizeURL(context.Background(), "state", "nonce", verifier); err == nil ||
		!strings.Contains(err.Error(), "does not match") {
		t.Fatalf("issuer mismatch error = %v", err)
	}
}

func TestUnreachableProviderReportsUsefulError(t *testing.T) {
	provider, err := New(Config{
		Issuer:      "http://127.0.0.1:1",
		ClientID:    "chast-client",
		RedirectURL: "http://127.0.0.1:1/callback",
	})
	if err != nil {
		t.Fatal(err)
	}
	verifier, _ := NewVerifier()
	if _, err := provider.AuthorizeURL(context.Background(), "state", "nonce", verifier); err == nil {
		t.Fatal("unreachable provider succeeded")
	}
}

func TestRandomTokensAreUniqueAndLong(t *testing.T) {
	first, err := RandomURLToken()
	if err != nil {
		t.Fatal(err)
	}
	second, err := RandomURLToken()
	if err != nil {
		t.Fatal(err)
	}
	if first == second || len(first) < 40 {
		t.Fatalf("tokens are not random: %q %q", first, second)
	}
	verifier, err := NewVerifier()
	if err != nil || len(verifier) != 43 {
		t.Fatalf("verifier = %q, %v", verifier, err)
	}
}
