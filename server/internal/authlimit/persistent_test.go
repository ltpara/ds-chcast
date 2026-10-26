package authlimit

import (
	"errors"
	"testing"
	"time"
)

type fakeBackend struct {
	records map[string]Record

	loadErr, saveErr, deleteErr, pruneErr error
	loads, saves, deletes, prunes         int
	prunedSeen, prunedLocked              time.Time
}

func newFakeBackend() *fakeBackend {
	return &fakeBackend{records: make(map[string]Record)}
}

func (f *fakeBackend) LoadLoginLock(key string) (Record, bool, error) {
	f.loads++
	if f.loadErr != nil {
		return Record{}, false, f.loadErr
	}
	record, ok := f.records[key]
	return record, ok, nil
}

func (f *fakeBackend) SaveLoginLock(key string, record Record) error {
	f.saves++
	if f.saveErr != nil {
		return f.saveErr
	}
	f.records[key] = record
	return nil
}

func (f *fakeBackend) DeleteLoginLock(key string) error {
	f.deletes++
	if f.deleteErr != nil {
		return f.deleteErr
	}
	delete(f.records, key)
	return nil
}

func (f *fakeBackend) PruneLoginLocks(seenBefore, lockedBefore time.Time) error {
	f.prunes++
	f.prunedSeen, f.prunedLocked = seenBefore, lockedBefore
	return f.pruneErr
}

type fakeAtomicBackend struct {
	*fakeBackend
	updates   int
	updateErr error
}

func newFakeAtomicBackend() *fakeAtomicBackend {
	return &fakeAtomicBackend{fakeBackend: newFakeBackend()}
}

func (f *fakeAtomicBackend) UpdateLoginLock(key string, update func(Record, bool) Record) (Record, error) {
	f.updates++
	if f.updateErr != nil {
		return Record{}, f.updateErr
	}
	record, found := f.records[key]
	record = update(record, found)
	f.records[key] = record
	return record, nil
}

func persistentConfig(now *time.Time) Config {
	return Config{
		Window:      10 * time.Minute,
		MaxFailures: 3,
		Lockout:     5 * time.Minute,
		Now:         func() time.Time { return *now },
	}
}

func TestAtomicBackendRecordsFailures(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	backend := newFakeAtomicBackend()
	limiter := NewPersistent(persistentConfig(&now), backend, nil)
	key := "alice|10.0.0.1"
	for i := 0; i < 2; i++ {
		if retry, ok := limiter.Failure(key); !ok {
			t.Fatalf("failure %d locked early: %v", i+1, retry)
		}
	}
	if retry, ok := limiter.Failure(key); ok || retry != 5*time.Minute {
		t.Fatalf("third failure = (%v, %v), want (5m, false)", retry, ok)
	}
	if backend.updates != 3 {
		t.Fatalf("atomic updates = %d, want 3", backend.updates)
	}
	if backend.loads != 0 || backend.saves != 0 {
		t.Fatalf("atomic path used read-modify-write: loads=%d saves=%d",
			backend.loads, backend.saves)
	}
	if retry, ok := limiter.Failure(key); ok || retry != 5*time.Minute {
		t.Fatalf("failure while locked = (%v, %v), want (5m, false)", retry, ok)
	}
	if stats := limiter.Stats(); stats.Failures != 3 || stats.Lockouts != 1 {
		t.Fatalf("stats = %+v, want 3 failures and 1 lockout", stats)
	}

	now = now.Add(5*time.Minute + time.Second)
	if retry, ok := limiter.Failure(key); !ok {
		t.Fatalf("failure after lock expiry = (%v, %v), want allowed", retry, ok)
	}
	record := backend.records[key]
	if record.Failures != 1 || !record.LockedUntil.IsZero() {
		t.Fatalf("record after expiry = %+v, want one fresh failure", record)
	}
}

func TestAtomicBackendErrorsFailOpen(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	backend := newFakeAtomicBackend()
	backend.updateErr = errors.New("database is locked")
	reports := 0
	limiter := NewPersistent(persistentConfig(&now), backend,
		func(error) { reports++ })
	if retry, ok := limiter.Failure("alice|10.0.0.1"); !ok {
		t.Fatalf("atomic error blocked failure: %v", retry)
	}
	if reports != 1 {
		t.Fatalf("atomic errors reported %d times, want 1", reports)
	}
	if _, found := backend.records["alice|10.0.0.1"]; found {
		t.Fatal("failed atomic update wrote a record")
	}
}

