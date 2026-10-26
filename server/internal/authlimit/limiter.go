// Package authlimit locks a login key out after repeated failures. Unlike
// ratelimit it does not slow steady traffic: only failed attempts count, a
// success clears the key, and a lockout rejects every attempt until it
// expires. State is in memory by default; NewPersistent keeps it in a Backend
// so lockouts survive restarts and are visible to another process sharing the
// database. Both modes are best-effort, like abuse.Blocker.
package authlimit

import (
	"sync"
	"time"
)

const (
	idleTTL  = 30 * time.Minute
	sweepAge = 5 * time.Minute
	maxKeys  = 65536
)

// Config controls one key's failure budget. MaxFailures of zero disables the
// limiter, which keeps the zero value and nil receivers permissive.
type Config struct {
	Window      time.Duration // failures older than this are forgotten
	MaxFailures int           // failures inside Window before a lockout
	Lockout     time.Duration // how long a locked key stays refused
	MaxKeys     int           // tracked keys before the oldest is evicted
	Now         func() time.Time
}

type state struct {
	failures    int
	windowStart time.Time
	lockedUntil time.Time
	lastSeen    time.Time
}

// Record is the persisted form of one key's state. A zero LockedUntil means
// the key is not locked; WindowStart anchors the current failure window.
type Record struct {
	Failures    int
	WindowStart time.Time
	LockedUntil time.Time
	LastSeen    time.Time
}

// Backend stores limiter state outside the process. A load or save error never
// blocks a login: the limiter fails open and reports the error through the
// callback passed to NewPersistent.
type Backend interface {
	LoadLoginLock(key string) (Record, bool, error)
	SaveLoginLock(key string, record Record) error
	DeleteLoginLock(key string) error
	PruneLoginLocks(seenBefore, lockedBefore time.Time) error
}

// AtomicBackend is an optional Backend extension. UpdateLoginLock applies
// update to the stored record and writes the result in one storage
// transaction, so two processes sharing a database cannot lose failures to
// each other's read-modify-write cycles. The limiter falls back to
// Load/Save when a backend does not implement it.
type AtomicBackend interface {
	UpdateLoginLock(key string, update func(record Record, found bool) Record) (Record, error)
}

// Stats is the console/metrics view of the limiter.
type Stats struct {
	Tracked  int
	Locked   int
	Failures uint64
	Lockouts uint64
}

type Limiter struct {
	cfg Config

	mu       sync.Mutex
	keys     map[string]*state
	backend  Backend
	onError  func(error)
	sweep    time.Time
	failures uint64
	lockouts uint64
}

