package api_test

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type loginDeviceChallengeReply struct {
	ChallengeID string `json:"challenge_id"`
	Message     string `json:"message"`
}

type loginDevicesReply struct {
	Enabled bool `json:"enabled"`
	Devices []struct {
		ID         string `json:"id"`
		DeviceName string `json:"device_name"`
	} `json:"devices"`
}

func signedLoginDeviceProof(t *testing.T, base string, public ed25519.PublicKey, private ed25519.PrivateKey) map[string]string {
	t.Helper()
	publicText := base64.RawURLEncoding.EncodeToString(public)
	body, err := json.Marshal(map[string]string{"public_key": publicText})
	if err != nil {
		t.Fatal(err)
	}
	status, data := request(t, http.MethodPost, base+"/v1/account/login-device-challenge", string(body), "")
	if status != http.StatusOK {
		t.Fatalf("create login device challenge: %d %s", status, data)
	}
	var challenge loginDeviceChallengeReply
	if err := json.Unmarshal(data, &challenge); err != nil {
		t.Fatalf("decode login device challenge: %v", err)
	}
	message, err := base64.RawURLEncoding.DecodeString(challenge.Message)
	if err != nil {
		t.Fatalf("decode challenge message: %v", err)
	}
	return map[string]string{
		"challenge_id": challenge.ChallengeID,
		"public_key":   publicText,
		"signature":    base64.RawURLEncoding.EncodeToString(ed25519.Sign(private, message)),
	}
}

func TestLoginDeviceAllowlistEnrollmentEnforcementAndRevocation(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatal(err)
	}
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	proof := signedLoginDeviceProof(t, s.URL, public, private)
	body, _ := json.Marshal(map[string]any{
		"username": "alice", "password": "correct horse", "device_name": "desk",
		"device_proof": proof,
	})
	status, data := request(t, http.MethodPost, s.URL+"/v1/account/login", string(body), "")
	if status != http.StatusOK {
		t.Fatalf("proof login: %d %s", status, data)
	}
	var first loginReply
	if err := json.Unmarshal(data, &first); err != nil {
		t.Fatal(err)
	}
	deviceID := accountLoginDeviceID(public)

	status, data = request(t, http.MethodGet, s.URL+"/v1/account/login-devices", "", first.Token)
	if status != http.StatusOK {
		t.Fatalf("list enrolled device: %d %s", status, data)
	}
	var state loginDevicesReply
	if err := json.Unmarshal(data, &state); err != nil {
		t.Fatal(err)
	}
	if state.Enabled || len(state.Devices) != 1 || state.Devices[0].ID != deviceID || state.Devices[0].DeviceName != "desk" {
		t.Fatalf("unexpected enrolled device state: %+v", state)
	}

	status, data = request(t, http.MethodPut, s.URL+"/v1/account/login-devices/policy", `{"enabled":true}`, first.Token)
	if status != http.StatusOK {
		t.Fatalf("enable allowlist: %d %s", status, data)
	}
	if err := json.Unmarshal(data, &state); err != nil || !state.Enabled {
		t.Fatalf("allowlist state = %+v, err=%v", state, err)
	}

	body, _ = json.Marshal(map[string]string{"username": "alice", "password": "correct horse"})
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", string(body), "")
	if status != http.StatusUnauthorized {
		t.Fatalf("login without device proof = %d %s, want 401", status, data)
	}

	otherPublic, otherPrivate, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	otherProof := signedLoginDeviceProof(t, s.URL, otherPublic, otherPrivate)
	body, _ = json.Marshal(map[string]any{
		"username": "alice", "password": "correct horse", "device_name": "untrusted",
		"device_proof": otherProof,
	})
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", string(body), "")
	if status != http.StatusForbidden {
		t.Fatalf("login from untrusted device = %d %s, want 403", status, data)
	}

	knownProof := signedLoginDeviceProof(t, s.URL, public, private)
	body, _ = json.Marshal(map[string]any{
		"username": "alice", "password": "correct horse", "device_name": "desk",
		"device_proof": knownProof,
	})
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", string(body), "")
	if status != http.StatusOK {
		t.Fatalf("trusted device login after enforcement: %d %s", status, data)
	}
	var second loginReply
	if err := json.Unmarshal(data, &second); err != nil {
		t.Fatal(err)
	}

	status, data = request(t, http.MethodDelete, s.URL+"/v1/account/login-devices/"+deviceID, "", second.Token)
	if status != http.StatusNoContent {
		t.Fatalf("revoke trusted device: %d %s", status, data)
	}
	status, data = request(t, http.MethodGet, s.URL+"/v1/account", "", first.Token)
	if status != http.StatusUnauthorized {
		t.Fatalf("revoked device's old session remained usable: %d %s", status, data)
	}
}

func TestAdminCanRestoreLoginDeviceAccess(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Accounts.CreateUser("bob", "correct horse"); err != nil {
		t.Fatal(err)
	}
	admin := login(t, s.URL, "admin", "correct horse")
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	publicText := base64.RawURLEncoding.EncodeToString(public)
	body, _ := json.Marshal(map[string]string{
		"public_key":  publicText,
		"device_name": "Bob's viewer",
	})
	status, data := request(t, http.MethodPost,
		s.URL+"/v1/accounts/bob/login-devices", string(body), admin.Token)
	if status != http.StatusCreated {
		t.Fatalf("admin trust device: %d %s", status, data)
	}
	var state loginDevicesReply
	if err := json.Unmarshal(data, &state); err != nil {
		t.Fatal(err)
	}
	if state.Enabled || len(state.Devices) != 1 || state.Devices[0].ID != accountLoginDeviceID(public) {
		t.Fatalf("admin-trusted state = %+v", state)
	}

	status, data = request(t, http.MethodPut,
		s.URL+"/v1/accounts/bob/login-devices/policy", `{"enabled":true}`, admin.Token)
	if status != http.StatusOK {
		t.Fatalf("admin enable allowlist: %d %s", status, data)
	}
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", `{"username":"bob","password":"correct horse"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("bob login without proof = %d %s, want 401", status, data)
	}
	proof := signedLoginDeviceProof(t, s.URL, public, private)
	body, _ = json.Marshal(map[string]any{
		"username": "bob", "password": "correct horse", "device_proof": proof,
	})
	status, data = request(t, http.MethodPost, s.URL+"/v1/account/login", string(body), "")
	if status != http.StatusOK {
		t.Fatalf("bob trusted login: %d %s", status, data)
	}

	status, data = request(t, http.MethodPut,
		s.URL+"/v1/accounts/bob/login-devices/policy", `{"enabled":false}`, admin.Token)
	if status != http.StatusOK {
		t.Fatalf("admin disable allowlist recovery: %d %s", status, data)
	}
}

func accountLoginDeviceID(public ed25519.PublicKey) string {
	digest := sha256.Sum256(public)
	return hex.EncodeToString(digest[:])
}
