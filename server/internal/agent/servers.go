package agent

import (
	"errors"
	"fmt"
	"net/url"
	"slices"
	"strings"
	"sync"
)

// serverPool is the client-side copy of the rendezvous failover list. The
// current endpoint stays in use until it stops answering; the pool only
// rotates when a request fails, mirroring RustDesk's rendezvous_servers
// behaviour without moving a healthy host between servers mid-lease.
type serverPool struct {
	mu      sync.Mutex
	urls    []string
	current string
	serial  int64
}

func normalizeServerBase(raw string) (string, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "", errors.New("empty server URL")
	}
	u, err := url.Parse(raw)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return "", fmt.Errorf("server %q must be an absolute http:// or https:// URL", raw)
	}
	return strings.TrimRight(u.String(), "/"), nil
}

func newServerPool(primary string, extra []string) (*serverPool, error) {
	current, err := normalizeServerBase(primary)
	if err != nil {
		return nil, err
	}
	pool := &serverPool{current: current, urls: []string{current}}
	for _, raw := range extra {
		server, err := normalizeServerBase(raw)
		if err != nil {
			return nil, err
		}
		if !slices.Contains(pool.urls, server) {
			pool.urls = append(pool.urls, server)
		}
	}
	return pool, nil
}

func (p *serverPool) Current() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.current
}

func (p *serverPool) Serial() int64 {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.serial
}

func (p *serverPool) Len() int {
	p.mu.Lock()
	defer p.mu.Unlock()
	return len(p.urls)
}

func (p *serverPool) List() []string {
	p.mu.Lock()
	defer p.mu.Unlock()
	return slices.Clone(p.urls)
}

// Apply adopts a server-pushed list. An older serial is ignored; an equal one
// is accepted because the server advertises the full list on every reply and
// a client that lost its state must be able to recover it.
func (p *serverPool) Apply(serial int64, servers []string) bool {
	if len(servers) == 0 {
		return false
	}
	normalized := make([]string, 0, len(servers))
	for _, raw := range servers {
		server, err := normalizeServerBase(raw)
		if err != nil {
			continue
		}
		if !slices.Contains(normalized, server) {
			normalized = append(normalized, server)
		}
	}
	if len(normalized) == 0 {
		return false
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if serial < p.serial {
		return false
	}
	if !slices.Contains(normalized, p.current) {
		// A list that does not mention the endpoint we are talking to must not
		// strand this process when the new entries are unreachable.
		normalized = append(normalized, p.current)
	}
	changed := !slices.Equal(p.urls, normalized) || serial != p.serial
	p.urls = normalized
	p.serial = serial
	return changed
}

// Next rotates to the following endpoint and reports whether more than one
// server is known. A single-entry pool returns false, so callers do not retry
// the same address in a tight loop.
func (p *serverPool) Next() (string, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if len(p.urls) < 2 {
		return p.current, false
	}
	index := slices.Index(p.urls, p.current)
	if index < 0 {
		index = 0
	} else {
		index = (index + 1) % len(p.urls)
	}
	p.current = p.urls[index]
	return p.current, true
}
