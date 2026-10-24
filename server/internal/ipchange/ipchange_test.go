package ipchange

import (
	"reflect"
	"sync"
	"testing"
	"time"
)

func newTestTracker(max int) (*Tracker, *time.Time) {
	now := time.Unix(1700000000, 0)
	return NewWithConfig(Config{MaxIDs: max, Now: func() time.Time { return now }}), &now
}

func TestObserveWindowAndPrune(t *testing.T) {
	tracker, now := newTestTracker(0)
	if !tracker.Observe("host", "") {
		t.Log("empty address ignored")
	}
	if tracker.Observe("host", "") {
		t.Fatal("empty address recorded a change")
	}
	if tracker.Observe("host", "192.0.2.1") {
		t.Fatal("first observation must seed the baseline")
	}
	if tracker.Observe("host", "192.0.2.1") {
		t.Fatal("unchanged address recorded a change")
	}
	if !tracker.Observe("host", "192.0.2.2") {
		t.Fatal("second address was not recorded")
	}
	entries := tracker.Snapshot(0, 10)
	if len(entries) != 1 {
		t.Fatalf("first change should be visible, got %+v", entries)
	}
	entry := entries[0]
	if entry.ID != "host" || entry.Total != 2 {
		t.Fatalf("unexpected entry: %+v", entry)
	}
	wantIPs := []IPCount{{IP: "192.0.2.1", Count: 1}, {IP: "192.0.2.2", Count: 1}}
	if !reflect.DeepEqual(entry.IPs, wantIPs) {
		t.Fatalf("ips = %+v, want %+v", entry.IPs, wantIPs)
	}
	if stats := tracker.Stats(); stats.Tracked != 1 || stats.Changes != 1 {
		t.Fatalf("unexpected stats after one change: %+v", stats)
	}

	if !tracker.Observe("host", "192.0.2.3") {
		t.Fatal("third address was not recorded")
	}
	entries = tracker.Snapshot(0, 10)
	if len(entries) != 1 || entries[0].Total != 3 {
		t.Fatalf("want three addresses in the window, got %+v", entries)
	}
	wantIPs = []IPCount{{IP: "192.0.2.1", Count: 1}, {IP: "192.0.2.2", Count: 1}, {IP: "192.0.2.3", Count: 1}}
	if !reflect.DeepEqual(entries[0].IPs, wantIPs) {
		t.Fatalf("ips = %+v, want %+v", entries[0].IPs, wantIPs)
	}

	*now = now.Add(179 * time.Second)
	if got := tracker.Snapshot(0, 10); len(got) != 1 || got[0].Age != 179*time.Second {
		t.Fatalf("entry should survive inside the window: %+v", got)
	}
	*now = now.Add(2 * time.Second)
	if got := tracker.Snapshot(0, 10); len(got) != 1 {
		t.Fatal("entry should survive until 360s")
	}
	if !tracker.Observe("host", "192.0.2.4") {
		t.Fatal("change after the window was not recorded")
	}
	if got := tracker.Snapshot(0, 10); len(got) != 0 {
		t.Fatalf("window reset should leave a single-address entry: %+v", got)
	}
	if !tracker.Observe("host", "192.0.2.5") {
		t.Fatal("change after reset was not recorded")
	}
	entries = tracker.Snapshot(0, 10)
	if len(entries) != 1 || entries[0].Total != 2 {
		t.Fatalf("want the reset window to collect two addresses: %+v", entries)
	}
}

func TestPruneByRetain(t *testing.T) {
	tracker, now := newTestTracker(0)
	tracker.Observe("host", "192.0.2.1")
	tracker.Observe("host", "192.0.2.2")
	tracker.Observe("host", "192.0.2.3")
	*now = now.Add(359 * time.Second)
	if got := tracker.Snapshot(0, 10); len(got) != 1 {
		t.Fatalf("entry pruned before 360s: %+v", got)
	}
	*now = now.Add(time.Second)
	if got := tracker.Snapshot(0, 10); len(got) != 0 {
		t.Fatalf("entry survived to 360s: %+v", got)
	}
}

