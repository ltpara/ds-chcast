package account

import (
	"errors"
	"testing"
)

// TestSessionPermissionCapIntersectsDevicePolicy covers the account-wide ceiling
// on its own, its intersection with a device policy, and removal by storing
// zero. The account cap must never widen a stricter device policy.
func TestSessionPermissionCapIntersectsDevicePolicy(t *testing.T) {
	store := openGroupStore(t)
	alice, err := store.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	bob, err := store.CreateUser("bob", "battery staple")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}

	if _, err := store.SetDevicePolicy("devbox", DevicePolicyOpen, 0x000f, nil, nil, "root"); err != nil {
		t.Fatalf("open policy: %v", err)
	}
	admission, err := store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil {
		t.Fatalf("uncapped admission: %v", err)
	}
	if admission.PermissionCeiling != 0x000f {
		t.Fatalf("uncapped ceiling = %#x", admission.PermissionCeiling)
	}

	if err := store.SetSessionPermissionCap("alice", 0x0003); err != nil {
		t.Fatalf("set alice cap: %v", err)
	}
	caps, err := store.SessionPermissionCaps()
	if err != nil {
		t.Fatalf("list caps: %v", err)
	}
	if len(caps) != 1 || caps["alice"] != 0x0003 {
		t.Fatalf("caps = %v", caps)
	}
	admission, err = store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil {
		t.Fatalf("capped admission: %v", err)
	}
	if admission.PermissionCeiling != 0x0003 {
		t.Fatalf("account cap on open device = %#x", admission.PermissionCeiling)
	}

	// The intersection is narrower than either layer on its own.
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyOpen, 0x0005, nil, nil, "root"); err != nil {
		t.Fatalf("narrow policy: %v", err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil {
		t.Fatalf("intersection admission: %v", err)
	}
	if admission.PermissionCeiling != 0x0001 {
		t.Fatalf("intersection = %#x, want 0x1", admission.PermissionCeiling)
	}
	admission, err = store.CheckDeviceAdmission("devbox", bob.ID)
	if err != nil {
		t.Fatalf("bob admission: %v", err)
	}
	if admission.PermissionCeiling != 0x0005 {
		t.Fatalf("uncapped bob = %#x", admission.PermissionCeiling)
	}

	if err := store.SetSessionPermissionCap("alice", 0); err != nil {
		t.Fatalf("clear alice cap: %v", err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil {
		t.Fatalf("cleared admission: %v", err)
	}
	if admission.PermissionCeiling != 0x0005 {
		t.Fatalf("cleared cap = %#x", admission.PermissionCeiling)
	}

	// A restrictive allow list still decides admission; the cap only narrows.
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 0x000c, []string{"alice"}, nil, "root"); err != nil {
		t.Fatalf("restricted policy: %v", err)
	}
	if err := store.SetSessionPermissionCap("alice", 0x0009); err != nil {
		t.Fatalf("restricted alice cap: %v", err)
	}
	if err := store.SetSessionPermissionCap("bob", 0xffff); err != nil {
		t.Fatalf("bob cap: %v", err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil || !admission.Allowed {
		t.Fatalf("restricted alice = %+v %v", admission, err)
	}
	if admission.PermissionCeiling != 0x0008 {
		t.Fatalf("restricted intersection = %#x", admission.PermissionCeiling)
	}
	admission, err = store.CheckDeviceAdmission("devbox", bob.ID)
	if err != nil {
		t.Fatalf("restricted bob: %v", err)
	}
	if admission.Allowed || admission.PermissionCeiling != 0x000c {
		t.Fatalf("cap admitted an unlisted account = %+v", admission)
	}

	// Unmanaged devices keep admitting the account and report its cap; the host
	// still authenticates locally, so no ticket is issued for them.
	if err := store.DeleteDevicePolicy("devbox"); err != nil {
		t.Fatalf("delete policy: %v", err)
	}
	admission, err = store.CheckDeviceAdmission("devbox", alice.ID)
	if err != nil || admission.Managed || !admission.Allowed {
		t.Fatalf("unmanaged admission = %+v %v", admission, err)
	}
	if admission.PermissionCeiling != 0x0009 {
		t.Fatalf("unmanaged account cap = %#x", admission.PermissionCeiling)
	}

	if _, err := store.SessionPermissionCap(alice.ID); err != nil {
		t.Fatalf("read alice cap: %v", err)
	}
	if _, err := store.SessionPermissionCap(""); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("blank subject = %v", err)
	}
	if _, err := store.SessionPermissionCap("ghost"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("unknown subject = %v", err)
	}
	if err := store.SetSessionPermissionCap("ghost", 1); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("set unknown cap = %v", err)
	}
}
