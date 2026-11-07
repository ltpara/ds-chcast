package account

import (
	"database/sql"
	"fmt"

	"github.com/ltpara/ds-chcast/server/internal/schemamig"
)

// accountSchemaVersion is the schema generation this binary writes. Bootstrap
// databases created before schema_migrations existed are upgraded by the
// baseline migration, which stays idempotent so a crash between applying it
// and recording the version is harmless.
const accountSchemaVersion = 9

var accountMigrations = []schemamig.Migration{
	{Version: 1, Name: "baseline", Apply: applyAccountBaseline},
	{Version: 2, Name: "archive checkpoints", Apply: applyArchiveCheckpoints},
	{Version: 3, Name: "address book nesting", Apply: applyAddressBookNesting},
	{Version: 4, Name: "recording archive", Apply: applyRecordingArchive},
	{Version: 5, Name: "refresh replay marker", Apply: applyRefreshReplayMarker},
	{Version: 6, Name: "oidc identities", Apply: applyOIDCIdentities},
	{Version: 7, Name: "account groups", Apply: applyAccountGroups},
	{Version: 8, Name: "audit location and route", Apply: applyAuditLocationRoute},
	{Version: 9, Name: "host audit identity and deduplication", Apply: applyHostAuditIdentity},
}

func applyHostAuditIdentity(db *sql.DB) error {
	for _, name := range []string{"host_id", "event_key"} {
		if err := ensureColumn(db, "audit_events", name, name+" TEXT NOT NULL DEFAULT ''"); err != nil {
			return fmt.Errorf("migrate audit_events.%s: %w", name, err)
		}
	}
	_, err := db.Exec(`CREATE UNIQUE INDEX IF NOT EXISTS audit_events_key
		ON audit_events(event_key) WHERE event_key <> ''`)
	return err
}

// applyAuditLocationRoute records where a session signed in and which path it
// took (direct, punched, relay or shared carrier). Both columns default to the
// empty string so databases without a GeoIP table keep recording only the
// client address.
func applyAuditLocationRoute(db *sql.DB) error {
	for _, column := range []struct {
		table string
		name  string
		ddl   string
	}{
		{"connection_events", "location", "location TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "route", "route TEXT NOT NULL DEFAULT ''"},
		{"file_events", "location", "location TEXT NOT NULL DEFAULT ''"},
		{"file_events", "route", "route TEXT NOT NULL DEFAULT ''"},
	} {
		if err := ensureColumn(db, column.table, column.name, column.ddl); err != nil {
			return fmt.Errorf("migrate %s.%s: %w", column.table, column.name, err)
		}
	}
	return nil
}

// applyAccountGroups adds operator-managed groups that scope console access
// and device ownership. These are separate from address book groups: an
// account group spans users, while an address book group is private to one
// book and only groups its saved peers.
func applyAccountGroups(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS account_groups (
			id           TEXT PRIMARY KEY,
			name         TEXT NOT NULL,
			description  TEXT NOT NULL DEFAULT '',
			member_quota INTEGER NOT NULL DEFAULT 0,
			device_quota INTEGER NOT NULL DEFAULT 0,
			created_at   INTEGER NOT NULL,
			updated_at   INTEGER NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS account_group_members (
			group_id   TEXT NOT NULL REFERENCES account_groups(id) ON DELETE CASCADE,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			role       TEXT NOT NULL DEFAULT 'readonly',
			created_at INTEGER NOT NULL,
			PRIMARY KEY (group_id, user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS account_group_members_user
			ON account_group_members(user_id)`,
		`CREATE TABLE IF NOT EXISTS account_group_devices (
			group_id   TEXT NOT NULL REFERENCES account_groups(id) ON DELETE CASCADE,
			peer_id    TEXT NOT NULL,
			added_by   TEXT NOT NULL DEFAULT '',
			created_at INTEGER NOT NULL,
			PRIMARY KEY (group_id, peer_id)
		)`,
		`CREATE INDEX IF NOT EXISTS account_group_devices_peer
			ON account_group_devices(peer_id)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create account group schema: %w", err)
		}
	}
	return nil
}

// applyOIDCIdentities links an external OpenID Connect subject to one local
// account. The issuer is part of the key so two providers may reuse subject
// values without colliding.
func applyOIDCIdentities(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS oidc_identities (
			issuer     TEXT NOT NULL,
			subject    TEXT NOT NULL,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at INTEGER NOT NULL,
			PRIMARY KEY (issuer, subject)
		)`,
		`CREATE INDEX IF NOT EXISTS oidc_identities_user ON oidc_identities(user_id)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create oidc identity link: %w", err)
		}
	}
	return nil
}

