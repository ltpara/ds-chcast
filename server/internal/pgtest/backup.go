package pgtest

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
)

// BackupRestore dumps and restores only a schema created by Schema. Callers
// must close their stores first. The supplied database's other schemas are
// never dropped or restored. Real PostgreSQL client tools are required.
func BackupRestore(t testing.TB, dsn string) {
	t.Helper()
	bin := os.Getenv("CHAST_TEST_POSTGRES_BIN")
	if bin == "" {
		t.Fatal("CHAST_TEST_POSTGRES_BIN is required for backup/restore")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	conn, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close(context.Background())
	var schema string
	if err := conn.QueryRow(ctx, "SELECT current_schema()").Scan(&schema); err != nil {
		t.Fatal(err)
	}
	if !regexp.MustCompile(`^chast_test_[0-9a-f]{24}$`).MatchString(schema) {
		t.Fatalf("refusing to replace non-test schema %q", schema)
	}
	base := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if base == "" {
		t.Fatal("CHAST_TEST_POSTGRES_DSN is required for backup/restore")
	}
	archive := filepath.Join(t.TempDir(), "console.dump")
	run := func(name string, args ...string) {
		t.Helper()
		ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
		defer cancel()
		cmd := exec.CommandContext(ctx, filepath.Join(bin, name), args...)
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%s failed: %v\n%s", name, err, output)
		}
	}
	// The base DSN must be libpq-compatible; the search_path added by Schema
	// belongs only to pgx. Avoid putting it in pg_dump/pg_restore parameters.
	run("pg_dump", "--no-password", "--format=custom", "--schema="+schema, "--file="+archive, "--dbname="+base)
	info, err := os.Stat(archive)
	if err != nil || info.Size() == 0 {
		t.Fatalf("backup archive missing or empty: %v", err)
	}
	// A clean destination proves that the restored data, indexes, constraints,
	// collation and migration ledger came from the archive.
	dropCtx, dropCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer dropCancel()
	if _, err := conn.Exec(dropCtx, "DROP SCHEMA "+pgx.Identifier{schema}.Sanitize()+" CASCADE"); err != nil {
		t.Fatal(err)
	}
	run("pg_restore", "--no-password", "--exit-on-error", "--single-transaction", "--no-owner", "--no-privileges", "--dbname="+base, archive)
	t.Logf("pg_dump/pg_restore round trip: %d-byte archive, isolated schema %s", info.Size(), schema)
}
