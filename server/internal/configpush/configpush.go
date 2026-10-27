// Package configpush holds the operator-controlled rendezvous server list and
// its serial. It mirrors RustDesk's ConfigureUpdate/ConfigUpdate pair: the
// serial only moves forward, and clients apply a list only when the serial they
// received is not older than the one they already know.
package configpush

import (
	"errors"
	"fmt"
	"net/url"
	"slices"
	"strings"
	"sync"
)

// MaxServers bounds the list handed to clients. RustDesk also keeps its
// rendezvous list small because every entry is tried during failover.
const MaxServers = 8

// Config is safe for concurrent use. A nil Config disables pushes; callers
// check for nil instead of relying on a zero value.
type Config struct {
	mu      sync.RWMutex
	serial  int64
	servers []string
}

// Normalize validates and deduplicates absolute HTTP API base URLs. Query
// strings and fragments are rejected because a rendezvous endpoint is a base,
// not a request.
func Normalize(servers []string) ([]string, error) {
	out := make([]string, 0, len(servers))
	for _, raw := range servers {
		raw = strings.TrimSpace(raw)
		if raw == "" {
			continue
		}
		u, err := url.Parse(raw)
		if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
			return nil, fmt.Errorf("rendezvous server %q must be an absolute http:// or https:// URL", raw)
		}
		if u.RawQuery != "" || u.Fragment != "" {
			return nil, fmt.Errorf("rendezvous server %q must not carry a query or fragment", raw)
		}
		base := strings.TrimRight(u.String(), "/")
		if !slices.Contains(out, base) {
			out = append(out, base)
		}
	}
	if len(out) > MaxServers {
		return nil, fmt.Errorf("at most %d rendezvous servers are allowed", MaxServers)
	}
	return out, nil
}

// New builds a config from startup flags. A non-empty list with serial 0 gets
// serial 1 so clients that start from 0 accept it.
func New(serial int64, servers []string) (*Config, error) {
	list, err := Normalize(servers)
	if err != nil {
		return nil, err
	}
	if serial < 0 {
		return nil, errors.New("config serial must not be negative")
	}
	if serial == 0 && len(list) > 0 {
		serial = 1
	}
	return &Config{serial: serial, servers: list}, nil
}

// Snapshot returns the serial and a copy of the list.
func (c *Config) Snapshot() (int64, []string) {
	if c == nil {
		return 0, nil
	}
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.serial, slices.Clone(c.servers)
}

// Serial returns the current serial.
func (c *Config) Serial() int64 {
	serial, _ := c.Snapshot()
	return serial
}

// Update replaces the list at an explicit serial. The serial must move
// forward, matching hbbs: a stale ConfigureUpdate is ignored.
func (c *Config) Update(serial int64, servers []string) error {
	list, err := Normalize(servers)
	if err != nil {
		return err
	}
	if serial <= 0 {
		return errors.New("config serial must be positive")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if serial <= c.serial {
		return fmt.Errorf("serial %d must be greater than the current serial %d", serial, c.serial)
	}
	c.serial = serial
	c.servers = list
	return nil
}

// Set replaces the list and increments the serial, for the common console
// case where the operator does not want to track numbers by hand.
func (c *Config) Set(servers []string) (int64, error) {
	list, err := Normalize(servers)
	if err != nil {
		return 0, err
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	c.serial++
	c.servers = list
	return c.serial, nil
}
