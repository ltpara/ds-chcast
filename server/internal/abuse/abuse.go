// Package abuse throttles registration attempts per source IP, mirroring
// hbbs's check_ip_blocker: a single address may not churn registrations or try
// an unbounded number of distinct IDs. It is deliberately in-memory and
// best-effort; a restart forgets the counters.
package abuse

import (
	"fmt"
	"sort"
	"sync"
	"time"
)

type Config struct {
	// Window and MaxPerWindow bound all registration attempts per IP.
	Window       time.Duration
	MaxPerWindow int
	// IDWindow and MaxIDs bound distinct IDs seen per IP. Existing IDs keep
	// renewing after the limit so shared NAT egresses are not locked out.
	IDWindow time.Duration
	MaxIDs   int
	// MaxIPs caps tracked addresses; zero uses 65536.
	MaxIPs int
	Now    func() time.Time
}

type ipState struct {
	windowStart time.Time
	count       int
	idsStart    time.Time
	ids         map[string]time.Time
}

type Stats struct {
	TrackedIPs  int    `json:"tracked_ips"`
	RateBlocked uint64 `json:"rate_blocked"`
	IDBlocked   uint64 `json:"id_blocked"`
}

type Entry struct {
	IP          string        `json:"ip"`
	WindowCount int           `json:"window_count"`
	WindowAge   time.Duration `json:"-"`
	IDs         int           `json:"ids"`
	IDsAge      time.Duration `json:"-"`
}

type Blocker struct {
	mu        sync.Mutex
	cfg       Config
	ips       map[string]*ipState
	lastSweep time.Time
	rate      uint64
	idFlood   uint64
}

func New(cfg Config) *Blocker {
	if cfg.Window <= 0 {
		cfg.Window = 60 * time.Second
	}
	if cfg.IDWindow <= 0 {
		cfg.IDWindow = 24 * time.Hour
	}
	if cfg.MaxIPs <= 0 {
		cfg.MaxIPs = 65536
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	return &Blocker{cfg: cfg, ips: make(map[string]*ipState)}
}

// SetClock is used by tests to advance the rate and ID windows.
func (b *Blocker) SetClock(now func() time.Time) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.cfg.Now = now
}

// Allow records an attempt and reports whether it may proceed. reason is
// "rate" or "ids" when denied.
func (b *Blocker) Allow(ip, id string) (bool, string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	now := b.cfg.Now()
	b.sweepLocked(now)

	st, ok := b.ips[ip]
	if !ok {
		if len(b.ips) >= b.cfg.MaxIPs {
			b.evictOldestLocked()
		}
		st = &ipState{windowStart: now, idsStart: now, ids: make(map[string]time.Time)}
		b.ips[ip] = st
	}
	if now.Sub(st.windowStart) > b.cfg.Window {
		st.windowStart = now
		st.count = 0
	}
	st.count++
	if b.cfg.MaxPerWindow > 0 && st.count > b.cfg.MaxPerWindow {
		b.rate++
		return false, "rate"
	}
	if b.cfg.MaxIDs > 0 {
		if now.Sub(st.idsStart) > b.cfg.IDWindow {
			st.ids = make(map[string]time.Time)
			st.idsStart = now
		}
		if _, seen := st.ids[id]; !seen && len(st.ids) >= b.cfg.MaxIDs {
			b.idFlood++
			return false, "ids"
		}
		st.ids[id] = now
	}
	return true, ""
}

func (b *Blocker) Stats() Stats {
	b.mu.Lock()
	defer b.mu.Unlock()
	return Stats{TrackedIPs: len(b.ips), RateBlocked: b.rate, IDBlocked: b.idFlood}
}

// Clear forgets every tracked address and returns how many were dropped.
// Counters stay monotonic so metrics do not reset under an operator command.
func (b *Blocker) Clear() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	removed := len(b.ips)
	b.ips = make(map[string]*ipState)
	return removed
}

// Entries lists tracked addresses sorted by IP for stable console output.
func (b *Blocker) Entries(start, count int) []Entry {
	b.mu.Lock()
	defer b.mu.Unlock()
	now := b.cfg.Now()
	ips := make([]string, 0, len(b.ips))
	for ip := range b.ips {
		ips = append(ips, ip)
	}
	sort.Strings(ips)
	if start < 0 {
		start = 0
	}
	if count <= 0 || start >= len(ips) {
		return nil
	}
	end := start + count
	if end > len(ips) {
		end = len(ips)
	}
	out := make([]Entry, 0, end-start)
	for _, ip := range ips[start:end] {
		out = append(out, b.entryLocked(ip, b.ips[ip], now))
	}
	return out
}

func (b *Blocker) IPEntry(ip string) (Entry, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	st, ok := b.ips[ip]
	if !ok {
		return Entry{}, false
	}
	return b.entryLocked(ip, st, b.cfg.Now()), true
}

func (b *Blocker) entryLocked(ip string, st *ipState, now time.Time) Entry {
	return Entry{
		IP:          ip,
		WindowCount: st.count,
		WindowAge:   now.Sub(st.windowStart),
		IDs:         len(st.ids),
		IDsAge:      now.Sub(st.idsStart),
	}
}

func (b *Blocker) sweepLocked(now time.Time) {
	if !b.lastSweep.IsZero() && now.Sub(b.lastSweep) < b.cfg.Window {
		return
	}
	b.lastSweep = now
	for ip, st := range b.ips {
		windowDone := now.Sub(st.windowStart) > b.cfg.Window
		idsDone := b.cfg.MaxIDs <= 0 || now.Sub(st.idsStart) > b.cfg.IDWindow
		if windowDone && idsDone {
			delete(b.ips, ip)
		}
	}
}

func (b *Blocker) evictOldestLocked() {
	var oldestIP string
	var oldest time.Time
	for ip, st := range b.ips {
		if oldestIP == "" || st.windowStart.Before(oldest) {
			oldestIP, oldest = ip, st.windowStart
		}
	}
	if oldestIP != "" {
		delete(b.ips, oldestIP)
	}
}

// Describe formats a window age the way the console reports it.
func (e Entry) Describe() string {
	return fmt.Sprintf("%s %d/%s %d ids/%s", e.IP, e.WindowCount, shortDur(e.WindowAge), e.IDs, shortDur(e.IDsAge))
}

func shortDur(d time.Duration) string {
	if d < 0 {
		d = 0
	}
	return d.Truncate(time.Second).String()
}
