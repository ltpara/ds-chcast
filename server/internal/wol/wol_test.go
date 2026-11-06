package wol

import (
	"bytes"
	"net"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestNormalizeMAC(t *testing.T) {
	valid := map[string]string{
		"AA:BB:CC:DD:EE:FF":   "AA:BB:CC:DD:EE:FF",
		"aa-bb-cc-dd-ee-ff":   "AA:BB:CC:DD:EE:FF",
		"AABB.CCDD.EEFF":      "AA:BB:CC:DD:EE:FF",
		"aabbccddeeff":        "AA:BB:CC:DD:EE:FF",
		" AA BB CC DD EE FF ": "AA:BB:CC:DD:EE:FF",
	}
	for input, want := range valid {
		got, ok := NormalizeMAC(input)
		if !ok || got != want {
			t.Fatalf("NormalizeMAC(%q) = %q, %v; want %q", input, got, ok, want)
		}
	}
	for _, input := range []string{"", "AA:BB:CC:DD:EE", "GG:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:FF:00"} {
		if got, ok := NormalizeMAC(input); ok {
			t.Fatalf("NormalizeMAC(%q) accepted as %q", input, got)
		}
	}
}

func TestMagicPacket(t *testing.T) {
	packet, err := MagicPacket("01:23:45:67:89:ab")
	if err != nil {
		t.Fatal(err)
	}
	if len(packet) != PacketLen {
		t.Fatalf("packet length = %d", len(packet))
	}
	if !bytes.Equal(packet[:6], []byte{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}) {
		t.Fatalf("prefix = %x", packet[:6])
	}
	address := []byte{0x01, 0x23, 0x45, 0x67, 0x89, 0xab}
	for repeat := 0; repeat < 16; repeat++ {
		if !bytes.Equal(packet[6+repeat*6:12+repeat*6], address) {
			t.Fatalf("repeat %d = %x", repeat, packet[6+repeat*6:12+repeat*6])
		}
	}
	if _, err := MagicPacket("not-a-mac"); err == nil {
		t.Fatal("invalid MAC accepted")
	}
}

func TestDirectedTargetsExcludeLimitedBroadcast(t *testing.T) {
	targets := DirectedTargets(9)
	for _, target := range targets {
		if target == "255.255.255.255:9" {
			t.Fatalf("limited broadcast in directed targets %v", targets)
		}
		if strings.HasPrefix(target, "127.") {
			t.Fatalf("loopback target %q", target)
		}
	}
	if got := DirectedTargets(0); len(got) != 0 {
		for _, target := range got {
			if !strings.HasSuffix(target, ":9") {
				t.Fatalf("default port target = %q", target)
			}
		}
	}
}

func TestNormalizeTargets(t *testing.T) {
	got := NormalizeTargets([]string{
		"192.168.1.255:9",
		" 192.168.1.255:9 ",
		"10.0.0.255:9",
		"255.255.255.255:9",
		"0.0.0.0:9",
		"224.0.0.1:9",
		"[fe80::1]:9",
		"relay.example:9",
		"192.168.1.255:0",
		"192.168.1.255:70000",
	})
	want := []string{"192.168.1.255:9", "10.0.0.255:9"}
	if len(got) != len(want) {
		t.Fatalf("NormalizeTargets = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("NormalizeTargets[%d] = %q, want %q", i, got[i], want[i])
		}
	}
	many := make([]string, 0, maxTargets+4)
	for i := 0; i < maxTargets+4; i++ {
		many = append(many, "10.0."+strconv.Itoa(i)+".255:9")
	}
	if got := NormalizeTargets(many); len(got) != maxTargets {
		t.Fatalf("NormalizeTargets cap = %d, want %d", len(got), maxTargets)
	}
}

func TestSendToLoopback(t *testing.T) {
	listener, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	want, err := MagicPacket("AA:BB:CC:DD:EE:FF")
	if err != nil {
		t.Fatal(err)
	}
	sent, err := SendTo("AA:BB:CC:DD:EE:FF", []string{listener.LocalAddr().String()})
	if err != nil || sent != 1 {
		t.Fatalf("SendTo = %d, %v", sent, err)
	}
	buf := make([]byte, 256)
	_ = listener.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err := listener.ReadFromUDP(buf)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(buf[:n], want) {
		t.Fatalf("received %d bytes that differ from the magic packet", n)
	}
	if _, err := SendTo("AA:BB:CC:DD:EE:FF", nil); err == nil {
		t.Fatal("empty target list accepted")
	}
}
