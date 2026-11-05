package agent

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

// A punch only proves the host path when it carries the nonce this viewer
// generated; otherwise any sender could claim the direct path.
func TestViewerPunchRequiresNonce(t *testing.T) {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	v := &ViewerAgent{
		conn:  conn,
		ctx:   ctx,
		log:   quietLogger(),
		nonce: "viewer-nonce-1234",
	}
	go v.readLoop()

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
