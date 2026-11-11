// Package lanmask classifies peer addresses against an operator-defined LAN.
//
// hbbs takes the same input as -m: a CIDR such as 192.168.0.0/16. A peer inside
// the mask and a peer outside it cannot reach each other by punching, because
// the outside half would only probe a private address, so RustDesk forces those
// pairs onto the relay. This package is the shared classifier for the UDP
// rendezvous, the TCP punch coordination and the lookup API.
package lanmask

import (
	"fmt"
	"net"
	"strings"
)

// Mask is a set of networks; the zero value classifies nothing.
type Mask struct {
	nets []*net.IPNet
}

// Parse reads a comma-separated list of CIDRs. A bare address is treated as a
// single-host network, so "10.8.0.0/16,172.16.0.0/12" and "::1" both work.
// Empty text yields a nil mask, which keeps the LAN rule disabled.
func Parse(text string) (*Mask, error) {
	var mask Mask
	for _, part := range strings.Split(text, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if !strings.Contains(part, "/") {
			ip := net.ParseIP(part)
			if ip == nil {
				return nil, fmt.Errorf("lan mask %q is not a CIDR or IP address", part)
			}
			bits := 32
			if ip.To4() == nil {
				bits = 128
			}
			part = fmt.Sprintf("%s/%d", ip.String(), bits)
		}
		_, network, err := net.ParseCIDR(part)
		if err != nil {
			return nil, fmt.Errorf("lan mask %q: %w", part, err)
		}
		mask.nets = append(mask.nets, network)
	}
	if len(mask.nets) == 0 {
		return nil, nil
	}
	return &mask, nil
}

// Enabled reports whether the mask classifies any address. All methods are safe
// on a nil mask so callers do not have to branch when the feature is off.
func (m *Mask) Enabled() bool { return m != nil && len(m.nets) > 0 }

// Contains reports whether the address falls inside the mask. IPv4-mapped IPv6
// addresses are compared as IPv4 so a dual-stack listener classifies both
// families of the same peer the same way.
func (m *Mask) Contains(ip net.IP) bool {
	if !m.Enabled() || ip == nil {
		return false
	}
	if v4 := ip.To4(); v4 != nil {
		ip = v4
	}
	for _, network := range m.nets {
		if network.Contains(ip) {
			return true
		}
	}
	return false
}

// ContainsAddr splits a host:port endpoint before classifying it.
func (m *Mask) ContainsAddr(addr string) bool {
	if !m.Enabled() || addr == "" {
		return false
	}
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		host = addr
	}
	return m.Contains(net.ParseIP(strings.Trim(host, "[]")))
}

// SameIntranet reports whether two peers can reach each other on the same
// network. hbbs accepts the pair when both observed addresses are inside the
// mask or when they are the same address; with the mask disabled only the
// latter holds, which preserves the pre-mask behavior.
func (m *Mask) SameIntranet(a, b net.IP) bool {
	if a == nil || b == nil {
		return false
	}
	if a.Equal(b) {
		return true
	}
	return m.Contains(a) && m.Contains(b)
}

// AddrIP extracts the address from a host:port endpoint, tolerating a bare
// address. It returns nil when the host part is not an IP.
func AddrIP(addr string) net.IP {
	if addr == "" {
		return nil
	}
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		host = addr
	}
	return net.ParseIP(strings.Trim(host, "[]"))
}