func TestRemoveAndClearResetBaseline(t *testing.T) {
	tracker, _ := newTestTracker(0)
	tracker.Observe("host", "192.0.2.1")
	tracker.Observe("host", "192.0.2.2")
	tracker.Observe("host", "192.0.2.3")
	if !tracker.Remove("host") {
		t.Fatal("Remove reported an unknown entry")
	}
	if _, ok := tracker.Lookup("host"); ok {
		t.Fatal("entry survived Remove")
	}
	if tracker.Remove("host") {
		t.Fatal("second Remove reported an entry")
	}
	if tracker.Observe("host", "192.0.2.3") {
		t.Fatal("Remove should forget the baseline")
	}
	if !tracker.Observe("host", "192.0.2.4") {
		t.Fatal("first address after Remove was not treated as a change")
	}
	if !tracker.Observe("host", "192.0.2.5") {
		t.Fatal("second address after Remove was not recorded")
	}
	if got := tracker.Snapshot(0, 10); len(got) != 1 {
		t.Fatalf("want a fresh entry, got %+v", got)
	}
	stats := tracker.Stats()
	if stats.Changes != 4 {
		t.Fatalf("changes = %d, want 4", stats.Changes)
	}
	if removed := tracker.Clear(); removed != 1 {
		t.Fatalf("Clear removed %d entries, want 1", removed)
	}
	if got := tracker.Stats(); got.Tracked != 0 || got.Changes != 4 {
		t.Fatalf("unexpected stats after Clear: %+v", got)
	}
	if tracker.Observe("host", "192.0.2.5") {
		t.Fatal("Clear should forget the baseline")
	}
}

func TestSnapshotPagination(t *testing.T) {
	tracker, _ := newTestTracker(0)
	for _, id := range []string{"zulu", "alpha", "mike"} {
		tracker.Observe(id, "192.0.2.1")
		tracker.Observe(id, "192.0.2.2")
		tracker.Observe(id, "192.0.2.3")
	}
	first := tracker.Snapshot(0, 2)
	if len(first) != 2 || first[0].ID != "alpha" || first[1].ID != "mike" {
		t.Fatalf("page 1 = %+v", first)
	}
	second := tracker.Snapshot(2, 2)
	if len(second) != 1 || second[0].ID != "zulu" {
		t.Fatalf("page 2 = %+v", second)
	}
	if got := tracker.Snapshot(3, 2); len(got) != 0 {
		t.Fatalf("page past the end = %+v", got)
	}
	if got := tracker.Snapshot(-1, 2); len(got) != 0 {
		t.Fatalf("negative start = %+v", got)
	}
	if got := tracker.Snapshot(0, 0); len(got) != 0 {
		t.Fatalf("zero count = %+v", got)
	}
}

func TestBaselineCapacityEvictsOldest(t *testing.T) {
	tracker, now := newTestTracker(2)
	tracker.Observe("a", "192.0.2.1")
	*now = now.Add(time.Second)
	tracker.Observe("b", "192.0.2.1")
	*now = now.Add(time.Second)
	tracker.Observe("c", "192.0.2.1")
	*now = now.Add(time.Second)
	if tracker.Observe("a", "192.0.2.2") {
		t.Fatal("evicted baseline should be seeded again")
	}
	if !tracker.Observe("a", "192.0.2.3") {
		t.Fatal("change after reseeding was not recorded")
	}
}

func TestConcurrentAccess(t *testing.T) {
	tracker, _ := newTestTracker(0)
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			ip := "192.0.2.1"
			for n := 0; n < 100; n++ {
				if n%2 == 0 {
					ip = "192.0.2.2"
				} else {
					ip = "192.0.2.1"
				}
				tracker.Observe("host", ip)
				tracker.Snapshot(0, 10)
				tracker.Lookup("host")
				tracker.Stats()
			}
		}(i)
	}
	wg.Wait()
	if stats := tracker.Stats(); stats.Changes == 0 {
		t.Fatal("concurrent observations recorded no changes")
	}
}
