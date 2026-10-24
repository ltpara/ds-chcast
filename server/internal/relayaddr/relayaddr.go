// Package relayaddr holds the relay endpoints advertised to hosts. chast
// allocates a UDP relay port per host, so entries are bare host names or IPs
// and the port is joined at registration time.
package relayaddr

import (
	"fmt"
	"net"
	"strings"
	"sync"
)

type List struct {
	mu       sync.Mutex
	defaults []string
	addrs    []string
	next     uint64
}

func New(defaults []string) (*List, error) {
	normalized, err := normalize(defaults)
	if err != nil {
		return nil, err
	}
	return &List{defaults: normalized, addrs: append([]string(nil), normalized...)}, nil
}

// Addrs returns a copy of the current list in configuration order.
func (l *List) Addrs() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]string(nil), l.addrs...)
}

func (l *List) Len() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.addrs)
}

// Next returns the next address in round-robin order, consuming one rotation.
// A single-address list never advances, matching hbbs.
func (l *List) Next() string {
	addr, _ := l.Assign()
	return addr
}

// Assign returns the next address together with the full list, already
// normalized. The full list lets a host accept relay challenges from any
// advertised endpoint even when only one is its primary.
func (l *List) Assign() (string, []string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	addrs := append([]string(nil), l.addrs...)
	if len(addrs) == 0 {
		return "", nil
	}
	if len(addrs) == 1 {
		return addrs[0], addrs
	}
	index := l.next % uint64(len(addrs))
	l.next++
	return addrs[index], addrs
}

// AssignFrom keeps previous when it is still advertised, otherwise it rotates
// to the next address. Keeping a lease on its address prevents every renewal
// from moving a running host to another relay.
func (l *List) AssignFrom(previous string) (string, []string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	addrs := append([]string(nil), l.addrs...)
	if len(addrs) == 0 {
		return "", nil
	}
	for _, addr := range addrs {
		if addr == previous {
			return addr, addrs
		}
	}
	if len(addrs) == 1 {
		return addrs[0], addrs
	}
	index := l.next % uint64(len(addrs))
	l.next++
	return addrs[index], addrs
}

func (l *List) Join(port int) []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	out := make([]string, 0, len(l.addrs))
	for _, addr := range l.addrs {
		out = append(out, net.JoinHostPort(addr, fmt.Sprint(port)))
	}
	return out
}

// SetCSV replaces the list. Empty items are ignored; an all-empty value
// clears the list.
func (l *List) SetCSV(csv string) error {
	items := strings.Split(csv, ",")
	normalized, err := normalize(items)
	if err != nil {
		return err
	}
	l.mu.Lock()
	l.addrs = normalized
	l.mu.Unlock()
	return nil
}

func (l *List) Reset() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.addrs = append([]string(nil), l.defaults...)
	return append([]string(nil), l.addrs...)
}

func normalize(items []string) ([]string, error) {
	out := make([]string, 0, len(items))
	seen := make(map[string]struct{}, len(items))
	for _, raw := range items {
		addr, err := normalizeOne(raw)
		if err != nil {
			return nil, err
		}
		if addr == "" {
			continue
		}
		if _, ok := seen[addr]; ok {
			continue
		}
		seen[addr] = struct{}{}
		out = append(out, addr)
	}
	return out, nil
}

func normalizeOne(raw string) (string, error) {
	addr := strings.TrimSpace(raw)
	if addr == "" {
		return "", nil
	}
	if strings.Contains(addr, "://") || strings.ContainsAny(addr, "/?#@\\ \t") {
		return "", fmt.Errorf("relay address %q is not a host name or IP", raw)
	}
	if strings.HasPrefix(addr, "[") {
		if !strings.HasSuffix(addr, "]") {
			return "", fmt.Errorf("relay address %q must not include a port", raw)
		}
		inner := addr[1 : len(addr)-1]
		ip := net.ParseIP(inner)
		if ip == nil || ip.To4() != nil {
			return "", fmt.Errorf("relay address %q is not a bracketed IPv6 address", raw)
		}
		return ip.String(), nil
	}
	if strings.Contains(addr, ":") {
		if ip := net.ParseIP(addr); ip != nil && ip.To4() == nil {
			return ip.String(), nil
		}
		return "", fmt.Errorf("relay address %q must not include a port", raw)
	}
	if ip := net.ParseIP(addr); ip != nil {
		return ip.String(), nil
	}
	if len(addr) > 253 {
		return "", fmt.Errorf("relay address %q is too long", raw)
	}
	for _, label := range strings.Split(addr, ".") {
		if label == "" || len(label) > 63 {
			return "", fmt.Errorf("relay address %q is not a valid host name", raw)
		}
		for _, r := range label {
			switch {
			case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_':
			default:
				return "", fmt.Errorf("relay address %q is not a valid host name", raw)
			}
		}
		if strings.HasPrefix(label, "-") || strings.HasSuffix(label, "-") {
			return "", fmt.Errorf("relay address %q is not a valid host name", raw)
		}
	}
	return strings.ToLower(addr), nil
}
