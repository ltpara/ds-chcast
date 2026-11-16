package account

import (
	"database/sql"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

func strategySettings(t *testing.T, raw string) strategy.Settings {
	t.Helper()
	var settings strategy.Settings
	if err := json.Unmarshal([]byte(raw), &settings); err != nil {
		t.Fatal(err)
	}
	return settings
}

func TestClientStrategyLifecycleAndAdmissionIsolation(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "managed", DevicePolicyOpen, 5, nil, nil)
	assignment := mustAssign(t, store, policy.ID, PolicyScopeAll, "", 0)
	initial, err := store.ClientStrategy(policy.ID)
	if err != nil || initial.Revision != 0 || len(initial.Settings) != 0 {
		t.Fatalf("initial = %+v %v", initial, err)
	}
	settings := strategySettings(t, `{"host.keep_awake":{"mode":"locked","value":false},"viewer.tunnel_mode":{"mode":"default","value":"relay"}}`)
	saved, err := store.SetClientStrategy(policy.ID, 0, settings, "root")
	if err != nil || saved.Revision != 1 {
		t.Fatalf("save = %+v %v", saved, err)
	}
	if _, err := store.SetClientStrategy(policy.ID, 0, settings, "root"); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("stale save = %v", err)
	}
	unchanged, err := store.SetClientStrategy(policy.ID, 1, settings, "root")
	if err != nil || unchanged.Revision != 1 {
		t.Fatalf("unchanged = %+v %v", unchanged, err)
	}
	still, err := store.PolicyObject(policy.ID)
	if err != nil || still.PolicyVersion != policy.PolicyVersion {
		t.Fatalf("config changed admission version: %+v %v", still, err)
	}
	host, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || host.Revision != 1 || len(host.Settings) != 1 || string(host.Settings["host.keep_awake"].Value) != "false" {
		t.Fatalf("host = %+v %v", host, err)
	}
	viewer, err := store.ResolveClientStrategy("devbox", strategy.Viewer)
	if err != nil || viewer.Revision != 1 || len(viewer.Settings) != 1 || string(viewer.Settings["viewer.tunnel_mode"].Value) != `"relay"` {
		t.Fatalf("viewer = %+v %v", viewer, err)
	}
	// A manual access override must not silently erase client configuration.
	if _, err := store.SetDevicePolicy("devbox", DevicePolicyRestricted, 3, nil, nil, "root"); err != nil {
		t.Fatal(err)
	}
	hostAgain, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || hostAgain.Revision != host.Revision || hostAgain.Digest != host.Digest {
		t.Fatalf("manual override changed strategy: %+v %v", hostAgain, err)
	}
	if err := store.DeletePolicyAssignment(assignment.ID); err != nil {
		t.Fatal(err)
	}
	removed, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || removed.Revision != 2 || len(removed.Settings) != 0 || removed.PolicyID != "" {
		t.Fatalf("removal = %+v %v", removed, err)
	}
	mustAssign(t, store, policy.ID, PolicyScopeDevice, "devbox", 0)
	restored, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || restored.Revision != 3 || restored.Digest != host.Digest {
		t.Fatalf("restore = %+v %v", restored, err)
	}
	cleared, err := store.SetClientStrategy(policy.ID, 1, strategy.Settings{}, "root")
	if err != nil || cleared.Revision != 2 {
		t.Fatalf("clear = %+v %v", cleared, err)
	}
	if _, err := store.SetClientStrategy(policy.ID, 0, settings, "root"); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("clear reset revision: %v", err)
	}
}

func TestClientStrategyAssignmentPrecedenceAndFiltering(t *testing.T) {
	store := openGroupStore(t)
	all := mustCreatePolicy(t, store, "all", DevicePolicyOpen, 0, nil, nil)
	device := mustCreatePolicy(t, store, "device", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, all.ID, PolicyScopeAll, "", 0)
	mustAssign(t, store, device.ID, PolicyScopeDevice, "devbox", 0)
	_, err := store.SetClientStrategy(all.ID, 0, strategySettings(t, `{"host.keep_awake":{"mode":"locked","value":true}}`), "root")
	if err != nil {
		t.Fatal(err)
	}
	// A narrower empty policy inherits local values, not a mix of losing policies.
	got, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || got.PolicyID != device.ID || len(got.Settings) != 0 {
		t.Fatalf("winner = %+v %v", got, err)
	}
	_, err = store.SetClientStrategy(device.ID, 0, strategySettings(t, `{"viewer.use_websocket":{"mode":"locked","value":true}}`), "root")
	if err != nil {
		t.Fatal(err)
	}
	again, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || again.Revision != got.Revision {
		t.Fatalf("unrelated consumer changed host revision: %+v %v", again, err)
	}
	if _, err := store.ResolveClientStrategy("../bad", strategy.Host); !errors.Is(err, ErrInvalidPeerID) {
		t.Fatal(err)
	}
	if _, err := store.ResolveClientStrategy("devbox", "invalid"); !errors.Is(err, strategy.ErrInvalid) {
		t.Fatal(err)
	}
}

