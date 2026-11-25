package account

import (
	"database/sql"
	"path/filepath"
	"testing"
	"time"
)

func TestAccountMigrationsFromSchemaV28(t *testing.T) {
	path := filepath.Join(t.TempDir(), "alarm-v28.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`CREATE TABLE schema_migrations (
		version INTEGER PRIMARY KEY,
		name TEXT NOT NULL DEFAULT '',
		applied_at INTEGER NOT NULL
	)`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	if _, err := db.Exec(`CREATE TABLE users (id TEXT PRIMARY KEY, email TEXT NOT NULL DEFAULT '')`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO schema_migrations (version, name, applied_at)
		VALUES (28, 'host recording upload owner binding', 0)`); err != nil {
		db.Close()
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}

	store, err := Open(path)
	if err != nil {
		t.Fatalf("upgrade schema v28: %v", err)
	}
	defer store.Close()
	var version int
	if err := store.db.QueryRow(`SELECT MAX(version) FROM schema_migrations`).Scan(&version); err != nil {
		t.Fatal(err)
	}
	if version != accountSchemaVersion {
		t.Fatalf("schema version = %d, want %d", version, accountSchemaVersion)
	}
	var tableCount int
	if err := store.db.QueryRow(`SELECT COUNT(*) FROM sqlite_master
		WHERE type='table' AND name='alarm_events'`).Scan(&tableCount); err != nil {
		t.Fatal(err)
	}
	if tableCount != 1 {
		t.Fatalf("alarm_events table count = %d", tableCount)
	}
	rows, err := store.db.Query(`SELECT email_verified_at FROM users LIMIT 0`)
	if err != nil {
		t.Fatalf("email verification column missing: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close email verification column check: %v", err)
	}
	rows, err = store.db.Query(`SELECT token_hash FROM email_verification_tokens LIMIT 0`)
	if err != nil {
		t.Fatalf("email verification token table missing: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close email verification table check: %v", err)
	}
	rows, err = store.db.Query(`SELECT alarm_email_enabled FROM users LIMIT 0`)
	if err != nil {
		t.Fatalf("alarm email preference column missing: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close alarm email preference column check: %v", err)
	}
	rows, err = store.db.Query(`SELECT alarm_id, user_id, recipient, status FROM alarm_email_deliveries LIMIT 0`)
	if err != nil {
		t.Fatalf("alarm email deliveries table missing: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close alarm email deliveries table check: %v", err)
	}
	rows, err = store.db.Query(`SELECT login_device_allowlist_enabled FROM users LIMIT 0`)
	if err != nil {
		t.Fatalf("login device allowlist column missing: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close login device allowlist column check: %v", err)
	}
	rows, err = store.db.Query(`SELECT challenge_id FROM account_login_device_challenges LIMIT 0`)
	if err != nil {
		t.Fatalf("login device challenge table missing: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close login device challenge table check: %v", err)
	}
}

