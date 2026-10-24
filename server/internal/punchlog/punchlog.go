// Package punchlog keeps a bounded, de-duplicated record of coordinated punch
// requests. hbbs exposes the same information through its loopback console; the
// ring is deliberately small and lossy so auditing never pressures the data
// plane.
package punchlog

import (
	"sync"
	"time"
)

const dedupeScan = 30

type Entry struct {
	Time   time.Time
	FromIP string
	ToIP   string
	ToID   string
}

type Stats struct {
	// Total counts Record calls that were eligible for logging.
	Total uint64 `json:"total"`
	// Deduped counts requests suppressed by the 60 s same-peer rule.
	Deduped uint64 `json:"deduped"`
	// Stored is the number of entries currently retained.
	Stored int `json:"stored"`
}

type Log struct {
	mu     sync.Mutex
	max    int
	dedupe time.Duration
	now    func() time.Time

	entries []Entry
	head    int
	full    bool
	total   uint64
	deduped uint64
}

// New creates a log that retains at most max entries. max <= 0 disables it;
// dedupe <= 0 falls back to 60 s.
func New(max int, dedupe time.Duration) *Log {
	if dedupe <= 0 {
		dedupe = 60 * time.Second
	}
	return &Log{max: max, dedupe: dedupe, now: time.Now}
}

// SetClock is used by tests to control dedupe expiry.
func (l *Log) SetClock(now func() time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.now = now
}

func (l *Log) Enabled() bool { return l.max > 0 }

func (l *Log) Record(fromIP, toIP, id string) {
	if l.max <= 0 {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.now()
	l.total++

	n := len(l.entries)
	scan := dedupeScan
	if n < scan {
		scan = n
	}
	for i := 1; i <= scan; i++ {
		e := l.at(n - i)
		if e.FromIP == fromIP && e.ToID == id {
			if now.Sub(e.Time) < l.dedupe {
				l.deduped++
				return
			}
			// Only the newest matching request can suppress a repeat; the
			// scan stops here exactly like hbbs's short tail scan.
			break
		}
	}

	entry := Entry{Time: now, FromIP: fromIP, ToIP: toIP, ToID: id}
	switch {
	case !l.full && len(l.entries) < l.max:
		l.entries = append(l.entries, entry)
		if len(l.entries) == l.max {
			l.full = true
		}
	case l.max > 0:
		l.entries[l.head] = entry
		l.head = (l.head + 1) % l.max
	}
}

// Snapshot returns entries in chronological order, oldest first.
func (l *Log) Snapshot(start, count int) []Entry {
	l.mu.Lock()
	defer l.mu.Unlock()
	n := len(l.entries)
	if start < 0 {
		start = 0
	}
	if count <= 0 || start >= n {
		return nil
	}
	end := start + count
	if end > n {
		end = n
	}
	out := make([]Entry, 0, end-start)
	for i := start; i < end; i++ {
		out = append(out, l.at(i))
	}
	return out
}

// Clear empties the retained entries but keeps the monotonic counters.
func (l *Log) Clear() {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.entries = nil
	l.head = 0
	l.full = false
}

func (l *Log) Stats() Stats {
	l.mu.Lock()
	defer l.mu.Unlock()
	return Stats{Total: l.total, Deduped: l.deduped, Stored: len(l.entries)}
}

func (l *Log) at(i int) Entry {
	if l.full {
		return l.entries[(l.head+i)%l.max]
	}
	return l.entries[i]
}
