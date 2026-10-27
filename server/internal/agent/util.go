package agent

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log/slog"
	"net"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/upnp"
)

const (
	NATUnknown    = "unknown"
	NATAsymmetric = "asymmetric"
	NATSymmetric  = "symmetric"
)

// localCandidates lists LAN addresses the bound socket can actually serve on
// its port. They are hints for peers on the same network; the rendezvous
// server never trusts them as a public address.
func localCandidates(bound *net.UDPAddr) []string {
	ifaces, err := net.InterfaceAddrs()
	if err != nil {
		return nil
	}
	var boundIP net.IP
	port := 0
	if bound != nil {
		boundIP = bound.IP
		port = bound.Port
	}
	seen := make(map[string]struct{}, len(ifaces))
	out := make([]string, 0, len(ifaces))
	for _, addr := range ifaces {
		ipnet, ok := addr.(*net.IPNet)
		if !ok {
			continue
		}
		ip := ipnet.IP
		if ip.IsLoopback() || ip.IsUnspecified() || ip.IsMulticast() || ip.IsLinkLocalUnicast() {
			continue
		}
		if boundIP != nil && !boundIP.IsUnspecified() && !boundIP.Equal(ip) {
			continue
		}
		if !familyAllowed(boundIP, ip) {
			continue
		}
		text := net.JoinHostPort(ip.String(), strconv.Itoa(port))
		if _, ok := seen[text]; ok {
			continue
		}
		seen[text] = struct{}{}
		out = append(out, text)
	}
	sort.Slice(out, func(i, j int) bool {
		left, right := rankAddrText(out[i]), rankAddrText(out[j])
		if left != right {
			return left < right
		}
		return out[i] < out[j]
	})
	return out
}

// isIPv6Text reports whether a host:port candidate is IPv6.
func isIPv6Text(text string) bool {
	host, _, err := net.SplitHostPort(text)
	if err != nil {
		return false
	}
	ip := net.ParseIP(strings.Trim(host, "[]"))
	return ip != nil && ip.To4() == nil
}

// splitByFamily keeps IPv4 and IPv6 candidates apart for reporting.
func splitByFamily(addrs []string) (v4, v6 []string) {
	for _, addr := range addrs {
		if isIPv6Text(addr) {
			v6 = append(v6, addr)
		} else {
			v4 = append(v4, addr)
		}
	}
	return v4, v6
}

// rankAddr orders direct candidates for punching: LAN IPv4 first, then LAN
// IPv6, then globally routable IPv6 (no NAT) and finally public IPv4. All
// candidates are probed in parallel; the order decides which one is reported
// as the preferred path.
func rankAddr(ip net.IP) int {
	switch {
	case ip.To4() != nil && ip.IsPrivate():
		return 0
	case ip.To4() == nil && ip.IsPrivate():
		return 1
	case ip.To4() == nil:
		return 2
	case ip.IsGlobalUnicast():
		return 3
	default:
		return 4
	}
}

func rankAddrText(text string) int {
	host, _, err := net.SplitHostPort(text)
	if err != nil {
		return 4
	}
	ip := net.ParseIP(strings.Trim(host, "[]"))
	if ip == nil {
		return 4
	}
	return rankAddr(ip)
}

// familyAllowed reports whether remote can be used from a socket bound to
// local. An unspecified IPv4 socket cannot send to IPv6, while Go's "::"
// wildcard is dual-stack; a specific address restricts candidates to its own
// family.
func familyAllowed(local, remote net.IP) bool {
	if local == nil {
		return true
	}
	if local.IsUnspecified() {
		if local.To4() != nil {
			return remote.To4() != nil
		}
		return true
	}
	return (local.To4() != nil) == (remote.To4() != nil)
}

// mergeCandidates resolves, family-filters, deduplicates and ranks candidate
// addresses collected from registration and punch replies.
func mergeCandidates(local *net.UDPAddr, lists ...[]string) []*net.UDPAddr {
	var localIP net.IP
	if local != nil {
		localIP = local.IP
	}
	seen := make(map[string]struct{})
	out := make([]*net.UDPAddr, 0, 8)
	for _, list := range lists {
		for _, text := range list {
			if text == "" {
				continue
			}
			addr, err := net.ResolveUDPAddr("udp", text)
			if err != nil || addr == nil || addr.Port == 0 {
				continue
			}
			if addr.IP == nil || addr.IP.IsUnspecified() || addr.IP.IsMulticast() || addr.IP.IsLinkLocalUnicast() {
				continue
			}
			if !familyAllowed(localIP, addr.IP) {
				continue
			}
			key := addr.String()
			if _, ok := seen[key]; ok {
				continue
			}
			seen[key] = struct{}{}
			out = append(out, addr)
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		left, right := rankAddr(out[i].IP), rankAddr(out[j].IP)
		if left != right {
			return left < right
		}
		return out[i].String() < out[j].String()
	})
	if len(out) > 16 {
		out = out[:16]
	}
	return out
}

// classifyNAT compares the public mapping observed by the rendezvous port with
// the one observed by a relay port. A stable mapping across destinations is
// cone-like, which RustDesk calls asymmetric; a per-destination port is a
// symmetric NAT and direct punching is much less likely to succeed.
func classifyNAT(primary, secondary *net.UDPAddr) string {
	if primary == nil || secondary == nil {
		return ""
	}
	if !primary.IP.Equal(secondary.IP) {
		return NATUnknown
	}
	if primary.Port == secondary.Port {
		return NATAsymmetric
	}
	return NATSymmetric
}