func TestPersistentLockoutSurvivesRestart(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	backend := newFakeBackend()
	first := NewPersistent(persistentConfig(&now), backend, nil)
	for i := 0; i < 2; i++ {
		if retry, ok := first.Failure("alice|10.0.0.1"); !ok {
			t.Fatalf("failure %d locked early: %v", i+1, retry)
		}
	}

	// A new limiter over the same backend behaves like a restarted server.
	now = now.Add(time.Minute)
	restarted := NewPersistent(persistentConfig(&now), backend, nil)
	if retry, ok := restarted.Allow("alice|10.0.0.1"); !ok {
		t.Fatalf("two persisted failures locked early: %v", retry)
	}
	retry, ok := restarted.Failure("alice|10.0.0.1")
	if ok || retry != 5*time.Minute {
		t.Fatalf("third failure = (%v, %v), want (5m, false)", retry, ok)
	}
	if retry, ok := restarted.Allow("alice|10.0.0.1"); ok || retry != 5*time.Minute {
		t.Fatalf("allow while locked = (%v, %v), want (5m, false)", retry, ok)
	}
	if retry, ok := restarted.Allow("bob|10.0.0.1"); !ok {
		t.Fatalf("other key locked: %v", retry)
	}

	now = now.Add(5*time.Minute + time.Second)
	if retry, ok := restarted.Allow("alice|10.0.0.1"); !ok {
		t.Fatalf("allow after lockout expiry: %v", retry)
	}
	if retry, ok := restarted.Failure("alice|10.0.0.1"); !ok {
		t.Fatalf("first failure after expiry locked: %v", retry)
	}
}

func TestPersistentSuccessClearsFailures(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	backend := newFakeBackend()
	first := NewPersistent(persistentConfig(&now), backend, nil)
	first.Failure("alice|10.0.0.1")
	first.Failure("alice|10.0.0.1")
	first.Success("alice|10.0.0.1")
	if _, found := backend.records["alice|10.0.0.1"]; found {
		t.Fatalf("success left persisted failures: %+v", backend.records)
	}

	restarted := NewPersistent(persistentConfig(&now), backend, nil)
	for i := 0; i < 2; i++ {
		if retry, ok := restarted.Failure("alice|10.0.0.1"); !ok {
			t.Fatalf("failure %d after success locked: %v", i+1, retry)
		}
	}
	if retry, ok := restarted.Failure("alice|10.0.0.1"); ok || retry != 5*time.Minute {
		t.Fatalf("third failure after success = (%v, %v)", retry, ok)
	}
}

func TestPersistentBackendErrorsFailOpen(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	backend := newFakeBackend()
	backend.loadErr = errors.New("database is locked")
	reports := 0
	limiter := NewPersistent(persistentConfig(&now), backend,
		func(error) { reports++ })

	if retry, ok := limiter.Allow("alice|10.0.0.1"); !ok {
		t.Fatalf("load error blocked allow: %v", retry)
	}
	if retry, ok := limiter.Failure("alice|10.0.0.1"); !ok {
		t.Fatalf("load error blocked failure: %v", retry)
	}
	if reports != 2 {
		t.Fatalf("load errors reported %d times, want 2", reports)
	}

	backend.loadErr = nil
	backend.deleteErr = errors.New("disk full")
	limiter.Success("alice|10.0.0.1")
	if reports != 3 {
		t.Fatalf("delete error reported %d times, want 3", reports)
	}

	backend.deleteErr = nil
	backend.saveErr = errors.New("disk full")
	if retry, ok := limiter.Failure("alice|10.0.0.1"); !ok {
		t.Fatalf("save error blocked failure: %v", retry)
	}
	if reports != 4 {
		t.Fatalf("save error reported %d times, want 4", reports)
	}
}

func TestPersistentPrunesIdleState(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	backend := newFakeBackend()
	limiter := NewPersistent(persistentConfig(&now), backend, nil)
	limiter.Failure("alice|10.0.0.1")
	if backend.prunes != 0 {
		t.Fatalf("pruned before the sweep interval: %d", backend.prunes)
	}

	now = now.Add(sweepAge)
	limiter.Allow("alice|10.0.0.1")
	if backend.prunes != 1 {
		t.Fatalf("prunes = %d, want 1", backend.prunes)
	}
	if want := now.Add(-idleTTL); !backend.prunedSeen.Equal(want) {
		t.Fatalf("pruned seen-before = %v, want %v", backend.prunedSeen, want)
	}
	if !backend.prunedLocked.Equal(now) {
		t.Fatalf("pruned locked-before = %v, want %v", backend.prunedLocked, now)
	}
}
