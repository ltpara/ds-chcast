// Package ipchange tracks short-lived public IP changes per host, mirroring
// hbbs's IP_CHANGES audit: only peers that showed more than one address within
// the change window are listed, and entries age out quickly.
package ipchange

import (
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	DefaultWindow = 180 * time.Second
	DefaultRetain = 360 * time.Second
	DefaultMaxIDs = 65536
)

type Config struct {
	// Window is how long addresses are accumulated before the entry resets to
	// the newest address.
	Window time.Duration
	// Retain is when an entry is pruned regardless of its addresses.
	Retain time.Duration
	// MaxIDs caps the baseline table and the audit table independently.
	MaxIDs int
	Now    func() time.Time
}

type Stats struct {
	// Tracked is the number of audit entries currently retained.
	Tracked int `json:"tracked"`
	// Changes counts observed address changes since start. Clearing the audit
	// keeps this counter, matching the punch log's lifetime total.
	Changes uint64 `json:"changes"`
}

type IPCount struct {
	IP    string `json:"ip"`
	Count uint64 `json:"count"`
}

type Entry struct {
	ID    string
	Age   time.Duration
	IPs   []IPCount
	Total uint64
}

type observation struct {
	ip string
	at time.Time
}

type changeEntry struct {
	windowStart time.Time
	ips         map[string]uint64
}

type Tracker struct {
	mu      sync.Mutex
	window  time.Duration
	retain  time.Duration
	maxIDs  int
	now     func() time.Time
	last    map[string]observation
	entries map[string]*changeEntry
	changes uint64
}

func New(max int) *Tracker {
	return NewWithConfig(Config{MaxIDs: max})
}

func NewWithConfig(cfg Config) *Tracker {
	if cfg.Window <= 0 {
		cfg.Window = DefaultWindow
	}
	if cfg.Retain <= 0 {
		cfg.Retain = DefaultRetain
	}
	if cfg.Retain < cfg.Window {
		cfg.Retain = cfg.Window * 2
	}
	if cfg.MaxIDs <= 0 {
		cfg.MaxIDs = DefaultMaxIDs
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &Tracker{
		window:  cfg.Window,
		retain:  cfg.Retain,
		maxIDs:  cfg.MaxIDs,
		now:     cfg.Now,
		last:    make(map[string]observation),
		entries: make(map[string]*changeEntry),
	}
}

func (t *Tracker) SetClock(now func() time.Time) {
	if now == nil {
		return
	}
	t.mu.Lock()
	t.now = now
	t.mu.Unlock()
}

// Observe records an authenticated observation of id at ip. It reports whether
// the address differs from the previous observation. The first observation
// only seeds the baseline.
func (t *Tracker) Observe(id, ip string) bool {
	id = strings.TrimSpace(id)
	ip = strings.TrimSpace(ip)
	if id == "" || ip == "" {
		return false
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	now := t.now()
	previous, seen := t.last[id]
	if seen && previous.ip == ip {
		t.last[id] = observation{ip: ip, at: now}
		return false
	}
	if !seen && len(t.last) >= t.maxIDs {
		t.evictOldestObservationLocked()
	}
	t.last[id] = observation{ip: ip, at: now}
	if !seen {
		return false
	}
	t.changes++
	entry, ok := t.entries[id]
	if !ok {
		if len(t.entries) >= t.maxIDs {
			t.evictOldestEntryLocked()
		}
		// Seed the entry with both addresses: the audit view should show the
		// pair that made the change visible, not just the new address.
		t.entries[id] = &changeEntry{windowStart: now, ips: map[string]uint64{ip: 1, previous.ip: 1}}
		return true
	}
	if now.Sub(entry.windowStart) >= t.window {
		entry.windowStart = now
		entry.ips = map[string]uint64{ip: 1}
		return true
	}
	entry.ips[ip]++
	return true
}

// Snapshot returns up to count visible entries starting at start. Entries
// are ordered by ID so admin pages are stable across calls. Single-address
// entries stay in the tracker until they gain a second address or age out;
// they are not counted or shown, matching hbbs's ip-changes view without
// letting automatic metrics reads reset the audit.
func (t *Tracker) Snapshot(start, count int) []Entry {
	t.mu.Lock()
	defer t.mu.Unlock()
	now := t.now()
	t.pruneLocked(now)
	if count <= 0 || start < 0 {
		return nil
	}
	ids := make([]string, 0, len(t.entries))
	for id, entry := range t.entries {
		if len(entry.ips) > 1 {
			ids = append(ids, id)
		}
	}
	sort.Strings(ids)
	if start >= len(ids) {
		return nil
	}
	end := start + count
	if end > len(ids) {
		end = len(ids)
	}
	out := make([]Entry, 0, end-start)
	for _, id := range ids[start:end] {
		out = append(out, t.entryLocked(id, t.entries[id], now))
	}
	return out
}

func (t *Tracker) Lookup(id string) (Entry, bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	now := t.now()
	t.pruneLocked(now)
	entry, ok := t.entries[id]
	if !ok || len(entry.ips) <= 1 {
		return Entry{}, false
	}
	return t.entryLocked(id, entry, now), true
}

// Remove forgets both the audit entry and the baseline, so the next
// observation starts a new baseline instead of immediately re-recording a
// change.
func (t *Tracker) Remove(id string) bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	_, tracked := t.entries[id]
	_, seen := t.last[id]
	delete(t.entries, id)
	delete(t.last, id)
	return tracked || seen
}

func (t *Tracker) Clear() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	removed := len(t.entries)
	t.entries = make(map[string]*changeEntry)
	t.last = make(map[string]observation)
	return removed
}

func (t *Tracker) Stats() Stats {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.pruneLocked(t.now())
	tracked := 0
	for _, entry := range t.entries {
		if len(entry.ips) > 1 {
			tracked++
		}
	}
	return Stats{Tracked: tracked, Changes: t.changes}
}

func (t *Tracker) pruneLocked(now time.Time) {
	for id, entry := range t.entries {
		if now.Sub(entry.windowStart) >= t.retain {
			delete(t.entries, id)
		}
	}
}

func (t *Tracker) entryLocked(id string, entry *changeEntry, now time.Time) Entry {
	out := Entry{ID: id, Age: now.Sub(entry.windowStart), IPs: make([]IPCount, 0, len(entry.ips))}
	for ip, count := range entry.ips {
		out.IPs = append(out.IPs, IPCount{IP: ip, Count: count})
		out.Total += count
	}
	sort.Slice(out.IPs, func(i, j int) bool { return out.IPs[i].IP < out.IPs[j].IP })
	return out
}

func (t *Tracker) evictOldestObservationLocked() {
	var oldestID string
	var oldest time.Time
	for id, observation := range t.last {
		if oldestID == "" || observation.at.Before(oldest) {
			oldestID, oldest = id, observation.at
		}
	}
	delete(t.last, oldestID)
}

func (t *Tracker) evictOldestEntryLocked() {
	var oldestID string
	var oldest time.Time
	for id, entry := range t.entries {
		if oldestID == "" || entry.windowStart.Before(oldest) {
			oldestID, oldest = id, entry.windowStart
		}
	}
	delete(t.entries, oldestID)
}
