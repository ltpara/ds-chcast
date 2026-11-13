package account

import (
	"database/sql"
	"fmt"

	"github.com/ltpara/ds-chcast/server/internal/schemamig"
)

// accountPostgresMigrations keeps PostgreSQL version numbers aligned with the
// SQLite list. PostgreSQL started at v8; keep that baseline at v8 so later
// migrations also run when upgrading an existing PostgreSQL database.
var accountPostgresMigrations = func() []schemamig.Migration {
	list := make([]schemamig.Migration, len(accountMigrations))
	for i, migration := range accountMigrations {
		list[i] = schemamig.Migration{
			Version: migration.Version,
			Name:    migration.Name,
			Apply:   func(*sql.DB) error { return nil },
		}
	}
	list[7].Apply = applyAccountPostgresBaseline
	list[8].Apply = applyHostAuditIdentityPostgres
	list[9].Apply = applyDevicePoliciesPostgres
	list[10].Apply = applyAPITokensPostgres
	list[11].Apply = applyRevokedAccessKidsPostgres
	list[12].Apply = applyPolicyObjectsPostgres
	list[13].Apply = applyDevicePolicyVersionSequencePostgres
	list[14].Apply = applyAddressBookTargetGrantsPostgres
	list[15].Apply = applySessionPermissionCapsPostgres
	list[16].Apply = applyLDAPIdentitiesPostgres
	list[17].Apply = applyAccountSessionActivityPostgres
	return list
}()

// applyAccountSessionActivityPostgres mirrors the stable login and recent-use
// timestamps used by the paginated session directory.
func applyAccountSessionActivityPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`ALTER TABLE user_tokens ADD COLUMN IF NOT EXISTS login_at BIGINT NOT NULL DEFAULT 0`,
		`ALTER TABLE user_tokens ADD COLUMN IF NOT EXISTS last_seen_at BIGINT NOT NULL DEFAULT 0`,
		`UPDATE user_tokens SET login_at = created_at WHERE login_at = 0`,
		`UPDATE user_tokens SET last_seen_at = created_at WHERE last_seen_at = 0`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("migrate account session activity: %w", err)
		}
	}
	return nil
}

// applyLDAPIdentitiesPostgres mirrors the SQLite directory link table.
func applyLDAPIdentitiesPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS ldap_identities (
			dn         TEXT PRIMARY KEY,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS ldap_identities_user ON ldap_identities(user_id)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL ldap identity link: %w", err)
		}
	}
	return nil
}

// applySessionPermissionCapsPostgres mirrors the SQLite account session cap
// column; PostgreSQL needs its own idempotent ALTER syntax.
func applySessionPermissionCapsPostgres(db *sql.DB) error {
	_, err := db.Exec(`ALTER TABLE users ADD COLUMN IF NOT EXISTS
		session_permission_ceiling BIGINT NOT NULL DEFAULT 0`)
	if err != nil {
		return fmt.Errorf("migrate PostgreSQL account session caps: %w", err)
	}
	return nil
}

// applyAddressBookTargetGrantsPostgres mirrors the SQLite target grant table.
func applyAddressBookTargetGrantsPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS address_book_target_grants (
			owner_user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			target_kind   TEXT NOT NULL,
			target_id     TEXT NOT NULL DEFAULT '',
			group_id      TEXT NOT NULL DEFAULT '',
			permission    TEXT NOT NULL DEFAULT 'read',
			created_at    BIGINT NOT NULL,
			PRIMARY KEY (owner_user_id, target_kind, target_id, group_id)
		)`,
		`CREATE INDEX IF NOT EXISTS address_book_target_grants_target
			ON address_book_target_grants(target_kind, target_id)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL address book target grants: %w", err)
		}
	}
	return nil
}

