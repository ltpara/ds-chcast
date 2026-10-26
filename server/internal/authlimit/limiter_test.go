package authlimit

import (
	"testing"
	"time"
)

func newTestLimiter(now *time.Time) *Limiter {
	return New(Config{
		Window:      10 * time.Minute,
		MaxFailures: 3,
		Lockout:     time.Minute,
		Now:         func() time.Time { return *now },
	})
}

func TestFailureBudgetLocksAndExpires(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	l := newTestLimiter(&now)

	for i := 0; i < 2; i++ {
		if retry, ok := l.Failure("alice|203.0.113.5"); !ok || retry != 0 {
			t.Fatalf("failure %d = %v, %v; want allowed", i+1, retry, ok)
		}
	}
	if retry, ok := l.Failure("alice|203.0.113.5"); ok || retry != time.Minute {
		t.Fatalf("third failure = %v, %v; want a minute lock", retry, ok)
	}
	if retry, ok := l.Allow("alice|203.0.113.5"); ok || retry != time.Minute {
		t.Fatalf("while locked Allow = %v, %v; want a minute lock", retry, ok)
	}
	if retry, ok := l.Allow("bob|203.0.113.5"); !ok || retry != 0 {
		t.Fatalf("other key Allow = %v, %v; want allowed", retry, ok)
	}

	now = now.Add(59 * time.Second)
	if retry, ok := l.Allow("alice|203.0.113.5"); ok || retry != time.Second {
		t.Fatalf("just before expiry Allow = %v, %v; want 1s", retry, ok)
	}
	now = now.Add(2 * time.Second)
	if retry, ok := l.Allow("alice|203.0.113.5"); !ok || retry != 0 {
		t.Fatalf("after expiry Allow = %v, %v; want allowed", retry, ok)
	}
	// The expired lockout resets the budget: the next attempts start over.
	for i := 0; i < 2; i++ {
		if _, ok := l.Failure("alice|203.0.113.5"); !ok {
			t.Fatalf("failure %d after expiry locked too early", i+1)
		}
	}
}

func TestSuccessClearsFailures(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	l := newTestLimiter(&now)
	l.Failure("alice|ip")
	l.Failure("alice|ip")
	l.Success("alice|ip")
	if _, ok := l.Failure("alice|ip"); !ok {
		t.Fatal("success did not clear the failure budget")
	}
	if _, ok := l.Failure("alice|ip"); !ok {
		t.Fatal("second failure after a success locked too early")
	}
}

func TestWindowForgetsOldFailures(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	l := newTestLimiter(&now)
	l.Failure("alice|ip")
	l.Failure("alice|ip")
	now = now.Add(11 * time.Minute)
	if _, ok := l.Failure("alice|ip"); !ok {
		t.Fatal("failure outside the window counted toward the lockout")
	}
	if _, ok := l.Failure("alice|ip"); !ok {
		t.Fatal("second failure inside the new window locked too early")
	}
	if _, ok := l.Failure("alice|ip"); ok {
		t.Fatal("third failure inside the new window should lock")
	}
}

func TestStatsAndSweep(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	l := newTestLimiter(&now)
	l.Failure("a")
	l.Failure("b")
	l.Failure("b")
	l.Failure("b")
	stats := l.Stats()
	if stats.Tracked != 2 || stats.Locked != 1 || stats.Failures != 4 || stats.Lockouts != 1 {
		t.Fatalf("stats = %+v", stats)
	}
	now = now.Add(idleTTL + sweepAge)
	if retry, ok := l.Allow("a"); !ok || retry != 0 {
		t.Fatalf("old key Allow = %v, %v; want allowed", retry, ok)
	}
	if got := l.Stats().Tracked; got != 0 {
		t.Fatalf("tracked keys after sweep = %d, want 0", got)
	}
}

func TestMaxKeysEvictsOldest(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	l := New(Config{
		Window:      time.Minute,
		MaxFailures: 5,
		Lockout:     time.Minute,
		MaxKeys:     2,
		Now:         func() time.Time { return now },
	})
	l.Failure("a")
	now = now.Add(time.Second)
	l.Failure("b")
	now = now.Add(time.Second)
	l.Failure("c")
	if got := l.Stats().Tracked; got != 2 {
		t.Fatalf("tracked keys = %d, want 2", got)
	}
}

func TestDisabledLimiterIsPermissive(t *testing.T) {
	var nilLimiter *Limiter
	if _, ok := nilLimiter.Allow("key"); !ok {
		t.Fatal("nil limiter refused a key")
	}
	if _, ok := nilLimiter.Failure("key"); !ok {
		t.Fatal("nil limiter locked on failure")
	}
	nilLimiter.Success("key")

	off := New(Config{MaxFailures: 0})
	for i := 0; i < 10; i++ {
		if _, ok := off.Failure("key"); !ok {
			t.Fatal("disabled limiter locked on failure")
		}
	}
	if _, ok := off.Allow("key"); !ok {
		t.Fatal("disabled limiter refused a key")
	}
}

func TestSetClock(t *testing.T) {
	now := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	l := New(Config{Window: time.Minute, MaxFailures: 1, Lockout: time.Minute})
	l.SetClock(func() time.Time { return now })
	if _, ok := l.Failure("key"); ok {
		t.Fatal("first failure should lock with MaxFailures 1")
	}
	now = now.Add(61 * time.Second)
	if _, ok := l.Allow("key"); !ok {
		t.Fatal("lock did not expire after SetClock advanced")
	}
}
