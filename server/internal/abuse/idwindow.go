package abuse

import (
	"sync"
	"time"
)

// IDWindow mirrors hbbs's per-peer reg_pk pacing: a signed registration for one
// host id is accepted a few times in a short burst, then refused until the
// burst goes quiet for the window. hbbs answers TOO_FREQUENT; the HTTP API
// answers 429 with Retry-After. Unlike Blocker it keys on the id rather than
// the source address, so rotating addresses cannot churn a single id.
type IDWindowConfig struct {
	// Window is the silence after which the counter resets; zero uses 6s like
	// hbbs.
	Window time.Duration
	// Max is how many registrations are accepted back to back; zero uses 3,
	// matching hbbs (its count > 2 check refuses the fourth attempt). Max <= 0
	// after defaults is impossible; use the API wiring to disable the check.
	Max int
	// MaxIDs caps tracked ids; zero uses 65536.
	MaxIDs int
	Now    func() time.Time
}

type idEntry struct {
	count int
	last  time.Time
}

type IDWindowStats struct {
	TrackedIDs int    `json:"tracked_ids"`
	Throttled  uint64 `json:"throttled"`
}

type IDWindow struct {
	mu        sync.Mutex
	window    time.Duration
	max       int
	maxIDs    int
	now       func() time.Time
	ids       map[string]*idEntry
	lastSweep time.Time
	throttled uint64
}

func NewIDWindow(cfg IDWindowConfig) *IDWindow {
	if cfg.Window <= 0 {
		cfg.Window = 6 * time.Second
	}
	if cfg.Max <= 0 {
		cfg.Max = 3
	}
	if cfg.MaxIDs <= 0 {
		cfg.MaxIDs = 65536
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &IDWindow{
		window: cfg.Window,
		max:    cfg.Max,
		maxIDs: cfg.MaxIDs,
		now:    cfg.Now,
		ids:    make(map[string]*idEntry),
	}
}

// SetClock is used by tests to advance the burst window.
func (w *IDWindow) SetClock(now func() time.Time) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.now = now
}

// Allow records a signed registration for id and reports whether it may
// proceed. A refusal also returns how long the caller should wait before the
// next attempt; accepted attempts return zero.
func (w *IDWindow) Allow(id string) (bool, time.Duration) {
	w.mu.Lock()
	defer w.mu.Unlock()
	now := w.now()
	w.sweepLocked(now)
	st, ok := w.ids[id]
	if !ok {
		if len(w.ids) >= w.maxIDs {
			w.evictOldestLocked()
		}
		st = &idEntry{}
		w.ids[id] = st
	}
	if !st.last.IsZero() && now.Sub(st.last) > w.window {
		st.count = 0
	}
	if st.count >= w.max {
		w.throttled++
		retry := w.window - now.Sub(st.last)
		if retry < time.Second {
			retry = time.Second
		}
		return false, retry
	}
	st.count++
	st.last = now
	return true, 0
}

func (w *IDWindow) Stats() IDWindowStats {
	w.mu.Lock()
	defer w.mu.Unlock()
	return IDWindowStats{TrackedIDs: len(w.ids), Throttled: w.throttled}
}

// Clear forgets every tracked id and returns how many were dropped. The
// throttled counter stays monotonic.
func (w *IDWindow) Clear() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	removed := len(w.ids)
	w.ids = make(map[string]*idEntry)
	return removed
}

func (w *IDWindow) sweepLocked(now time.Time) {
	if !w.lastSweep.IsZero() && now.Sub(w.lastSweep) < w.window {
		return
	}
	w.lastSweep = now
	for id, st := range w.ids {
		if now.Sub(st.last) > w.window {
			delete(w.ids, id)
		}
	}
}

func (w *IDWindow) evictOldestLocked() {
	var oldestID string
	var oldest time.Time
	for id, st := range w.ids {
		if oldestID == "" || st.last.Before(oldest) {
			oldestID, oldest = id, st.last
		}
	}
	if oldestID != "" {
		delete(w.ids, oldestID)
	}
}
