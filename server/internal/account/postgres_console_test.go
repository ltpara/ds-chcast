package account

import (
	"errors"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/dbmetric"
	"github.com/ltpara/ds-chcast/server/internal/liveconn"
	"github.com/ltpara/ds-chcast/server/internal/pgtest"
	"github.com/ltpara/ds-chcast/server/internal/schemamig"
	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

func TestPostgresConsoleUpgrade23ToCurrent(t *testing.T) {
	dsn := pgtest.Schema(t)
	db, err := dbmetric.OpenPostgres("migration-fixture", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	// Build the old schema from its actual migration prefix rather than marking
	// a current database as old while retaining newer tables and ledger rows.
	if _, err := schemamig.RunPostgres(db, "schema_migrations", accountPostgresMigrations[:23]); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`INSERT INTO users(id,username,role,password_hash,created_at) VALUES ('legacy-id','legacy-admin','admin','',1234)`); err != nil {
		t.Fatal(err)
	}
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	legacy, err := store.FindUser("legacy-admin")
	if err != nil || legacy.ID != "legacy-id" || legacy.Role != RoleAdmin {
		t.Fatalf("legacy account changed: %+v %v", legacy, err)
	}
	admin, err := store.AdminRole(RoleAdmin)
	if err != nil || !reflect.DeepEqual(admin.Permissions, AllPermissions()) {
		t.Fatalf("admin catalogue: %+v %v", admin, err)
	}
	custom, err := store.CreateAdminRole("custom", "Custom", "preserve", []string{PermissionConnectionsDisconnect})
	if err != nil {
		t.Fatal(err)
	}
	// Compare persisted values: creation returns the clock's nanoseconds while
	// the storage contract intentionally uses millisecond timestamps.
	custom, err = store.AdminRole(custom.ID)
	if err != nil {
		t.Fatal(err)
	}
	binding, err := store.BindRole(legacy.Username, custom.ID, RoleScopeObject, "eastbox", "device")
	if err != nil {
		t.Fatal(err)
	}
	// Model an existing v24 catalogue, including customer roles and bindings.
	old := []string{}
	for _, key := range AllPermissions() {
		if key != PermissionDevicesEmergencyAccess {
			old = append(old, key)
		}
	}
	if _, err := db.Exec(`UPDATE admin_roles SET permissions=? WHERE id='admin'`, strings.Join(old, ",")); err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`DELETE FROM schema_migrations WHERE version>=25`); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		reopened, err := OpenPostgres(dsn)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { reopened.Close() })
		got, err := reopened.AdminRole(RoleAdmin)
		if err != nil || !reflect.DeepEqual(got.Permissions, AllPermissions()) {
			t.Fatalf("v25 catalogue: %+v %v", got, err)
		}
		gotCustom, err := reopened.AdminRole(custom.ID)
		if err != nil || !reflect.DeepEqual(gotCustom, custom) {
			t.Fatalf("custom role changed: %+v %v", gotCustom, err)
		}
		bindings, err := reopened.RoleBindings(legacy.ID)
		if err != nil || len(bindings) != 1 || bindings[0].ID != binding.ID {
			t.Fatalf("bindings changed: %+v %v", bindings, err)
		}
		if err := reopened.Close(); err != nil {
			t.Fatal(err)
		}
	}
	var count, version int
	if err := db.QueryRow(`SELECT COUNT(*),MAX(version) FROM schema_migrations`).Scan(&count, &version); err != nil {
		t.Fatal(err)
	}
	if count != accountSchemaVersion || version != accountSchemaVersion {
		t.Fatalf("ledger: count=%d version=%d", count, version)
	}
	// Older binaries must refuse the upgraded database; rollback restores a
	// pre-upgrade backup instead of writing through an old schema contract.
	_, err = schemamig.RunPostgres(db, "schema_migrations", accountPostgresMigrations[:24])
	var tooNew *schemamig.TooNewError
	if !errors.As(err, &tooNew) || tooNew.Found != accountSchemaVersion || tooNew.Supported != 24 {
		t.Fatalf("downgrade guard: %v", err)
	}
}

