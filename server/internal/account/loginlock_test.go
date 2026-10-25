package account

import (
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/authlimit"
)

func TestLoginLockLifecycle(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	store, err := Open(path)
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	key := "alice|10.0.0.1"
	if _, found, err := store.LoadLoginLock(key); err != nil || found {
		t.Fatalf("empty load = found=%v, %v", found, err)
	}

	window := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	record := authlimit.Record{
		Failures:    2,
		WindowStart: window,
		LastSeen:    window.Add(time.Minute),
	}
	if err := store.SaveLoginLock(key, record); err != nil {
		t.Fatalf("save lock: %v", err)
	}
	loaded, found, err := store.LoadLoginLock(key)
	if err != nil || !found {
		t.Fatalf("load lock = found=%v, %v", found, err)
	}
	if loaded.Failures != 2 || !loaded.WindowStart.Equal(window) ||
		!loaded.LastSeen.Equal(window.Add(time.Minute)) || !loaded.LockedUntil.IsZero() {
		t.Fatalf("loaded lock = %+v", loaded)
	}

	record.Failures = 0
	record.LockedUntil = window.Add(5 * time.Minute)
	if err := store.SaveLoginLock(key, record); err != nil {
		t.Fatalf("overwrite lock: %v", err)
	}
	loaded, _, err = store.LoadLoginLock(key)
	if err != nil || !loaded.LockedUntil.Equal(window.Add(5*time.Minute)) ||
		loaded.Failures != 0 {
		t.Fatalf("overwritten lock = %+v, %v", loaded, err)
	}

	if err := store.DeleteLoginLock(key); err != nil {
		t.Fatalf("delete lock: %v", err)
	}
	if _, found, err := store.LoadLoginLock(key); err != nil || found {
		t.Fatalf("load after delete = found=%v, %v", found, err)
	}

	if err := store.SaveLoginLock(key, record); err != nil {
		t.Fatalf("save before reopen: %v", err)
	}
	if err := store.Close(); err != nil {
		t.Fatalf("close store: %v", err)
	}
	reopened, err := Open(path)
	if err != nil {
		t.Fatalf("reopen account store: %v", err)
	}
	defer reopened.Close()
	loaded, found, err = reopened.LoadLoginLock(key)
	if err != nil || !found {
		t.Fatalf("load after reopen = found=%v, %v", found, err)
	}
	if loaded.Failures != 0 || !loaded.LockedUntil.Equal(window.Add(5*time.Minute)) ||
		!loaded.WindowStart.Equal(window) || !loaded.LastSeen.Equal(window.Add(time.Minute)) {
		t.Fatalf("persisted lock = %+v", loaded)
	}
}

func TestLoginLockFailuresAreAtomicAcrossInstances(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.db")
	first, err := Open(path)
	if err != nil {
		t.Fatalf("open first store: %v", err)
	}
	defer first.Close()
	second, err := Open(path)
	if err != nil {
		t.Fatalf("open second store: %v", err)
	}
	defer second.Close()

	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	cfg := authlimit.Config{
		Window:      10 * time.Minute,
		MaxFailures: 20,
		Lockout:     time.Minute,
		Now:         func() time.Time { return now },
	}
	one := authlimit.NewPersistent(cfg, first, nil)
	two := authlimit.NewPersistent(cfg, second, nil)

	var locked atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		limiter := one
		if i%2 == 1 {
			limiter = two
		}
		wg.Add(1)
		go func(l *authlimit.Limiter) {
			defer wg.Done()
			if _, ok := l.Failure("alice|10.0.0.1"); !ok {
				locked.Add(1)
			}
		}(limiter)
	}
	wg.Wait()
	if got := locked.Load(); got != 1 {
		t.Fatalf("failures that locked = %d, want exactly 1", got)
	}

	record, found, err := first.LoadLoginLock("alice|10.0.0.1")
	if err != nil || !found {
		t.Fatalf("load after concurrent failures = found=%v, %v", found, err)
	}
	if record.Failures != 0 || !record.LockedUntil.Equal(now.Add(time.Minute)) {
		t.Fatalf("record after 20 concurrent failures = %+v", record)
	}
	if retry, ok := two.Allow("alice|10.0.0.1"); ok || retry != time.Minute {
		t.Fatalf("Allow from the other instance = (%v, %v), want (1m, false)", retry, ok)
	}
}

func TestClearLoginLocks(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	defer store.Close()
	base := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	record := authlimit.Record{
		Failures:    5,
		WindowStart: base,
		LockedUntil: base.Add(time.Minute),
		LastSeen:    base,
	}
	for _, key := range []string{
		"alice|10.0.0.1", "alice|10.0.0.2", "alicebob|10.0.0.3", "用户|10.0.0.4",
	} {
		if err := store.SaveLoginLock(key, record); err != nil {
			t.Fatalf("save %s: %v", key, err)
		}
	}
	cleared, err := store.ClearLoginLocks(" Alice ")
	if err != nil || cleared != 2 {
		t.Fatalf("clear alice = %d, %v", cleared, err)
	}
	for _, key := range []string{"alice|10.0.0.1", "alice|10.0.0.2"} {
		if _, found, err := store.LoadLoginLock(key); err != nil || found {
			t.Fatalf("%s survived clearing: found=%v, %v", key, found, err)
		}
	}
	for _, key := range []string{"alicebob|10.0.0.3", "用户|10.0.0.4"} {
		if _, found, err := store.LoadLoginLock(key); err != nil || !found {
			t.Fatalf("%s was cleared: found=%v, %v", key, found, err)
		}
	}
	if cleared, err := store.ClearLoginLocks("用户"); err != nil || cleared != 1 {
		t.Fatalf("clear unicode username = %d, %v", cleared, err)
	}
	if cleared, err := store.ClearLoginLocks("alice"); err != nil || cleared != 0 {
		t.Fatalf("repeat clear = %d, %v", cleared, err)
	}
	if _, err := store.ClearLoginLocks("  "); err == nil {
		t.Fatal("empty username should fail")
	}
}

func TestPruneLoginLocks(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "state.db"))
	if err != nil {
		t.Fatalf("open account store: %v", err)
	}
	defer store.Close()
	base := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	now := base.Add(time.Hour)
	records := map[string]authlimit.Record{
		"idle":    {Failures: 1, WindowStart: base, LastSeen: base},
		"locked":  {WindowStart: base, LockedUntil: now.Add(time.Minute), LastSeen: base},
		"expired": {WindowStart: base, LockedUntil: base.Add(time.Minute), LastSeen: base},
		"recent":  {Failures: 1, WindowStart: now.Add(-time.Minute), LastSeen: now.Add(-time.Minute)},
	}
	for key, record := range records {
		if err := store.SaveLoginLock(key, record); err != nil {
			t.Fatalf("save %s: %v", key, err)
		}
	}
	if err := store.PruneLoginLocks(now.Add(-30*time.Minute), now); err != nil {
		t.Fatalf("prune locks: %v", err)
	}
	for _, key := range []string{"idle", "expired"} {
		if _, found, err := store.LoadLoginLock(key); err != nil || found {
			t.Fatalf("%s survived pruning: found=%v, %v", key, found, err)
		}
	}
	for _, key := range []string{"locked", "recent"} {
		if _, found, err := store.LoadLoginLock(key); err != nil || !found {
			t.Fatalf("%s was pruned: found=%v, %v", key, found, err)
		}
	}
}
