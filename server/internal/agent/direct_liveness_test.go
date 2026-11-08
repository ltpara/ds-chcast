package agent

import (
	"bufio"
	"context"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func TestUDPDirectLivenessRequiresFreshSelectedPeerAck(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	out, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	peer, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer peer.Close()
	addr := peer.LocalAddr().(*net.UDPAddr)
	v := &ViewerAgent{ctx: ctx, out: out, cfg: ViewerConfig{Mode: ModeAuto}, log: quietLogger(),
		punchTID: "original-punch-id", direct: addr, remote: addr, latched: true,
		relay: &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40000}}
	v.directOK.Store(true)
	old := time.Now().Add(-6 * time.Second)
	v.lastDirectReceive = old
	if !v.checkUDPDirect(time.Now()) {
		t.Fatal("healthy path expired")
	}
	buf := make([]byte, 2048)
	_ = peer.SetReadDeadline(time.Now().Add(time.Second))
	n, _, err := peer.ReadFromUDP(buf)
	if err != nil {
		t.Fatal(err)
	}
	probe, err := wire.Decode(buf[:n])
	if err != nil || probe.Type != wire.TypePunchProbe || len(probe.TID) != punchTIDLen {
		t.Fatalf("probe: %+v %v", probe, err)
	}
	v.confirmPunchAck(addr, wire.Message{TID: v.punchTID})
	v.confirmPunchAck(v.relay, wire.Message{TID: probe.TID})
	if v.lastDirectReceive != old {
		t.Fatal("old or wrong-source ACK refreshed liveness")
	}
	v.confirmPunchAck(addr, wire.Message{TID: probe.TID})
	if !v.lastDirectReceive.After(old) {
		t.Fatal("matching selected-peer ACK ignored")
	}
	accepted := v.lastDirectReceive
	v.confirmPunchAck(addr, wire.Message{TID: probe.TID})
	if v.lastDirectReceive != accepted {
		t.Fatal("replayed ACK refreshed liveness")
	}
	if v.checkUDPDirect(accepted.Add(directPathIdle)) {
		t.Fatal("silent path remained alive")
	}
	if !v.fellBackNow() || v.PathInfo().Path != "relay" {
		t.Fatal("UDP loss did not select relay")
	}
	if v.onDirectPacket(addr) {
		t.Fatal("late packet revived failed path")
	}
}

func TestUDPDirectOnlyLossWaitsForNewSession(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40001}
	v := &ViewerAgent{ctx: ctx, cfg: ViewerConfig{Mode: ModeDirect}, log: quietLogger(), remote: addr, direct: addr, latched: true}
	v.directOK.Store(true)
	v.lastDirectReceive = time.Now().Add(-directPathIdle)
	if v.checkUDPDirect(time.Now()) {
		t.Fatal("silent direct-only path stayed alive")
	}
	if v.fellBackNow() || v.PathInfo().Path != "connecting" || v.PathInfo().Remote != "" {
		t.Fatalf("failed path = %+v", v.PathInfo())
	}
	if v.onDirectPacket(addr) {
		t.Fatal("late packet reselected the stale address")
	}
}

// Exercise the real TCP read deadline while the peer silently consumes writes
// but never sends a frame (neither EOF nor an immediate send error).
func TestTCPBlackholeEndsBeforeQUICDisconnect(t *testing.T) {
	client, peer := net.Pipe()
	defer client.Close()
	defer peer.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	v := &ViewerAgent{ctx: ctx, cfg: ViewerConfig{Mode: ModeAuto}, log: quietLogger(),
		relay: &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40000}}
	if !v.attachTCP(client) {
		t.Fatal("attach failed")
	}
	go func() {
		r := carrier.NewFrameReader(peer)
		for {
			if _, err := r.ReadFrame(); err != nil {
				return
			}
		}
	}()
	start := time.Now()
	v.tcpReadLoop(client, bufio.NewReader(client))
	v.detachTCP(client)
	v.fallBackToRelay("test blackhole")
	if elapsed := time.Since(start); elapsed < 7*time.Second || elapsed >= 12*time.Second {
		t.Fatalf("blackhole detection = %v", elapsed)
	}
	if !v.fellBackNow() || v.PathInfo().Path != "relay" {
		t.Fatalf("blackhole recovery = %+v", v.PathInfo())
	}
	if v.onDirectPacket(v.relay) {
		t.Fatal("late direct path accepted after TCP loss")
	}
}

func TestNewViewerLooksUpChangedHostEndpoint(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	first := startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())
	cfg := ViewerConfig{ServerURL: s.URL, ServerKey: testutil.ServerKey, HostID: "devbox", ListenAddr: "127.0.0.1:0", Mode: ModeTCP, Logger: quietLogger()}
	old, err := StartViewer(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(old.Close)
	viewerRoundTrip(t, old, []byte("before-host-restart"))
	first.Close()
	second := startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())
	if first.conn.LocalAddr().String() == second.conn.LocalAddr().String() {
		t.Fatal("host endpoint did not change")
	}
	old.Close()
	fresh, err := StartViewer(ctx, cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(fresh.Close)
	viewerRoundTrip(t, fresh, []byte("after-host-restart"))
	if !sameAddr(fresh.direct, second.conn.LocalAddr().(*net.UDPAddr)) {
		t.Fatalf("lookup retained stale UDP endpoint: %v", fresh.direct)
	}
	if fresh.PathInfo().Transport != "tcp" {
		t.Fatalf("fresh path: %+v", fresh.PathInfo())
	}
}