func TestPostgresNamedRolesAndRevocation(t *testing.T) {
	dsn := pgtest.Schema(t)
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, username := range []string{"root", "manager", "scoped"} {
		if _, err := store.CreateUser(username, "correct horse battery"); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := store.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	role, err := store.CreateAdminRole("full", "Full manager", "", AllPermissions())
	if err != nil {
		t.Fatal(err)
	}
	binding, err := store.BindRole("manager", role.ID, RoleScopeGlobal, "", "")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.SetRole("root", RoleUser); err != nil {
		t.Fatal(err)
	}
	if _, err := store.SetBanned("manager", true); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("last named manager ban: %v", err)
	}
	if _, err := store.UnbindRole(binding.ID); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("last named manager unbind: %v", err)
	}
	if err := store.DeleteAdminRole(role.ID, role.Revision); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("last named manager role delete: %v", err)
	}
	if _, err := store.UpdateAdminRole(role.ID, role.Name, "", []string{PermissionAccountsWrite}, role.Revision); !errors.Is(err, ErrLastAdmin) {
		t.Fatalf("last named manager downgrade: %v", err)
	}
	reader, err := store.CreateAdminRole("reader", "Scoped reader", "", []string{PermissionConnectionsRead})
	if err != nil {
		t.Fatal(err)
	}
	group, err := store.BindRole("scoped", reader.ID, RoleScopeGroup, "east", "")
	if err != nil {
		t.Fatal(err)
	}
	object, err := store.BindRole("scoped", reader.ID, RoleScopeObject, "westbox", "device")
	if err != nil {
		t.Fatal(err)
	}
	user, err := store.FindUser("scoped")
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		group, kind, id string
		want            bool
	}{{"", "", "", false}, {"east", "", "", true}, {"west", "", "", false}, {"", "device", "westbox", true}, {"", "device", "eastbox", false}} {
		perms, err := store.EffectiveConsolePermissions(user.ID, tc.group, tc.kind, tc.id)
		if err != nil || perms[PermissionConnectionsRead] != tc.want {
			t.Fatalf("scope %+v: %v %v", tc, perms, err)
		}
	}
	// A second store observes revocation without reopening or a local cache.
	other, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close()
	for _, id := range []string{group.ID, object.ID} {
		if _, err := store.UnbindRole(id); err != nil {
			t.Fatal(err)
		}
	}
	perms, err := other.EffectiveConsolePermissions(user.ID, "east", "device", "westbox")
	if err != nil || perms[PermissionConnectionsRead] {
		t.Fatalf("revocation: %v %v", perms, err)
	}
	if _, err := store.SetRole("root", RoleAdmin); err != nil {
		t.Fatal(err)
	}
	if _, err := store.UnbindRole(binding.ID); err != nil {
		t.Fatal(err)
	}
}

func TestPostgresLiveConnectionRestartAndScope(t *testing.T) {
	dsn := pgtest.Schema(t)
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if store != nil {
			store.Close()
		}
	}()
	now := time.Now().UTC().Truncate(time.Millisecond)
	store.SetClock(func() time.Time { return now })
	if _, err := store.CreateGroup("east", "East", "", 0, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := store.AddGroupDevice("east", "eastbox", "root"); err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"eastbox", "westbox"} {
		if _, err := store.ApplyLiveSnapshot(id, testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB)); err != nil {
			t.Fatal(err)
		}
		now = now.Add(time.Second)
	}
	command, err := store.DisconnectLiveConnection("eastbox", liveConnA, "pg-disconnect", "root", "acceptance")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	store.SetClock(func() time.Time { return now })
	for _, list := range []func() ([]LiveConnection, error){
		func() ([]LiveConnection, error) { return store.LiveConnectionsInGroup("east", 1) },
		func() ([]LiveConnection, error) {
			return store.LiveConnectionsForDevices(map[string]bool{"eastbox": true}, 1)
		},
	} {
		rows, err := list()
		if err != nil || len(rows) != 1 || rows[0].HostID != "eastbox" {
			t.Fatalf("scope before limit: %+v %v", rows, err)
		}
	}
	retry, err := store.DisconnectLiveConnection("eastbox", liveConnA, "pg-disconnect", "root", "acceptance")
	if err != nil || retry.CommandID != command.CommandID {
		t.Fatalf("restart idempotency: %+v %v", retry, err)
	}
	for i := 0; i < 2; i++ {
		rows, err := store.PollLiveCommands("eastbox", liveRuntimeA, 10)
		if err != nil || len(rows) != 1 || rows[0].CommandID != command.CommandID {
			t.Fatalf("redelivery: %+v %v", rows, err)
		}
	}
	receipt := liveconn.Receipt{CommandID: command.CommandID, ReceiptSequence: 1, Status: liveconn.CommandApplied, ConfirmedAbsent: true, Detail: "exact session absent"}
	for i := 0; i < 2; i++ {
		if _, err := store.RecordLiveCommandReceipt("eastbox", liveRuntimeA, receipt); err != nil {
			t.Fatal(err)
		}
	}
	closed, err := store.LiveConnection("eastbox", liveConnA)
	if err != nil || closed.State != liveconn.ConnectionClosed || !closed.ReceiptConfirmedAbsent {
		t.Fatalf("confirmed close: %+v %v", closed, err)
	}
	other, err := store.LiveConnection("eastbox", liveConnB)
	if err != nil || other.State != liveconn.ConnectionActive {
		t.Fatalf("other viewer affected: %+v %v", other, err)
	}
}

