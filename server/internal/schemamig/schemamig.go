// Package schemamig applies ordered, forward-only schema migrations to the
// account and peer databases. Each store keeps its own bookkeeping table, so
// the account store and the peer registry can share one database without
// sharing version numbers.
package schemamig

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"regexp"
	"time"
)

var tableName = regexp.MustCompile(`^[a-z][a-z0-9_]*$`)

// Migration is one forward-only step. Apply must be idempotent: a process
// that dies between applying a step and recording its version re-runs the
// step on the next start.
type Migration struct {
	Version int
	Name    string
	Apply   func(*sql.DB) error
}

// TooNewError reports a database written by a newer binary. Writing to it
// with an older binary could store rows the newer schema cannot represent, so
// the caller must upgrade instead.
type TooNewError struct {
	Found     int
	Supported int
}

func (e *TooNewError) Error() string {
	return fmt.Sprintf("schema version %d is newer than this binary supports (%d); upgrade the server binary",
		e.Found, e.Supported)
}

// RunPostgres is Run for a PostgreSQL pool. A session advisory lock serialises
// instances that start against the same database at once: CREATE TABLE IF NOT
// EXISTS can still fail when two sessions create the same table concurrently.
func RunPostgres(db *sql.DB, table string, migrations []Migration) (int, error) {
	ctx := context.Background()
	conn, err := db.Conn(ctx)
	if err != nil {
		return 0, fmt.Errorf("schemamig: migration lock: %w", err)
	}
	defer conn.Close()
	const lockKey = int64(0x4d6967726174696f) // "Migratio", the first eight bytes
	if _, err := conn.ExecContext(ctx, `SELECT pg_advisory_lock($1)`, lockKey); err != nil {
		return 0, fmt.Errorf("schemamig: migration lock: %w", err)
	}
	defer func() {
		_, _ = conn.ExecContext(ctx, `SELECT pg_advisory_unlock($1)`, lockKey)
	}()
	return Run(db, table, migrations)
}

// Run creates the bookkeeping table when it is missing, refuses databases
// newer than the last migration, and applies pending migrations in order. The
// returned version is the one recorded after the run.
func Run(db *sql.DB, table string, migrations []Migration) (int, error) {
	if !tableName.MatchString(table) {
		return 0, fmt.Errorf("schemamig: invalid migration table %q", table)
	}
	if len(migrations) == 0 {
		return 0, errors.New("schemamig: no migrations")
	}
	for i, migration := range migrations {
		if migration.Version != i+1 {
			return 0, fmt.Errorf("schemamig: migrations must be numbered from 1 without gaps, got %d at position %d",
				migration.Version, i)
		}
		if migration.Apply == nil {
			return 0, fmt.Errorf("schemamig: migration %d has no apply function", migration.Version)
		}
	}
	latest := migrations[len(migrations)-1].Version
	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS ` + table + ` (
		version    INTEGER PRIMARY KEY,
		name       TEXT NOT NULL DEFAULT '',
		applied_at BIGINT NOT NULL
	)`); err != nil {
		return 0, fmt.Errorf("schemamig: create %s: %w", table, err)
	}
	var current int
	if err := db.QueryRow(`SELECT COALESCE(MAX(version), 0) FROM ` + table).Scan(&current); err != nil {
		return 0, fmt.Errorf("schemamig: read %s: %w", table, err)
	}
	if current > latest {
		return current, &TooNewError{Found: current, Supported: latest}
	}
	for _, migration := range migrations[current:] {
		if err := migration.Apply(db); err != nil {
			return current, fmt.Errorf("schemamig: migration %d (%s): %w",
				migration.Version, migration.Name, err)
		}
		if _, err := db.Exec(
			`INSERT INTO `+table+` (version, name, applied_at) VALUES (?, ?, ?)
			 ON CONFLICT(version) DO NOTHING`,
			migration.Version, migration.Name, time.Now().UTC().UnixMilli(),
		); err != nil {
			return current, fmt.Errorf("schemamig: record migration %d: %w", migration.Version, err)
		}
		current = migration.Version
	}
	return current, nil
}
