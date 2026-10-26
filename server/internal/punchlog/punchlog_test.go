package punchlog

import (
	"testing"
	"time"
)

func clock(start time.Time) (func() time.Time, func(time.Duration)) {
	now := start
	return func() time.Time { return now }, func(d time.Duration) { now = now.Add(d) }
}

func TestRecordOrderAndPagination(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	l := New(10, time.Minute)
	l.SetClock(now)
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	advance(time.Second)
	l.Record("10.0.0.3", "10.0.0.4", "relaybox")
	advance(time.Second)
	l.Record("10.0.0.5", "10.0.0.6", "devbox")

	all := l.Snapshot(0, 10)
	if len(all) != 3 || all[0].ToID != "devbox" || all[1].ToID != "relaybox" || all[2].ToID != "devbox" {
		t.Fatalf("snapshot order: %+v", all)
	}
	page := l.Snapshot(1, 1)
	if len(page) != 1 || page[0].ToID != "relaybox" {
		t.Fatalf("page: %+v", page)
	}
	if got := l.Snapshot(9, 5); got != nil {
		t.Fatalf("out-of-range page: %+v", got)
	}
}

func TestDedupeWindow(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	l := New(10, 60*time.Second)
	l.SetClock(now)
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	advance(30 * time.Second)
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	if stats := l.Stats(); stats.Total != 2 || stats.Deduped != 1 || stats.Stored != 1 {
		t.Fatalf("dedupe stats: %+v", stats)
	}
	advance(31 * time.Second)
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	if stats := l.Stats(); stats.Stored != 2 || stats.Deduped != 1 {
		t.Fatalf("expired dedupe: %+v", stats)
	}
	// A different viewer IP or target ID is a distinct request.
	l.Record("10.0.0.9", "10.0.0.2", "devbox")
	l.Record("10.0.0.1", "10.0.0.2", "relaybox")
	if stats := l.Stats(); stats.Stored != 4 {
		t.Fatalf("distinct requests: %+v", stats)
	}
}

func TestCapacityOverwritesOldest(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	l := New(3, time.Minute)
	l.SetClock(now)
	for i := 0; i < 5; i++ {
		l.Record("10.0.0.1", "10.0.0.2", string(rune('a'+i)))
		advance(time.Second)
	}
	all := l.Snapshot(0, 10)
	if len(all) != 3 {
		t.Fatalf("capacity: %d entries", len(all))
	}
	want := []string{"c", "d", "e"}
	for i, wantID := range want {
		if all[i].ToID != wantID {
			t.Fatalf("entry %d = %q, want %q", i, all[i].ToID, wantID)
		}
	}
	if stats := l.Stats(); stats.Stored != 3 || stats.Total != 5 {
		t.Fatalf("capacity stats: %+v", stats)
	}
}

func TestClearKeepsCounters(t *testing.T) {
	now, _ := clock(time.Unix(1_700_000_000, 0))
	l := New(4, time.Minute)
	l.SetClock(now)
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	l.Clear()
	if got := l.Snapshot(0, 10); got != nil {
		t.Fatalf("cleared log: %+v", got)
	}
	stats := l.Stats()
	if stats.Stored != 0 || stats.Total != 1 {
		t.Fatalf("clear stats: %+v", stats)
	}
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	if got := l.Snapshot(0, 10); len(got) != 1 {
		t.Fatalf("record after clear: %+v", got)
	}
}

func TestDisabledLog(t *testing.T) {
	l := New(0, time.Minute)
	l.Record("10.0.0.1", "10.0.0.2", "devbox")
	if l.Enabled() {
		t.Fatal("log with max 0 should be disabled")
	}
	stats := l.Stats()
	if stats.Total != 0 || stats.Stored != 0 {
		t.Fatalf("disabled stats: %+v", stats)
	}
}
