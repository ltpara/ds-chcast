package abuse

import (
	"testing"
	"time"
)

func clock(start time.Time) (func() time.Time, func(time.Duration)) {
	now := start
	return func() time.Time { return now }, func(d time.Duration) { now = now.Add(d) }
}

func newTestBlocker(now func() time.Time) *Blocker {
	b := New(Config{Window: time.Minute, MaxPerWindow: 30, IDWindow: 24 * time.Hour, MaxIDs: 300})
	b.SetClock(now)
	return b
}

func TestRateWindow(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	b := newTestBlocker(now)
	for i := 0; i < 30; i++ {
		if ok, reason := b.Allow("10.0.0.1", "devbox"); !ok {
			t.Fatalf("attempt %d denied: %s", i+1, reason)
		}
	}
	if ok, reason := b.Allow("10.0.0.1", "devbox"); ok || reason != "rate" {
		t.Fatalf("31st attempt: ok=%v reason=%q", ok, reason)
	}
	if ok, _ := b.Allow("10.0.0.2", "devbox"); !ok {
		t.Fatal("another IP must not be affected")
	}
	advance(61 * time.Second)
	if ok, reason := b.Allow("10.0.0.1", "devbox"); !ok {
		t.Fatalf("window did not roll over: %s", reason)
	}
	stats := b.Stats()
	if stats.RateBlocked != 1 || stats.TrackedIPs != 2 {
		t.Fatalf("stats: %+v", stats)
	}
}

func TestDistinctIDFlood(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	b := New(Config{Window: time.Minute, MaxPerWindow: 1000, IDWindow: time.Hour, MaxIDs: 2})
	b.SetClock(now)
	if ok, _ := b.Allow("10.0.0.1", "id-a"); !ok {
		t.Fatal("first id denied")
	}
	if ok, _ := b.Allow("10.0.0.1", "id-b"); !ok {
		t.Fatal("second id denied")
	}
	if ok, reason := b.Allow("10.0.0.1", "id-c"); ok || reason != "ids" {
		t.Fatalf("new id after limit: ok=%v reason=%q", ok, reason)
	}
	if ok, reason := b.Allow("10.0.0.1", "id-a"); !ok {
		t.Fatalf("known id renew denied: %s", reason)
	}
	advance(61 * time.Minute)
	if ok, reason := b.Allow("10.0.0.1", "id-c"); !ok {
		t.Fatalf("id window did not roll over: %s", reason)
	}
	stats := b.Stats()
	if stats.IDBlocked != 1 {
		t.Fatalf("stats: %+v", stats)
	}
}

func TestEntriesAndClear(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	b := newTestBlocker(now)
	b.Allow("10.0.0.2", "a")
	advance(time.Second)
	b.Allow("10.0.0.1", "a")
	b.Allow("10.0.0.1", "b")

	entries := b.Entries(0, 10)
	if len(entries) != 2 || entries[0].IP != "10.0.0.1" || entries[0].WindowCount != 2 || entries[0].IDs != 2 {
		t.Fatalf("entries: %+v", entries)
	}
	if page := b.Entries(1, 1); len(page) != 1 || page[0].IP != "10.0.0.2" {
		t.Fatalf("entry page: %+v", page)
	}
	if e, ok := b.IPEntry("10.0.0.2"); !ok || e.WindowCount != 1 {
		t.Fatalf("single entry: %+v ok=%v", e, ok)
	}
	if _, ok := b.IPEntry("192.0.2.1"); ok {
		t.Fatal("unknown IP reported")
	}
	if removed := b.Clear(); removed != 2 {
		t.Fatalf("clear removed %d", removed)
	}
	if stats := b.Stats(); stats.TrackedIPs != 0 {
		t.Fatalf("stats after clear: %+v", stats)
	}
}

func TestCapacityEvictsOldest(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	b := New(Config{Window: time.Minute, MaxPerWindow: 30, IDWindow: time.Hour, MaxIDs: 10, MaxIPs: 2})
	b.SetClock(now)
	b.Allow("10.0.0.1", "a")
	advance(time.Second)
	b.Allow("10.0.0.2", "a")
	advance(time.Second)
	b.Allow("10.0.0.3", "a") // evicts 10.0.0.1
	if _, ok := b.IPEntry("10.0.0.1"); ok {
		t.Fatal("oldest IP was not evicted")
	}
	if stats := b.Stats(); stats.TrackedIPs != 2 {
		t.Fatalf("capacity stats: %+v", stats)
	}
}

func TestSweepDropsExpiredState(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	b := New(Config{Window: time.Minute, MaxPerWindow: 30, IDWindow: time.Hour, MaxIDs: 10})
	b.SetClock(now)
	b.Allow("10.0.0.1", "a")
	advance(2 * time.Hour)
	b.Allow("10.0.0.2", "b")
	if _, ok := b.IPEntry("10.0.0.1"); ok {
		t.Fatal("expired state survived the sweep")
	}
}