func TestPostgresConsoleBackupRestore(t *testing.T) {
	if os.Getenv("CHAST_TEST_POSTGRES_BIN") == "" {
		t.Skip("CHAST_TEST_POSTGRES_BIN is not set")
	}
	dsn := pgtest.Schema(t)
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if store != nil {
			store.Close()
		}
	}()
	now := time.Now().UTC().Truncate(time.Millisecond)
	store.SetClock(func() time.Time { return now })
	const password = "backup acceptance password"
	if _, err := store.CreateUser("BackupAdmin", password); err != nil {
		t.Fatal(err)
	}
	user, err := store.CreateUser("BackupScoped", password)
	if err != nil {
		t.Fatal(err)
	}
	role, err := store.CreateAdminRole("backup-reader", "Backup reader", "restore me", []string{PermissionConnectionsRead})
	if err != nil {
		t.Fatal(err)
	}
	binding, err := store.BindRole(user.Username, role.ID, RoleScopeObject, "backupbox", "device")
	if err != nil {
		t.Fatal(err)
	}
	beforeBindings, err := store.RoleBindings(user.ID)
	if err != nil || len(beforeBindings) != 1 {
		t.Fatalf("bindings before backup: %+v %v", beforeBindings, err)
	}
	policy := mustCreatePolicy(t, store, "backup-policy", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeDevice, "backupbox", 0)
	config, err := store.SetClientStrategy(policy.ID, 0, strategySettings(t, `{"host.keep_awake":{"mode":"locked","value":false}}`), "BackupAdmin")
	if err != nil {
		t.Fatal(err)
	}
	snapshot, err := store.ResolveClientStrategy("backupbox", strategy.Host)
	if err != nil {
		t.Fatal(err)
	}
	strategyReceipt, err := store.RecordHostStrategyReceipt("backupbox", "backup-key", strategy.Receipt{
		Revision: snapshot.Revision, Digest: snapshot.Digest, Sequence: 1, Status: strategy.PendingRestart,
		Results: map[string]strategy.Result{"host.keep_awake": {Status: strategy.PendingRestart}},
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.ApplyLiveSnapshot("backupbox", testLiveSnapshot(liveRuntimeA, 1, false, liveConnA, liveConnB)); err != nil {
		t.Fatal(err)
	}
	command, err := store.DisconnectLiveConnection("backupbox", liveConnA, "backup-request", "BackupAdmin", "backup acceptance")
	if err != nil {
		t.Fatal(err)
	}
	commands, err := store.PollLiveCommands("backupbox", liveRuntimeA, 10)
	if err != nil || len(commands) != 1 || commands[0].CommandID != command.CommandID {
		t.Fatalf("deliver disconnect before backup: %+v %v", commands, err)
	}
	receipt := liveconn.Receipt{CommandID: command.CommandID, ReceiptSequence: 1, Status: liveconn.CommandApplied, ConfirmedAbsent: true, Detail: "confirmed before backup"}
	if _, err := store.RecordLiveCommandReceipt("backupbox", liveRuntimeA, receipt); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	pgtest.BackupRestore(t, dsn)
	// Check the archive's ledger before OpenPostgres can run any migration.
	restoredDB, err := dbmetric.OpenPostgres("restore-check", dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer restoredDB.Close()
	var count, version int
	if err := restoredDB.QueryRow(`SELECT COUNT(*),MAX(version) FROM schema_migrations`).Scan(&count, &version); err != nil {
		t.Fatal(err)
	}
	if count != accountSchemaVersion || version != accountSchemaVersion {
		t.Fatalf("restored migration ledger: count=%d version=%d", count, version)
	}
	store, err = OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	store.SetClock(func() time.Time { return now })
	logged, _, _, err := store.Login(strings.ToLower(user.Username), password)
	if err != nil || logged.ID != user.ID {
		t.Fatalf("restored account/collation: id=%s err=%v", logged.ID, err)
	}
	gotRole, err := store.AdminRole(role.ID)
	if err != nil || !reflect.DeepEqual(gotRole, role) {
		t.Fatalf("restored role: %+v %v", gotRole, err)
	}
	bindings, err := store.RoleBindings(user.ID)
	if err != nil || !reflect.DeepEqual(bindings, beforeBindings) {
		t.Fatalf("restored binding: %+v %v", bindings, err)
	}
	for _, tc := range []struct {
		device string
		want   bool
	}{{"backupbox", true}, {"otherbox", false}} {
		perms, err := store.EffectiveConsolePermissions(user.ID, "", "device", tc.device)
		if err != nil || perms[PermissionConnectionsRead] != tc.want {
			t.Fatalf("restored scope %s: %v %v", tc.device, perms, err)
		}
	}
	gotConfig, err := store.ClientStrategy(policy.ID)
	if err != nil || !reflect.DeepEqual(gotConfig, config) {
		t.Fatalf("restored strategy config: %+v %v", gotConfig, err)
	}
	gotSnapshot, err := store.ResolveClientStrategy("backupbox", strategy.Host)
	if err != nil || !reflect.DeepEqual(gotSnapshot, snapshot) {
		t.Fatalf("restored strategy generation: %+v %v", gotSnapshot, err)
	}
	gotReceipt, err := store.CurrentClientStrategyReceipt(gotSnapshot, "backup-key")
	strategyReceipt.Changed = false // This flag describes the write, not stored state.
	if err != nil || !reflect.DeepEqual(gotReceipt, &strategyReceipt) {
		t.Fatalf("restored strategy receipt: %+v %v", gotReceipt, err)
	}
	retry, err := store.DisconnectLiveConnection("backupbox", liveConnA, "backup-request", "BackupAdmin", "backup acceptance")
	if err != nil || retry.CommandID != command.CommandID || retry.Status != liveconn.CommandApplied {
		t.Fatalf("restored disconnect idempotency: %+v %v", retry, err)
	}
	if _, err := store.RecordLiveCommandReceipt("backupbox", liveRuntimeA, receipt); err != nil {
		t.Fatalf("restored receipt replay: %v", err)
	}
	closed, err := store.LiveConnection("backupbox", liveConnA)
	if err != nil || closed.State != liveconn.ConnectionClosed || !closed.ReceiptConfirmedAbsent || closed.ReceiptDetail != receipt.Detail {
		t.Fatalf("restored confirmed close: %+v %v", closed, err)
	}
	other, err := store.LiveConnection("backupbox", liveConnB)
	if err != nil || other.State != liveconn.ConnectionActive {
		t.Fatalf("restored other viewer: %+v %v", other, err)
	}
	// Restored state must remain writable, with CAS and scope revocation intact.
	if _, err := store.SetClientStrategy(policy.ID, 0, config.Settings, "BackupAdmin"); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("restored CAS: %v", err)
	}
	if _, err := store.UnbindRole(binding.ID); err != nil {
		t.Fatal(err)
	}
	perms, err := store.EffectiveConsolePermissions(user.ID, "", "device", "backupbox")
	if err != nil || perms[PermissionConnectionsRead] {
		t.Fatalf("restored revocation: %v %v", perms, err)
	}
}