// applyPolicyObjectsPostgres mirrors the SQLite migration. PostgreSQL needs
// its own ALTER syntax because ensureColumn reads PRAGMA table_info.
func applyPolicyObjectsPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS policy_objects (
			id                 TEXT PRIMARY KEY,
			name               TEXT NOT NULL UNIQUE,
			mode               TEXT NOT NULL DEFAULT 'open',
			permission_ceiling BIGINT NOT NULL DEFAULT 0,
			policy_version     BIGINT NOT NULL DEFAULT 1,
			created_at         BIGINT NOT NULL,
			updated_at         BIGINT NOT NULL,
			updated_by         TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE TABLE IF NOT EXISTS policy_object_users (
			policy_id  TEXT NOT NULL REFERENCES policy_objects(id) ON DELETE CASCADE,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL,
			PRIMARY KEY (policy_id, user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS policy_object_users_user
			ON policy_object_users(user_id)`,
		`CREATE TABLE IF NOT EXISTS policy_object_groups (
			policy_id  TEXT NOT NULL REFERENCES policy_objects(id) ON DELETE CASCADE,
			group_id   TEXT NOT NULL REFERENCES account_groups(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL,
			PRIMARY KEY (policy_id, group_id)
		)`,
		`CREATE INDEX IF NOT EXISTS policy_object_groups_group
			ON policy_object_groups(group_id)`,
		`CREATE TABLE IF NOT EXISTS policy_assignments (
			id          TEXT PRIMARY KEY,
			policy_id   TEXT NOT NULL REFERENCES policy_objects(id) ON DELETE CASCADE,
			scope_kind  TEXT NOT NULL,
			scope_value TEXT NOT NULL DEFAULT '',
			priority    BIGINT NOT NULL DEFAULT 0,
			created_at  BIGINT NOT NULL,
			created_by  TEXT NOT NULL DEFAULT '',
			UNIQUE (scope_kind, scope_value)
		)`,
		`CREATE INDEX IF NOT EXISTS policy_assignments_policy
			ON policy_assignments(policy_id)`,
		`ALTER TABLE device_policies ADD COLUMN IF NOT EXISTS source_kind TEXT NOT NULL DEFAULT 'manual'`,
		`ALTER TABLE device_policies ADD COLUMN IF NOT EXISTS source_id TEXT NOT NULL DEFAULT ''`,
		`ALTER TABLE device_policies ADD COLUMN IF NOT EXISTS source_version BIGINT NOT NULL DEFAULT 0`,
		`ALTER TABLE device_policies ADD COLUMN IF NOT EXISTS source_priority BIGINT NOT NULL DEFAULT 0`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL policy object schema: %w", err)
		}
	}
	return nil
}

// applyDevicePolicyVersionSequencePostgres mirrors the SQLite sequence table.
func applyDevicePolicyVersionSequencePostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS device_policy_versions (
			peer_id      TEXT PRIMARY KEY,
			last_version BIGINT NOT NULL DEFAULT 0
		)`,
		`INSERT INTO device_policy_versions (peer_id, last_version)
		 SELECT peer_id, policy_version FROM device_policies WHERE true
		 ON CONFLICT (peer_id) DO NOTHING`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL device policy version sequence: %w", err)
		}
	}
	return nil
}

func applyRevokedAccessKidsPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS revoked_access_kids (
			kid        TEXT PRIMARY KEY,
			revoked_at BIGINT NOT NULL,
			revoked_by TEXT NOT NULL DEFAULT '',
			reason     TEXT NOT NULL DEFAULT ''
		)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL revoked access key schema: %w", err)
		}
	}
	return nil
}

func applyAPITokensPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS api_tokens (
			id            TEXT PRIMARY KEY,
			token_hash    TEXT NOT NULL UNIQUE,
			user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			name          TEXT NOT NULL,
			role          TEXT NOT NULL,
			resources     TEXT NOT NULL DEFAULT '',
			group_id      TEXT NOT NULL DEFAULT '',
			created_at    BIGINT NOT NULL,
			expires_at    BIGINT NOT NULL,
			revoked_at    BIGINT NOT NULL DEFAULT 0,
			last_used_at  BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS api_tokens_user ON api_tokens(user_id, created_at)`,
		`CREATE INDEX IF NOT EXISTS api_tokens_expiry ON api_tokens(expires_at)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL API token schema: %w", err)
		}
	}
	return nil
}

func applyDevicePoliciesPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`CREATE TABLE IF NOT EXISTS device_policies (
			peer_id            TEXT PRIMARY KEY,
			mode               TEXT NOT NULL DEFAULT 'open',
			permission_ceiling BIGINT NOT NULL DEFAULT 0,
			policy_version     BIGINT NOT NULL DEFAULT 1,
			updated_at         BIGINT NOT NULL,
			updated_by         TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE TABLE IF NOT EXISTS device_policy_users (
			peer_id    TEXT NOT NULL REFERENCES device_policies(peer_id) ON DELETE CASCADE,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL,
			PRIMARY KEY (peer_id, user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS device_policy_users_user
			ON device_policy_users(user_id)`,
		`CREATE TABLE IF NOT EXISTS device_policy_groups (
			peer_id    TEXT NOT NULL REFERENCES device_policies(peer_id) ON DELETE CASCADE,
			group_id   TEXT NOT NULL REFERENCES account_groups(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL,
			PRIMARY KEY (peer_id, group_id)
		)`,
		`CREATE INDEX IF NOT EXISTS device_policy_groups_group
			ON device_policy_groups(group_id)`,
		`CREATE TABLE IF NOT EXISTS device_policy_receipts (
			peer_id        TEXT PRIMARY KEY REFERENCES device_policies(peer_id) ON DELETE CASCADE,
			policy_version BIGINT NOT NULL,
			applied        BOOLEAN NOT NULL,
			detail         TEXT NOT NULL DEFAULT '',
			reported_at    BIGINT NOT NULL
		)`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("migrate PostgreSQL device policies: %w", err)
		}
	}
	return nil
}

func applyHostAuditIdentityPostgres(db *sql.DB) error {
	for _, statement := range []string{
		`ALTER TABLE audit_events ADD COLUMN IF NOT EXISTS host_id TEXT NOT NULL DEFAULT ''`,
		`ALTER TABLE audit_events ADD COLUMN IF NOT EXISTS event_key TEXT NOT NULL DEFAULT ''`,
		`CREATE UNIQUE INDEX IF NOT EXISTS audit_events_key ON audit_events(event_key) WHERE event_key <> ''`,
	} {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("migrate PostgreSQL host audit: %w", err)
		}
	}
	return nil
}