func TestClientStrategySurvivesRestartAndConcurrentEdits(t *testing.T) {
	path := filepath.Join(t.TempDir(), "strategy.db")
	store, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if store != nil {
			store.Close()
		}
	}()
	other, err := Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer other.Close()
	policy := mustCreatePolicy(t, store, "managed", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeAll, "", 0)
	var wg sync.WaitGroup
	errorsOut := make(chan error, 2)
	for i, raw := range []string{`{"host.max_viewers":{"mode":"locked","value":1}}`, `{"host.max_viewers":{"mode":"locked","value":8}}`} {
		settings := strategySettings(t, raw)
		writer := []*Store{store, other}[i]
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := writer.SetClientStrategy(policy.ID, 0, settings, "root")
			errorsOut <- err
		}()
	}
	wg.Wait()
	close(errorsOut)
	success, conflict := 0, 0
	for err := range errorsOut {
		if err == nil {
			success++
		} else if errors.Is(err, ErrStrategyConflict) {
			conflict++
		} else {
			t.Fatal(err)
		}
	}
	if success != 1 || conflict != 1 {
		t.Fatalf("success=%d conflict=%d", success, conflict)
	}
	before, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path)
	if err != nil {
		t.Fatal(err)
	}
	after, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil || after.Revision != before.Revision || after.Digest != before.Digest {
		t.Fatalf("restart = %+v %v", after, err)
	}
	current, err := store.ClientStrategy(policy.ID)
	if err != nil || current.Revision != 1 {
		t.Fatalf("restart config = %+v %v", current, err)
	}
}

func TestPostgresClientStrategy(t *testing.T) {
	dsn := os.Getenv("CHAST_TEST_POSTGRES_DSN")
	if dsn == "" {
		t.Skip("CHAST_TEST_POSTGRES_DSN is not set")
	}
	store, err := OpenPostgres(dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	suffix := strconv.FormatInt(time.Now().UnixNano(), 36)
	policy := mustCreatePolicy(t, store, "strategy-"+suffix, DevicePolicyOpen, 0, nil, nil)
	defer store.DeletePolicyObject(policy.ID)
	peer := "strategy-" + suffix
	assignment := mustAssign(t, store, policy.ID, PolicyScopeDevice, peer, 10000)
	defer store.DeletePolicyAssignment(assignment.ID)
	defer store.db.Exec(`DELETE FROM device_client_strategies WHERE peer_id = ?`, peer)
	defer store.db.Exec(`DELETE FROM client_strategy_receipts WHERE peer_id = ?`, peer)
	settings := strategySettings(t, `{"host.keep_awake":{"mode":"locked","value":false}}`)
	config, err := store.SetClientStrategy(policy.ID, 0, settings, "root")
	if err != nil || config.Revision != 1 {
		t.Fatalf("save: %+v %v", config, err)
	}
	if _, err := store.SetClientStrategy(policy.ID, 0, settings, "root"); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("CAS: %v", err)
	}
	first, err := store.ResolveClientStrategy(peer, strategy.Host)
	if err != nil || len(first.Settings) != 1 {
		t.Fatalf("snapshot: %+v %v", first, err)
	}
	receipt := strategy.Receipt{Revision: first.Revision, Digest: first.Digest, Sequence: 1, Status: strategy.PendingRestart,
		Results: map[string]strategy.Result{"host.keep_awake": {Status: strategy.PendingRestart}}}
	if _, err := store.RecordHostStrategyReceipt(peer, "test-key", receipt); err != nil {
		t.Fatalf("receipt: %v", err)
	}
	if _, err := store.RecordHostStrategyReceipt(peer, "test-key", receipt); err != nil {
		t.Fatalf("repeat receipt: %v", err)
	}
	_, err = store.ImportPolicyObjects([]PolicyObjectImport{{Name: policy.Name, Mode: policy.Mode, ClientSettings: &strategy.Settings{}}}, PolicyImportConflictUpdate, "root")
	if err != nil {
		t.Fatal(err)
	}
	removed, err := store.ResolveClientStrategy(peer, strategy.Host)
	if err != nil || len(removed.Settings) != 0 || removed.Revision <= first.Revision {
		t.Fatalf("removal: %+v %v", removed, err)
	}
	if got, err := store.CurrentClientStrategyReceipt(removed, "test-key"); err != nil || got != nil {
		t.Fatalf("stale receipt: %+v %v", got, err)
	}
}

func TestClientStrategyMigrationIsAdditive(t *testing.T) {
	store := openGroupStore(t)
	// Drop only the new tables from this disposable test database to model v19.
	for _, query := range []string{
		`DROP TABLE viewer_strategy_receipts`,
		`DROP TABLE viewer_strategy_deliveries`,
		`DROP TABLE client_strategy_receipts`,
		`DROP TABLE device_client_strategies`,
		`DROP TABLE policy_client_strategies`,
	} {
		if _, err := store.db.Exec(query); err != nil {
			t.Fatal(err)
		}
	}
	policy := mustCreatePolicy(t, store, "existing policy", DevicePolicyRestricted, 7, nil, nil)
	// Both backends must wire the portable migration, not leave PostgreSQL's
	// default no-op registration in place. This checks dispatch/DDL only;
	// TestPostgresClientStrategy performs the real backend integration.
	for _, migration := range []func(*sql.DB) error{
		accountPostgresMigrations[19].Apply,
		accountMigrations[19].Apply,
		accountPostgresMigrations[20].Apply,
		accountMigrations[20].Apply,
		accountPostgresMigrations[21].Apply,
		accountMigrations[21].Apply,
	} {
		if err := migration(store.db); err != nil {
			t.Fatal(err)
		}
	}
	got, err := store.PolicyObject(policy.ID)
	if err != nil || got.PolicyVersion != 1 || got.PermissionCeiling != 7 {
		t.Fatalf("migration changed policy: %+v %v", got, err)
	}
	config, err := store.ClientStrategy(policy.ID)
	if err != nil || config.Revision != 0 || len(config.Settings) != 0 {
		t.Fatalf("migration default: %+v %v", config, err)
	}
	if _, err := store.db.Exec(`SELECT receipt_json FROM client_strategy_receipts`); err != nil {
		t.Fatal(err)
	}
	if _, err := store.db.Exec(`SELECT receipt_json FROM viewer_strategy_receipts`); err != nil {
		t.Fatal(err)
	}
}
