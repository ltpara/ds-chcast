// Command chast-webauthn-smoke drives a running chast-server through the
// security-key HTTP ceremonies using the in-repo software authenticator. It is
// the real-process acceptance probe for P2-19: registration, listing,
// password-gated and passwordless assertion login, replay and
// tampered-signature rejection, and removal.
package main

import (
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/webauthn"
	"github.com/ltpara/ds-chcast/server/internal/webauthn/webauthntest"
)

type credentialReply struct {
	ID          string `json:"id"`
	Alg         int    `json:"alg"`
	Name        string `json:"name"`
	CreatedAtMS int64  `json:"created_at_ms"`
	LastUsedMS  int64  `json:"last_used_at_ms"`
}

type beginReply struct {
	Challenge          string            `json:"challenge"`
	RPID               string            `json:"rp_id"`
	UserID             string            `json:"user_id"`
	Username           string            `json:"username"`
	TimeoutMS          int               `json:"timeout_ms"`
	ExcludeCredentials []credentialReply `json:"exclude_credentials"`
	Credentials        []credentialReply `json:"credentials"`
}

type loginReply struct {
	Token        string `json:"token"`
	RefreshToken string `json:"refresh_token"`
	User         struct {
		Username string `json:"username"`
	} `json:"user"`
}

type checker struct {
	failed int
}

func (c *checker) check(name string, ok bool, detail string) {
	status := "ok"
	if !ok {
		status = "FAIL"
		c.failed++
	}
	fmt.Printf("[%s] %s", status, name)
	if detail != "" && !ok {
		fmt.Printf(" (%s)", strings.TrimSpace(detail))
	}
	fmt.Println()
}

type apiClient struct {
	base string
	http *http.Client
}

func (c *apiClient) call(method, path, token string, body any) (int, []byte, error) {
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			return 0, nil, err
		}
		reader = strings.NewReader(string(data))
	}
	req, err := http.NewRequest(method, c.base+path, reader)
	if err != nil {
		return 0, nil, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	reply, err := c.http.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer reply.Body.Close()
	data, err := io.ReadAll(reply.Body)
	if err != nil {
		return 0, nil, err
	}
	return reply.StatusCode, data, nil
}

func adminCommand(addr, command string) (string, error) {
	conn, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		return "", err
	}
	defer conn.Close()
	if err := conn.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return "", err
	}
	if _, err := io.WriteString(conn, command+"\n"); err != nil {
		return "", err
	}
	out, err := io.ReadAll(conn)
	if err != nil {
		return "", err
	}
	return string(out), nil
}