func TestAlarmProjectionFailureRollsBackAuditEvent(t *testing.T) {
	store := openTemp(t, "alarm-transaction.db")
	if _, err := store.db.Exec(`DROP TABLE alarm_events`); err != nil {
		t.Fatal(err)
	}
	err := store.AppendAudit(AuditEvent{
		Username: "operator", Action: AuditEmergencyAccess,
		Detail: "granted emergency access", Success: true,
	})
	if err == nil {
		t.Fatal("audit write succeeded without its alarm projection")
	}
	events, err := store.AuditEventsFor(AuditEmergencyAccess, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(events) != 0 {
		t.Fatalf("audit source survived failed alarm projection: %+v", events)
	}
}

func TestAlarmSurvivesSourceAuditRetention(t *testing.T) {
	store := openTemp(t, "alarm-independent-retention.db")
	if err := store.AppendAudit(AuditEvent{
		Username: "operator", Action: AuditEmergencyAccess,
		Detail: "granted emergency access", Success: true,
	}); err != nil {
		t.Fatal(err)
	}
	if _, err := store.db.Exec(`DELETE FROM audit_events WHERE action = ?`, AuditEmergencyAccess); err != nil {
		t.Fatal(err)
	}
	alarms, err := store.AlarmEventsForFilterBefore("", AlarmFilter{Code: "emergency_access_granted"}, 0, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(alarms) != 1 || alarms[0].SourceAuditID == 0 || alarms[0].Detail != "granted emergency access" {
		t.Fatalf("alarm did not survive audit retention: %+v", alarms)
	}
}

func TestAlarmAgeRetentionIsIndependentFromAuditRetention(t *testing.T) {
	store := openTemp(t, "alarm-age-retention.db")
	now := time.Date(2026, 11, 24, 12, 0, 0, 0, time.UTC)
	for _, event := range []AuditEvent{
		{Username: "operator", Action: AuditEmergencyAccess, Detail: "old alarm", CreatedAt: now.Add(-48 * time.Hour), Success: true},
		{Username: "operator", Action: AuditEmergencyAccess, Detail: "fresh alarm", CreatedAt: now.Add(-time.Hour), Success: true},
	} {
		if err := store.AppendAudit(event); err != nil {
			t.Fatalf("append %s: %v", event.Detail, err)
		}
	}

	cutoff := now.Add(-24 * time.Hour)
	if _, err := store.PruneBefore(cutoff); err != nil {
		t.Fatalf("prune source audit history: %v", err)
	}
	alarms, err := store.AlarmEventsForFilterBefore("", AlarmFilter{Code: "emergency_access_granted"}, 0, 10)
	if err != nil || len(alarms) != 2 {
		t.Fatalf("alarms after source audit prune = %+v, %v", alarms, err)
	}
	if _, err := store.PruneAlarmsBefore(time.Time{}); err == nil {
		t.Fatal("zero alarm retention cutoff was accepted")
	}
	report, err := store.PruneAlarmsBefore(cutoff)
	if err != nil || report.Alarms != 1 {
		t.Fatalf("alarm age retention = %+v, %v", report, err)
	}
	alarms, err = store.AlarmEventsForFilterBefore("", AlarmFilter{Code: "emergency_access_granted"}, 0, 10)
	if err != nil || len(alarms) != 1 || alarms[0].Detail != "fresh alarm" {
		t.Fatalf("alarms after their own retention = %+v, %v", alarms, err)
	}
}

func TestAlarmClassifiesSuccessfulSecurityAndAccessChanges(t *testing.T) {
	cases := []struct {
		action   string
		category string
		severity string
		code     string
	}{
		{AuditAccountPassword, "account", "notice", "account_password_reset"},
		{AuditPasswordChange, "account", "notice", "account_password_changed"},
		{AuditTwoFactorEnable, "security", "notice", "two_factor_enabled"},
		{AuditTwoFactorDisable, "security", "warning", "two_factor_disabled"},
		{AuditTwoFactorReset, "security", "warning", "two_factor_reset"},
		{AuditTwoFactorRecovery, "security", "notice", "two_factor_recovery_codes_generated"},
		{AuditWebAuthnRegister, "security", "notice", "webauthn_credential_added"},
		{AuditWebAuthnRemove, "security", "warning", "webauthn_credential_removed"},
		{AuditLoginDevicePolicy, "security", "warning", "login_devices_changed"},
		{AuditLoginDeviceTrust, "security", "warning", "login_devices_changed"},
		{AuditLoginDeviceRevoke, "security", "warning", "login_devices_changed"},
		{AuditRoleChange, "access", "warning", "admin_access_changed"},
		{AuditRoleBind, "access", "warning", "admin_access_changed"},
		{AuditDevicePolicyUpdate, "access", "warning", "host_access_changed"},
		{AuditDeviceDisable, "access", "warning", "host_access_changed"},
		{AuditAccessKidRevoke, "security", "warning", "access_signing_key_revoked"},
	}
	for _, test := range cases {
		t.Run(test.action, func(t *testing.T) {
			store := openTemp(t, "alarm-security-change.db")
			if err := store.AppendAudit(AuditEvent{
				Username: "operator", Action: test.action, Detail: "changed", Success: true,
			}); err != nil {
				t.Fatalf("append %s: %v", test.action, err)
			}
			alarms, err := store.AlarmEventsForFilterBefore("", AlarmFilter{Code: test.code}, 0, 20)
			if err != nil {
				t.Fatalf("list %s alarms: %v", test.action, err)
			}
			if len(alarms) != 1 || alarms[0].SourceAction != test.action ||
				alarms[0].Category != test.category || alarms[0].Severity != test.severity {
				t.Fatalf("%s alarms = %+v", test.action, alarms)
			}
		})
	}

	store := openTemp(t, "alarm-rejected-security-change.db")
	if err := store.AppendAudit(AuditEvent{
		Username: "operator", Action: AuditTwoFactorDisable,
		Detail: "invalid credentials", Success: false,
	}); err != nil {
		t.Fatalf("append rejected security change: %v", err)
	}
	alarms, err := store.AlarmEventsForFilterBefore("", AlarmFilter{Code: "two_factor_disabled"}, 0, 20)
	if err != nil {
		t.Fatalf("list failed-change alarms: %v", err)
	}
	if len(alarms) != 0 {
		t.Fatalf("failed security change generated alarms: %+v", alarms)
	}
}