func addrList(addrs []*net.UDPAddr) []string {
	out := make([]string, 0, len(addrs))
	for _, addr := range addrs {
		if addr != nil {
			out = append(out, addr.String())
		}
	}
	return out
}

func sameAddr(a, b *net.UDPAddr) bool {
	return a != nil && b != nil && a.Port == b.Port && a.IP.Equal(b.IP)
}

// candidateWithUpnpPort combines a peer's observed public IP with the external
// port it mapped on its gateway. UPnP is IPv4-only, so IPv6 candidates and
// malformed ports are ignored rather than guessed at.
func candidateWithUpnpPort(candidate string, port int) string {
	if candidate == "" || port <= 0 || port > 65535 {
		return ""
	}
	host, _, err := net.SplitHostPort(candidate)
	if err != nil {
		return ""
	}
	ip := net.ParseIP(host)
	if ip == nil || ip.To4() == nil {
		return ""
	}
	return net.JoinHostPort(host, strconv.Itoa(port))
}

// renewUPnP keeps the gateway lease alive and removes it when the agent
// stops. Renewal failures are logged and retried: the mapping may still be
// present, and the observed NAT address remains the primary direct path.
func renewUPnP(ctx context.Context, log *slog.Logger, mapper *upnp.Mapper) {
	ticker := time.NewTicker(upnp.DefaultLease / 2)
	defer ticker.Stop()
	defer mapper.Close()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			renewCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
			err := mapper.Renew(renewCtx)
			cancel()
			if err != nil {
				log.Warn("UPnP mapping renewal failed",
					"port", mapper.ExternalPort(), "error", err)
				continue
			}
			log.Debug("UPnP mapping renewed", "port", mapper.ExternalPort())
		}
	}
}

func cloneAddr(a *net.UDPAddr) *net.UDPAddr {
	if a == nil {
		return nil
	}
	out := *a
	out.IP = append(net.IP(nil), a.IP...)
	return &out
}

func resolveUDP(text string) (*net.UDPAddr, error) {
	if text == "" {
		return nil, nil
	}
	addr, err := net.ResolveUDPAddr("udp", text)
	if err != nil {
		return nil, fmt.Errorf("resolve %q: %w", text, err)
	}
	return addr, nil
}

// selectRelayAddr picks the first advertised relay endpoint this socket can
// reach. Names are expanded to every address they publish, so one host name
// can carry both the IPv4 and IPv6 listener of the same relay.
func selectRelayAddr(local *net.UDPAddr, endpoints []string) *net.UDPAddr {
	var localIP net.IP
	if local != nil {
		localIP = local.IP
	}
	for _, text := range endpoints {
		text = strings.TrimSpace(text)
		if text == "" {
			continue
		}
		host, portText, err := net.SplitHostPort(text)
		if err != nil {
			continue
		}
		port, err := strconv.Atoi(portText)
		if err != nil || port <= 0 || port > 65535 {
			continue
		}
		host = strings.Trim(host, "[]")
		var ips []net.IP
		if ip := net.ParseIP(host); ip != nil {
			ips = append(ips, ip)
		} else {
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			addrs, err := net.DefaultResolver.LookupIPAddr(ctx, host)
			cancel()
			if err != nil {
				continue
			}
			for _, addr := range addrs {
				ips = append(ips, addr.IP)
			}
		}
		for _, ip := range ips {
			if ip == nil || ip.IsUnspecified() || ip.IsMulticast() {
				continue
			}
			if familyAllowed(localIP, ip) {
				return &net.UDPAddr{IP: ip, Port: port}
			}
		}
	}
	return nil
}

// resolveUDPAll expands every endpoint to all addresses its host name
// resolves to, because the relay may answer from any address family while the
// primary address is pinned to one.
func resolveUDPAll(endpoints []string) []*net.UDPAddr {
	seen := make(map[string]struct{})
	var out []*net.UDPAddr
	add := func(addr *net.UDPAddr) {
		if addr == nil || addr.IP == nil {
			return
		}
		key := net.JoinHostPort(addr.IP.String(), strconv.Itoa(addr.Port))
		if _, ok := seen[key]; ok {
			return
		}
		seen[key] = struct{}{}
		out = append(out, addr)
	}
	for _, text := range endpoints {
		host, portText, err := net.SplitHostPort(text)
		if err != nil {
			addr, resolveErr := resolveUDP(text)
			if resolveErr == nil {
				add(addr)
			}
			continue
		}
		port, err := strconv.Atoi(portText)
		if err != nil || port < 1 || port > 65535 {
			continue
		}
		if ip := net.ParseIP(host); ip != nil {
			add(&net.UDPAddr{IP: ip, Port: port})
			continue
		}
		ips, err := net.LookupIP(host)
		if err != nil {
			continue
		}
		for _, ip := range ips {
			add(&net.UDPAddr{IP: ip, Port: port})
		}
	}
	return out
}

func containsUDPAddr(addrs []*net.UDPAddr, target *net.UDPAddr) bool {
	for _, addr := range addrs {
		if sameAddr(addr, target) {
			return true
		}
	}
	return false
}

func sameUDPAddrSet(a, b []*net.UDPAddr) bool {
	if len(a) != len(b) {
		return false
	}
	for _, addr := range a {
		if !containsUDPAddr(b, addr) {
			return false
		}
	}
	return true
}

func newNonce() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return hex.EncodeToString([]byte(time.Now().Format(time.RFC3339Nano)))
	}
	return hex.EncodeToString(b[:])
}
