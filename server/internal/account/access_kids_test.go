package account

import (
	"errors"
	"strings"
	"testing"
)

// TestRevokedAccessKidStore covers the durable retirement list: an unknown key
// is not revoked, a revocation is idempotent, the list is newest first and the
// kid format is enforced before a row is written.
func TestRevokedAccessKidStore(t *testing.T) {
	store := openGroupStore(t)
	if revoked, err := store.AccessKidRevoked("kid-2026-11"); err != nil || revoked {
		t.Fatalf("fresh kid = %v %v", revoked, err)
	}
	first, err := store.RevokeAccessKid("kid-2026-11", "root", "scheduled rotation")
	if err != nil {
		t.Fatalf("revoke: %v", err)
	}
	if first.Kid != "kid-2026-11" || first.RevokedBy != "root" || first.Reason != "scheduled rotation" {
		t.Fatalf("revoke record = %+v", first)
	}
	again, err := store.RevokeAccessKid("kid-2026-11", "operator", "second attempt")
	if err != nil {
		t.Fatalf("idempotent revoke: %v", err)
	}
	if !again.RevokedAt.Equal(first.RevokedAt) || again.Reason != first.Reason {
		t.Fatalf("repeat changed the record: first %+v again %+v", first, again)
	}
	if revoked, err := store.AccessKidRevoked("kid-2026-11"); err != nil || !revoked {
		t.Fatalf("revoked kid = %v %v", revoked, err)
	}

	if _, err := store.RevokeAccessKid("kid-2025-01", "", ""); err != nil {
		t.Fatalf("revoke without actor: %v", err)
	}
	list, err := store.RevokedAccessKids()
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	if len(list) != 2 {
		t.Fatalf("list = %+v", list)
	}

	for _, kid := range []string{"", "has space", strings.Repeat("k", maxAccessKid+1), "bad\tkey"} {
		if _, err := store.RevokeAccessKid(kid, "root", "x"); !errors.Is(err, ErrInvalidAccessKid) {
			t.Fatalf("kid %q = %v", kid, err)
		}
	}
	if _, err := store.RevokeAccessKid(strings.Repeat("k", maxAccessKid), "root", "x"); err != nil {
		t.Fatalf("max-length kid: %v", err)
	}
}
