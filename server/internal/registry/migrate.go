package registry

import (
	"database/sql"
	"fmt"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/schemamig"
)

// registrySchemaVersion is the schema generation this binary writes. Databases
// created before registry_schema_migrations existed are upgraded by the
// idempotent baseline migration.
const registrySchemaVersion = 2

var registryMigrations = []schemamig.Migration{
	{Version: 1, Name: "baseline", Apply: applyRegistryBaseline},
	{Version: 2, Name: "metadata_signed", Apply: applyRegistryMetadataSigned},
}

// applyRegistryMetadataSigned adds the trust flag for host-reported display
// metadata. The baseline intentionally keeps its original columns so this
// migration also upgrades databases that already recorded schema version 1.
func applyRegistryMetadataSigned(db *sql.DB) error {
	_, err := db.Exec(`ALTER TABLE peers ADD COLUMN metadata_signed INTEGER NOT NULL DEFAULT 0`)
	if err != nil && strings.Contains(err.Error(), "duplicate column name") {
		return nil
	}
	if err != nil {
		return fmt.Errorf("add metadata_signed: %w", err)
	}
	return nil
}

func applyRegistryBaseline(db *sql.DB) error {
	const schema = `CREATE TABLE IF NOT EXISTS peers (
		id          TEXT PRIMARY KEY,
		public_key  TEXT NOT NULL,
		fingerprint TEXT NOT NULL DEFAULT '',
		hostname    TEXT NOT NULL DEFAULT '',
		platform    TEXT NOT NULL DEFAULT '',
		version     TEXT NOT NULL DEFAULT '',
		alias       TEXT NOT NULL DEFAULT '',
		tags        TEXT NOT NULL DEFAULT '',
		created_at  INTEGER NOT NULL,
		last_seen   INTEGER NOT NULL DEFAULT 0
	)`
	if _, err := db.Exec(schema); err != nil {
		return fmt.Errorf("create schema: %w", err)
	}
	if err := ensurePeerColumns(db); err != nil {
		return fmt.Errorf("upgrade schema: %w", err)
	}
	return nil
}
