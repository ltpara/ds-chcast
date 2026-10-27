package agent

import (
	"net"
	"strings"
)

// carrierCandidateOrder orders the shared-carrier endpoints advertised for a
// peer. The preferred address family comes first and the other family stays as
// a fallback, so a dual-stack peer does not pay a failed dial on every
// reconnect. Invalid or host-less endpoints are dropped and duplicates are
// collapsed.
func carrierCandidateOrder(primary, alternate string, preferAlternate bool) []string {
	order := [2]string{primary, alternate}
	if preferAlternate {
		order[0], order[1] = order[1], order[0]
	}
	out := make([]string, 0, len(order))
	seen := make(map[string]struct{}, len(order))
	for _, addr := range order {
		addr = strings.TrimSpace(addr)
		host, _, err := net.SplitHostPort(addr)
		if err != nil || host == "" {
			continue
		}
		if _, ok := seen[addr]; ok {
			continue
		}
		seen[addr] = struct{}{}
		out = append(out, addr)
	}
	return out
}

// preferIPv6Outbound reports whether this machine holds a global IPv6
// address. Without one, dialing an IPv6 carrier endpoint can only fail, so the
// IPv4 endpoint is tried first.
func preferIPv6Outbound() bool {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return false
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok {
			continue
		}
		ip := ipnet.IP
		if ip.To4() == nil && ip.IsGlobalUnicast() && !ip.IsLinkLocalUnicast() {
			return true
		}
	}
	return false
}
