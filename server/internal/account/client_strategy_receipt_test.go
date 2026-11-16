package account

import (
	"errors"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

func TestClientStrategyReceiptsTrackApplicationAndRejectStaleResults(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "strategy", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeDevice, "devbox", 0)
	settings := strategySettings(t, `{"host.keep_awake":{"mode":"locked","value":true}}`)
	if _, err := store.SetClientStrategy(policy.ID, 0, settings, "root"); err != nil {
		t.Fatal(err)
	}
	snapshot, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil {
		t.Fatal(err)
	}
	if got, err := store.CurrentClientStrategyReceipt(snapshot, "test-key"); err != nil || got != nil {
		t.Fatalf("delivery implies success: %+v %v", got, err)
	}
	receipt := strategy.Receipt{Revision: snapshot.Revision, Digest: snapshot.Digest, Sequence: 1, Status: strategy.PendingRestart,
		Results: map[string]strategy.Result{"host.keep_awake": {Status: strategy.PendingRestart}}}
	first, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt)
	if err != nil {
		t.Fatal(err)
	}
	repeated, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt)
	if err != nil || repeated.ReportedAtMS != first.ReportedAtMS {
		t.Fatalf("repeat = %+v %v", repeated, err)
	}
	receipt.Status = strategy.Applied
	receipt.Results["host.keep_awake"] = strategy.Result{Status: strategy.Applied}
	if _, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("same sequence replaced outcome: %v", err)
	}
	receipt.Sequence = 2
	if _, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt); err != nil {
		t.Fatal(err)
	}
	current, err := store.CurrentClientStrategyReceipt(snapshot, "test-key")
	if err != nil || current == nil || current.Status != strategy.Applied {
		t.Fatalf("current = %+v %v", current, err)
	}
	receipt.Sequence = 1
	if _, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("late result = %v", err)
	}
	if _, err := store.SetClientStrategy(policy.ID, 1, strategy.Settings{}, "root"); err != nil {
		t.Fatal(err)
	}
	receipt.Sequence = 3
	if _, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("old config result = %v", err)
	}
	empty, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil {
		t.Fatal(err)
	}
	if got, err := store.CurrentClientStrategyReceipt(empty, "test-key"); err != nil || got != nil {
		t.Fatalf("old success visible on new config = %+v %v", got, err)
	}
	receipt = strategy.Receipt{Revision: empty.Revision, Digest: empty.Digest, Sequence: 1, Status: strategy.Applied, Results: map[string]strategy.Result{}}
	if _, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt); err != nil {
		t.Fatalf("removal ack: %v", err)
	}
	if got, err := store.CurrentClientStrategyReceipt(empty, "replacement-key"); err != nil || got != nil {
		t.Fatalf("replacement identity inherited old success: %+v %v", got, err)
	}
	receipt.Status = strategy.PendingRestart
	if _, err := store.RecordHostStrategyReceipt("devbox", "replacement-key", receipt); err != nil {
		t.Fatalf("new identity cannot report its own sequence: %v", err)
	}
	if got, err := store.CurrentClientStrategyReceipt(empty, "replacement-key"); err != nil || got == nil || got.Status != strategy.PendingRestart {
		t.Fatalf("new identity outcome: %+v %v", got, err)
	}
}

func TestClientStrategyReceiptRequiresCompleteOutcomes(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "strategy", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeAll, "", 0)
	settings := strategySettings(t, `{"host.keep_awake":{"mode":"locked","value":true},"host.max_viewers":{"mode":"locked","value":2}}`)
	if _, err := store.SetClientStrategy(policy.ID, 0, settings, "root"); err != nil {
		t.Fatal(err)
	}
	snapshot, err := store.ResolveClientStrategy("devbox", strategy.Host)
	if err != nil {
		t.Fatal(err)
	}
	for _, results := range []map[string]strategy.Result{
		nil, {}, {"host.keep_awake": {Status: strategy.Applied}},
		{"host.keep_awake": {Status: strategy.Applied}, "wrong": {Status: strategy.Applied}},
		{"host.keep_awake": {Status: strategy.Applied}, "host.max_viewers": {Status: strategy.Unsupported}},
		{"host.keep_awake": {Status: "received"}, "host.max_viewers": {Status: strategy.Applied}},
	} {
		receipt := strategy.Receipt{Revision: snapshot.Revision, Digest: snapshot.Digest, Sequence: 1, Status: strategy.Applied, Results: results}
		if _, err := store.RecordHostStrategyReceipt("devbox", "test-key", receipt); !errors.Is(err, strategy.ErrInvalid) {
			t.Fatalf("incomplete receipt accepted: %+v %v", results, err)
		}
	}
}
