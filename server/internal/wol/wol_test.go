package wol

import (
	"bytes"
	"net"
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

func TestTargetsIncludeLimitedBroadcast(t *testing.T) {
	targets := Targets(9)
	found := false
	for _, target := range targets {
		if target == "255.255.255.255:9" {
			found = true
		}
		if strings.HasPrefix(target, "127.") {
			t.Fatalf("loopback target %q", target)
		}
	}
	if !found {
		t.Fatalf("limited broadcast missing from %v", targets)
	}
	if got := Targets(0); len(got) == 0 || got[0] != "255.255.255.255:9" {
		t.Fatalf("default port targets = %v", got)
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
