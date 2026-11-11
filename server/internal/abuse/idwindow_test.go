package abuse

import (
	"testing"
	"time"
)

func TestIDWindowBurst(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	w := NewIDWindow(IDWindowConfig{})
	w.SetClock(now)
	for i := 0; i < 3; i++ {
		if ok, retry := w.Allow("devbox"); !ok {
			t.Fatalf("attempt %d refused, retry %s", i+1, retry)
		}
	}
	ok, retry := w.Allow("devbox")
	if ok {
		t.Fatal("fourth attempt inside the window was accepted")
	}
	if retry <= 0 || retry > 6*time.Second {
		t.Fatalf("retry = %s", retry)
	}
	if ok, _ := w.Allow("other"); !ok {
		t.Fatal("another id must not be throttled")
	}
	if stats := w.Stats(); stats.Throttled != 1 || stats.TrackedIDs != 2 {
		t.Fatalf("stats: %+v", stats)
	}
	advance(7 * time.Second)
	if ok, _ := w.Allow("devbox"); !ok {
		t.Fatal("window did not reset after silence")
	}
}

func TestIDWindowSlidingBurstDoesNotReset(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	w := NewIDWindow(IDWindowConfig{Window: 6 * time.Second, Max: 2})
	w.SetClock(now)
	if ok, _ := w.Allow("devbox"); !ok {
		t.Fatal("first attempt refused")
	}
	advance(4 * time.Second)
	if ok, _ := w.Allow("devbox"); !ok {
		t.Fatal("second attempt refused")
	}
	advance(4 * time.Second)
	// Eight seconds have passed since the first attempt, but only four since
	// the last one, so hbbs would still answer TOO_FREQUENT.
	if ok, retry := w.Allow("devbox"); ok || retry != 2*time.Second {
		t.Fatalf("sliding attempt: ok=%v retry=%s", ok, retry)
	}
}

func TestIDWindowCapacityEvictsOldest(t *testing.T) {
	now, advance := clock(time.Unix(1_700_000_000, 0))
	w := NewIDWindow(IDWindowConfig{Window: time.Minute, Max: 1, MaxIDs: 2})
	w.SetClock(now)
	w.Allow("a")
	advance(time.Second)
	w.Allow("b")
	advance(time.Second)
	w.Allow("c") // evicts "a"
	if stats := w.Stats(); stats.TrackedIDs != 2 {
		t.Fatalf("capacity stats: %+v", stats)
	}
	if ok, _ := w.Allow("a"); !ok {
		t.Fatal("evicted id should start a fresh burst")
	}
	if removed := w.Clear(); removed != 2 {
		t.Fatalf("clear removed %d", removed)
	}
	if stats := w.Stats(); stats.TrackedIDs != 0 {
		t.Fatalf("stats after clear: %+v", stats)
	}
}
