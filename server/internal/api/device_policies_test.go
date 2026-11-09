package api_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type devicePolicyReply struct {
	Managed bool `json:"managed"`
	Policy  *struct {
		PeerID            string   `json:"peer_id"`
		Mode              string   `json:"mode"`
		PermissionCeiling uint32   `json:"permission_ceiling"`
		PolicyVersion     uint32   `json:"policy_version"`
		AllowedUsers      []string `json:"allowed_users"`
		AllowedGroups     []string `json:"allowed_groups"`
		Receipt           *struct {
			PolicyVersion uint32 `json:"policy_version"`
			Applied       bool   `json:"applied"`
			Detail        string `json:"detail"`
		} `json:"receipt"`
	} `json:"policy"`
}

func decodePolicy(t *testing.T, data []byte) devicePolicyReply {
	t.Helper()
	var reply devicePolicyReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("policy reply: %v", err)
	}
	return reply
}

// TestDevicePolicyRoutes covers the console lifecycle and the host receipt
// authentication: only an admin edits policies, and only the registered host
// key may acknowledge a version.
func TestDevicePolicyRoutes(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, err := s.Accounts.SetRole("bob", account.RoleOperator); err != nil {
		t.Fatalf("promote bob: %v", err)
	}
	admin := login(t, s.URL, "alice", "correct horse").Token
	operator := login(t, s.URL, "bob", "battery staple").Token

	now := time.Now().Unix()
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "devbox secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register devbox: %d %s", status, body)
	}

	status, body := request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("unmanaged detail: %d %s", status, body)
	}
	if reply := decodePolicy(t, body); reply.Managed || reply.Policy != nil {
		t.Fatalf("unmanaged detail = %+v", reply)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", operator); status != http.StatusForbidden {
		t.Fatalf("operator detail = %d %s", status, body)
	}

	putBody := func(payload map[string]any) string {
		t.Helper()
		data, err := json.Marshal(payload)
		if err != nil {
			t.Fatal(err)
		}
		return string(data)
	}
	if status, body := request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		putBody(map[string]any{"mode": "open"}), operator); status != http.StatusForbidden {
		t.Fatalf("operator set = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		putBody(map[string]any{"mode": "closed"}), admin); status != http.StatusBadRequest {
		t.Fatalf("invalid mode = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		putBody(map[string]any{"mode": "restricted", "allowed_users": []string{"ghost"}}), admin); status != http.StatusBadRequest {
		t.Fatalf("unknown user = %d %s", status, body)
	}

	status, body = request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		putBody(map[string]any{"mode": "open"}), admin)
	if status != http.StatusOK {
		t.Fatalf("set open = %d %s", status, body)
	}
	if reply := decodePolicy(t, body); !reply.Managed || reply.Policy.PolicyVersion != 1 || reply.Policy.Mode != "open" {
		t.Fatalf("open policy = %+v", reply)
	}
	status, body = request(t, http.MethodPut, s.URL+"/v1/device-policies/devbox",
		putBody(map[string]any{
			"mode":               "restricted",
			"permission_ceiling": 5,
			"allowed_users":      []string{"alice"},
		}), admin)
	if status != http.StatusOK {
		t.Fatalf("set restricted = %d %s", status, body)
	}
	reply := decodePolicy(t, body)
	if reply.Policy.PolicyVersion != 2 || reply.Policy.Mode != "restricted" ||
		reply.Policy.PermissionCeiling != 5 || len(reply.Policy.AllowedUsers) != 1 {
		t.Fatalf("restricted policy = %+v", reply.Policy)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies", "", admin)
	if status != http.StatusOK {
		t.Fatalf("list policies: %d %s", status, body)
	}
	var list struct {
		Policies []struct {
			PeerID string `json:"peer_id"`
		} `json:"policies"`
	}
	if err := json.Unmarshal(body, &list); err != nil {
		t.Fatalf("list reply: %v", err)
	}
	if len(list.Policies) != 1 || list.Policies[0].PeerID != "devbox" {
		t.Fatalf("list = %+v", list.Policies)
	}

	// Registration is how a host learns the policy it must enforce; unmanaged
	// hosts get no device_policy field.
	type hostPolicyReply struct {
		DevicePolicy *struct {
			Mode              string `json:"mode"`
			PermissionCeiling uint32 `json:"permission_ceiling"`
			PolicyVersion     uint32 `json:"policy_version"`
			UpdatedAtMS       int64  `json:"updated_at_ms"`
		} `json:"device_policy"`
	}
	register := func() hostPolicyReply {
		t.Helper()
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, "devbox", "devbox secret", "AB12", time.Now().Unix()), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("re-register devbox: %d %s", status, body)
		}
		var reply hostPolicyReply
		if err := json.Unmarshal(body, &reply); err != nil {
			t.Fatalf("register reply: %v", err)
		}
		return reply
	}
	if reply := register(); reply.DevicePolicy == nil || reply.DevicePolicy.PolicyVersion != 2 ||
		reply.DevicePolicy.Mode != "restricted" || reply.DevicePolicy.PermissionCeiling != 5 ||
		reply.DevicePolicy.UpdatedAtMS == 0 {
		t.Fatalf("registration policy = %+v", reply.DevicePolicy)
	}

	receiptBody := func(version uint32, applied bool, detail string, timestamp int64, signature string) string {
		t.Helper()
		data, err := json.Marshal(map[string]any{
			"policy_version": version,
			"applied":        applied,
			"detail":         detail,
			"timestamp":      timestamp,
			"signature":      signature,
		})
		if err != nil {
			t.Fatal(err)
		}
		return string(data)
	}
	priv, _ := hostKey("devbox secret")
	receiptNow := time.Now().Unix()
	sign := func(version uint32, applied bool, detail string) string {
		return identity.Sign(priv, identity.HostDevicePolicyReceiptMessage(
			"devbox", version, applied, detail, receiptNow))
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/device-policies/devbox/receipt",
		receiptBody(2, true, "applied", receiptNow, "not-a-signature"), testutil.ServerKey); status != http.StatusUnauthorized {
		t.Fatalf("bad receipt signature = %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/device-policies/devbox/receipt",
		receiptBody(1, true, "applied", receiptNow, sign(1, true, "applied")), testutil.ServerKey); status != http.StatusConflict {
		t.Fatalf("stale receipt = %d %s", status, body)
	}
	status, body = request(t, http.MethodPost, s.URL+"/v1/device-policies/devbox/receipt",
		receiptBody(2, false, "card busy", receiptNow, sign(2, false, "card busy")), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("receipt = %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("detail after receipt: %d %s", status, body)
	}
	reply = decodePolicy(t, body)
	if reply.Policy.Receipt == nil || reply.Policy.Receipt.Applied ||
		reply.Policy.Receipt.PolicyVersion != 2 || reply.Policy.Receipt.Detail != "card busy" {
		t.Fatalf("receipt view = %+v", reply.Policy.Receipt)
	}

	if status, body := request(t, http.MethodDelete, s.URL+"/v1/device-policies/devbox", "", operator); status != http.StatusForbidden {
		t.Fatalf("operator delete = %d %s", status, body)
	}
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/device-policies/devbox", "", admin); status != http.StatusNoContent {
		t.Fatalf("delete = %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, s.URL+"/v1/device-policies/devbox", "", admin)
	if status != http.StatusOK {
		t.Fatalf("detail after delete: %d %s", status, body)
	}
	if reply := decodePolicy(t, body); reply.Managed {
		t.Fatalf("still managed after delete: %+v", reply)
	}
	if reply := register(); reply.DevicePolicy != nil {
		t.Fatalf("registration still carries a policy after delete: %+v", reply.DevicePolicy)
	}
}
