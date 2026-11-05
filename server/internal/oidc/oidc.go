// Package oidc implements the relying-party half of an OpenID Connect
// authorization-code flow with PKCE. It performs discovery, validates the ID
// token signature against the provider's JWKS and never trusts the token
// endpoint response beyond the claims it can verify itself.
package oidc

import (
	"context"
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

// Config describes one registered client at an OpenID provider.
type Config struct {
	// Issuer is the exact issuer identifier registered with the provider.
	Issuer string
	// ClientID and ClientSecret identify this server. An empty secret makes
	// the client public, which PKCE alone protects.
	ClientID     string
	ClientSecret string
	// RedirectURL is the registered callback on this server.
	RedirectURL string
	// Scopes defaults to "openid profile email".
	Scopes []string
	// DisplayName is shown by viewers; empty falls back to the issuer host.
	DisplayName string
	// AutoCreate links an unknown provider subject to a new local account.
	// When false only subjects an operator already linked may sign in.
	AutoCreate bool
	// HTTPClient overrides the transport; tests use it to reach a fake
	// provider. Nil uses http.DefaultClient with a 15 second timeout.
	HTTPClient *http.Client
}

// Identity is the subset of verified ID-token claims chast accounts use.
type Identity struct {
	Subject           string
	PreferredUsername string
	Email             string
	EmailVerified     bool
	Name              string
}

// Provider performs discovery lazily so an unreachable identity provider at
// startup does not stop the rendezvous server; affected logins fail instead.
type Provider struct {
	cfg  Config
	http *http.Client

	mu          sync.Mutex
	meta        *metadata
	keys        []jwk
	keysFetched time.Time
	now         func() time.Time
}

type metadata struct {
	Issuer                  string   `json:"issuer"`
	AuthorizationEndpoint   string   `json:"authorization_endpoint"`
	TokenEndpoint           string   `json:"token_endpoint"`
	JWKSURI                 string   `json:"jwks_uri"`
	IDTokenSigningAlgValues []string `json:"id_token_signing_alg_values_supported"`
}

type jwk struct {
	Kty string `json:"kty"`
	Kid string `json:"kid"`
	Alg string `json:"alg"`
	Use string `json:"use"`
	N   string `json:"n"`
	E   string `json:"e"`
	Crv string `json:"crv"`
	X   string `json:"x"`
	Y   string `json:"y"`
}

type jwks struct {
	Keys []jwk `json:"keys"`
}

type tokenResponse struct {
	IDToken     string `json:"id_token"`
	AccessToken string `json:"access_token"`
	TokenType   string `json:"token_type"`
	Error       string `json:"error"`
	ErrorDesc   string `json:"error_description"`
}

type idClaims struct {
	Issuer            string          `json:"iss"`
	Subject           string          `json:"sub"`
	Audience          json.RawMessage `json:"aud"`
	Expiry            int64           `json:"exp"`
	NotBefore         int64           `json:"nbf"`
	IssuedAt          int64           `json:"iat"`
	Nonce             string          `json:"nonce"`
	AuthorizedParty   string          `json:"azp"`
	PreferredUsername string          `json:"preferred_username"`
	Email             string          `json:"email"`
	EmailVerified     bool            `json:"email_verified"`
	Name              string          `json:"name"`
}

const (
	discoveryTimeout = 15 * time.Second
	clockSkew        = 60 * time.Second
	maxKeysAge       = time.Hour
)

// New validates cfg and returns a provider. No network traffic happens until
// the first login, so callers can start the server while the provider is down.
func New(cfg Config) (*Provider, error) {
	cfg.Issuer = strings.TrimRight(strings.TrimSpace(cfg.Issuer), "/")
	cfg.ClientID = strings.TrimSpace(cfg.ClientID)
	cfg.RedirectURL = strings.TrimSpace(cfg.RedirectURL)
	if cfg.Issuer == "" || cfg.ClientID == "" || cfg.RedirectURL == "" {
		return nil, errors.New("oidc: issuer, client id and redirect url are required")
	}
	issuer, err := url.Parse(cfg.Issuer)
	if err != nil || issuer.Host == "" || (issuer.Scheme != "https" && !isLoopback(issuer)) {
		return nil, errors.New("oidc: issuer must be an https URL (http is allowed for loopback)")
	}
	redirect, err := url.Parse(cfg.RedirectURL)
	if err != nil || redirect.Host == "" || (redirect.Scheme != "https" && !isLoopback(redirect)) {
		return nil, errors.New("oidc: redirect url must be an https URL (http is allowed for loopback)")
	}
	if len(cfg.Scopes) == 0 {
		cfg.Scopes = []string{"openid", "profile", "email"}
	}
	hasOpenID := false
	for _, scope := range cfg.Scopes {
		if strings.TrimSpace(scope) == "openid" {
			hasOpenID = true
		}
	}
	if !hasOpenID {
		return nil, errors.New("oidc: scopes must include openid")
	}
	client := cfg.HTTPClient
	if client == nil {
		client = &http.Client{Timeout: discoveryTimeout}
	}
	return &Provider{cfg: cfg, http: client, now: time.Now}, nil
}

func isLoopback(u *url.URL) bool {
	if u.Scheme != "http" {
		return false
	}
	host := u.Hostname()
	if host == "localhost" {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// AutoCreate reports whether unknown subjects may create local accounts.
func (p *Provider) AutoCreate() bool { return p.cfg.AutoCreate }

// Issuer is the normalized issuer identifier used to scope identity links.
func (p *Provider) Issuer() string { return p.cfg.Issuer }

// Name is the provider label shown in viewers.
func (p *Provider) Name() string {
	if name := strings.TrimSpace(p.cfg.DisplayName); name != "" {
		return name
	}
	if u, err := url.Parse(p.cfg.Issuer); err == nil && u.Host != "" {
		return u.Host
	}
	return p.cfg.Issuer
}

// SetClock replaces the time source; tests use it for expiry checks.
func (p *Provider) SetClock(now func() time.Time) {
	p.mu.Lock()
	p.now = now
	p.mu.Unlock()
}

// AuthorizeURL builds the provider redirect for one login attempt.
func (p *Provider) AuthorizeURL(ctx context.Context, state, nonce, verifier string) (string, error) {
	if strings.TrimSpace(state) == "" || strings.TrimSpace(nonce) == "" || len(verifier) < 43 {
		return "", errors.New("oidc: state, nonce and a PKCE verifier are required")
	}
	meta, err := p.discover(ctx)
	if err != nil {
		return "", err
	}
	endpoint, err := url.Parse(meta.AuthorizationEndpoint)
	if err != nil {
		return "", fmt.Errorf("oidc: authorization endpoint: %w", err)
	}
	challenge := sha256.Sum256([]byte(verifier))
	query := endpoint.Query()
	query.Set("response_type", "code")
	query.Set("client_id", p.cfg.ClientID)
	query.Set("redirect_uri", p.cfg.RedirectURL)
	query.Set("scope", strings.Join(p.cfg.Scopes, " "))
	query.Set("state", state)
	query.Set("nonce", nonce)
	query.Set("code_challenge", base64.RawURLEncoding.EncodeToString(challenge[:]))
	query.Set("code_challenge_method", "S256")
	endpoint.RawQuery = query.Encode()
	return endpoint.String(), nil
}

// Exchange redeems an authorization code and returns the verified identity.
// The returned error is safe to log: it never contains tokens or codes.
func (p *Provider) Exchange(ctx context.Context, code, verifier, nonce string) (Identity, error) {
	if strings.TrimSpace(code) == "" || len(verifier) < 43 || strings.TrimSpace(nonce) == "" {
		return Identity{}, errors.New("oidc: authorization code, PKCE verifier and nonce are required")
	}
	meta, err := p.discover(ctx)
	if err != nil {
		return Identity{}, err
	}
	form := url.Values{}
	form.Set("grant_type", "authorization_code")
	form.Set("code", code)
	form.Set("redirect_uri", p.cfg.RedirectURL)
	form.Set("client_id", p.cfg.ClientID)
	form.Set("code_verifier", verifier)
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, meta.TokenEndpoint, strings.NewReader(form.Encode()))
	if err != nil {
		return Identity{}, fmt.Errorf("oidc: token request: %w", err)
	}
	request.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	request.Header.Set("Accept", "application/json")
	if p.cfg.ClientSecret != "" {
		request.SetBasicAuth(url.QueryEscape(p.cfg.ClientID), url.QueryEscape(p.cfg.ClientSecret))
	}
	response, err := p.http.Do(request)
	if err != nil {
		return Identity{}, fmt.Errorf("oidc: token endpoint: %w", err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if err != nil {
		return Identity{}, fmt.Errorf("oidc: read token response: %w", err)
	}
	var tokens tokenResponse
	if err := json.Unmarshal(body, &tokens); err != nil {
		return Identity{}, fmt.Errorf("oidc: token response is not JSON (status %d)", response.StatusCode)
	}
	if response.StatusCode != http.StatusOK {
		detail := tokens.Error
		if detail == "" {
			detail = fmt.Sprintf("status %d", response.StatusCode)
		}
		return Identity{}, fmt.Errorf("oidc: token endpoint refused the code: %s", detail)
	}
	if tokens.IDToken == "" {
		return Identity{}, errors.New("oidc: token response has no id_token")
	}
	claims, err := p.verifyIDToken(ctx, tokens.IDToken, nonce)
	if err != nil {
		return Identity{}, err
	}
	return Identity{
		Subject:           claims.Subject,
		PreferredUsername: claims.PreferredUsername,
		Email:             claims.Email,
		EmailVerified:     claims.EmailVerified,
		Name:              claims.Name,
	}, nil
}

func (p *Provider) discover(ctx context.Context) (*metadata, error) {
	p.mu.Lock()
	meta := p.meta
	p.mu.Unlock()
	if meta != nil {
		return meta, nil
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, p.cfg.Issuer+"/.well-known/openid-configuration", nil)
	if err != nil {
		return nil, fmt.Errorf("oidc: discovery request: %w", err)
	}
	request.Header.Set("Accept", "application/json")
	response, err := p.http.Do(request)
	if err != nil {
		return nil, fmt.Errorf("oidc: discovery: %w", err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("oidc: read discovery document: %w", err)
	}
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("oidc: discovery returned status %d", response.StatusCode)
	}
	var document metadata
	if err := json.Unmarshal(body, &document); err != nil {
		return nil, fmt.Errorf("oidc: discovery document is not JSON")
	}
	// The provider must echo the configured issuer, otherwise tokens from a
	// different tenant would validate against the wrong keys.
	if strings.TrimRight(document.Issuer, "/") != p.cfg.Issuer {
		return nil, fmt.Errorf("oidc: discovery issuer %q does not match %q", document.Issuer, p.cfg.Issuer)
	}
	for name, endpoint := range map[string]string{
		"authorization_endpoint": document.AuthorizationEndpoint,
		"token_endpoint":         document.TokenEndpoint,
		"jwks_uri":               document.JWKSURI,
	} {
		parsed, err := url.Parse(endpoint)
		if err != nil || parsed.Host == "" || (parsed.Scheme != "https" && !isLoopback(parsed)) {
			return nil, fmt.Errorf("oidc: discovery %s is not an https URL", name)
		}
	}
	p.mu.Lock()
	if p.meta == nil {
		p.meta = &document
	}
	meta = p.meta
	p.mu.Unlock()
	return meta, nil
}

func (p *Provider) verifyIDToken(ctx context.Context, token, nonce string) (idClaims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return idClaims{}, errors.New("oidc: id_token is not a compact JWS")
	}
	headerJSON, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return idClaims{}, errors.New("oidc: id_token header is not base64url")
	}
	payloadJSON, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return idClaims{}, errors.New("oidc: id_token payload is not base64url")
	}
	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return idClaims{}, errors.New("oidc: id_token signature is not base64url")
	}
	var header struct {
		Alg string `json:"alg"`
		Kid string `json:"kid"`
	}
	if err := json.Unmarshal(headerJSON, &header); err != nil {
		return idClaims{}, errors.New("oidc: id_token header is not JSON")
	}
	if header.Alg != "RS256" && header.Alg != "PS256" && header.Alg != "ES256" {
		return idClaims{}, fmt.Errorf("oidc: unsupported id_token algorithm %q", header.Alg)
	}
	key, err := p.signingKey(ctx, header.Kid, header.Alg)
	if err != nil {
		return idClaims{}, err
	}
	digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
	switch header.Alg {
	case "RS256":
		public, ok := key.(*rsa.PublicKey)
		if !ok {
			return idClaims{}, errors.New("oidc: signing key type does not match RS256")
		}
		if err := rsa.VerifyPKCS1v15(public, crypto.SHA256, digest[:], signature); err != nil {
			return idClaims{}, errors.New("oidc: id_token signature is invalid")
		}
	case "PS256":
		public, ok := key.(*rsa.PublicKey)
		if !ok {
			return idClaims{}, errors.New("oidc: signing key type does not match PS256")
		}
		if err := rsa.VerifyPSS(public, crypto.SHA256, digest[:], signature, &rsa.PSSOptions{SaltLength: rsa.PSSSaltLengthEqualsHash}); err != nil {
			return idClaims{}, errors.New("oidc: id_token signature is invalid")
		}
	case "ES256":
		public, ok := key.(*ecdsa.PublicKey)
		if !ok || len(signature) != 64 {
			return idClaims{}, errors.New("oidc: signing key type does not match ES256")
		}
		r := new(big.Int).SetBytes(signature[:32])
		s := new(big.Int).SetBytes(signature[32:])
		if !ecdsa.Verify(public, digest[:], r, s) {
			return idClaims{}, errors.New("oidc: id_token signature is invalid")
		}
	}
	var claims idClaims
	if err := json.Unmarshal(payloadJSON, &claims); err != nil {
		return idClaims{}, errors.New("oidc: id_token payload is not JSON")
	}
	if strings.TrimRight(claims.Issuer, "/") != p.cfg.Issuer {
		return idClaims{}, errors.New("oidc: id_token issuer does not match")
	}
	if !audienceContains(claims.Audience, p.cfg.ClientID) {
		return idClaims{}, errors.New("oidc: id_token audience does not include this client")
	}
	if claims.AuthorizedParty != "" && claims.AuthorizedParty != p.cfg.ClientID {
		return idClaims{}, errors.New("oidc: id_token authorized party does not match")
	}
	now := p.clockNow()
	if claims.Expiry == 0 || now.After(time.Unix(claims.Expiry, 0).Add(clockSkew)) {
		return idClaims{}, errors.New("oidc: id_token has expired")
	}
	if claims.NotBefore != 0 && now.Add(clockSkew).Before(time.Unix(claims.NotBefore, 0)) {
		return idClaims{}, errors.New("oidc: id_token is not valid yet")
	}
	if claims.IssuedAt != 0 && now.Add(clockSkew).Before(time.Unix(claims.IssuedAt, 0)) {
		return idClaims{}, errors.New("oidc: id_token was issued in the future")
	}
	if strings.TrimSpace(claims.Subject) == "" {
		return idClaims{}, errors.New("oidc: id_token has no subject")
	}
	// The nonce binds this token to the login attempt that produced the code,
	// so a token replayed from another session is rejected.
	if claims.Nonce != nonce {
		return idClaims{}, errors.New("oidc: id_token nonce does not match")
	}
	return claims, nil
}

