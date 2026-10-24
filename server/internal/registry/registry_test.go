package registry

import (
	"crypto/ed25519"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func hostKey(secret string) (ed25519.PrivateKey, string) {
	priv := identity.PrivateKeyFromSecret(secret)
	return priv, identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
}

func TestRegisterLookupAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json")
	reg, err := NewPersistent(10*time.Second, path)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Unix(1000, 0)
	reg.SetClock(func() time.Time { return now })

	priv, pub := hostKey("host-secret")
	_ = priv
	host, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, Fingerprint: "AB12", TTL: time.Minute})
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	if host.ID != "devbox" || host.Fingerprint != "AB12" {
		t.Fatalf("unexpected host: %+v", host)
	}
	reg.SetRelay("devbox", 41000, "")
	if !reg.VerifyHost("devbox", 41000) {
		t.Fatal("verify with the registered host failed")
	}
	if reg.VerifyHost("devbox", 41001) {
		t.Fatal("verify accepted a wrong port")
	}

	if reg.Online(host) {
		t.Fatal("host without a keepalive reported online")
	}
	if err := reg.Touch("devbox", "192.0.2.5:4000", []string{"10.0.0.5:4000"}, "cone"); err != nil {
		t.Fatalf("touch: %v", err)
	}
	host, _ = reg.Lookup("devbox")
	if !reg.Online(host) {
		t.Fatal("host with a fresh keepalive reported offline")
	}
	if host.Candidate != "192.0.2.5:4000" || host.NATType != "cone" || len(host.LocalAddrs) != 1 {
		t.Fatalf("candidate = %+v", host)
	}

	reloaded, err := NewPersistent(10*time.Second, path)
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := reloaded.Identity("devbox"); !ok {
		t.Fatal("identity did not survive a restart")
	}
	if _, err := reloaded.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatalf("re-register after restart: %v", err)
	}
}

func TestKeyMismatchAndIDValidation(t *testing.T) {
	reg := New(time.Minute)
	_, pub := hostKey("host-secret")
	_, other := hostKey("another-secret")
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	_, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: other, TTL: time.Minute})
	if !errors.Is(err, ErrKeyMismatch) {
		t.Fatalf("renew with another key: %v", err)
	}
	for _, id := range []string{"", "has space", "../x", strings.Repeat("a", 65)} {
		if ValidID(id) {
			t.Fatalf("id %q accepted", id)
		}
		if _, err := reg.Register(RegisterRequest{ID: id, PublicKey: pub, TTL: time.Minute}); err == nil {
			t.Fatalf("register accepted id %q", id)
		}
	}
	if err := reg.Touch("missing", "192.0.2.5:1", nil, ""); !errors.Is(err, ErrUnknownHost) {
		t.Fatalf("touch an unknown host: %v", err)
	}
}

func TestExpiryKeepsIdentity(t *testing.T) {
	reg := New(3 * time.Second)
	now := time.Unix(2000, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")
	_, other := hostKey("another-secret")
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: 20 * time.Second}); err != nil {
		t.Fatal(err)
	}
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
	if reg.IdentityCount() != 1 {
		t.Fatalf("identity count = %d", reg.IdentityCount())
	}
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: other, TTL: time.Minute}); !errors.Is(err, ErrKeyMismatch) {
		t.Fatalf("expired id was taken over: %v", err)
	}
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatalf("original key could not return: %v", err)
	}
	removed, err := reg.RemoveIdentity("devbox")
	if err != nil || !removed {
		t.Fatalf("remove identity: %v %v", removed, err)
	}
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: other, TTL: time.Minute}); err != nil {
		t.Fatalf("new key after administrative reset: %v", err)
	}
}

func TestOfflineAfterSilence(t *testing.T) {
	reg := New(5 * time.Second)
	now := time.Unix(3000, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")
	reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute})
	reg.Touch("devbox", "192.0.2.5:4000", nil, "")
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

