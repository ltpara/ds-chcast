package registry

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func hash(ch rune) string { return strings.Repeat(string(ch), 64) }

func TestRegisterAndLookup(t *testing.T) {
	reg := New(10 * time.Second)
	now := time.Unix(1000, 0)
	reg.SetClock(func() time.Time { return now })

	host, err := reg.Register(RegisterRequest{ID: "devbox", KeyHash: hash('a'), Fingerprint: "AB12", TTL: time.Minute})
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	if host.ID != "devbox" || host.Fingerprint != "AB12" {
		t.Fatalf("unexpected host: %+v", host)
	}
	reg.SetRelayPort("devbox", 41000)
	if !reg.VerifyHost("devbox", hash('a'), 41000) {
		t.Fatal("verify with the registered key failed")
	}
	if reg.VerifyHost("devbox", hash('b'), 41000) {
		t.Fatal("verify accepted a wrong key")
	}
	if reg.VerifyHost("devbox", hash('a'), 41001) {
		t.Fatal("verify accepted a wrong port")
	}

	if reg.Online(host) {
		t.Fatal("host without a keepalive reported online")
	}
	if err := reg.Touch("devbox", hash('a'), "192.0.2.5:4000"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	host, _ = reg.Lookup("devbox")
	if !reg.Online(host) {
		t.Fatal("host with a fresh keepalive reported offline")
	}
	if host.Candidate != "192.0.2.5:4000" {
		t.Fatalf("candidate = %q", host.Candidate)
	}
}

func TestKeyMismatchAndIDValidation(t *testing.T) {
	reg := New(time.Minute)
	if _, err := reg.Register(RegisterRequest{ID: "devbox", KeyHash: hash('a'), TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	_, err := reg.Register(RegisterRequest{ID: "devbox", KeyHash: hash('b'), TTL: time.Minute})
	if !errors.Is(err, ErrKeyMismatch) {
		t.Fatalf("renew with another key: %v", err)
	}
	for _, id := range []string{"", "has space", "../x", strings.Repeat("a", 65)} {
		if ValidID(id) {
			t.Fatalf("id %q accepted", id)
		}
		if _, err := reg.Register(RegisterRequest{ID: id, KeyHash: hash('a'), TTL: time.Minute}); err == nil {
			t.Fatalf("register accepted id %q", id)
		}
	}
	if err := reg.Touch("devbox", hash('b'), "192.0.2.5:1"); !errors.Is(err, ErrKeyMismatch) {
		t.Fatalf("touch with a wrong key: %v", err)
	}
	if err := reg.Touch("missing", hash('a'), "192.0.2.5:1"); !errors.Is(err, ErrUnknownHost) {
		t.Fatalf("touch an unknown host: %v", err)
	}
}

func TestExpiry(t *testing.T) {
	reg := New(3 * time.Second)
	now := time.Unix(2000, 0)
	reg.SetClock(func() time.Time { return now })
	reg.Register(RegisterRequest{ID: "devbox", KeyHash: hash('a'), TTL: 20 * time.Second})
	now = now.Add(10 * time.Second)
	if _, ok := reg.Lookup("devbox"); !ok {
		t.Fatal("host vanished before its lease")
	}
	if expired := reg.Expire(); len(expired) != 0 {
		t.Fatalf("expired too early: %+v", expired)
	}
	now = now.Add(11 * time.Second)
	if _, ok := reg.Lookup("devbox"); ok {
		t.Fatal("host survived its lease")
	}
	expired := reg.Expire()
	if len(expired) != 1 || expired[0].ID != "devbox" {
		t.Fatalf("expire returned %+v", expired)
	}
	if reg.Count() != 0 {
		t.Fatalf("count = %d", reg.Count())
	}
}

func TestOfflineAfterSilence(t *testing.T) {
	reg := New(5 * time.Second)
	now := time.Unix(3000, 0)
	reg.SetClock(func() time.Time { return now })
	reg.Register(RegisterRequest{ID: "devbox", KeyHash: hash('a'), TTL: time.Minute})
	reg.Touch("devbox", hash('a'), "192.0.2.5:4000")
	host, _ := reg.Lookup("devbox")
	if !reg.Online(host) {
		t.Fatal("fresh keepalive reported offline")
	}
	now = now.Add(6 * time.Second)
	if reg.Online(host) {
		t.Fatal("stale keepalive reported online")
	}
	if _, ok := reg.Lookup("devbox"); !ok {
		t.Fatal("lease should outlive the offline window")
	}
}
