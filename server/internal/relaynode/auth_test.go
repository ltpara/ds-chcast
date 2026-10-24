package relaynode

import (
	"crypto/ed25519"
	"errors"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func testKey(secret string) (ed25519.PrivateKey, string) {
	priv := identity.PrivateKeyFromSecret(secret)
	return priv, identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
}

func TestAuthenticatorLeaseLifecycle(t *testing.T) {
	auth := NewAuthenticator()
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	auth.SetClock(func() time.Time { return now })

	_, pub := testKey("host-a")
	if _, err := auth.Put("devbox", pub, "token-a", 41000, time.Minute); err != nil {
		t.Fatalf("put: %v", err)
	}
	info, ok := auth.Lookup("devbox")
	if !ok || info.PublicKey != pub || info.Port != 41000 {
		t.Fatalf("lookup = %+v, %v", info, ok)
	}
	if !auth.VerifyHost("devbox", 41000) || auth.VerifyHost("devbox", 41001) {
		t.Fatal("VerifyHost accepted the wrong port")
	}
	if !auth.HostOnline("devbox") || auth.HostOnline("ghost") {
		t.Fatal("HostOnline disagreed with the lease set")
	}
	if !auth.VerifyCarrier("devbox", "token-a") {
		t.Fatal("correct carrier token was refused")
	}
	for _, token := range []string{"", "token-b", "token-aa"} {
		if auth.VerifyCarrier("devbox", token) {
			t.Fatalf("carrier token %q was accepted", token)
		}
	}
	if auth.Count() != 1 {
		t.Fatalf("count = %d", auth.Count())
	}

	now = now.Add(30 * time.Second)
	renewed, err := auth.Put("devbox", pub, "token-b", 41000, time.Minute)
	if err != nil {
		t.Fatalf("renew: %v", err)
	}
	if !renewed.ExpiresAt.Equal(now.Add(time.Minute)) {
		t.Fatalf("renewed expiry = %v", renewed.ExpiresAt)
	}
	if !auth.VerifyCarrier("devbox", "token-b") || auth.VerifyCarrier("devbox", "token-a") {
		t.Fatal("renewal did not replace the carrier token")
	}

	_, otherPub := testKey("host-b")
	if _, err := auth.Put("devbox", otherPub, "token-c", 41000, time.Minute); !errors.Is(err, ErrKeyMismatch) {
		t.Fatalf("key mismatch = %v", err)
	}

	now = now.Add(2 * time.Minute)
	if _, ok := auth.Lookup("devbox"); ok {
		t.Fatal("expired lease still resolves")
	}
	if auth.HostOnline("devbox") || auth.VerifyHost("devbox", 41000) || auth.Count() != 0 {
		t.Fatal("expired lease is still live")
	}
	if ids := auth.IDs(); len(ids) != 0 {
		t.Fatalf("ids = %v", ids)
	}

	// An id whose lease lapsed may be rebound to a new host key.
	if _, err := auth.Put("devbox", otherPub, "token-c", 41001, time.Minute); err != nil {
		t.Fatalf("rebind after expiry: %v", err)
	}
	if !auth.VerifyHost("devbox", 41001) {
		t.Fatal("rebound lease does not verify")
	}
}

func TestAuthenticatorChallengeIsOneShot(t *testing.T) {
	auth := NewAuthenticator()
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	auth.SetClock(func() time.Time { return now })
	priv, pub := testKey("host-a")
	if _, err := auth.Put("devbox", pub, "token", 41000, time.Minute); err != nil {
		t.Fatalf("put: %v", err)
	}

	nonce, err := auth.IssueChallenge("devbox", identity.DomainRelay)
	if err != nil {
		t.Fatalf("challenge: %v", err)
	}
	message := identity.RelayMessage("devbox", 41000, "127.0.0.1:5000", nonce)
	signature := identity.Sign(priv, message)
	if !auth.CheckChallenge("devbox", identity.DomainRelay, nonce, string(message), signature) {
		t.Fatal("valid relay proof was refused")
	}
	if auth.CheckChallenge("devbox", identity.DomainRelay, nonce, string(message), signature) {
		t.Fatal("relay proof was accepted twice")
	}

	nonce, _ = auth.IssueChallenge("devbox", identity.DomainRelay)
	if auth.CheckChallenge("devbox", identity.DomainCarrierHost, nonce, string(message), signature) {
		t.Fatal("challenge was accepted for another purpose")
	}
	message = identity.RelayMessage("devbox", 41000, "127.0.0.1:5000", nonce)
	signature = identity.Sign(priv, message)
	if auth.CheckChallenge("devbox", identity.DomainRelay, nonce, string(message)+"x", signature) {
		t.Fatal("challenge was accepted for another message")
	}

	nonce, _ = auth.IssueChallenge("devbox", identity.DomainRelay)
	message = identity.RelayMessage("devbox", 41000, "127.0.0.1:5000", nonce)
	signature = identity.Sign(priv, message)
	now = now.Add(challengeTTL + time.Second)
	if auth.CheckChallenge("devbox", identity.DomainRelay, nonce, string(message), signature) {
		t.Fatal("expired challenge was accepted")
	}

	if _, err := auth.IssueChallenge("ghost", identity.DomainRelay); !errors.Is(err, ErrUnknownLease) {
		t.Fatalf("challenge for unknown lease = %v", err)
	}
}

func TestAuthenticatorPruneAndRemove(t *testing.T) {
	auth := NewAuthenticator()
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	auth.SetClock(func() time.Time { return now })
	_, pub := testKey("host-a")
	if _, err := auth.Put("devbox", pub, "token", 41000, time.Minute); err != nil {
		t.Fatalf("put: %v", err)
	}
	nonce, _ := auth.IssueChallenge("devbox", identity.DomainRelay)
	if _, err := auth.Put("keep", pub, "token", 41001, 10*time.Minute); err != nil {
		t.Fatalf("put keep: %v", err)
	}

	now = now.Add(2 * time.Minute)
	expired := auth.Prune(now)
	if len(expired) != 1 || expired[0] != "devbox" {
		t.Fatalf("pruned = %v", expired)
	}
	if auth.Count() != 1 || !auth.HostOnline("keep") {
		t.Fatal("prune dropped a live lease")
	}
	if auth.CheckChallenge("devbox", identity.DomainRelay, nonce, "m", "s") {
		t.Fatal("pruned lease still answers challenges")
	}
	if again := auth.Prune(now); len(again) != 0 {
		t.Fatalf("second prune = %v", again)
	}

	nonce, err := auth.IssueChallenge("keep", identity.DomainRelay)
	if err != nil {
		t.Fatalf("challenge keep: %v", err)
	}
	if !auth.Remove("keep") {
		t.Fatal("remove reported no lease")
	}
	if auth.Remove("keep") {
		t.Fatal("remove reported a lease twice")
	}
	if auth.HostOnline("keep") || auth.CheckChallenge("keep", identity.DomainRelay, nonce, "m", "s") {
		t.Fatal("removed lease is still authenticated")
	}
}

func TestAuthenticatorRejectsMalformedPublicKey(t *testing.T) {
	auth := NewAuthenticator()
	if _, err := auth.Put("devbox", "not-a-key", "token", 41000, time.Minute); !errors.Is(err, identity.ErrBadPublicKey) {
		t.Fatalf("malformed key = %v", err)
	}
	if auth.Count() != 0 {
		t.Fatal("malformed key created a lease")
	}
}
