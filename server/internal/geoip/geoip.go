// Package geoip resolves client addresses to operator-supplied location labels.
// The table is a plain text file so deployments can generate it from any data
// source (GeoLite2, ip2region, an internal allocation list) and refresh it
// without rebuilding the server. An empty or missing table leaves locations
// blank; callers keep recording the IP.
package geoip

import (
	"bufio"
	"fmt"
	"net/netip"
	"os"
	"sort"
	"strings"
)

// Locator answers location lookups from a sorted prefix table. A nil Locator
// is valid and always returns an empty label.
type Locator struct {
	entries []entry
}

type entry struct {
	prefix netip.Prefix
	label  string
}

// Load reads lines of "CIDR location". The first whitespace-separated field is
// an IPv4/IPv6 prefix, the rest of the line is the label (spaces allowed).
// Blank lines and lines starting with # are ignored; malformed lines fail the
// load so a typo is caught at startup instead of silently losing coverage.
func Load(path string) (*Locator, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	locator := &Locator{}
	scanner := bufio.NewScanner(file)
	line := 0
	for scanner.Scan() {
		line++
		text := strings.TrimSpace(strings.TrimPrefix(scanner.Text(), "\ufeff"))
		if text == "" || strings.HasPrefix(text, "#") {
			continue
		}
		fields := strings.Fields(text)
		if len(fields) < 2 {
			return nil, fmt.Errorf("%s:%d: want \"CIDR location\"", path, line)
		}
		prefix, err := netip.ParsePrefix(fields[0])
		if err != nil {
			return nil, fmt.Errorf("%s:%d: %w", path, line, err)
		}
		locator.entries = append(locator.entries, entry{
			prefix: prefix.Masked(),
			label:  strings.Join(fields[1:], " "),
		})
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	// Longest prefix first, so the first match is the most specific one.
	sort.SliceStable(locator.entries, func(i, j int) bool {
		return locator.entries[i].prefix.Bits() > locator.entries[j].prefix.Bits()
	})
	return locator, nil
}

// Lookup returns the most specific label containing addr, or "" when the
// address is malformed or outside the table.
func (l *Locator) Lookup(addr string) string {
	if l == nil {
		return ""
	}
	ip, err := netip.ParseAddr(strings.TrimSpace(addr))
	if err != nil {
		return ""
	}
	if ip.Is4In6() {
		ip = ip.Unmap()
	}
	if ip.Zone() != "" {
		ip = ip.WithZone("")
	}
	for _, item := range l.entries {
		if item.prefix.Contains(ip) {
			return item.label
		}
	}
	return ""
}

// Len reports the number of prefixes loaded, for startup logging.
func (l *Locator) Len() int {
	if l == nil {
		return 0
	}
	return len(l.entries)
}
