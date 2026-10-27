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

func TestSplitByFamily(t *testing.T) {
	v4, v6 := splitByFamily([]string{
		"10.0.0.9:4000", "[fd00::9]:4000", "192.0.2.9:4000", "[2001:db8::9]:4000",
	})
	if len(v4) != 2 || v4[0] != "10.0.0.9:4000" || v4[1] != "192.0.2.9:4000" {
		t.Fatalf("ipv4 candidates: %+v", v4)
	}
	if len(v6) != 2 || v6[0] != "[fd00::9]:4000" || v6[1] != "[2001:db8::9]:4000" {
		t.Fatalf("ipv6 candidates: %+v", v6)
	}
}

func TestSelectRelayAddrMatchesSocketFamily(t *testing.T) {
	endpoints := []string{"127.0.0.1:41000", "[::1]:41000"}
	v4 := selectRelayAddr(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)}, endpoints)
	if v4 == nil || v4.IP.To4() == nil || v4.Port != 41000 {
		t.Fatalf("IPv4 socket selected %v", v4)
	}
	v6 := selectRelayAddr(&net.UDPAddr{IP: net.IPv6loopback}, endpoints)
	if v6 == nil || v6.IP.To4() != nil || v6.Port != 41000 {
		t.Fatalf("IPv6 socket selected %v", v6)
	}
	if got := selectRelayAddr(&net.UDPAddr{IP: net.IPv6loopback}, endpoints[:1]); got != nil {
		t.Fatalf("IPv6 socket fell back to an IPv4 relay: %v", got)
	}
	if got := selectRelayAddr(nil, []string{"", "garbage", "127.0.0.1:0", "[::1]:41000"}); got == nil || got.IP.To4() != nil {
		t.Fatalf("malformed endpoints were not skipped: %v", got)
	}
	// A name may expand to both families; the chosen address must still match
	// the socket. Skip only if this host does not resolve localhost to IPv6.
	if got := selectRelayAddr(&net.UDPAddr{IP: net.IPv6loopback}, []string{"localhost:41000"}); got != nil && got.IP.To4() != nil {
		t.Fatalf("name expansion ignored the socket family: %v", got)
	}
}

func TestAcceptsControlFromBothRendezvousFamilies(t *testing.T) {
	h := &HostAgent{}
	punch := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 18444}
	punch6 := &net.UDPAddr{IP: net.IPv6loopback, Port: 18444}
	if !h.acceptsControl(punch, punch, punch6) {
		t.Fatal("IPv4 rendezvous source was rejected")
	}
	if !h.acceptsControl(punch6, punch, punch6) {
		t.Fatal("IPv6 rendezvous source was rejected")
	}
	if h.acceptsControl(&net.UDPAddr{IP: net.IPv6loopback, Port: 9999}, punch, punch6) {
		t.Fatal("an unrelated source was accepted")
	}
}

func TestCandidateWithUpnpPort(t *testing.T) {
	cases := []struct {
		name      string
		candidate string
		port      int
		want      string
	}{
		{"ipv4 mapping", "198.51.100.7:51234", 40000, "198.51.100.7:40000"},
		{"no mapping", "198.51.100.7:51234", 0, ""},
		{"ipv6 gateway is not upnp", "[2001:db8::7]:51234", 40000, ""},
		{"bad candidate", "198.51.100.7", 40000, ""},
		{"out of range", "198.51.100.7:51234", 70000, ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := candidateWithUpnpPort(tc.candidate, tc.port); got != tc.want {
				t.Fatalf("candidateWithUpnpPort(%q, %d) = %q, want %q",
					tc.candidate, tc.port, got, tc.want)
			}
		})
	}
}