// applyRefreshReplayMarker records that a retired refresh token was presented
// again. The grace window tolerates one such retry per session; a second one
// means two clients hold copies of the same token chain.
func applyRefreshReplayMarker(db *sql.DB) error {
	if err := ensureColumn(db, "refresh_tokens", "replayed_at", "replayed_at INTEGER NOT NULL DEFAULT 0"); err != nil {
		return fmt.Errorf("migrate refresh_tokens.replayed_at: %w", err)
	}
	return nil
}

// applyRecordingArchive adds the metadata side of uploaded viewer recordings.
// The MP4 bytes live outside the database; a row disappears with its account
// so a deleted user cannot keep files visible to operators.
func applyRecordingArchive(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS recordings (
			id          TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			name        TEXT NOT NULL DEFAULT '',
			peer_id     TEXT NOT NULL DEFAULT '',
			size        INTEGER NOT NULL,
			sha256      TEXT NOT NULL DEFAULT '',
			duration_ms INTEGER NOT NULL DEFAULT 0,
			started_at  INTEGER NOT NULL DEFAULT 0,
			created_at  INTEGER NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS recordings_user ON recordings(user_id, created_at)`,
		`CREATE INDEX IF NOT EXISTS recordings_created ON recordings(created_at)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create recording archive: %w", err)
		}
	}
	return nil
}

