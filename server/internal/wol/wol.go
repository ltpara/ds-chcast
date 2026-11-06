// Package wol builds and sends Wake-on-LAN magic packets. Host agents use it to
// relay a wake request to their own link when the viewer is on another network.
package wol

import (
	"errors"
	"fmt"
	"net"
	"strconv"
	"strings"
)

const (
	// Port is the conventional Wake-on-LAN destination port.
	Port = 9
	// PacketLen is six 0xFF bytes followed by the MAC address sixteen times.
	PacketLen = 102
)

// ErrInvalidMAC reports a value that is not twelve hexadecimal digits with
// optional separators.
var ErrInvalidMAC = errors.New("wol: invalid MAC address")

// NormalizeMAC accepts the same forms as the viewer (colons, dashes, dots,
// whitespace or no separators) and returns AA:BB:CC:DD:EE:FF.
func NormalizeMAC(value string) (string, bool) {
	var digits strings.Builder
	digits.Grow(12)
	for _, r := range value {
		switch {
		case r >= '0' && r <= '9':
			digits.WriteRune(r)
		case r >= 'a' && r <= 'f':
			digits.WriteRune(r - 'a' + 'A')
		case r >= 'A' && r <= 'F':
			digits.WriteRune(r)
		case r == ':' || r == '-' || r == '.' || r == ' ' || r == '\t':
		default:
			return "", false
		}
	}
	if digits.Len() != 12 {
		return "", false
	}
	hex := digits.String()
	parts := make([]string, 6)
	for i := range parts {
		parts[i] = hex[i*2 : i*2+2]
	}
	return strings.Join(parts, ":"), true
}

// MagicPacket returns the 102-byte packet for a MAC in any accepted form.
func MagicPacket(mac string) ([]byte, error) {
	normalized, ok := NormalizeMAC(mac)
	if !ok {
		return nil, ErrInvalidMAC
	}
	var address [6]byte
	for i := range address {
		value, err := strconv.ParseUint(normalized[i*3:i*3+2], 16, 8)
		if err != nil {
			return nil, ErrInvalidMAC
		}
		address[i] = byte(value)
	}
	packet := make([]byte, PacketLen)
	for i := 0; i < 6; i++ {
		packet[i] = 0xFF
	}
	for repeat := 0; repeat < 16; repeat++ {
		copy(packet[6+repeat*6:], address[:])
	}
	return packet, nil
}

// Targets returns the limited broadcast address plus every up interface's
// directed broadcast. A magic packet only matters on the target's own link, so
// loopback and interface addresses are intentionally absent.
func Targets(port int) []string {
	if port <= 0 || port > 65535 {
		port = Port
	}
	portText := strconv.Itoa(port)
	seen := make(map[string]bool)
	var targets []string
	add := func(ip net.IP) {
		if ip == nil || ip.IsUnspecified() {
			return
		}
		addr := net.JoinHostPort(ip.String(), portText)
		if seen[addr] {
			return
		}
		seen[addr] = true
		targets = append(targets, addr)
	}
	add(net.IPv4bcast)
	interfaces, err := net.Interfaces()
	if err != nil {
		return targets
	}
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			ipnet, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipnet.IP.To4()
			if ip4 == nil || len(ipnet.Mask) != 4 {
				continue
			}
			broadcast := make(net.IP, 4)
			for i := 0; i < 4; i++ {
				broadcast[i] = ip4[i] | ^ipnet.Mask[i]
			}
			if broadcast.Equal(ip4) {
				continue
			}
			add(broadcast)
		}
	}
	return targets
}

// SendTo writes one magic packet to each target. It returns how many datagrams
// left the host; a partial send counts as success because a single reachable
// broadcast domain is enough and duplicates are harmless.
func SendTo(mac string, targets []string) (int, error) {
	packet, err := MagicPacket(mac)
	if err != nil {
		return 0, err
	}
	if len(targets) == 0 {
		return 0, errors.New("wol: no targets")
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		return 0, fmt.Errorf("wol: open socket: %w", err)
	}
	defer conn.Close()
	// Windows and most Unix kernels refuse a datagram to a broadcast address
	// until SO_BROADCAST is set on the socket.
	if err := enableBroadcast(conn); err != nil {
		return 0, fmt.Errorf("wol: enable broadcast: %w", err)
	}
	sent := 0
	var firstErr error
	for _, target := range targets {
		addr, err := net.ResolveUDPAddr("udp4", target)
		if err != nil {
			if firstErr == nil {
				firstErr = fmt.Errorf("wol: target %q: %w", target, err)
			}
			continue
		}
		if _, err := conn.WriteToUDP(packet, addr); err != nil {
			if firstErr == nil {
				firstErr = fmt.Errorf("wol: send to %q: %w", target, err)
			}
			continue
		}
		sent++
	}
	if sent == 0 {
		return 0, firstErr
	}
	return sent, nil
}