// applyAccountPostgresBaseline creates the v8 account schema in an empty
// PostgreSQL database. Case-insensitive columns use a non-deterministic ICU
// collation so '=' and ORDER BY keep the SQLite NOCASE semantics without
// rewriting every query.
func applyAccountPostgresBaseline(db *sql.DB) error {
	statements := []string{
		`CREATE COLLATION IF NOT EXISTS chast_nocase (
			provider = icu,
			locale = 'und-u-ks-level2',
			deterministic = false
		)`,
		`CREATE TABLE IF NOT EXISTS users (
			id             TEXT PRIMARY KEY,
			username       TEXT COLLATE chast_nocase NOT NULL UNIQUE,
			password_hash  TEXT NOT NULL,
			created_at     BIGINT NOT NULL,
			role           TEXT NOT NULL DEFAULT 'user',
			book_revision  BIGINT NOT NULL DEFAULT 0,
			totp_secret    TEXT NOT NULL DEFAULT '',
			totp_enabled   BOOLEAN NOT NULL DEFAULT false,
			totp_last_step BIGINT NOT NULL DEFAULT 0,
			banned         BOOLEAN NOT NULL DEFAULT false,
			email          TEXT COLLATE chast_nocase NOT NULL DEFAULT '',
			phone          TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE TABLE IF NOT EXISTS user_tokens (
			token_hash         TEXT PRIMARY KEY,
			user_id            TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at         BIGINT NOT NULL,
			expires_at         BIGINT NOT NULL,
			session_id         TEXT NOT NULL DEFAULT '',
			device_id          TEXT NOT NULL DEFAULT '',
			device_name        TEXT NOT NULL DEFAULT '',
			login_ip           TEXT NOT NULL DEFAULT '',
			login_location     TEXT NOT NULL DEFAULT '',
			access_expires_at  BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS user_tokens_expiry ON user_tokens(expires_at)`,
		`CREATE UNIQUE INDEX IF NOT EXISTS user_tokens_session
			ON user_tokens(session_id) WHERE session_id <> ''`,
		`CREATE TABLE IF NOT EXISTS refresh_tokens (
			token_hash  TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			session_id  TEXT NOT NULL,
			created_at  BIGINT NOT NULL,
			expires_at  BIGINT NOT NULL,
			rotated_at  BIGINT NOT NULL DEFAULT 0,
			replaced_by TEXT NOT NULL DEFAULT '',
			replayed_at BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS refresh_tokens_expiry ON refresh_tokens(expires_at)`,
		`CREATE INDEX IF NOT EXISTS refresh_tokens_session ON refresh_tokens(session_id)`,
		`CREATE TABLE IF NOT EXISTS recovery_codes (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at BIGINT NOT NULL,
			used_at    BIGINT NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, code_hash)
		)`,
		`CREATE TABLE IF NOT EXISTS password_resets (
			token_hash TEXT PRIMARY KEY,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL,
			expires_at BIGINT NOT NULL,
			used_at    BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS password_resets_expiry ON password_resets(expires_at)`,
		`CREATE TABLE IF NOT EXISTS phone_resets (
			user_id    TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at BIGINT NOT NULL,
			expires_at BIGINT NOT NULL,
			attempts   BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS phone_resets_expiry ON phone_resets(expires_at)`,
		`CREATE TABLE IF NOT EXISTS login_sms_codes (
			user_id    TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at BIGINT NOT NULL,
			expires_at BIGINT NOT NULL,
			attempts   BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS login_sms_codes_expiry ON login_sms_codes(expires_at)`,
		`CREATE TABLE IF NOT EXISTS login_email_codes (
			user_id    TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
			code_hash  TEXT NOT NULL,
			created_at BIGINT NOT NULL,
			expires_at BIGINT NOT NULL,
			attempts   BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS login_email_codes_expiry ON login_email_codes(expires_at)`,
		`CREATE TABLE IF NOT EXISTS webauthn_credentials (
			credential_id BYTEA PRIMARY KEY,
			user_id       TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			public_key    BYTEA NOT NULL,
			alg           BIGINT NOT NULL,
			sign_count    BIGINT NOT NULL DEFAULT 0,
			name          TEXT NOT NULL DEFAULT '',
			created_at    BIGINT NOT NULL,
			last_used_at  BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE INDEX IF NOT EXISTS webauthn_credentials_user ON webauthn_credentials(user_id)`,
		`CREATE TABLE IF NOT EXISTS webauthn_challenges (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			purpose    TEXT NOT NULL,
			challenge  BYTEA NOT NULL,
			expires_at BIGINT NOT NULL,
			PRIMARY KEY (user_id, purpose)
		)`,
		`CREATE INDEX IF NOT EXISTS webauthn_challenges_expiry ON webauthn_challenges(expires_at)`,
		`CREATE TABLE IF NOT EXISTS address_book (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			peer_id    TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			tags       TEXT NOT NULL DEFAULT '',
			note       TEXT NOT NULL DEFAULT '',
			updated_at BIGINT NOT NULL,
			revision   BIGINT NOT NULL DEFAULT 0,
			deleted    BOOLEAN NOT NULL DEFAULT false,
			group_id   TEXT NOT NULL DEFAULT '',
			position   BIGINT NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, peer_id)
		)`,
		`CREATE TABLE IF NOT EXISTS address_book_grants (
			owner_user_id   TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			grantee_user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id        TEXT NOT NULL DEFAULT '',
			permission      TEXT NOT NULL DEFAULT 'read',
			created_at      BIGINT NOT NULL,
			PRIMARY KEY (owner_user_id, grantee_user_id, group_id),
			CHECK (owner_user_id <> grantee_user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS address_book_grants_grantee
			ON address_book_grants(grantee_user_id)`,
		`CREATE TABLE IF NOT EXISTS address_book_groups (
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			group_id   TEXT NOT NULL,
			name       TEXT NOT NULL DEFAULT '',
			updated_at BIGINT NOT NULL,
			revision   BIGINT NOT NULL DEFAULT 0,
			deleted    BOOLEAN NOT NULL DEFAULT false,
			parent_id  TEXT NOT NULL DEFAULT '',
			position   BIGINT NOT NULL DEFAULT 0,
			PRIMARY KEY (user_id, group_id)
		)`,
		`CREATE TABLE IF NOT EXISTS audit_events (
			id         BIGSERIAL PRIMARY KEY,
			created_at BIGINT NOT NULL,
			username   TEXT COLLATE chast_nocase NOT NULL,
			action     TEXT NOT NULL,
			detail     TEXT NOT NULL DEFAULT '',
			client_ip  TEXT NOT NULL DEFAULT '',
			location   TEXT NOT NULL DEFAULT '',
			success    BOOLEAN NOT NULL DEFAULT false
		)`,
		`CREATE INDEX IF NOT EXISTS audit_events_created ON audit_events(created_at)`,
		`CREATE TABLE IF NOT EXISTS connection_events (
			id         BIGSERIAL PRIMARY KEY,
			user_id    TEXT NOT NULL,
			username   TEXT COLLATE chast_nocase NOT NULL,
			peer_id    TEXT NOT NULL,
			direction  TEXT NOT NULL DEFAULT 'viewer',
			client_ip  TEXT NOT NULL DEFAULT '',
			started_at BIGINT NOT NULL,
			ended_at   BIGINT NOT NULL,
			success    BOOLEAN NOT NULL DEFAULT true,
			detail     TEXT NOT NULL DEFAULT '',
			host_id    TEXT NOT NULL DEFAULT '',
			audit_ref  TEXT NOT NULL DEFAULT '',
			source     TEXT NOT NULL DEFAULT 'viewer',
			location   TEXT NOT NULL DEFAULT '',
			route      TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS connection_events_started
			ON connection_events(started_at)`,
		`CREATE INDEX IF NOT EXISTS connection_events_audit
			ON connection_events(audit_ref) WHERE audit_ref <> ''`,
		`CREATE TABLE IF NOT EXISTS file_events (
			id          BIGSERIAL PRIMARY KEY,
			user_id     TEXT NOT NULL,
			username    TEXT COLLATE chast_nocase NOT NULL,
			peer_id     TEXT NOT NULL,
			direction   TEXT NOT NULL,
			name        TEXT NOT NULL DEFAULT '',
			size        BIGINT NOT NULL DEFAULT 0,
			files       BIGINT NOT NULL DEFAULT 1,
			client_ip   TEXT NOT NULL DEFAULT '',
			occurred_at BIGINT NOT NULL,
			success     BOOLEAN NOT NULL DEFAULT true,
			detail      TEXT NOT NULL DEFAULT '',
			host_id     TEXT NOT NULL DEFAULT '',
			audit_ref   TEXT NOT NULL DEFAULT '',
			source      TEXT NOT NULL DEFAULT 'viewer',
			location    TEXT NOT NULL DEFAULT '',
			route       TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS file_events_occurred
			ON file_events(occurred_at)`,
		`CREATE TABLE IF NOT EXISTS login_locks (
			login_key    TEXT PRIMARY KEY,
			failures     BIGINT NOT NULL,
			window_start BIGINT NOT NULL,
			locked_until BIGINT NOT NULL DEFAULT 0,
			last_seen    BIGINT NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS login_locks_seen ON login_locks(last_seen)`,
		`CREATE UNIQUE INDEX IF NOT EXISTS users_email
			ON users(email) WHERE email <> ''`,
		`CREATE UNIQUE INDEX IF NOT EXISTS users_phone
			ON users(phone) WHERE phone <> ''`,
		`CREATE TABLE IF NOT EXISTS archive_checkpoints (
			stream     TEXT PRIMARY KEY,
			last_id    BIGINT NOT NULL DEFAULT 0,
			updated_at BIGINT NOT NULL DEFAULT 0
		)`,
		`CREATE TABLE IF NOT EXISTS recordings (
			rowid       BIGSERIAL NOT NULL,
			id          TEXT PRIMARY KEY,
			user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			name        TEXT NOT NULL DEFAULT '',
			peer_id     TEXT NOT NULL DEFAULT '',
			size        BIGINT NOT NULL,
			sha256      TEXT NOT NULL DEFAULT '',
			duration_ms BIGINT NOT NULL DEFAULT 0,
			started_at  BIGINT NOT NULL DEFAULT 0,
			created_at  BIGINT NOT NULL
		)`,
		`CREATE INDEX IF NOT EXISTS recordings_user ON recordings(user_id, created_at)`,
		`CREATE INDEX IF NOT EXISTS recordings_created ON recordings(created_at)`,
		`CREATE TABLE IF NOT EXISTS oidc_identities (
			issuer     TEXT NOT NULL,
			subject    TEXT NOT NULL,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			created_at BIGINT NOT NULL,
			PRIMARY KEY (issuer, subject)
		)`,
		`CREATE INDEX IF NOT EXISTS oidc_identities_user ON oidc_identities(user_id)`,
		`CREATE TABLE IF NOT EXISTS account_groups (
			id           TEXT PRIMARY KEY,
			name         TEXT NOT NULL,
			description  TEXT NOT NULL DEFAULT '',
			member_quota BIGINT NOT NULL DEFAULT 0,
			device_quota BIGINT NOT NULL DEFAULT 0,
			created_at   BIGINT NOT NULL,
			updated_at   BIGINT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS account_group_members (
			group_id   TEXT NOT NULL REFERENCES account_groups(id) ON DELETE CASCADE,
			user_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			role       TEXT NOT NULL DEFAULT 'readonly',
			created_at BIGINT NOT NULL,
			PRIMARY KEY (group_id, user_id)
		)`,
		`CREATE INDEX IF NOT EXISTS account_group_members_user
			ON account_group_members(user_id)`,
		`CREATE TABLE IF NOT EXISTS account_group_devices (
			group_id   TEXT NOT NULL REFERENCES account_groups(id) ON DELETE CASCADE,
			peer_id    TEXT NOT NULL,
			added_by   TEXT NOT NULL DEFAULT '',
			created_at BIGINT NOT NULL,
			PRIMARY KEY (group_id, peer_id)
		)`,
		`CREATE INDEX IF NOT EXISTS account_group_devices_peer
			ON account_group_devices(peer_id)`,
		`ALTER TABLE connection_events ADD COLUMN IF NOT EXISTS
			location TEXT NOT NULL DEFAULT ''`,
		`ALTER TABLE connection_events ADD COLUMN IF NOT EXISTS
			route TEXT NOT NULL DEFAULT ''`,
		`ALTER TABLE file_events ADD COLUMN IF NOT EXISTS
			location TEXT NOT NULL DEFAULT ''`,
		`ALTER TABLE file_events ADD COLUMN IF NOT EXISTS
			route TEXT NOT NULL DEFAULT ''`,
		`ALTER TABLE address_book ADD COLUMN IF NOT EXISTS
			note TEXT NOT NULL DEFAULT ''`,
	}
	for _, statement := range statements {
		if _, err := db.Exec(statement); err != nil {
			return fmt.Errorf("create PostgreSQL account schema: %w", err)
		}
	}
	return nil
}
