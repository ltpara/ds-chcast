// Package pgtest isolates PostgreSQL integration tests in disposable schemas.
// It is only used by tests; normal runs need no PostgreSQL installation.
package pgtest

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

// Schema creates a unique schema and returns a DSN whose search_path contains
// only that schema. In particular, destructive migration fixtures never touch
// the supplied database's public tables or another test's migration ledger.
// Stores opened with the result must close before the test cleanup runs.
func Schema(t testing.TB) string {
	t.Helper()
	base := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if base == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, base)
	if err != nil {
		t.Fatalf("connect PostgreSQL test database: %v", err)
	}
	defer conn.Close(context.Background())
	var random [12]byte
	if _, err := rand.Read(random[:]); err != nil {
		t.Fatal(err)
	}
	schema := "chast_test_" + hex.EncodeToString(random[:])
	quoted := pgx.Identifier{schema}.Sanitize()
	if _, err := conn.Exec(ctx, "CREATE SCHEMA "+quoted); err != nil {
		t.Fatalf("create isolated test schema: %v", err)
	}
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		conn, err := pgx.Connect(ctx, base)
		if err != nil {
			t.Errorf("connect for test schema cleanup: %v", err)
			return
		}
		defer conn.Close(context.Background())
		if _, err := conn.Exec(ctx, "DROP SCHEMA "+quoted+" CASCADE"); err != nil {
			t.Errorf("drop isolated test schema: %v", err)
		}
	})
	if strings.HasPrefix(base, "postgres://") || strings.HasPrefix(base, "postgresql://") {
		parsed, err := url.Parse(base)
		if err != nil {
			t.Fatal(err)
		}
		query := parsed.Query()
		query.Set("search_path", schema)
		parsed.RawQuery = query.Encode()
		return parsed.String()
	}
	// The generated identifier contains only lowercase letters, digits and '_'.
	return base + " search_path=" + schema
}