func (p *Provider) clockNow() time.Time {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.now()
}

func audienceContains(raw json.RawMessage, clientID string) bool {
	if len(raw) == 0 {
		return false
	}
	var single string
	if err := json.Unmarshal(raw, &single); err == nil {
		return single == clientID
	}
	var list []string
	if err := json.Unmarshal(raw, &list); err != nil {
		return false
	}
	for _, aud := range list {
		if aud == clientID {
			return true
		}
	}
	return false
}

func (p *Provider) signingKey(ctx context.Context, kid, alg string) (crypto.PublicKey, error) {
	p.mu.Lock()
	keys := p.keys
	fetched := p.keysFetched
	p.mu.Unlock()
	if key, ok := matchKey(keys, kid, alg); ok {
		return key, nil
	}
	if len(keys) != 0 && p.clockNow().Sub(fetched) < 30*time.Second {
		return nil, errors.New("oidc: no signing key matches the id_token")
	}
	meta, err := p.discover(ctx)
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, meta.JWKSURI, nil)
	if err != nil {
		return nil, fmt.Errorf("oidc: JWKS request: %w", err)
	}
	request.Header.Set("Accept", "application/json")
	response, err := p.http.Do(request)
	if err != nil {
		return nil, fmt.Errorf("oidc: JWKS: %w", err)
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, 1<<20))
	if err != nil {
		return nil, fmt.Errorf("oidc: read JWKS: %w", err)
	}
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("oidc: JWKS returned status %d", response.StatusCode)
	}
	var set jwks
	if err := json.Unmarshal(body, &set); err != nil {
		return nil, errors.New("oidc: JWKS is not JSON")
	}
	now := p.clockNow()
	p.mu.Lock()
	p.keys = set.Keys
	p.keysFetched = now
	p.mu.Unlock()
	if key, ok := matchKey(set.Keys, kid, alg); ok {
		return key, nil
	}
	return nil, errors.New("oidc: no signing key matches the id_token")
}