func waitForAdmin(addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		conn, err := net.DialTimeout("tcp", addr, time.Second)
		if err == nil {
			conn.Close()
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("admin console %s did not come up: %w", addr, err)
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func encode(data []byte) string {
	return base64.RawURLEncoding.EncodeToString(data)
}

func main() {
	serverURL := flag.String("server", "http://127.0.0.1:18443", "chast-server HTTP API base URL")
	adminAddr := flag.String("admin", "127.0.0.1:18446", "chast-server admin console address")
	rpID := flag.String("rp-id", "127.0.0.1", "relying party id the server was started with")
	origin := flag.String("origin", "", "origin the server was started with; defaults to --server")
	username := flag.String("username", "webauthn-smoke", "account used for the smoke run")
	password := flag.String("password", "smoke-pass-123", "password for that account")
	keep := flag.Bool("keep-account", false, "leave the smoke account in place")
	wait := flag.Duration("wait", 10*time.Second, "how long to wait for the admin console")
	flag.Parse()

	if err := run(*serverURL, *adminAddr, *rpID, *origin, *username, *password, *keep, *wait); err != nil {
		fmt.Fprintln(os.Stderr, "webauthn smoke:", err)
		os.Exit(1)
	}
}

func run(serverURL, adminAddr, rpID, origin, username, password string, keep bool, wait time.Duration) error {
	serverURL = strings.TrimRight(serverURL, "/")
	if origin == "" {
		origin = serverURL
	}
	if err := waitForAdmin(adminAddr, wait); err != nil {
		return err
	}
	if _, err := adminCommand(adminAddr, "user del "+username); err == nil {
		fmt.Printf("[ok] removed any previous %s account\n", username)
	}
	out, err := adminCommand(adminAddr, "user add "+username+" "+password)
	if err != nil {
		return fmt.Errorf("create account: %w", err)
	}
	if !strings.Contains(out, username+" created") {
		return fmt.Errorf("create account: %s", strings.TrimSpace(out))
	}
	if !keep {
		defer func() {
			if cleaned, err := adminCommand(adminAddr, "user del "+username); err == nil {
				fmt.Printf("[ok] cleaned up account (%s)\n", strings.TrimSpace(cleaned))
			}
		}()
	}

	api := &apiClient{base: serverURL, http: &http.Client{Timeout: 10 * time.Second}}
	report := &checker{}
	report.check("admin console creates the smoke account", strings.Contains(out, username+" created"), strings.TrimSpace(out))

	status, data, err := api.call(http.MethodPost, "/v1/account/login", "", map[string]string{
		"username": username, "password": password,
	})
	if err != nil {
		return fmt.Errorf("password login: %w", err)
	}
	var session loginReply
	if err := json.Unmarshal(data, &session); err != nil {
		return fmt.Errorf("password login reply: %w", err)
	}
	report.check("password login issues a token", status == http.StatusOK && session.Token != "", string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/webauthn/register/begin", session.Token,
		map[string]string{"password": "wrong-pass"})
	if err != nil {
		return fmt.Errorf("register begin: %w", err)
	}
	report.check("registration rejects a wrong password", status == http.StatusForbidden, string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/webauthn/register/begin", session.Token,
		map[string]string{"password": password})
	if err != nil {
		return fmt.Errorf("register begin: %w", err)
	}
	var register beginReply
	if err := json.Unmarshal(data, &register); err != nil {
		return fmt.Errorf("register begin reply: %w", err)
	}
	report.check("registration begin returns options",
		status == http.StatusOK && register.Challenge != "" && register.RPID == rpID &&
			register.Username == username && register.UserID != "" && register.TimeoutMS > 0,
		string(data))

	auth, err := webauthntest.New(webauthn.AlgES256, rpID, origin)
	if err != nil {
		return fmt.Errorf("software authenticator: %w", err)
	}
	challenge, err := base64.RawURLEncoding.DecodeString(register.Challenge)
	if err != nil {
		return fmt.Errorf("registration challenge: %w", err)
	}
	clientData, attestation, err := auth.MakeCredential(challenge)
	if err != nil {
		return fmt.Errorf("make credential: %w", err)
	}
	status, data, err = api.call(http.MethodPost, "/v1/account/webauthn/register/finish", session.Token,
		map[string]string{
			"name":               "smoke key",
			"client_data_json":   encode(clientData),
			"attestation_object": encode(attestation),
		})
	if err != nil {
		return fmt.Errorf("register finish: %w", err)
	}
	var stored credentialReply
	if err := json.Unmarshal(data, &stored); err != nil {
		return fmt.Errorf("register finish reply: %w", err)
	}
	credential := auth.Credential()
	report.check("registration stores the credential",
		status == http.StatusOK && stored.ID == encode(credential.ID) &&
			stored.Alg == webauthn.AlgES256 && stored.Name == "smoke key" && stored.CreatedAtMS > 0,
		string(data))

	status, data, err = api.call(http.MethodGet, "/v1/account/webauthn/credentials", session.Token, nil)
	if err != nil {
		return fmt.Errorf("list credentials: %w", err)
	}
	var list struct {
		Credentials []credentialReply `json:"credentials"`
	}
	if err := json.Unmarshal(data, &list); err != nil {
		return fmt.Errorf("credentials reply: %w", err)
	}
	report.check("credential list contains the key",
		status == http.StatusOK && len(list.Credentials) == 1 && list.Credentials[0].ID == stored.ID,
		string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/webauthn/register/begin", session.Token,
		map[string]string{"password": password})
	if err != nil {
		return fmt.Errorf("second register begin: %w", err)
	}
	var second beginReply
	if err := json.Unmarshal(data, &second); err != nil {
		return fmt.Errorf("second register begin reply: %w", err)
	}
	report.check("a second ceremony excludes the registered key",
		status == http.StatusOK && len(second.ExcludeCredentials) == 1 &&
			second.ExcludeCredentials[0].ID == stored.ID,
		string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/begin", "",
		map[string]string{"username": username, "password": password})
	if err != nil {
		return fmt.Errorf("security-key login begin: %w", err)
	}
	var loginBegin beginReply
	if err := json.Unmarshal(data, &loginBegin); err != nil {
		return fmt.Errorf("security-key login begin reply: %w", err)
	}
	report.check("login begin offers the registered key",
		status == http.StatusOK && loginBegin.Challenge != "" && loginBegin.RPID == rpID &&
			len(loginBegin.Credentials) == 1 && loginBegin.Credentials[0].ID == stored.ID,
		string(data))

	loginChallenge, err := base64.RawURLEncoding.DecodeString(loginBegin.Challenge)
	if err != nil {
		return fmt.Errorf("login challenge: %w", err)
	}
	assertClientData, authenticatorData, signature, err := auth.Assert(loginChallenge)
	if err != nil {
		return fmt.Errorf("assert: %w", err)
	}
	assertion := map[string]string{
		"username":           username,
		"credential_id":      stored.ID,
		"client_data_json":   encode(assertClientData),
		"authenticator_data": encode(authenticatorData),
		"signature":          encode(signature),
		"device_name":        "smoke",
	}
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/finish", "", assertion)
	if err != nil {
		return fmt.Errorf("security-key login: %w", err)
	}
	var secured loginReply
	if err := json.Unmarshal(data, &secured); err != nil {
		return fmt.Errorf("security-key login reply: %w", err)
	}
	report.check("security-key login issues password and refresh tokens",
		status == http.StatusOK && secured.Token != "" && secured.RefreshToken != "",
		string(data))

	status, data, err = api.call(http.MethodGet, "/v1/account", secured.Token, nil)
	if err != nil {
		return fmt.Errorf("account probe: %w", err)
	}
	report.check("issued token is accepted", status == http.StatusOK, string(data))

	// Passwordless login: the username selects the key and no password is sent.
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/passwordless/begin", "",
		map[string]string{"username": username})
	if err != nil {
		return fmt.Errorf("passwordless login begin: %w", err)
	}
	var passwordlessBegin beginReply
	if err := json.Unmarshal(data, &passwordlessBegin); err != nil {
		return fmt.Errorf("passwordless login begin reply: %w", err)
	}
	report.check("passwordless begin offers the registered key",
		status == http.StatusOK && passwordlessBegin.Challenge != "" && passwordlessBegin.RPID == rpID &&
			len(passwordlessBegin.Credentials) == 1 && passwordlessBegin.Credentials[0].ID == stored.ID,
		string(data))
	passwordlessChallenge, err := base64.RawURLEncoding.DecodeString(passwordlessBegin.Challenge)
	if err != nil {
		return fmt.Errorf("passwordless challenge: %w", err)
	}
	passwordlessClientData, passwordlessAuthData, passwordlessSignature, err := auth.Assert(passwordlessChallenge)
	if err != nil {
		return fmt.Errorf("passwordless assert: %w", err)
	}
	passwordlessAssertion := map[string]string{
		"username":           username,
		"credential_id":      stored.ID,
		"client_data_json":   encode(passwordlessClientData),
		"authenticator_data": encode(passwordlessAuthData),
		"signature":          encode(passwordlessSignature),
		"device_name":        "smoke-passwordless",
	}
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/passwordless/finish", "", passwordlessAssertion)
	if err != nil {
		return fmt.Errorf("passwordless login: %w", err)
	}
	var passwordlessSession loginReply
	if err := json.Unmarshal(data, &passwordlessSession); err != nil {
		return fmt.Errorf("passwordless login reply: %w", err)
	}
	report.check("passwordless login issues password and refresh tokens",
		status == http.StatusOK && passwordlessSession.Token != "" && passwordlessSession.RefreshToken != "",
		string(data))
	status, data, err = api.call(http.MethodGet, "/v1/account", passwordlessSession.Token, nil)
	if err != nil {
		return fmt.Errorf("passwordless account probe: %w", err)
	}
	report.check("passwordless token is accepted", status == http.StatusOK, string(data))
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/passwordless/finish", "", passwordlessAssertion)
	if err != nil {
		return fmt.Errorf("replay passwordless assertion: %w", err)
	}
	report.check("replayed passwordless assertion is rejected", status == http.StatusUnauthorized, string(data))
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/passwordless/begin", "",
		map[string]string{"username": username + "-missing"})
	if err != nil {
		return fmt.Errorf("unknown passwordless begin: %w", err)
	}
	report.check("passwordless begin hides unknown accounts",
		status == http.StatusAccepted && strings.Contains(string(data), "accepted"), string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/finish", "", assertion)
	if err != nil {
		return fmt.Errorf("replay assertion: %w", err)
	}
	report.check("replayed assertion is rejected", status == http.StatusUnauthorized, string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/begin", "",
		map[string]string{"username": username, "password": password})
	if err != nil {
		return fmt.Errorf("third login begin: %w", err)
	}
	var retryBegin beginReply
	if err := json.Unmarshal(data, &retryBegin); err != nil {
		return fmt.Errorf("third login begin reply: %w", err)
	}
	retryChallenge, err := base64.RawURLEncoding.DecodeString(retryBegin.Challenge)
	if err != nil {
		return fmt.Errorf("retry challenge: %w", err)
	}
	tamperedClientData, tamperedAuthData, tamperedSignature, err := auth.Assert(retryChallenge)
	if err != nil {
		return fmt.Errorf("tampered assert: %w", err)
	}
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/finish", "",
		map[string]string{
			"username":           username,
			"credential_id":      stored.ID,
			"client_data_json":   encode(tamperedClientData),
			"authenticator_data": encode(tamperedAuthData),
			"signature":          encode(webauthntest.TamperSignature(tamperedSignature)),
		})
	if err != nil {
		return fmt.Errorf("tampered finish: %w", err)
	}
	report.check("tampered signature is rejected", status == http.StatusUnauthorized, string(data))

	validClientData, validAuthData, validSignature, err := auth.Assert(retryChallenge)
	if err != nil {
		return fmt.Errorf("valid assert: %w", err)
	}
	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/finish", "",
		map[string]string{
			"username":           username,
			"credential_id":      stored.ID,
			"client_data_json":   encode(validClientData),
			"authenticator_data": encode(validAuthData),
			"signature":          encode(validSignature),
		})
	if err != nil {
		return fmt.Errorf("valid finish after rejection: %w", err)
	}
	report.check("challenge survives a failed assertion", status == http.StatusOK, string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/webauthn/credentials/remove", session.Token,
		map[string]string{"password": "wrong-pass", "credential_id": stored.ID})
	if err != nil {
		return fmt.Errorf("remove with wrong password: %w", err)
	}
	report.check("removal rejects a wrong password", status == http.StatusForbidden, string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/webauthn/credentials/remove", session.Token,
		map[string]string{"password": password, "credential_id": stored.ID})
	if err != nil {
		return fmt.Errorf("remove credential: %w", err)
	}
	report.check("removal deletes the credential", status == http.StatusNoContent, string(data))

	status, data, err = api.call(http.MethodGet, "/v1/account/webauthn/credentials", session.Token, nil)
	if err != nil {
		return fmt.Errorf("list after removal: %w", err)
	}
	report.check("credential list is empty after removal",
		status == http.StatusOK && !strings.Contains(string(data), stored.ID), string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/begin", "",
		map[string]string{"username": username, "password": password})
	if err != nil {
		return fmt.Errorf("login begin without keys: %w", err)
	}
	report.check("login without keys is answered like an enabled account",
		status == http.StatusAccepted && strings.Contains(string(data), "accepted"), string(data))

	status, data, err = api.call(http.MethodPost, "/v1/account/login/webauthn/passwordless/begin", "",
		map[string]string{"username": username})
	if err != nil {
		return fmt.Errorf("passwordless begin without keys: %w", err)
	}
	report.check("passwordless begin without keys is answered like an enabled account",
		status == http.StatusAccepted && strings.Contains(string(data), "accepted"), string(data))

	audit, err := adminCommand(adminAddr, "audit 50")
	if err != nil {
		return fmt.Errorf("audit: %w", err)
	}
	report.check("audit records registration, both logins and removal",
		strings.Contains(audit, "action=webauthn_register") &&
			strings.Contains(audit, "action=webauthn_remove") &&
			strings.Contains(audit, "password+security-key") &&
			strings.Contains(audit, "passwordless-security-key"),
		audit)

	if report.failed > 0 {
		return fmt.Errorf("%d checks failed", report.failed)
	}
	fmt.Println("all security-key checks passed")
	return nil
}
