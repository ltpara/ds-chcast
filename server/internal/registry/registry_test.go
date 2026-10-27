package registry

import (
	"bytes"
	"crypto/ed25519"
	"database/sql"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func hostKey(secret string) (ed25519.PrivateKey, string) {
	priv := identity.PrivateKeyFromSecret(secret)
	return priv, identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
}

func TestConcurrentStoreOpen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	errs := make([]error, 2)
	var wg sync.WaitGroup
	for i := range errs {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			reg, err := NewPersistent(10*time.Second, path)
			if err != nil {
				errs[i] = err
				return
			}
			reg.Close()
		}(i)
	}
	wg.Wait()
	for i, err := range errs {
		if err != nil {
			t.Fatalf("instance %d: %v", i+1, err)
		}
	}
	reg, err := NewPersistent(10*time.Second, path)
	if err != nil {
		t.Fatalf("reopen shared database: %v", err)
	}
	reg.Close()
}

func TestRegisterLookupAndPersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	reg, err := NewPersistent(10*time.Second, path)
	if err != nil {
		t.Fatal(err)
	}
	defer reg.Close()
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
	defer reloaded.Close()
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

func TestRegisterMovesIdentityToNewID(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	reg, err := NewPersistent(time.Minute, path)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Unix(3000, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")
	created, err := reg.Register(RegisterRequest{
		ID: "devbox", PublicKey: pub, Fingerprint: "AB12", Hostname: "Old Box", TTL: time.Minute,
	})
	if err != nil {
		t.Fatal(err)
	}
	alias := "Workshop"
	tags := []string{"lab", "desk"}
	if _, _, err := reg.SetLabels("devbox", &alias, &tags); err != nil {
		t.Fatal(err)
	}
	before, _ := reg.Identity("devbox")
	reg.SetRelay("devbox", 41000, "")

	now = now.Add(5 * time.Second)
	host, err := reg.Register(RegisterRequest{
		ID: "newbox", OldID: "devbox", PublicKey: pub, Hostname: "New Box", TTL: time.Minute,
	})
	if err != nil {
		t.Fatal(err)
	}
	if host.ID != "newbox" || host.Fingerprint != "AB12" || !host.ExpiresAt.After(now) {
		t.Fatalf("migrated lease = %+v", host)
	}
	if _, ok := reg.Identity("devbox"); ok {
		t.Fatal("the old identity survived the move")
	}
	if _, ok := reg.Lookup("devbox"); ok {
		t.Fatal("the old lease survived the move")
	}
	moved, ok := reg.Identity("newbox")
	if !ok {
		t.Fatal("the new identity was not created")
	}
	if !moved.CreatedAt.Equal(before.CreatedAt) {
		t.Fatalf("created at = %v, want %v", moved.CreatedAt, before.CreatedAt)
	}
	if moved.Alias != alias || len(moved.Tags) != 2 || moved.Tags[0] != "lab" {
		t.Fatalf("operator labels were lost: %+v", moved)
	}
	if moved.Hostname != "New Box" || moved.Fingerprint != "AB12" || moved.PublicKey != pub {
		t.Fatalf("migrated metadata = %+v", moved)
	}
	if created.ID != "devbox" {
		t.Fatalf("first registration id = %q", created.ID)
	}

	// A retry after the server already moved the record is a plain renewal.
	if _, err := reg.Register(RegisterRequest{
		ID: "newbox", OldID: "devbox", PublicKey: pub, TTL: time.Minute,
	}); err != nil {
		t.Fatalf("idempotent retry: %v", err)
	}
	if err := reg.Close(); err != nil {
		t.Fatal(err)
	}

	reloaded, err := NewPersistent(time.Minute, path)
	if err != nil {
		t.Fatal(err)
	}
	defer reloaded.Close()
	if _, ok := reloaded.Identity("devbox"); ok {
		t.Fatal("the old identity reappeared after a restart")
	}
	persisted, ok := reloaded.Identity("newbox")
	if !ok || persisted.Alias != alias {
		t.Fatalf("migrated identity after restart = %+v", persisted)
	}
}

func TestRegisterMigrationRequiresOwnership(t *testing.T) {
	reg := New(time.Minute)
	_, pub := hostKey("host-secret")
	_, other := hostKey("another-secret")
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	if _, err := reg.Register(RegisterRequest{ID: "taken", PublicKey: other, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	if _, err := reg.Register(RegisterRequest{
		ID: "hijack", OldID: "devbox", PublicKey: other, TTL: time.Minute,
	}); !errors.Is(err, ErrMigrationDenied) {
		t.Fatalf("foreign key migration: %v", err)
	}
	if _, err := reg.Register(RegisterRequest{
		ID: "taken", OldID: "devbox", PublicKey: pub, TTL: time.Minute,
	}); !errors.Is(err, ErrKeyMismatch) {
		t.Fatalf("migration onto a taken id: %v", err)
	}
	if _, err := reg.Register(RegisterRequest{
		ID: "devbox", OldID: "devbox", PublicKey: pub, TTL: time.Minute,
	}); err == nil {
		t.Fatal("migration accepted an unchanged id")
	}
	if _, err := reg.Register(RegisterRequest{
		ID: "newbox", OldID: "bad id", PublicKey: pub, TTL: time.Minute,
	}); err == nil {
		t.Fatal("migration accepted a malformed old id")
	}
	// An unknown old id is not proof of anything, but it is also not an error:
	// the client may retry after the server already moved the record.
	if _, err := reg.Register(RegisterRequest{
		ID: "standalone", OldID: "ghost", PublicKey: other, TTL: time.Minute,
	}); err != nil {
		t.Fatalf("registration with an unknown old id: %v", err)
	}
	if _, ok := reg.Identity("devbox"); !ok {
		t.Fatal("a denied migration changed the old identity")
	}
	if _, ok := reg.Identity("hijack"); ok {
		t.Fatal("a denied migration created the new identity")
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

func TestKeepaliveChallengesPerFamily(t *testing.T) {
	reg := New(time.Minute)
	priv, pub := hostKey("host-secret")
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	v4 := "198.51.100.7:51234"
	v6 := "[2001:db8::7]:51234"
	nonce4, err := reg.IssueChallengeScoped("devbox", identity.DomainKeepalive, "v4")
	if err != nil {
		t.Fatal(err)
	}
	nonce6, err := reg.IssueChallengeScoped("devbox", identity.DomainKeepalive, "v6")
	if err != nil {
		t.Fatal(err)
	}
	for _, step := range []struct{ nonce, candidate string }{
		{nonce6, v6},
		{nonce4, v4},
	} {
		msg := identity.KeepaliveMessage("devbox", step.candidate, step.nonce)
		if err := reg.TouchChallenge("devbox", step.nonce, step.candidate,
			identity.Sign(priv, msg), string(msg), nil, "", 0); err != nil {
			t.Fatalf("touch %s: %v", step.candidate, err)
		}
	}
	host, ok := reg.Lookup("devbox")
	if !ok || host.Candidate != v4 || host.CandidateV6 != v6 {
		t.Fatalf("one family erased the other: %+v", host)
	}
}

func TestPeerMetadataPersists(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	reg, err := NewPersistent(10*time.Second, path)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Unix(7000, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")
	if _, err := reg.Register(RegisterRequest{
		ID:          "devbox",
		PublicKey:   pub,
		Fingerprint: "AB12",
		Hostname:    "Studio PC",
		Platform:    "windows/amd64",
		Version:     "1.2.3",
		TTL:         time.Minute,
	}); err != nil {
		t.Fatalf("register: %v", err)
	}
	record, ok := reg.Identity("devbox")
	if !ok {
		t.Fatal("peer record missing after registration")
	}
	if record.Hostname != "Studio PC" || record.Platform != "windows/amd64" || record.Version != "1.2.3" {
		t.Fatalf("metadata = %+v", record)
	}
	if !record.CreatedAt.Equal(now) || !record.LastSeen.Equal(now) {
		t.Fatalf("times = created %v last seen %v", record.CreatedAt, record.LastSeen)
	}
	peers := reg.Identities()
	if len(peers) != 1 || peers[0].ID != "devbox" {
		t.Fatalf("peer list = %+v", peers)
	}
	if err := reg.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}

	reloaded, err := NewPersistent(10*time.Second, path)
	if err != nil {
		t.Fatal(err)
	}
	defer reloaded.Close()
	record, ok = reloaded.Identity("devbox")
	if !ok {
		t.Fatal("peer record did not survive a restart")
	}
	if record.Hostname != "Studio PC" || record.Platform != "windows/amd64" ||
		record.Version != "1.2.3" || record.Fingerprint != "AB12" {
		t.Fatalf("reloaded metadata = %+v", record)
	}
	if !record.LastSeen.Equal(now) {
		t.Fatalf("reloaded last seen = %v", record.LastSeen)
	}
}

func TestPeerMetadataSanitizedAndMerged(t *testing.T) {
	reg := New(time.Minute)
	now := time.Unix(7100, 0)
	reg.SetClock(func() time.Time { return now })
	_, pub := hostKey("host-secret")
	if _, err := reg.Register(RegisterRequest{
		ID:        "devbox",
		PublicKey: pub,
		Hostname:  "  Studio\x00 PC\x07  ",
		Platform:  strings.Repeat("p", maxPlatform+10),
		TTL:       time.Minute,
	}); err != nil {
		t.Fatal(err)
	}
	record, _ := reg.Identity("devbox")
	if record.Hostname != "Studio PC" {
		t.Fatalf("hostname = %q", record.Hostname)
	}
	if len([]rune(record.Platform)) != maxPlatform {
		t.Fatalf("platform length = %d", len([]rune(record.Platform)))
	}

	now = now.Add(5 * time.Second)
	if err := reg.Touch("devbox", "192.0.2.9:4000", nil, ""); err != nil {
		t.Fatal(err)
	}
	record, _ = reg.Identity("devbox")
	if !record.LastSeen.Equal(now) {
		t.Fatalf("last seen = %v", record.LastSeen)
	}

	// A renewal without metadata keeps what the host reported earlier.
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	record, _ = reg.Identity("devbox")
	if record.Hostname != "Studio PC" || record.Platform == "" {
		t.Fatalf("empty renewal cleared metadata: %+v", record)
	}
	if _, err := reg.Register(RegisterRequest{
		ID: "devbox", PublicKey: pub, Platform: "linux/arm64", Version: "2.0", TTL: time.Minute,
	}); err != nil {
		t.Fatal(err)
	}
	record, _ = reg.Identity("devbox")
	if record.Hostname != "Studio PC" || record.Platform != "linux/arm64" || record.Version != "2.0" {
		t.Fatalf("renewal merge = %+v", record)
	}
}

func TestLegacyJSONStateMigratesNextToDatabase(t *testing.T) {
	dir := t.TempDir()
	_, pub := hostKey("host-secret")
	legacy := filepath.Join(dir, "chast-server-state.json")
	state := fmt.Sprintf(`{"version":1,"identities":[{"id":"devbox","public_key":%q,"fingerprint":"AB12"}]}`, pub)
	if err := os.WriteFile(legacy, []byte(state), 0o600); err != nil {
		t.Fatal(err)
	}
	dbPath := filepath.Join(dir, "chast-server-state.db")
	reg, err := NewPersistent(time.Minute, dbPath)
	if err != nil {
		t.Fatalf("import legacy state: %v", err)
	}
	record, ok := reg.Identity("devbox")
	if !ok || record.Fingerprint != "AB12" {
		t.Fatalf("imported record = %+v", record)
	}
	if _, err := os.Stat(legacy + ".imported"); err != nil {
		t.Fatalf("legacy state was not archived: %v", err)
	}
	if err := reg.Close(); err != nil {
		t.Fatal(err)
	}
	reloaded, err := NewPersistent(time.Minute, dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer reloaded.Close()
	if _, ok := reloaded.Identity("devbox"); !ok {
		t.Fatal("imported peer did not survive a restart")
	}
}

func TestLegacyJSONStateMigratesInPlace(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json")
	_, pub := hostKey("host-secret")
	state := fmt.Sprintf(`{"version":1,"identities":[{"id":"devbox","public_key":%q}]}`, pub)
	if err := os.WriteFile(path, []byte(state), 0o600); err != nil {
		t.Fatal(err)
	}
	reg, err := NewPersistent(time.Minute, path)
	if err != nil {
		t.Fatalf("migrate in place: %v", err)
	}
	if _, ok := reg.Identity("devbox"); !ok {
		t.Fatal("in-place migration lost the peer")
	}
	if err := reg.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(data, []byte(sqliteHeader)) {
		t.Fatalf("state was not rewritten as SQLite: %q", data[:min(len(data), 16)])
	}
	if _, err := os.Stat(path + ".legacy"); err != nil {
		t.Fatalf("legacy state was not archived: %v", err)
	}
}

func TestStoreRejectsCorruptDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	if err := os.WriteFile(path, []byte(sqliteHeader+"this is not a database"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := NewPersistent(time.Minute, path); err == nil {
		t.Fatal("corrupt SQLite state was accepted")
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

func TestLabelsPersistAndSurviveRegistration(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	reg, err := NewPersistent(time.Minute, path)
	if err != nil {
		t.Fatal(err)
	}
	defer reg.Close()
	_, pub := hostKey("host-secret")
	if _, err := reg.Register(RegisterRequest{ID: "devbox", PublicKey: pub, Hostname: "Dev Box", TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	alias := "Studio PC"
	tags := []string{"office", "prod", "office", ""}
	record, found, err := reg.SetLabels("devbox", &alias, &tags)
	if err != nil || !found {
		t.Fatalf("set labels: %v %v", found, err)
	}
	if record.Alias != "Studio PC" || len(record.Tags) != 2 || record.Tags[0] != "office" || record.Tags[1] != "prod" {
		t.Fatalf("labels = %+v", record)
	}
	if _, found, err := reg.SetLabels("missing", &alias, nil); err != nil || found {
		t.Fatalf("labels for an unknown peer: found=%v err=%v", found, err)
	}

	// A host renewal updates host-reported metadata but never operator labels.
	if _, err := reg.Register(RegisterRequest{
		ID: "devbox", PublicKey: pub, Hostname: "Renamed", Version: "2.0", TTL: time.Minute,
	}); err != nil {
		t.Fatal(err)
	}
	record, _ = reg.Identity("devbox")
	if record.Alias != "Studio PC" || len(record.Tags) != 2 || record.Hostname != "Renamed" {
		t.Fatalf("labels after renewal: %+v", record)
	}

	reloaded, err := NewPersistent(time.Minute, path)
	if err != nil {
		t.Fatal(err)
	}
	defer reloaded.Close()
	record, ok := reloaded.Identity("devbox")
	if !ok || record.Alias != "Studio PC" || len(record.Tags) != 2 {
		t.Fatalf("labels after reopen: %+v ok=%v", record, ok)
	}
	empty, emptyTags := "", []string{}
	record, found, err = reloaded.SetLabels("devbox", &empty, &emptyTags)
	if err != nil || !found {
		t.Fatalf("clear labels: %v %v", found, err)
	}
	if record.Alias != "" || len(record.Tags) != 0 {
		t.Fatalf("cleared labels = %+v", record)
	}
	if err := reg.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestStoreUpgradesPreLabelSchema(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`CREATE TABLE peers (
		id          TEXT PRIMARY KEY,
		public_key  TEXT NOT NULL,
		fingerprint TEXT NOT NULL DEFAULT '',
		hostname    TEXT NOT NULL DEFAULT '',
		platform    TEXT NOT NULL DEFAULT '',
		version     TEXT NOT NULL DEFAULT '',
		created_at  INTEGER NOT NULL,
		last_seen   INTEGER NOT NULL DEFAULT 0
	)`); err != nil {
		t.Fatal(err)
	}
	_, pub := hostKey("host-secret")
	if _, err := db.Exec(`INSERT INTO peers
		(id, public_key, fingerprint, hostname, platform, version, created_at, last_seen)
		VALUES (?, ?, '', '', '', '', 1000, 0)`, "devbox", pub); err != nil {
		t.Fatal(err)
	}
	db.Close()

	store, err := NewStore(path)
	if err != nil {
		t.Fatalf("open pre-label schema: %v", err)
	}
	ids, err := store.Load()
	if err != nil || len(ids) != 1 {
		t.Fatalf("load pre-label schema: %v %d", err, len(ids))
	}
	if ids[0].Alias != "" || len(ids[0].Tags) != 0 {
		t.Fatalf("unexpected labels in old row: %+v", ids[0])
	}
	ids[0].Alias = "Legacy"
	ids[0].Tags = []string{"migrated"}
	if err := store.Save(ids[0]); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	reopened, err := NewStore(path)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	ids, err = reopened.Load()
	if err != nil || len(ids) != 1 {
		t.Fatalf("reload upgraded schema: %v %d", err, len(ids))
	}
	if ids[0].Alias != "Legacy" || len(ids[0].Tags) != 1 || ids[0].Tags[0] != "migrated" {
		t.Fatalf("labels after upgrade: %+v", ids[0])
	}
}

func TestTouchKeepsAddressFamiliesApart(t *testing.T) {
	reg := New(time.Minute)
	_, pub := hostKey("host-secret")
	if _, err := reg.Register(RegisterRequest{ID: "dual", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	if err := reg.Touch("dual", "192.0.2.10:4000",
		[]string{"10.0.0.10:4000", "[fd00::10]:4000"}, "cone"); err != nil {
		t.Fatal(err)
	}
	host, _ := reg.Lookup("dual")
	if host.Candidate != "192.0.2.10:4000" || host.CandidateV6 != "" {
		t.Fatalf("candidates: %+v", host)
	}
	if len(host.LocalAddrs) != 1 || host.LocalAddrs[0] != "10.0.0.10:4000" {
		t.Fatalf("ipv4 local addrs: %+v", host.LocalAddrs)
	}
	if len(host.LocalAddrsV6) != 1 || host.LocalAddrsV6[0] != "[fd00::10]:4000" {
		t.Fatalf("ipv6 local addrs: %+v", host.LocalAddrsV6)
	}

	// A later IPv6 keepalive must not erase the IPv4 mapping.
	if err := reg.Touch("dual", "[2001:db8::10]:4000", nil, ""); err != nil {
		t.Fatal(err)
	}
	host, _ = reg.Lookup("dual")
	if host.Candidate != "192.0.2.10:4000" || host.CandidateV6 != "[2001:db8::10]:4000" {
		t.Fatalf("candidates after v6 keepalive: %+v", host)
	}
	if !reg.Online(host) {
		t.Fatal("dual-stack host reported offline")
	}
}

func TestIPv6OnlyHostIsOnline(t *testing.T) {
	reg := New(time.Minute)
	_, pub := hostKey("host-secret")
	if _, err := reg.Register(RegisterRequest{ID: "v6only", PublicKey: pub, TTL: time.Minute}); err != nil {
		t.Fatal(err)
	}
	if err := reg.Touch("v6only", "[2001:db8::11]:4000", []string{"[2001:db8::11]:4000"}, "cone"); err != nil {
		t.Fatal(err)
	}
	host, _ := reg.Lookup("v6only")
	if host.Candidate != "" || host.CandidateV6 != "[2001:db8::11]:4000" {
		t.Fatalf("candidates: %+v", host)
	}
	if !reg.Online(host) {
		t.Fatal("IPv6-only host reported offline")
	}
}
