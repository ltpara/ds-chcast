package agent

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// DevicePolicy is the server's managed-device statement delivered on every
// registration. A nil policy means the device is unmanaged and keeps local
// authentication. The agent transports the statement to the embedding host;
// only the host protocol can enforce it, so a host must treat a managed device
// with an unknown mode as managed rather than falling back to unmanaged.
type DevicePolicy struct {
	Mode              string `json:"mode"`
	PermissionCeiling uint32 `json:"permission_ceiling,omitempty"`
	PolicyVersion     uint32 `json:"policy_version"`
	UpdatedAtMS       int64  `json:"updated_at_ms,omitempty"`
}

// Valid reports whether the mode is one this build understands. The agent
// still forwards an unknown mode so a newer server policy reaches the host.
func (p DevicePolicy) Valid() bool {
	return p.Mode == "open" || p.Mode == "restricted"
}

func cloneDevicePolicy(policy *DevicePolicy) *DevicePolicy {
	if policy == nil {
		return nil
	}
	copied := *policy
	return &copied
}

func sameDevicePolicy(a, b *DevicePolicy) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	return *a == *b
}

// policyStateFile is the durable form of the last registration policy. The
// embedding host reads the same file as its managed marker, so a removed
// policy is written as managed=false instead of deleting the file: a stale
// "managed" marker would otherwise survive a restart. RecordedAtMS lets the
// host order state left by an interrupted write.
type policyStateFile struct {
	Managed           bool   `json:"managed"`
	Mode              string `json:"mode,omitempty"`
	PermissionCeiling uint32 `json:"permission_ceiling,omitempty"`
	PolicyVersion     uint32 `json:"policy_version,omitempty"`
	UpdatedAtMS       int64  `json:"updated_at_ms,omitempty"`
	RecordedAtMS      int64  `json:"recorded_at_ms"`
}

// loadPolicyState returns the persisted policy and whether the file exists.
// An unknown mode is preserved so the host can keep the device managed; only
// an incomplete managed statement is an error. A missing file is not managed.
func loadPolicyState(path string) (*DevicePolicy, bool, error) {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	var state policyStateFile
	if err := json.Unmarshal(data, &state); err != nil {
		return nil, false, fmt.Errorf("device policy state %s: %w", path, err)
	}
	if !state.Managed {
		return nil, true, nil
	}
	policy := &DevicePolicy{
		Mode:              state.Mode,
		PermissionCeiling: state.PermissionCeiling,
		PolicyVersion:     state.PolicyVersion,
		UpdatedAtMS:       state.UpdatedAtMS,
	}
	if policy.Mode == "" || policy.PolicyVersion == 0 {
		return nil, false, fmt.Errorf("device policy state %s: managed policy is incomplete", path)
	}
	return policy, true, nil
}

// policyAckFile is the host's confirmation that it accepted a policy. It
// repeats the policy's own update timestamp, so an acknowledgement left by a
// policy that was later deleted and recreated at the same version cannot
// confirm the new policy.
type policyAckFile struct {
	PolicyVersion     uint32 `json:"policy_version"`
	Mode              string `json:"mode"`
	PermissionCeiling uint32 `json:"permission_ceiling"`
	UpdatedAtMS       int64  `json:"updated_at_ms"`
	Applied           bool   `json:"applied"`
	AppliedAtMS       int64  `json:"applied_at_ms"`
	Detail            string `json:"detail"`
}

// readPolicyAck returns false when the host has not acknowledged anything.
// A malformed acknowledgement is treated the same way: the receipt stays
// unsent rather than claiming a result the host did not state.
func readPolicyAck(path string) (policyAckFile, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return policyAckFile{}, false
	}
	var ack policyAckFile
	if err := json.Unmarshal(data, &ack); err != nil {
		return policyAckFile{}, false
	}
	return ack, true
}

// confirms reports whether the acknowledgement names the exact policy
// instance, not merely a version number that may have been reused after a
// delete.
func (a policyAckFile) confirms(policy *DevicePolicy) bool {
	if policy == nil {
		return false
	}
	return a.PolicyVersion == policy.PolicyVersion && a.Mode == policy.Mode &&
		a.PermissionCeiling == policy.PermissionCeiling && a.UpdatedAtMS == policy.UpdatedAtMS
}

// policyReceiptRequest is the signed body of POST
// /v1/device-policies/{peer}/receipt.
type policyReceiptRequest struct {
	PolicyVersion uint32 `json:"policy_version"`
	Applied       bool   `json:"applied"`
	Detail        string `json:"detail"`
	Timestamp     int64  `json:"timestamp"`
	Signature     string `json:"signature"`
}

// savePolicyState atomically replaces the state file so an interrupted write
// cannot leave a half policy for the host to enforce. A nil policy records
// managed=false, which is the online-confirmed unmanaged state.
func savePolicyState(path string, policy *DevicePolicy, now time.Time) error {
	state := policyStateFile{RecordedAtMS: now.UnixMilli()}
	if policy != nil {
		state.Managed = true
		state.Mode = policy.Mode
		state.PermissionCeiling = policy.PermissionCeiling
		state.PolicyVersion = policy.PolicyVersion
		state.UpdatedAtMS = policy.UpdatedAtMS
	}
	data, err := json.MarshalIndent(state, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".policy-*.tmp")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0600); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}