func matchKey(keys []jwk, kid, alg string) (crypto.PublicKey, bool) {
	for _, key := range keys {
		if kid != "" && key.Kid != kid {
			continue
		}
		if key.Use != "" && key.Use != "sig" {
			continue
		}
		if key.Alg != "" && key.Alg != alg {
			continue
		}
		switch key.Kty {
		case "RSA":
			if alg != "RS256" && alg != "PS256" {
				continue
			}
			public, err := rsaKey(key)
			if err == nil {
				return public, true
			}
		case "EC":
			if alg != "ES256" {
				continue
			}
			public, err := ecKey(key)
			if err == nil {
				return public, true
			}
		}
	}
	return nil, false
}

func rsaKey(key jwk) (*rsa.PublicKey, error) {
	nb, err := base64.RawURLEncoding.DecodeString(key.N)
	if err != nil {
		return nil, err
	}
	eb, err := base64.RawURLEncoding.DecodeString(key.E)
	if err != nil {
		return nil, err
	}
	if len(nb) == 0 || len(eb) == 0 || len(eb) > 4 {
		return nil, errors.New("oidc: malformed RSA key")
	}
	e := 0
	for _, b := range eb {
		e = e<<8 | int(b)
	}
	if e < 3 {
		return nil, errors.New("oidc: malformed RSA exponent")
	}
	return &rsa.PublicKey{N: new(big.Int).SetBytes(nb), E: e}, nil
}

func ecKey(key jwk) (*ecdsa.PublicKey, error) {
	if key.Crv != "P-256" {
		return nil, errors.New("oidc: unsupported curve")
	}
	x, err := base64.RawURLEncoding.DecodeString(key.X)
	if err != nil {
		return nil, err
	}
	y, err := base64.RawURLEncoding.DecodeString(key.Y)
	if err != nil {
		return nil, err
	}
	curve := elliptic.P256()
	public := &ecdsa.PublicKey{Curve: curve, X: new(big.Int).SetBytes(x), Y: new(big.Int).SetBytes(y)}
	if !curve.IsOnCurve(public.X, public.Y) {
		return nil, errors.New("oidc: EC key is not on the curve")
	}
	return public, nil
}

// RandomURLToken returns a base64url secret suitable for state or nonce.
func RandomURLToken() (string, error) {
	buffer := make([]byte, 32)
	if _, err := rand.Read(buffer); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buffer), nil
}

// NewVerifier returns a PKCE code verifier (43 characters). AuthorizeURL
// derives its S256 challenge, so only the verifier needs to be stored.
func NewVerifier() (string, error) {
	buffer := make([]byte, 32)
	if _, err := rand.Read(buffer); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buffer), nil
}
