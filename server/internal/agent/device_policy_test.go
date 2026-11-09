package agent

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestApplyRegistrationCarriesDevicePolicy(t *testing.T) {
	h, _ := testHostAgent(t)
	var infos []HostInfo
	h.cfg.OnInfo = func(info HostInfo) { infos = append(infos, info) }

	h.applyRegistration(registerResult{
		Relay: "127.0.0.1:41000",
		DevicePolicy: &DevicePolicy{
			Mode:              "restricted",
			PermissionCeiling: 0x0f,
			PolicyVersion:     2,
			UpdatedAtMS:       99,
		},
	})
	info := h.Info()
	if info.Policy == nil {
		t.Fatal("registration policy was not exposed through HostInfo")
	}
	if info.Policy.Mode != "restricted" || info.Policy.PermissionCeiling != 0x0f ||
		info.Policy.PolicyVersion != 2 || info.Policy.UpdatedAtMS != 99 {
		t.Fatalf("policy = %+v", *info.Policy)
	}
	if len(infos) == 0 || infos[len(infos)-1].Policy == nil {
		t.Fatalf("policy change was not reported: %+v", infos)
	}
	events := len(infos)

	// The host renews every TTL; an unchanged statement must not repeat events.
	h.applyRegistration(registerResult{
		Relay: "127.0.0.1:41000",
		DevicePolicy: &DevicePolicy{
			Mode:              "restricted",
			PermissionCeiling: 0x0f,
			PolicyVersion:     2,
			UpdatedAtMS:       99,
		},
	})
	if len(infos) != events {
		t.Fatalf("repeated policy emitted %d extra events", len(infos)-events)
	}

	// Deleting the policy returns the device to unmanaged and is reported.
	h.applyRegistration(registerResult{Relay: "127.0.0.1:41000"})
	if h.Info().Policy != nil {
		t.Fatal("cleared policy is still exposed")
	}
	if len(infos) != events+1 {
		t.Fatalf("policy removal emitted %d events", len(infos)-events)
	}
	if infos[len(infos)-1].Policy != nil {
		t.Fatal("policy removal event still carries a policy")
	}
}

func TestRegisterResultDecodesDevicePolicy(t *testing.T) {
	var result registerResult
	body := `{"device_policy":{"mode":"open","permission_ceiling":15,"policy_version":4,"updated_at_ms":123}}`
	if err := json.Unmarshal([]byte(body), &result); err != nil {
		t.Fatal(err)
	}
	if result.DevicePolicy == nil {
		t.Fatal("device_policy was not decoded")
	}
	if got := *result.DevicePolicy; got.Mode != "open" || got.PermissionCeiling != 15 ||
		got.PolicyVersion != 4 || got.UpdatedAtMS != 123 {
		t.Fatalf("policy = %+v", got)
	}

	if err := json.Unmarshal([]byte(`{"device_policy":null}`), &result); err != nil {
		t.Fatal(err)
	}
	if result.DevicePolicy != nil {
		t.Fatalf("null policy decoded as %+v", *result.DevicePolicy)
	}
}

func TestDevicePolicyValidModes(t *testing.T) {
	for _, tc := range []struct {
		mode string
		want bool
	}{
		{"open", true},
		{"restricted", true},
		{"closed", false},
		{"", false},
	} {
		if got := (DevicePolicy{Mode: tc.mode}).Valid(); got != tc.want {
			t.Errorf("mode %q valid = %v, want %v", tc.mode, got, tc.want)
		}
	}
}

func TestPolicyAckConfirmsExactPolicy(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "host.policy-ack.json")
	policy := &DevicePolicy{Mode: "restricted", PermissionCeiling: 15, PolicyVersion: 3, UpdatedAtMS: 1000}
	write := func(body string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
	}

	write(`{"policy_version":3,"mode":"restricted","permission_ceiling":15,"updated_at_ms":1000,"applied":true,"applied_at_ms":1200,"detail":"host applied"}`)
	ack, ok := readPolicyAck(path)
	if !ok || !ack.confirms(policy) || !ack.Applied || ack.Detail != "host applied" {
		t.Fatalf("matching ack = %+v ok=%v", ack, ok)
	}

	write(`{"policy_version":2,"mode":"restricted","permission_ceiling":15,"updated_at_ms":1000,"applied":true}`)
	if ack, _ := readPolicyAck(path); ack.confirms(policy) {
		t.Fatal("an older version confirmed the current policy")
	}
	write(`{"policy_version":3,"mode":"open","permission_ceiling":15,"updated_at_ms":1000,"applied":true}`)
	if ack, _ := readPolicyAck(path); ack.confirms(policy) {
		t.Fatal("a different mode confirmed the current policy")
	}
	// A host that did not understand the ceiling (or saw a different one) must
	// not confirm a policy it cannot enforce.
	write(`{"policy_version":3,"mode":"restricted","permission_ceiling":7,"updated_at_ms":1000,"applied":true}`)
	if ack, _ := readPolicyAck(path); ack.confirms(policy) {
		t.Fatal("a different permission ceiling confirmed the current policy")
	}
	// A deleted policy recreated at the same version must not reuse the old
	// acknowledgement, which carries the old policy's update timestamp.
	write(`{"policy_version":3,"mode":"restricted","permission_ceiling":15,"updated_at_ms":900,"applied":true}`)
	if ack, _ := readPolicyAck(path); ack.confirms(policy) {
		t.Fatal("a stale acknowledgement confirmed a recreated policy")
	}

	write("not json")
	if _, ok := readPolicyAck(path); ok {
		t.Fatal("a malformed acknowledgement was accepted")
	}
	os.Remove(path)
	if _, ok := readPolicyAck(path); ok {
		t.Fatal("a missing acknowledgement was accepted")
	}
	if (policyAckFile{}).confirms(nil) {
		t.Fatal("an unmanaged device must not be confirmed")
	}
}

