package relay

import (
	"bytes"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func expectNoDatagram(t *testing.T, conn *net.UDPConn, timeout time.Duration) {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(timeout))
	buf := make([]byte, 2048)
	n, _, err := conn.ReadFromUDP(buf)
	if err == nil {
		t.Fatalf("unexpected %d-byte datagram", n)
	}
	if netErr, ok := err.(net.Error); !ok || !netErr.Timeout() {
		t.Fatalf("read: %v", err)
	}
}

func blocklistFile(t *testing.T, ips ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "blocklist.txt")
	data := ""
	for _, ip := range ips {
		data += ip + "\n"
	}
	if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
		t.Fatalf("write blocklist: %v", err)
	}
	return path
}

func TestRelayUDPRateLimitDropsDatagrams(t *testing.T) {
	m, _, relayAddr, priv := startRelay(t, "devbox", "host-secret-rate")
	if err := m.SetPolicyConfig(PolicyConfig{SingleBandwidthBps: 1024}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}
	payload := make([]byte, 60000)
	payload[0] = 'q'
	if _, err := viewer.WriteToUDP(payload, relayAddr); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	host.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, len(payload)+1)
	n, _, err := host.ReadFromUDP(buf)
	if err != nil || n != len(payload) {
		t.Fatalf("host received %d bytes, %v", n, err)
	}
	if _, err := viewer.WriteToUDP(payload, relayAddr); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	expectNoDatagram(t, host, 300*time.Millisecond)

	stats := m.Stats()
	if stats.Policy.RateDroppedDatagrams != 1 || stats.Policy.RateDroppedBytes != uint64(len(payload)) {
		t.Fatalf("policy counters: %+v", stats.Policy)
	}
	if stats.Ports[0].Dropped != 1 {
		t.Fatalf("port dropped = %d", stats.Ports[0].Dropped)
	}
}

func TestRelayBlocklistRefusesUDPViewer(t *testing.T) {
	m, _, relayAddr, priv := startRelay(t, "devbox", "host-secret-block")
	if err := m.SetPolicyConfig(PolicyConfig{BlocklistFile: blocklistFile(t, "127.0.0.1")}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}
	if _, err := viewer.WriteToUDP([]byte("blocked-viewer"), relayAddr); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	expectNoDatagram(t, host, 300*time.Millisecond)

	stats := m.Stats()
	if stats.Ports[0].Blocked != 1 {
		t.Fatalf("port blocked sessions = %d", stats.Ports[0].Blocked)
	}
	if stats.Policy.BlockedSessions != 1 {
		t.Fatalf("policy blocked sessions = %d", stats.Policy.BlockedSessions)
	}
}

func TestCarrierBlocklistRefusesViewer(t *testing.T) {
	const secret = "host-secret-carrier-block"
	m, _, relayAddr, _ := startRelay(t, "devbox", secret)
	if err := m.SetPolicyConfig(PolicyConfig{BlocklistFile: blocklistFile(t, "127.0.0.1")}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	openHostLink(t, relayAddr, "devbox", secret)

	viewer, _, _ := openRawViewer(t, relayAddr)
	expectClosed(t, viewer)
	stats := m.Stats()
	if stats.Ports[0].Blocked != 1 {
		t.Fatalf("port blocked sessions = %d", stats.Ports[0].Blocked)
	}
}

func TestSharedCarrierBlocklistRefusesViewer(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-shared-block")
	if err := m.SetPolicyConfig(PolicyConfig{BlocklistFile: blocklistFile(t, "127.0.0.1")}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	openSharedHost(t, addr, "devbox", secret)

	viewer, _, _ := openSharedRawViewer(t, addr, "devbox", token)
	expectClosed(t, viewer)
	stats := waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Blocked == 1 })
	if stats.Viewers != 0 {
		t.Fatalf("viewers = %d", stats.Viewers)
	}
}

func TestRelayBlocklistDetachesUDPViewer(t *testing.T) {
	m, _, relayAddr, priv := startRelay(t, "devbox", "host-secret-detach")
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := relayHandshake(t, host, relayAddr, "devbox", priv); !ack.OK {
		t.Fatalf("relay handshake: %+v", ack)
	}
	payload := []byte("viewer-hello")
	if _, err := viewer.WriteToUDP(payload, relayAddr); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	buf := make([]byte, 2048)
	host.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err := host.ReadFromUDP(buf)
	if err != nil || !bytes.Equal(buf[:n], payload) {
		t.Fatalf("host received %q, %v", buf[:n], err)
	}

	if _, ok := m.Policy().Command("Ba 127.0.0.1"); !ok {
		t.Fatal("blocklist-add was rejected")
	}
	time.Sleep(listCheckInterval + 100*time.Millisecond)
	if _, err := host.WriteToUDP([]byte("host-reply"), relayAddr); err != nil {
		t.Fatalf("host send: %v", err)
	}
	expectNoDatagram(t, viewer, 300*time.Millisecond)

	stats := m.Stats()
	if stats.Ports[0].Viewer != "" {
		t.Fatalf("viewer slot still held by %q", stats.Ports[0].Viewer)
	}
	if stats.Ports[0].Dropped != 1 || stats.Policy.BlockedSessions != 1 {
		t.Fatalf("blocked counters: port dropped=%d policy=%d", stats.Ports[0].Dropped, stats.Policy.BlockedSessions)
	}
	if stats.Policy.Sessions != 0 {
		t.Fatalf("policy sessions = %d", stats.Policy.Sessions)
	}

	if _, ok := m.Policy().Command("Br all"); !ok {
		t.Fatal("blocklist-remove was rejected")
	}
	if _, err := viewer.WriteToUDP(payload, relayAddr); err != nil {
		t.Fatalf("viewer send: %v", err)
	}
	host.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err = host.ReadFromUDP(buf)
	if err != nil || !bytes.Equal(buf[:n], payload) {
		t.Fatalf("host received %q after clearing the blocklist, %v", buf[:n], err)
	}
}