// applyAddressBookNesting adds the parent link and manual ordering columns.
// Every existing group becomes a root at position zero, which keeps the old
// name-ordered presentation until a client reorders it.
func applyAddressBookNesting(db *sql.DB) error {
	for _, column := range []struct {
		table string
		name  string
		ddl   string
	}{
		{"address_book_groups", "parent_id", "parent_id TEXT NOT NULL DEFAULT ''"},
		{"address_book_groups", "position", "position INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "position", "position INTEGER NOT NULL DEFAULT 0"},
	} {
		if err := ensureColumn(db, column.table, column.name, column.ddl); err != nil {
			return fmt.Errorf("migrate %s.%s: %w", column.table, column.name, err)
		}
	}
	return nil
}

// applyArchiveCheckpoints tracks how far the external JSONL archive has copied
// each history table so a restart resumes instead of rewriting everything.
func applyArchiveCheckpoints(db *sql.DB) error {
	_, err := db.Exec(`CREATE TABLE IF NOT EXISTS archive_checkpoints (
		stream     TEXT PRIMARY KEY,
		last_id    INTEGER NOT NULL DEFAULT 0,
		updated_at INTEGER NOT NULL DEFAULT 0
	)`)
	if err != nil {
		return fmt.Errorf("create archive checkpoints: %w", err)
	}
	return nil
}

func applyAccountBaseline(db *sql.DB) error {
	for _, schema := range []string{
		`CREATE TABLE IF NOT EXISTS users (
			id            TEXT PRIMARY KEY,
			username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
			password_hash  TEXT NOT NULL,
			created_at     INTEGER NOT NULL,
			role           TEXT NOT NULL DEFAULT 'user',
			book_revision  INTEGER NOT NULL DEFAULT 0,
			totp_secret    TEXT NOT NULL DEFAULT '',
			totp_enabled   INTEGER NOT NULL DEFAULT 0,
			totp_last_step INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE TABLE IF NOT EXISTS user_tokens (
			token_hash  TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at  INTEGER NOT NULL,
			expires_at  INTEGER NOT NULL,
			session_id  TEXT NOT NULL DEFAULT '',
			device_id   TEXT NOT NULL DEFAULT '',
			device_name TEXT NOT NULL DEFAULT '',
			login_ip    TEXT NOT NULL DEFAULT '',
			login_location TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS user_tokens_expiry ON user_tokens(expires_at)`,
		`CREATE TABLE IF NOT EXISTS refresh_tokens (
			token_hash  TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			session_id  TEXT NOT NULL,
			created_at  INTEGER NOT NULL,
			expires_at  INTEGER NOT NULL,
			rotated_at  INTEGER NOT NULL DEFAULT 0,
			replaced_by TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS refresh_tokens_expiry ON refresh_tokens(expires_at)`,
		`CREATE INDEX IF NOT EXISTS refresh_tokens_session ON refresh_tokens(session_id)`,
		`CREATE TABLE IF NOT EXISTS recovery_codes (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at INTEGER NOT NULL,
			used_at    INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, code_hash)
		)`,
		`CREATE TABLE IF NOT EXISTS password_resets (
			token_hash TEXT PRIMARY KEY,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at INTEGER NOT NULL,
			expires_at INTEGER NOT NULL,
			used_at    INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS password_resets_expiry ON password_resets(expires_at)`,
		`CREATE TABLE IF NOT EXISTS phone_resets (
			user_id    TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at INTEGER NOT NULL,
			expires_at INTEGER NOT NULL,
			attempts   INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS phone_resets_expiry ON phone_resets(expires_at)`,
		`CREATE TABLE IF NOT EXISTS login_sms_codes (
			user_id    TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at INTEGER NOT NULL,
			expires_at INTEGER NOT NULL,
			attempts   INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS login_sms_codes_expiry ON login_sms_codes(expires_at)`,
		`CREATE TABLE IF NOT EXISTS login_email_codes (
			user_id    TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at INTEGER NOT NULL,
			expires_at INTEGER NOT NULL,
			attempts   INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS login_email_codes_expiry ON login_email_codes(expires_at)`,
		`CREATE TABLE IF NOT EXISTS webauthn_credentials (
			credential_id BLOB PRIMARY KEY,
			user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			public_key    BLOB NOT NULL,
			alg           INTEGER NOT NULL,
			sign_count    INTEGER NOT NULL DEFAULT 0,
			name          TEXT NOT NULL DEFAULT '',
			created_at    INTEGER NOT NULL,
			last_used_at  INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS webauthn_credentials_user ON webauthn_credentials(user_id)`,
		`CREATE TABLE IF NOT EXISTS webauthn_challenges (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			purpose    TEXT NOT NULL,
			challenge  BLOB NOT NULL,
			expires_at INTEGER NOT NULL,
			PRIMARY KEY (user_id, purpose)
		)`,
		`CREATE INDEX IF NOT EXISTS webauthn_challenges_expiry ON webauthn_challenges(expires_at)`,
		`CREATE TABLE IF NOT EXISTS address_book (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			peer_id    TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			tags       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			revision   INTEGER NOT NULL DEFAULT 0,
			deleted    INTEGER NOT NULL DEFAULT 0,
			group_id   TEXT NOT NULL DEFAULT '',
			position   INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, peer_id)
		)`,
		`CREATE TABLE IF NOT EXISTS address_book_grants (
			owner_user_id   TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			grantee_user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id        TEXT NOT NULL DEFAULT '',
			permission      TEXT NOT NULL DEFAULT 'read',
			created_at      INTEGER NOT NULL,
			PRIMARY KEY (owner_user_id, grantee_user_id, group_id),
			CHECK (owner_user_id <> grantee_user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS address_book_grants_grantee
			ON address_book_grants(grantee_user_id)`,
		`CREATE TABLE IF NOT EXISTS address_book_groups (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id   TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			updated_at INTEGER NOT NULL,
			revision   INTEGER NOT NULL DEFAULT 0,
			deleted    INTEGER NOT NULL DEFAULT 0,
			parent_id  TEXT NOT NULL DEFAULT '',
			position   INTEGER NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, group_id)
		)`,
		`CREATE TABLE IF NOT EXISTS audit_events (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			created_at INTEGER NOT NULL,
			username   TEXT NOT NULL,
			action     TEXT NOT NULL,
			detail     TEXT NOT NULL DEFAULT '',
			client_ip  TEXT NOT NULL DEFAULT '',
			location   TEXT NOT NULL DEFAULT '',
			success    INTEGER NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS audit_events_created ON audit_events(created_at)`,
		`CREATE TABLE IF NOT EXISTS connection_events (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			user_id    TEXT NOT NULL,
			username   TEXT NOT NULL,
			peer_id    TEXT NOT NULL,
			direction  TEXT NOT NULL DEFAULT 'viewer',
			client_ip  TEXT NOT NULL DEFAULT '',
			started_at INTEGER NOT NULL,
			ended_at   INTEGER NOT NULL,
			success    INTEGER NOT NULL DEFAULT 1,
			detail     TEXT NOT NULL DEFAULT '',
			host_id    TEXT NOT NULL DEFAULT '',
			audit_ref  TEXT NOT NULL DEFAULT '',
			source     TEXT NOT NULL DEFAULT 'viewer'
		)`,
		`CREATE INDEX IF NOT EXISTS connection_events_started
			ON connection_events(started_at)`,
		`CREATE INDEX IF NOT EXISTS connection_events_audit
			ON connection_events(audit_ref) WHERE audit_ref <> ''`,
		`CREATE TABLE IF NOT EXISTS file_events (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			user_id     TEXT NOT NULL,
			username    TEXT NOT NULL,
			peer_id     TEXT NOT NULL,
			direction   TEXT NOT NULL,
			name        TEXT NOT NULL DEFAULT '',
			size        INTEGER NOT NULL DEFAULT 0,
			files       INTEGER NOT NULL DEFAULT 1,
			client_ip   TEXT NOT NULL DEFAULT '',
			occurred_at INTEGER NOT NULL,
			success     INTEGER NOT NULL DEFAULT 1,
			detail      TEXT NOT NULL DEFAULT '',
			host_id     TEXT NOT NULL DEFAULT '',
			audit_ref   TEXT NOT NULL DEFAULT '',
			source      TEXT NOT NULL DEFAULT 'viewer'
		)`,
		`CREATE INDEX IF NOT EXISTS file_events_occurred
			ON file_events(occurred_at)`,
		`CREATE TABLE IF NOT EXISTS login_locks (
			login_key    TEXT PRIMARY KEY,
			failures     INTEGER NOT NULL,
			window_start INTEGER NOT NULL,
			locked_until INTEGER NOT NULL DEFAULT 0,
			last_seen    INTEGER NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS login_locks_seen ON login_locks(last_seen)`,
	} {
		if _, err := db.Exec(schema); err != nil {
			return fmt.Errorf("create schema: %w", err)
		}
	}
	if err := migrateGrantScopes(db); err != nil {
		return fmt.Errorf("migrate address book grants: %w", err)
	}
	for _, column := range []struct {
		table string
		name  string
		ddl   string
	}{
		{"users", "book_revision", "book_revision INTEGER NOT NULL DEFAULT 0"},
		{"users", "totp_secret", "totp_secret TEXT NOT NULL DEFAULT ''"},
		{"users", "totp_enabled", "totp_enabled INTEGER NOT NULL DEFAULT 0"},
		{"users", "totp_last_step", "totp_last_step INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "revision", "revision INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "deleted", "deleted INTEGER NOT NULL DEFAULT 0"},
		{"address_book", "group_id", "group_id TEXT NOT NULL DEFAULT ''"},
		{"address_book_grants", "permission", "permission TEXT NOT NULL DEFAULT 'read'"},
		{"user_tokens", "session_id", "session_id TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "device_id", "device_id TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "device_name", "device_name TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "login_ip", "login_ip TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "login_location", "login_location TEXT NOT NULL DEFAULT ''"},
		{"user_tokens", "access_expires_at", "access_expires_at INTEGER NOT NULL DEFAULT 0"},
		{"users", "role", "role TEXT NOT NULL DEFAULT 'user'"},
		{"users", "banned", "banned INTEGER NOT NULL DEFAULT 0"},
		{"users", "email", "email TEXT NOT NULL DEFAULT ''"},
		{"users", "phone", "phone TEXT NOT NULL DEFAULT ''"},
		{"audit_events", "location", "location TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "host_id", "host_id TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "audit_ref", "audit_ref TEXT NOT NULL DEFAULT ''"},
		{"connection_events", "source", "source TEXT NOT NULL DEFAULT 'viewer'"},
		{"file_events", "host_id", "host_id TEXT NOT NULL DEFAULT ''"},
		{"file_events", "audit_ref", "audit_ref TEXT NOT NULL DEFAULT ''"},
		{"file_events", "source", "source TEXT NOT NULL DEFAULT 'viewer'"},
	} {
		if err := ensureColumn(db, column.table, column.name, column.ddl); err != nil {
			return fmt.Errorf("migrate %s.%s: %w", column.table, column.name, err)
		}
	}
	if err := backfillSessionIDs(db); err != nil {
		return fmt.Errorf("migrate session ids: %w", err)
	}
	if err := backfillAdminRole(db); err != nil {
		return fmt.Errorf("migrate roles: %w", err)
	}
	for _, index := range []string{
		`CREATE UNIQUE INDEX IF NOT EXISTS user_tokens_session
		 ON user_tokens(session_id) WHERE session_id <> ''`,
		`CREATE UNIQUE INDEX IF NOT EXISTS users_email
		 ON users(email COLLATE NOCASE) WHERE email <> ''`,
		`CREATE UNIQUE INDEX IF NOT EXISTS users_phone
		 ON users(phone) WHERE phone <> ''`,
	} {
		if _, err := db.Exec(index); err != nil {
			return fmt.Errorf("create unique index: %w", err)
		}
	}
	if err := backfillRevisions(db); err != nil {
		return fmt.Errorf("migrate address book revisions: %w", err)
	}
	return nil
}
