package acl

import (
	"sync"
	"time"
)

// ReplayGuard remembers ticket ids until their expiry. It never evicts a live
// entry to make room: when the bounded cache is full, new admissions are
// refused instead of silently allowing a replay.
type ReplayGuard struct {
	mu       sync.Mutex
	entries  map[string]int64
	capacity int
}

func NewReplayGuard(capacity int) *ReplayGuard {
	if capacity < 1 {
		capacity = 1
	}
	return &ReplayGuard{entries: make(map[string]int64), capacity: capacity}
}

// Consume records id and reports whether it was unseen. The entry lives until
// the ticket expiry plus the allowed clock skew, so a replay after expiry is
// rejected by ticket validation rather than by eviction.
func (g *ReplayGuard) Consume(id string, expiresAt int64, now time.Time) bool {
	nowUnix := now.Unix()
	g.mu.Lock()
	defer g.mu.Unlock()
	for key, expires := range g.entries {
		if expires <= nowUnix {
			delete(g.entries, key)
		}
	}
	if _, seen := g.entries[id]; seen {
		return false
	}
	if len(g.entries) >= g.capacity {
		return false
	}
	expiry := expiresAt + int64(ClockSkew/time.Second)
	if expiry <= nowUnix {
		expiry = nowUnix + 1
	}
	g.entries[id] = expiry
	return true
}

// Len reports the number of live entries; tests and metrics use it.
func (g *ReplayGuard) Len() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	return len(g.entries)
}