func TestPolicyStateRecordsRemoval(t *testing.T) {
	h, _ := testHostAgent(t)
	statePath := filepath.Join(t.TempDir(), "host.policy.json")
	h.cfg.PolicyState = statePath

	h.applyRegistration(registerResult{
		Relay: "127.0.0.1:41000",
		DevicePolicy: &DevicePolicy{
			Mode:              "restricted",
			PermissionCeiling: 0x0f,
			PolicyVersion:     3,
			UpdatedAtMS:       1234,
		},
	})
	policy, exists, err := loadPolicyState(statePath)
	if err != nil || !exists || policy == nil {
		t.Fatalf("load = %+v, %v, %v", policy, exists, err)
	}
	if policy.Mode != "restricted" || policy.PermissionCeiling != 0x0f ||
		policy.PolicyVersion != 3 || policy.UpdatedAtMS != 1234 {
		t.Fatalf("persisted policy = %+v", *policy)
	}

	// A restart seeded from the file must notice that the server removed the
	// policy and rewrite the marker instead of leaving it managed forever.
	h2, _ := testHostAgent(t)
	h2.cfg.PolicyState = statePath
	loaded, exists, err := loadPolicyState(statePath)
	if err != nil || !exists || loaded == nil {
		t.Fatalf("restart load = %+v, %v, %v", loaded, exists, err)
	}
	h2.policy = loaded
	h2.policySaved = true
	h2.applyRegistration(registerResult{Relay: "127.0.0.1:41000"})
	if policy, exists, err = loadPolicyState(statePath); err != nil || !exists || policy != nil {
		t.Fatalf("after removal load = %+v, %v, %v", policy, exists, err)
	}
	data, err := os.ReadFile(statePath)
	if err != nil {
		t.Fatal(err)
	}
	var state policyStateFile
	if err := json.Unmarshal(data, &state); err != nil {
		t.Fatal(err)
	}
	if state.Managed || state.PolicyVersion != 0 {
		t.Fatalf("removal state = %+v", state)
	}
}

func TestPolicyStateRejectsIncompleteFile(t *testing.T) {
	dir := t.TempDir()
	if policy, exists, err := loadPolicyState(filepath.Join(dir, "missing.json")); err != nil ||
		exists || policy != nil {
		t.Fatalf("missing file = %+v, %v, %v", policy, exists, err)
	}

	unmanaged := filepath.Join(dir, "unmanaged.json")
	if err := os.WriteFile(unmanaged, []byte(`{"managed":false,"recorded_at_ms":7}`), 0600); err != nil {
		t.Fatal(err)
	}
	if policy, exists, err := loadPolicyState(unmanaged); err != nil || !exists || policy != nil {
		t.Fatalf("unmanaged file = %+v, %v, %v", policy, exists, err)
	}

	for name, body := range map[string]string{
		"garbage":  `not json`,
		"no mode":  `{"managed":true,"policy_version":2}`,
		"no vers.": `{"managed":true,"mode":"open"}`,
	} {
		path := filepath.Join(dir, name+".json")
		if err := os.WriteFile(path, []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
		if _, _, err := loadPolicyState(path); err == nil {
			t.Errorf("%s was accepted", name)
		}
	}
}

func TestPolicyStateRetriesFailedWrite(t *testing.T) {
	dir := t.TempDir()
	blocker := filepath.Join(dir, "blocker")
	if err := os.WriteFile(blocker, []byte("x"), 0600); err != nil {
		t.Fatal(err)
	}
	h, _ := testHostAgent(t)
	// The parent of the state path is a file, so MkdirAll must fail.
	h.cfg.PolicyState = filepath.Join(blocker, "host.policy.json")
	policy := &DevicePolicy{Mode: "open", PolicyVersion: 5}
	h.applyRegistration(registerResult{Relay: "127.0.0.1:41000", DevicePolicy: policy})
	if h.policySaved {
		t.Fatal("failed write was recorded as saved")
	}
	if _, exists, _ := loadPolicyState(h.cfg.PolicyState); exists {
		t.Fatal("blocked path unexpectedly holds a policy")
	}

	// An unchanged renewal retries the write once the path becomes usable.
	usable := filepath.Join(dir, "host.policy.json")
	h.cfg.PolicyState = usable
	h.applyRegistration(registerResult{Relay: "127.0.0.1:41000", DevicePolicy: policy})
	if !h.policySaved {
		t.Fatal("retry did not mark the policy saved")
	}
	stored, exists, err := loadPolicyState(usable)
	if err != nil || !exists || stored == nil || stored.PolicyVersion != 5 {
		t.Fatalf("retried load = %+v, %v, %v", stored, exists, err)
	}
}