func TestChallengeIsOneTimeAndBound(t *testing.T) {
	reg := New(time.Minute)
	now := time.Unix(4000, 0)
	reg.SetClock(func() time.Time { return now })
	priv, pub := hostKey("host-secret")
	reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute})

	nonce, err := reg.IssueChallenge("devbox", identity.DomainKeepalive)
	if err != nil {
		t.Fatal(err)
	}
	candidate := "198.51.100.7:51234"
	msg := identity.KeepaliveMessage("devbox", candidate, nonce)
	sig := identity.Sign(priv, msg)
	host, err := reg.VerifyChallenge("devbox", identity.DomainKeepalive, nonce, string(msg), sig)
	if err != nil || host.ID != "devbox" {
		t.Fatalf("verify: %v %+v", err, host)
	}
	if _, err := reg.VerifyChallenge("devbox", identity.DomainKeepalive, nonce, string(msg), sig); !errors.Is(err, ErrChallenge) {
		t.Fatalf("nonce was reusable: %v", err)
	}

	nonce, _ = reg.IssueChallenge("devbox", identity.DomainKeepalive)
	msg = identity.KeepaliveMessage("devbox", "198.51.100.8:1", nonce)
	if _, err := reg.VerifyChallenge("devbox", identity.DomainKeepalive, nonce, string(msg), identity.Sign(priv, msg)); err != nil {
		t.Fatal(err)
	}
	nonce, _ = reg.IssueChallenge("devbox", identity.DomainRelay)
	msg = identity.RelayMessage("devbox", 41000, candidate, nonce)
	if _, err := reg.VerifyChallenge("devbox", identity.DomainKeepalive, nonce, string(msg), identity.Sign(priv, msg)); !errors.Is(err, ErrChallenge) {
		t.Fatalf("challenge accepted for another purpose: %v", err)
	}
	nonce, _ = reg.IssueChallenge("devbox", identity.DomainKeepalive)
	otherPriv, _ := hostKey("attacker-secret")
	msg = identity.KeepaliveMessage("devbox", candidate, nonce)
	if _, err := reg.VerifyChallenge("devbox", identity.DomainKeepalive, nonce, string(msg), identity.Sign(otherPriv, msg)); !errors.Is(err, identity.ErrBadSignature) {
		t.Fatalf("signature from another key: %v", err)
	}

	nonce, _ = reg.IssueChallenge("devbox", identity.DomainKeepalive)
	now = now.Add(challengeTTL + time.Second)
	msg = identity.KeepaliveMessage("devbox", candidate, nonce)
	if _, err := reg.VerifyChallenge("devbox", identity.DomainKeepalive, nonce, string(msg), identity.Sign(priv, msg)); !errors.Is(err, ErrChallenge) {
		t.Fatalf("expired challenge: %v", err)
	}
}

func TestStoreRejectsCorruptState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json")
	if err := os.WriteFile(path, []byte(`{"version":1,"identities":[{"id":"bad id","public_key":"x"}]}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewPersistent(time.Minute, path); err == nil {
		t.Fatal("corrupt identity state was accepted")
	}
}

func TestCarrierTokenLifecycle(t *testing.T) {
	reg := New(3 * time.Second)
	now := time.Unix(5000, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")

	host, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute})
	if err != nil {
		t.Fatal(err)
	}
	token := host.CarrierToken
	if len(token) != 32 {
		t.Fatalf("carrier token = %q, want 32 hex characters", token)
	}
	if !reg.VerifyCarrier("devbox", token) {
		t.Fatal("fresh token was not accepted")
	}
	if reg.VerifyCarrier("devbox", token+"0") {
		t.Fatal("wrong token was accepted")
	}
	if reg.VerifyCarrier("elsewhere", token) {
		t.Fatal("token was accepted for an unknown host")
	}

	now = now.Add(30 * time.Second)
	renewed, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute})
	if err != nil {
		t.Fatal(err)
	}
	if renewed.CarrierToken != token {
		t.Fatalf("renewal rotated the token: %q -> %q", token, renewed.CarrierToken)
	}

	now = now.Add(61 * time.Second)
	if reg.VerifyCarrier("devbox", token) {
		t.Fatal("token outlived the lease")
	}
	back, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute})
	if err != nil {
		t.Fatal(err)
	}
	if back.CarrierToken == token {
		t.Fatal("a new lease reused the expired token")
	}
	if !reg.VerifyCarrier("devbox", back.CarrierToken) {
		t.Fatal("new lease token was not accepted")
	}
}

func TestHostOnlineTracksLease(t *testing.T) {
	reg := New(3 * time.Second)
	now := time.Unix(6000, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")
	if reg.HostOnline("devbox") {
		t.Fatal("unknown host reported online")
	}
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: 20 * time.Second}); err != nil {
		t.Fatal(err)
	}
	if !reg.HostOnline("devbox") {
		t.Fatal("registered host reported offline")
	}
	now = now.Add(21 * time.Second)
	if reg.HostOnline("devbox") {
		t.Fatal("expired host reported online")
	}
}
