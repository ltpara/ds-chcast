package agent

import (
	"net"
	"testing"
)

func TestClassifyNAT(t *testing.T) {
	same := &net.UDPAddr{IP: net.IPv4(203, 0, 113, 5), Port: 4000}
	cases := []struct {
		name      string
		primary   *net.UDPAddr
		secondary *net.UDPAddr
		want      string
	}{
		{"missing mapping", same, nil, ""},
		{"stable mapping", same, &net.UDPAddr{IP: net.IPv4(203, 0, 113, 5), Port: 4000}, NATAsymmetric},
		{"port changed per destination", same, &net.UDPAddr{IP: net.IPv4(203, 0, 113, 5), Port: 4001}, NATSymmetric},
		{"different exit address", same, &net.UDPAddr{IP: net.IPv4(198, 51, 100, 9), Port: 4000}, NATUnknown},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := classifyNAT(tc.primary, tc.secondary); got != tc.want {
				t.Fatalf("classifyNAT = %q, want %q", got, tc.want)
			}
		})
	}
}

func TestMergeCandidatesFiltersFamilyAndRanks(t *testing.T) {
	local := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 2000}
	got := mergeCandidates(local,
		[]string{"192.0.2.10:5000"},
		[]string{"10.0.0.5:5000", "10.0.0.5:5000", "[2001:db8::1]:5000", "not-an-address", linkLocal()},
	)
	want := []string{"10.0.0.5:5000", "192.0.2.10:5000"}
	if len(got) != len(want) {
		t.Fatalf("candidates = %v, want %v", got, want)
	}
	for i := range want {
		if got[i].String() != want[i] {
			t.Fatalf("candidate %d = %s, want %s", i, got[i], want[i])
		}
	}
}

func TestMergeCandidatesPrefersIPv6AndSkipsLinkLocal(t *testing.T) {
	got := mergeCandidates(nil,
		[]string{"192.0.2.10:5000", "[2001:db8::1]:5000", linkLocal()},
	)
	if len(got) != 2 || got[0].String() != "[2001:db8::1]:5000" {
		t.Fatalf("candidates = %v", got)
	}
}

func linkLocal() string {
	return "[fe80::1]:5000"
}

func TestLocalCandidatesRespectBoundAddress(t *testing.T) {
	if got := localCandidates(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 5000}); len(got) != 0 {
		t.Fatalf("a loopback-bound socket advertised LAN candidates: %v", got)
	}
	for _, text := range localCandidates(&net.UDPAddr{IP: net.IPv4zero, Port: 5000}) {
		host, _, err := net.SplitHostPort(text)
		if err != nil {
			t.Fatalf("candidate %q: %v", text, err)
		}
		if ip := net.ParseIP(host); ip == nil || ip.To4() == nil {
			t.Fatalf("an IPv4-bound socket advertised %q", text)
		}
	}
}

func TestFamilyAllowed(t *testing.T) {
	if familyAllowed(net.IPv4zero, net.ParseIP("2001:db8::1")) {
		t.Fatal("an IPv4 wildcard socket was allowed to probe IPv6")
	}
	if !familyAllowed(net.IPv6unspecified, net.ParseIP("192.0.2.10")) {
		t.Fatal("a dual-stack wildcard socket refused IPv4")
	}
	if familyAllowed(net.IPv4(127, 0, 0, 1), net.ParseIP("2001:db8::1")) {
		t.Fatal("an IPv4 socket was allowed to probe IPv6")
	}
}
