package api

import (
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestPunchV6Derivation(t *testing.T) {
	s := &server{cfg: Config{PunchAddr: "198.51.100.7:8444"}}
	base := httptest.NewRequest(http.MethodPost, "/v1/hosts", nil)
	local := func(ip string) *http.Request {
		ctx := context.WithValue(base.Context(), http.LocalAddrContextKey,
			&net.TCPAddr{IP: net.ParseIP(ip), Port: 8443})
		return base.Clone(ctx)
	}

	if got := s.punchV6(local("2001:db8::7")); got != "[2001:db8::7]:8444" {
		t.Fatalf("ipv6 local address: %q", got)
	}
	if got := s.punchV6(local("192.0.2.7")); got != "" {
		t.Fatalf("ipv4 local address produced %q", got)
	}
	if got := s.punchV6(local("::")); got != "" {
		t.Fatalf("wildcard local address produced %q", got)
	}

	s.cfg.PunchAddrV6 = "[2001:db8::9]:8444"
	if got := s.punchV6(local("192.0.2.7")); got != "[2001:db8::9]:8444" {
		t.Fatalf("explicit punch_v6: %q", got)
	}
}
