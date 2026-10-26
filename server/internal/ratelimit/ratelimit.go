// Package ratelimit provides a per-key token bucket for the HTTP API. The
// data planes keep their own limits; this only protects request handling.
package ratelimit

import (
	"sync"
	"time"
)

const (
	idleTTL   = 10 * time.Minute
	sweepAge  = 5 * time.Minute
	maxIdle   = 4096 // sweep threshold; buckets are also dropped by age
	oneSecond = float64(time.Second)
)

type bucket struct {
	tokens   float64
	last     time.Time
	lastSeen time.Time
}

// Limiter is safe for concurrent use. A nil or disabled Limiter allows all
// requests, which keeps the zero value useful in tests.
type Limiter struct {
	rate  float64
	burst float64
	now   func() time.Time

	mu      sync.Mutex
	buckets map[string]*bucket
	sweep   time.Time
}

// New returns a limiter that sustains rate requests per second per key with an
// initial burst. A rate of zero or less disables limiting.
func New(rate, burst float64) *Limiter {
	l := &Limiter{
		rate:    rate,
		burst:   burst,
		now:     time.Now,
		buckets: make(map[string]*bucket),
	}
	if burst < 1 {
		l.burst = 1
	}
	if rate > 0 {
		l.sweep = l.now().Add(sweepAge)
	}
	return l
}

// Enabled reports whether Allow can reject requests.
func (l *Limiter) Enabled() bool { return l != nil && l.rate > 0 }

// Allow consumes one token for key. Unknown keys start with a full bucket, so
// a single client can burst and then settles at rate per second.
func (l *Limiter) Allow(key string) bool {
	if !l.Enabled() {
		return true
	}
	now := l.now()
	l.mu.Lock()
	defer l.mu.Unlock()

	if now.After(l.sweep) {
		l.gcLocked(now)
		l.sweep = now.Add(sweepAge)
	}
	b := l.buckets[key]
	if b == nil {
		b = &bucket{tokens: l.burst, last: now}
		l.buckets[key] = b
	} else {
		b.tokens += now.Sub(b.last).Seconds() * l.rate
		if b.tokens > l.burst {
			b.tokens = l.burst
		}
		b.last = now
	}
	b.lastSeen = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// SetClock replaces the time source. Tests use it to advance time without
// sleeping; production never calls it.
func (l *Limiter) SetClock(now func() time.Time) {
	if l == nil || now == nil {
		return
	}
	l.now = now
	l.mu.Lock()
	l.sweep = now().Add(sweepAge)
	l.mu.Unlock()
}

func (l *Limiter) gcLocked(now time.Time) {
	for key, b := range l.buckets {
		if now.Sub(b.lastSeen) > idleTTL {
			delete(l.buckets, key)
		}
	}
	if len(l.buckets) <= maxIdle {
		return
	}
	// An attacker can rotate addresses faster than idleTTL; drop the least
	// recently seen half so memory stays bounded.
	for key := range l.buckets {
		if len(l.buckets) <= maxIdle/2 {
			break
		}
		if now.Sub(l.buckets[key].lastSeen) > idleTTL/2 {
			delete(l.buckets, key)
		}
	}
}

// Len reports the number of tracked keys. It exists for tests and ops probes.
func (l *Limiter) Len() int {
	if l == nil {
		return 0
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.buckets)
}
