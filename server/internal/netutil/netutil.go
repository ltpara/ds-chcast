// Package netutil holds the bootstrap helpers shared by the chast server
// binaries: relay port ranges, public address detection and TLS loading.
package netutil

import (
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"strconv"
	"strings"
)

func ParsePorts(text string) ([]int, error) {
	var ports []int
	for _, part := range strings.Split(text, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if start, end, ok := strings.Cut(part, "-"); ok {
			from, err := strconv.Atoi(strings.TrimSpace(start))
			if err != nil {
				return nil, fmt.Errorf("relay-ports %q: %w", part, err)
			}
			to, err := strconv.Atoi(strings.TrimSpace(end))
			if err != nil {
				return nil, fmt.Errorf("relay-ports %q: %w", part, err)
			}
			if from < 1 || to > 65535 || from > to {
				return nil, fmt.Errorf("relay-ports %q is not an ascending range within 1..65535", part)
			}
			for p := from; p <= to; p++ {
				ports = append(ports, p)
			}
		} else {
			p, err := strconv.Atoi(part)
			if err != nil || p < 1 || p > 65535 {
				return nil, fmt.Errorf("relay port %q is not 1..65535", part)
			}
			ports = append(ports, p)
		}
	}
	if len(ports) == 0 {
		return nil, errors.New("--relay-ports must name at least one port")
	}
	if len(ports) > 4096 {
		return nil, errors.New("too many relay ports (limit 4096)")
	}
	return ports, nil
}

// DetectAdvertise returns the first non-loopback IPv4 address, which is the
// address peers can usually reach on a single-homed host.
func DetectAdvertise() (string, error) {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return "", fmt.Errorf("discover a public address: %w; pass --advertise", err)
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok || ipnet.IP.IsLoopback() {
			continue
		}
		if ip4 := ipnet.IP.To4(); ip4 != nil {
			return ip4.String(), nil
		}
	}
	return "", errors.New("no non-loopback IPv4 address found; pass --advertise <ip-or-name>")
}

// LoadTLSConfig enables HTTPS only when both files are present. Loading the
// pair at startup surfaces a bad certificate before the listener opens.
func LoadTLSConfig(certFile, keyFile string) (*tls.Config, error) {
	if certFile == "" && keyFile == "" {
		return nil, nil
	}
	if certFile == "" || keyFile == "" {
		return nil, errors.New("--tls-cert and --tls-key must be given together")
	}
	cert, err := tls.LoadX509KeyPair(certFile, keyFile)
	if err != nil {
		return nil, fmt.Errorf("load TLS certificate: %w", err)
	}
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
	}, nil
}