func New(cfg Config) *Limiter {
	if cfg.Window <= 0 {
		cfg.Window = 10 * time.Minute
	}
	if cfg.MaxFailures < 0 {
		cfg.MaxFailures = 0
	}
	if cfg.Lockout <= 0 {
		cfg.Lockout = time.Minute
	}
	if cfg.MaxKeys <= 0 {
		cfg.MaxKeys = maxKeys
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	l := &Limiter{cfg: cfg, keys: make(map[string]*state)}
	if l.Enabled() {
		l.sweep = cfg.Now().Add(sweepAge)
	}
	return l
}

// NewPersistent reads and writes lockout state through backend instead of the
// process map. onError may be nil; it receives storage failures, which are
// always ignored so a broken state file cannot lock every user out.
func NewPersistent(cfg Config, backend Backend, onError func(error)) *Limiter {
	l := New(cfg)
	if backend != nil {
		l.backend = backend
		l.onError = onError
	}
	return l
}

// Enabled reports whether Allow and Failure can refuse a key.
func (l *Limiter) Enabled() bool { return l != nil && l.cfg.MaxFailures > 0 }

// Allow reports whether key may attempt a login. When it may not, the first
// result is the remaining lockout, rounded up by the caller for Retry-After.
func (l *Limiter) Allow(key string) (time.Duration, bool) {
	if !l.Enabled() {
		return 0, true
	}
	now := l.cfg.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.backend != nil {
		l.pruneLocked(now)
		return l.persistAllowLocked(key, now)
	}
	l.sweepLocked(now)
	st := l.keys[key]
	if st == nil {
		return 0, true
	}
	st.lastSeen = now
	if now.Before(st.lockedUntil) {
		return st.lockedUntil.Sub(now), false
	}
	if !st.lockedUntil.IsZero() {
		st.lockedUntil = time.Time{}
		st.failures = 0
		st.windowStart = now
	}
	return 0, true
}

// Failure records one rejected credential check. The boolean is false when
// the attempt reached the limit and the key is now locked; the duration is
// that lockout.
func (l *Limiter) Failure(key string) (time.Duration, bool) {
	if !l.Enabled() {
		return 0, true
	}
	now := l.cfg.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.backend != nil {
		l.pruneLocked(now)
		return l.persistFailureLocked(key, now)
	}
	l.sweepLocked(now)
	st := l.keys[key]
	if st == nil {
		if len(l.keys) >= l.cfg.MaxKeys {
			l.evictOldestLocked()
		}
		st = &state{windowStart: now}
		l.keys[key] = st
	}
	st.lastSeen = now
	if now.Before(st.lockedUntil) {
		return st.lockedUntil.Sub(now), false
	}
	if st.windowStart.IsZero() || now.Sub(st.windowStart) > l.cfg.Window {
		st.windowStart = now
		st.failures = 0
	}
	st.failures++
	l.failures++
	if st.failures >= l.cfg.MaxFailures {
		st.failures = 0
		st.windowStart = now
		st.lockedUntil = now.Add(l.cfg.Lockout)
		l.lockouts++
		return l.cfg.Lockout, false
	}
	return 0, true
}

// Success forgets a key after a completed login.
func (l *Limiter) Success(key string) {
	if !l.Enabled() {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.backend != nil {
		if err := l.backend.DeleteLoginLock(key); err != nil {
			l.report(err)
		}
		return
	}
	delete(l.keys, key)
}

// persistAllowLocked mirrors Allow against the backend. Storage errors fail
// open; a stale lastSeen write only delays idle pruning.
func (l *Limiter) persistAllowLocked(key string, now time.Time) (time.Duration, bool) {
	record, found, err := l.backend.LoadLoginLock(key)
	if err != nil {
		l.report(err)
		return 0, true
	}
	if !found {
		return 0, true
	}
	if now.Before(record.LockedUntil) {
		record.LastSeen = now
		l.saveLocked(key, record)
		return record.LockedUntil.Sub(now), false
	}
	if !record.LockedUntil.IsZero() {
		record.LockedUntil = time.Time{}
		record.Failures = 0
		record.WindowStart = now
	}
	record.LastSeen = now
	l.saveLocked(key, record)
	return 0, true
}

// persistFailureLocked mirrors Failure against the backend. There is no map
// cap here: PruneLoginLocks bounds the table by idle time instead.
func (l *Limiter) persistFailureLocked(key string, now time.Time) (time.Duration, bool) {
	if backend, ok := l.backend.(AtomicBackend); ok {
		return l.persistFailureAtomicLocked(backend, key, now)
	}
	record, found, err := l.backend.LoadLoginLock(key)
	if err != nil {
		l.report(err)
		return 0, true
	}
	if !found {
		record = Record{WindowStart: now}
	}
	record.LastSeen = now
	if now.Before(record.LockedUntil) {
		l.saveLocked(key, record)
		return record.LockedUntil.Sub(now), false
	}
	if !record.LockedUntil.IsZero() || record.WindowStart.IsZero() ||
		now.Sub(record.WindowStart) > l.cfg.Window {
		record.LockedUntil = time.Time{}
		record.Failures = 0
		record.WindowStart = now
	}
	record.Failures++
	l.failures++
	if record.Failures >= l.cfg.MaxFailures {
		record.Failures = 0
		record.WindowStart = now
		record.LockedUntil = now.Add(l.cfg.Lockout)
		l.lockouts++
		l.saveLocked(key, record)
		return l.cfg.Lockout, false
	}
	l.saveLocked(key, record)
	return 0, true
}

// persistFailureAtomicLocked records one failure through an atomic backend so
// concurrent servers sharing the database cannot lose an increment.
func (l *Limiter) persistFailureAtomicLocked(backend AtomicBackend, key string, now time.Time) (time.Duration, bool) {
	var locked, alreadyLocked, triggered bool
	record, err := backend.UpdateLoginLock(key, func(record Record, found bool) Record {
		if !found {
			record = Record{WindowStart: now}
		}
		record.LastSeen = now
		if now.Before(record.LockedUntil) {
			locked, alreadyLocked = true, true
			return record
		}
		if !record.LockedUntil.IsZero() || record.WindowStart.IsZero() ||
			now.Sub(record.WindowStart) > l.cfg.Window {
			record.LockedUntil = time.Time{}
			record.Failures = 0
			record.WindowStart = now
		}
		record.Failures++
		if record.Failures >= l.cfg.MaxFailures {
			record.Failures = 0
			record.WindowStart = now
			record.LockedUntil = now.Add(l.cfg.Lockout)
			locked, triggered = true, true
		}
		return record
	})
	if err != nil {
		l.report(err)
		return 0, true
	}
	if triggered {
		l.failures++
		l.lockouts++
	} else if !alreadyLocked {
		l.failures++
	}
	if locked && record.LockedUntil.After(now) {
		return record.LockedUntil.Sub(now), false
	}
	return 0, true
}

func (l *Limiter) saveLocked(key string, record Record) {
	if err := l.backend.SaveLoginLock(key, record); err != nil {
		l.report(err)
	}
}

func (l *Limiter) report(err error) {
	if l.onError != nil {
		l.onError(err)
	}
}

func (l *Limiter) pruneLocked(now time.Time) {
	if now.Before(l.sweep) {
		return
	}
	l.sweep = now.Add(sweepAge)
	if err := l.backend.PruneLoginLocks(now.Add(-idleTTL), now); err != nil {
		l.report(err)
	}
}

func (l *Limiter) Stats() Stats {
	if l == nil {
		return Stats{}
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	now := l.cfg.Now()
	out := Stats{Tracked: len(l.keys), Failures: l.failures, Lockouts: l.lockouts}
	if l.backend != nil {
		out.Tracked = 0
		return out
	}
	for _, st := range l.keys {
		if now.Before(st.lockedUntil) {
			out.Locked++
		}
	}
	return out
}

// SetClock replaces the time source. Tests use it to advance windows and
// lockouts without sleeping; production never calls it.
func (l *Limiter) SetClock(now func() time.Time) {
	if l == nil || now == nil {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	l.cfg.Now = now
	if l.Enabled() {
		l.sweep = now().Add(sweepAge)
	}
}

func (l *Limiter) sweepLocked(now time.Time) {
	if now.Before(l.sweep) {
		return
	}
	l.sweep = now.Add(sweepAge)
	for key, st := range l.keys {
		if now.Sub(st.lastSeen) > idleTTL &&
			!now.Before(st.lockedUntil) &&
			(now.Sub(st.windowStart) > l.cfg.Window || st.windowStart.IsZero()) {
			delete(l.keys, key)
		}
	}
}

func (l *Limiter) evictOldestLocked() {
	var (
		oldestKey string
		oldest    time.Time
	)
	for key, st := range l.keys {
		if oldestKey == "" || st.lastSeen.Before(oldest) {
			oldestKey, oldest = key, st.lastSeen
		}
	}
	if oldestKey != "" {
		delete(l.keys, oldestKey)
	}
}
