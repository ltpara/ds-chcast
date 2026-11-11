package lanmask

import (
	"net"
	"testing"
)

func TestParseAcceptsCIDRsAndBareAddresses(t *testing.T) {
	mask, err := Parse("192.168.0.0/16, 10.8.0.1 ,fd00::/8")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if !mask.Enabled() {
		t.Fatal("mask is not enabled")
	}
	cases := []struct {
		ip   string
		want bool
	}{
		{"192.168.1.20", true},
		{"10.8.0.1", true},
		{"10.8.0.2", false},
		{"fd00::1", true},
		{"2001:db8::1", false},
		{"::ffff:192.168.5.6", true},
		{"203.0.113.7", false},
	}
	for _, tc := range cases {
		if got := mask.Contains(net.ParseIP(tc.ip)); got != tc.want {
			t.Errorf("Contains(%s) = %v, want %v", tc.ip, got, tc.want)
		}
	}
}

func TestParseEmptyDisables(t *testing.T) {
	mask, err := Parse(" , ")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if mask.Enabled() {
		t.Fatal("empty text enabled the mask")
	}
	if mask.Contains(net.IPv4(10, 0, 0, 1)) {
		t.Fatal("disabled mask classified an address")
	}
}

func TestParseRejectsGarbage(t *testing.T) {
	for _, text := range []string{"192.168.0.0/33", "not-an-ip", "10.0.0.0/8/8"} {
		if mask, err := Parse(text); err == nil {
			t.Fatalf("Parse(%q) = %+v, want an error", text, mask)
		}
	}
}

func TestContainsAddr(t *testing.T) {
	mask, err := Parse("192.168.0.0/16")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if !mask.ContainsAddr("192.168.4.5:41000") {
		t.Fatal("host:port inside the mask was rejected")
	}
	if mask.ContainsAddr("198.51.100.4:41000") {
		t.Fatal("host:port outside the mask was accepted")
	}
	if mask.ContainsAddr("relay.example.com:41000") {
		t.Fatal("a host name was classified")
	}
}

func TestSameIntranet(t *testing.T) {
	mask, err := Parse("10.0.0.0/8")
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if !mask.SameIntranet(net.ParseIP("10.1.2.3"), net.ParseIP("10.9.8.7")) {
		t.Fatal("two LAN peers were not paired")
	}
	if mask.SameIntranet(net.ParseIP("10.1.2.3"), net.ParseIP("198.51.100.4")) {
		t.Fatal("a LAN and a WAN peer were paired")
	}
	if !mask.SameIntranet(net.ParseIP("198.51.100.4"), net.ParseIP("198.51.100.4")) {
		t.Fatal("identical addresses must pair like hbbs's same-IP rule")
	}
	if mask.SameIntranet(nil, net.ParseIP("198.51.100.4")) {
		t.Fatal("a missing address paired")
	}
	var disabled *Mask
	if !disabled.SameIntranet(net.ParseIP("198.51.100.4"), net.ParseIP("198.51.100.4")) {
		t.Fatal("the same-IP rule must survive a disabled mask")
	}
	if disabled.SameIntranet(net.ParseIP("10.1.2.3"), net.ParseIP("10.9.8.7")) {
		t.Fatal("a disabled mask paired two addresses")
	}
}

func TestAddrIP(t *testing.T) {
	if ip := AddrIP("192.0.2.9:41000"); !ip.Equal(net.ParseIP("192.0.2.9")) {
		t.Fatalf("host:port = %v", ip)
	}
	if ip := AddrIP("2001:db8::1"); !ip.Equal(net.ParseIP("2001:db8::1")) {
		t.Fatalf("bare v6 = %v", ip)
	}
	if ip := AddrIP("relay.example.com:41000"); ip != nil {
		t.Fatalf("host name = %v", ip)
	}
}
