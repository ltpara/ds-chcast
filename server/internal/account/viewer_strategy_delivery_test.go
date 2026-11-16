package account

import (
	"errors"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/strategy"
)

func TestViewerStrategyDeliveryReceiptIsolationAndSequence(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "viewer strategy", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeDevice, "devbox", 0)
	if _, err := store.SetClientStrategy(policy.ID, 0, strategySettings(t, `{
		"viewer.tunnel_mode":{"mode":"locked","value":"relay"},
		"viewer.sync_initial_clipboard":{"mode":"default","value":true}
	}`), "root"); err != nil {
		t.Fatal(err)
	}
	snapshot, err := store.ResolveClientStrategy("devbox", strategy.Viewer)
	if err != nil {
		t.Fatal(err)
	}
	delivery, err := store.CreateViewerStrategyDelivery("user-a", "devbox", "client-a", snapshot)
	if err != nil || delivery.Context == "" || delivery.Snapshot.Digest != snapshot.Digest {
		t.Fatalf("delivery = %+v %v", delivery, err)
	}
	receipt := strategy.Receipt{
		Revision: snapshot.Revision,
		Digest:   snapshot.Digest,
		Sequence: 1,
		Status:   strategy.Applied,
		Results: map[string]strategy.Result{
			"viewer.sync_initial_clipboard": {Status: strategy.Applied},
			"viewer.tunnel_mode":            {Status: strategy.Applied},
		},
	}
	for _, tc := range []struct {
		name, user, peer, context, key string
	}{
		{"user", "user-b", "devbox", delivery.Context, "client-a"},
		{"peer", "user-a", "other", delivery.Context, "client-a"},
		{"context", "user-a", "devbox", "stolen", "client-a"},
		{"client key", "user-a", "devbox", delivery.Context, "client-b"},
	} {
		if _, err := store.RecordViewerStrategyReceipt(tc.user, tc.peer, tc.context, tc.key, receipt); !errors.Is(err, ErrStrategyConflict) {
			t.Fatalf("%s crossed delivery boundary: %v", tc.name, err)
		}
	}
	first, err := store.RecordViewerStrategyReceipt("user-a", "devbox", delivery.Context, "client-a", receipt)
	if err != nil || !first.Changed {
		t.Fatalf("first = %+v %v", first, err)
	}
	repeated, err := store.RecordViewerStrategyReceipt("user-a", "devbox", delivery.Context, "client-a", receipt)
	if err != nil || repeated.Changed || repeated.ReportedAtMS != first.ReportedAtMS {
		t.Fatalf("repeat = %+v %v", repeated, err)
	}
	receipt.Status = strategy.Failed
	receipt.Results["viewer.tunnel_mode"] = strategy.Result{Status: strategy.Failed, Detail: "transport unavailable"}
	if _, err := store.RecordViewerStrategyReceipt("user-a", "devbox", delivery.Context, "client-a", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("same sequence replaced outcome: %v", err)
	}
	receipt.Sequence = 2
	if _, err := store.RecordViewerStrategyReceipt("user-a", "devbox", delivery.Context, "client-a", receipt); err != nil {
		t.Fatal(err)
	}
	receipt.Sequence = 1
	if _, err := store.RecordViewerStrategyReceipt("user-a", "devbox", delivery.Context, "client-a", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("late sequence accepted: %v", err)
	}
	current, err := store.ViewerStrategyReceipt("user-a", "devbox", delivery.Context)
	if err != nil || current == nil || current.Sequence != 2 || current.Status != strategy.Failed {
		t.Fatalf("current = %+v %v", current, err)
	}
}

func TestViewerStrategyDeliveryRejectsRevisionChangeAndExpiry(t *testing.T) {
	store := openGroupStore(t)
	policy := mustCreatePolicy(t, store, "viewer strategy", DevicePolicyOpen, 0, nil, nil)
	mustAssign(t, store, policy.ID, PolicyScopeAll, "", 0)
	if _, err := store.SetClientStrategy(policy.ID, 0, strategySettings(t, `{
		"viewer.view_fit":{"mode":"locked","value":1}
	}`), "root"); err != nil {
		t.Fatal(err)
	}
	snapshot, err := store.ResolveClientStrategy("devbox", strategy.Viewer)
	if err != nil {
		t.Fatal(err)
	}
	delivery, err := store.CreateViewerStrategyDelivery("user-a", "devbox", "client-a", snapshot)
	if err != nil {
		t.Fatal(err)
	}
	receipt := strategy.Receipt{
		Revision: snapshot.Revision,
		Digest:   snapshot.Digest,
		Sequence: 1,
		Status:   strategy.Applied,
		Results:  map[string]strategy.Result{"viewer.view_fit": {Status: strategy.Applied}},
	}
	if _, err := store.SetClientStrategy(policy.ID, 1, strategy.Settings{}, "root"); err != nil {
		t.Fatal(err)
	}
	if _, err := store.RecordViewerStrategyReceipt("user-a", "devbox", delivery.Context, "client-a", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("stale revision accepted: %v", err)
	}
	current, err := store.ResolveClientStrategy("devbox", strategy.Viewer)
	if err != nil {
		t.Fatal(err)
	}
	fresh, err := store.CreateViewerStrategyDelivery("user-a", "devbox", "client-a", current)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.db.Exec(`UPDATE viewer_strategy_deliveries SET expires_at = 0 WHERE context_id = ?`, fresh.Context); err != nil {
		t.Fatal(err)
	}
	receipt = strategy.Receipt{
		Revision: current.Revision,
		Digest:   current.Digest,
		Sequence: 1,
		Status:   strategy.Applied,
		Results:  map[string]strategy.Result{},
	}
	if _, err := store.RecordViewerStrategyReceipt("user-a", "devbox", fresh.Context, "client-a", receipt); !errors.Is(err, ErrStrategyConflict) {
		t.Fatalf("expired delivery accepted: %v", err)
	}
}
