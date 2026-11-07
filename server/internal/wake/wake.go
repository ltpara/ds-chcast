// Package wake coordinates cross-subnet Wake-on-LAN requests. The server keeps
// a short-lived queue of jobs; host agents act as proxies on their own link and
// pick up pending jobs when they refresh their registration. Jobs are never
// persisted, so a server restart simply forgets an unclaimed wake.
package wake

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"sync"
	"time"
)

var (
	// ErrCooldown reports a second wake for the same target inside the cooldown
	// window. Repeated requests are the most likely form of abuse.
	ErrCooldown = errors.New("wake: target was woken recently")
	// ErrQueueFull reports that too many unclaimed jobs are pending.
	ErrQueueFull = errors.New("wake: relay queue is full")
)

const (
	DefaultTTL      = 60 * time.Second
	DefaultCooldown = 30 * time.Second
	DefaultMaxJobs  = 64
)

// Job is one wake request delivered to a host agent. The job id lets an agent
// recognise a repeated delivery; ExpiresInSec is filled in by the server when
// the job is serialized. Broadcasts are the target's last-reported directed
// broadcast endpoints: a relay only receives the job when one matches its own
// report and sends to the matching subset.
type Job struct {
	ID           string   `json:"job"`
	Target       string   `json:"target"`
	MAC          string   `json:"mac"`
	Broadcasts   []string `json:"broadcasts,omitempty"`
	ExpiresInSec int64    `json:"expires_in_sec"`
}

type entry struct {
	job     Job
	expires time.Time
	relays  map[string]bool
}

// Broker owns the in-memory queue. It is safe for concurrent use.
type Broker struct {
	mu       sync.Mutex
	now      func() time.Time
	ttl      time.Duration
	cooldown time.Duration
	maxJobs  int
	jobs     []*entry
	last     map[string]time.Time
}

// New creates a broker. Zero values fall back to the defaults.
func New(ttl, cooldown time.Duration, maxJobs int) *Broker {
	if ttl <= 0 {
		ttl = DefaultTTL
	}
	if cooldown <= 0 {
		cooldown = DefaultCooldown
	}
	if maxJobs <= 0 {
		maxJobs = DefaultMaxJobs
	}
	return &Broker{
		now:      time.Now,
		ttl:      ttl,
		cooldown: cooldown,
		maxJobs:  maxJobs,
		last:     make(map[string]time.Time),
	}
}

// SetClock replaces the time source; tests use it to advance expiry.
func (b *Broker) SetClock(now func() time.Time) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.now = now
}

// Request queues a wake for target. Broadcasts are the target's last-reported
// directed broadcast endpoints. Relays must be explicitly authorized by the
// server operator; matching private broadcast addresses alone prove no trust.
// A job without broadcasts or authorized relays is never handed to an agent.
// It returns the cooldown remaining when the target was woken too recently and
// ErrQueueFull when the queue is saturated.
func (b *Broker) Request(target, mac string, broadcasts, relays []string) (Job, time.Duration, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	now := b.now()
	b.pruneLocked(now)
	if last, ok := b.last[target]; ok {
		if wait := b.cooldown - now.Sub(last); wait > 0 {
			return Job{}, wait, ErrCooldown
		}
	}
	if len(b.jobs) >= b.maxJobs {
		return Job{}, 0, ErrQueueFull
	}
	job := Job{
		ID:           newID(),
		Target:       target,
		MAC:          mac,
		Broadcasts:   append([]string(nil), broadcasts...),
		ExpiresInSec: int64(b.ttl / time.Second),
	}
	allowed := make(map[string]bool, len(relays))
	for _, id := range relays {
		allowed[id] = true
	}
	b.jobs = append(b.jobs, &entry{job: job, expires: now.Add(b.ttl), relays: allowed})
	b.last[target] = now
	return job, 0, nil
}

// PendingFor returns every unexpired job the agent may relay. An agent only
// sees jobs when the operator authorized its ID for the target, it registered
// as a relay, and its directed broadcast endpoints intersect the target's
// report. The address intersection is routing, not authorization: unrelated
// sites commonly reuse the same private subnet. Jobs remain eligible until expiry
// because the server cannot know whether a registration response reached the
// agent; agents use the job id to suppress a successfully relayed duplicate.
func (b *Broker) PendingFor(agentID string, relay bool, broadcasts []string) []Job {
	if !relay {
		return nil
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	now := b.now()
	b.pruneLocked(now)
	var out []Job
	for _, item := range b.jobs {
		if !item.relays[agentID] || item.job.Target == agentID || !sharedEndpoint(item.job.Broadcasts, broadcasts) {
			continue
		}
		out = append(out, item.job)
	}
	return out
}

func sharedEndpoint(a, b []string) bool {
	if len(a) == 0 || len(b) == 0 {
		return false
	}
	set := make(map[string]struct{}, len(b))
	for _, value := range b {
		set[value] = struct{}{}
	}
	for _, value := range a {
		if _, ok := set[value]; ok {
			return true
		}
	}
	return false
}

// Len reports the number of unexpired jobs.
func (b *Broker) Len() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.pruneLocked(b.now())
	return len(b.jobs)
}

func (b *Broker) pruneLocked(now time.Time) {
	kept := b.jobs[:0]
	for _, item := range b.jobs {
		if now.Before(item.expires) {
			kept = append(kept, item)
		}
	}
	b.jobs = kept
	for target, at := range b.last {
		if now.Sub(at) >= b.cooldown {
			delete(b.last, target)
		}
	}
}

func newID() string {
	var raw [8]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return fmt.Sprintf("t%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(raw[:])
}
