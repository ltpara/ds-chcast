package registry

import (
	"database/sql"
	"fmt"

	"github.com/ltpara/ds-chcast/server/internal/schemamig"
)

var registryPostgresMigrations = []schemamig.Migration{
	{Version: 1, Name: "baseline", Apply: applyRegistryPostgresBaseline},
	{Version: 2, Name: "wake targets", Apply: applyRegistryPostgresWakeTargets},
}

func applyRegistryPostgresBaseline(db *sql.DB) error {
	const schema = `CREATE TABLE IF NOT EXISTS peers (
		id                 TEXT PRIMARY KEY,
		public_key         TEXT NOT NULL,
		fingerprint        TEXT NOT NULL DEFAULT '',
		hostname           TEXT NOT NULL DEFAULT '',
		platform           TEXT NOT NULL DEFAULT '',
		version            TEXT NOT NULL DEFAULT '',
		metadata_signature TEXT NOT NULL DEFAULT '',
		alias              TEXT NOT NULL DEFAULT '',
		tags               TEXT NOT NULL DEFAULT '',
		wake_targets       TEXT NOT NULL DEFAULT '',
		created_at         BIGINT NOT NULL,
		last_seen          BIGINT NOT NULL DEFAULT 0
	)`
	if _, err := db.Exec(schema); err != nil {
		return fmt.Errorf("create PostgreSQL schema: %w", err)
	}
	return nil
}

func applyRegistryPostgresWakeTargets(db *sql.DB) error {
	if _, err := db.Exec(
		`ALTER TABLE peers ADD COLUMN IF NOT EXISTS wake_targets TEXT NOT NULL DEFAULT ''`); err != nil {
		return fmt.Errorf("add wake targets column: %w", err)
	}
	return nil
}
