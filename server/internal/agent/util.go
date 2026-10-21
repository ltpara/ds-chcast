package agent

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net"
	"time"
)

func keyHash(key string) string {
	sum := sha256.Sum256([]byte(key))
	return hex.EncodeToString(sum[:])
}

func sameAddr(a, b *net.UDPAddr) bool {
	return a != nil && b != nil && a.Port == b.Port && a.IP.Equal(b.IP)
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

func newNonce() string {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		return hex.EncodeToString([]byte(time.Now().Format(time.RFC3339Nano)))
	}
	return hex.EncodeToString(b[:])
}
