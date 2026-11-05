package registry

import (
	"crypto/ed25519"
	"os"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

// TestPostgresStore is gated on CHAST_TEST_POSTGRES_DSN like the account
// integration test so the default test run needs no external database.
func TestPostgresStore(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	store, err := NewStorePostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	id := "pg" + strconv.FormatInt(time.Now().UnixNano(), 36)
	priv := identity.PrivateKeyFromSecret("pg-registry-test")
	record := Identity{
		ID:          id,
		PublicKey:   identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		Fingerprint: "PG01",
		Hostname:    "postgres-host",
		Platform:    "windows",
		Version:     "1.0.0",
		Alias:       "pg alias",
		Tags:        []string{"lab", "pg"},
		CreatedAt:   time.Now().UTC().Truncate(time.Millisecond),
		LastSeen:    time.Now().UTC().Truncate(time.Millisecond),
	}
	if err := store.Save(record); err != nil {
		t.Fatal(err)
	}
	records, err := store.Load()
	if err != nil {
		t.Fatal(err)
	}
	var found *Identity
	for i := range records {
		if records[i].ID == id {
			found = &records[i]
		}
	}
	if found == nil {
		t.Fatalf("peer %s was not loaded", id)
	}
	if found.Hostname != record.Hostname || found.Alias != record.Alias || len(found.Tags) != 2 {
		t.Fatalf("loaded record: %+v", found)
	}
	if !found.CreatedAt.Equal(record.CreatedAt) {
		t.Fatalf("created_at = %v, want %v", found.CreatedAt, record.CreatedAt)
	}
	if err := store.Delete(id); err != nil {
		t.Fatal(err)
	}
	records, err = store.Load()
	if err != nil {
		t.Fatal(err)
	}
	for _, other := range records {
		if other.ID == id {
			t.Fatalf("peer %s survived delete", id)
		}
	}
}
