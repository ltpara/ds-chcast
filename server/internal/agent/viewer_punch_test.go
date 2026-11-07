package agent

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// A punch is confirmed only by the acknowledgement of this round's own
// transaction id. A probe from the peer must be answered by echoing its id, so
// the host can prove the path carries traffic in both directions.
func TestViewerPunchProbeAndAck(t *testing.T) {
	local, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer local.Close()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	v := &ViewerAgent{
		conn:     local,
		out:      conn,
		ctx:      ctx,
		log:      quietLogger(),
		nonce:    "viewer-nonce-1234",
		punchTID: "aaaaaaaaaaaaaaaa",
	}
	v.punchActive.Store(true)
	go v.readLoop()
	target := conn.LocalAddr().(*net.UDPAddr)

	host, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer host.Close()
	hostAddr := host.LocalAddr().(*net.UDPAddr)

	host.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchProbe, TID: "bbbbbbbbbbbbbbbb",
	}), target)
	host.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, wire.MaxControlSize)
	n, _, err := host.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("probe was not answered: %v", err)
	}
	ack, err := wire.Decode(buf[:n])
	if err != nil || ack.Type != wire.TypePunchAck || ack.TID != "bbbbbbbbbbbbbbbb" {
		t.Fatalf("probe answer = %+v, err=%v", ack, err)
	}

	// An acknowledgement for another round proves nothing about this one.
	host.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchAck, TID: "cccccccccccccccc",
	}), target)
	time.Sleep(200 * time.Millisecond)
	if v.directOK.Load() {
		t.Fatal("a foreign transaction id confirmed the direct path")
	}

	host.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchAck, TID: v.punchTID,
	}), target)
	deadline := time.Now().Add(2 * time.Second)
	for !v.directOK.Load() {
		if time.Now().After(deadline) {
			t.Fatal("the matching acknowledgement did not confirm the direct path")
		}
		time.Sleep(10 * time.Millisecond)
	}
	v.mu.Lock()
	direct := cloneAddr(v.direct)
	v.mu.Unlock()
	if direct == nil || direct.Port != hostAddr.Port {
		t.Fatalf("direct path = %v, want %s", direct, hostAddr)
	}
}

// The punch request reports the viewer's own global IPv6 endpoint so the host
// can punch back over IPv6 even when the rendezvous port is IPv4-only.
func TestViewerPunchRequestReportsSelfV6(t *testing.T) {
	out, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	server, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	v := &ViewerAgent{
		out:    out,
		punch:  server.LocalAddr().(*net.UDPAddr),
		cfg:    ViewerConfig{HostID: "devbox"},
		nonce:  "viewer-nonce-1234",
		selfV6: "[2001:db8::5]:4000",
	}
	v.sendPunchRequest()
	_ = server.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, wire.MaxControlSize)
	n, _, err := server.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("punch request was not sent: %v", err)
	}
	msg, err := wire.Decode(buf[:n])
	if err != nil || msg.Type != wire.TypePunchRequest || msg.HostID != "devbox" || msg.Nonce != v.nonce {
		t.Fatalf("punch request = %+v, err=%v", msg, err)
	}
	if msg.SelfV6 != "[2001:db8::5]:4000" {
		t.Fatalf("self IPv6 endpoint = %q", msg.SelfV6)
	}
}

// The punch request also carries this viewer's LAN endpoints. The rendezvous
// service forwards them to the host only when both peers share a public
// address; sending them lets the host punch a path a NAT without hairpinning
// cannot provide.
func TestViewerPunchRequestReportsLocalAddrs(t *testing.T) {
	out, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	server, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()
	v := &ViewerAgent{
		out:          out,
		punch:        server.LocalAddr().(*net.UDPAddr),
		cfg:          ViewerConfig{HostID: "devbox"},
		nonce:        "viewer-nonce-1234",
		localAddrs:   []string{"192.168.1.20:55000"},
		localAddrsV6: []string{"[fd00::20]:55000"},
	}
	v.sendPunchRequest()
	_ = server.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, wire.MaxControlSize)
	n, _, err := server.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("punch request was not sent: %v", err)
	}
	msg, err := wire.Decode(buf[:n])
	if err != nil {
		t.Fatal(err)
	}
	if len(msg.LocalAddrs) != 1 || msg.LocalAddrs[0] != "192.168.1.20:55000" {
		t.Fatalf("viewer LAN candidates = %v", msg.LocalAddrs)
	}
	if len(msg.LocalAddrsV6) != 1 || msg.LocalAddrsV6[0] != "[fd00::20]:55000" {
		t.Fatalf("viewer LAN v6 candidates = %v", msg.LocalAddrsV6)
	}
}

// Without a global IPv6 source the viewer must not probe v6 candidates; with
// one it keeps both families.
func TestViewerDropsIPv6CandidatesWithoutGlobalV6(t *testing.T) {
	out, err := net.ListenUDP("udp", &net.UDPAddr{})
	if err != nil {
		t.Skipf("dual-stack socket unavailable: %v", err)
	}
	defer out.Close()
	if out.LocalAddr().(*net.UDPAddr).IP.To4() != nil {
		t.Skip("wildcard socket bound IPv4 only")
	}
	v := &ViewerAgent{out: out}
	texts := []string{"192.0.2.10:5000", "[2001:db8::1]:5000"}
	got := v.mergeDirect(texts)
	if len(got) != 1 || got[0].IP.To4() == nil {
		t.Fatalf("candidates without a global v6 source = %v", got)
	}
	v.selfV6 = "[2001:db8::5]:4000"
	if got := v.mergeDirect(texts); len(got) != 2 {
		t.Fatalf("candidates with a global v6 source = %v", got)
	}
}

// A legacy punch only proves the host path when it carries the nonce this
// viewer generated; otherwise any sender could claim the direct path.
func TestViewerPunchRequiresNonce(t *testing.T) {
	local, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer local.Close()
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	v := &ViewerAgent{
		conn:  local,
		out:   conn,
		ctx:   ctx,
		log:   quietLogger(),
		nonce: "viewer-nonce-1234",
	}
	go v.readLoop()
	go v.localLoop()

	stranger, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer stranger.Close()
	target := conn.LocalAddr().(*net.UDPAddr)

	stranger.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypePunch, Nonce: "not-the-nonce"}), target)
	deadline := time.Now().Add(300 * time.Millisecond)
	for time.Now().Before(deadline) {
		v.mu.Lock()
		direct := cloneAddr(v.direct)
		v.mu.Unlock()
		if direct != nil {
			t.Fatalf("punch with a foreign nonce was accepted from %s", direct)
		}
		time.Sleep(10 * time.Millisecond)
	}

	host, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer host.Close()
	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypePunch, Nonce: v.nonce}), target)
	limit := time.Now().Add(2 * time.Second)
	for time.Now().Before(limit) {
		v.mu.Lock()
		direct := cloneAddr(v.direct)
		v.mu.Unlock()
		if direct != nil {
			if direct.Port != host.LocalAddr().(*net.UDPAddr).Port {
				t.Fatalf("direct path = %s, want %s", direct, host.LocalAddr())
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("punch with the matching nonce was not accepted")
}
