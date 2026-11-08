package agent

import (
	"bufio"
	"context"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func TestDirectConnectTimeoutRustDesk(t *testing.T) {
	cases := []struct {
		name, peer, own      string
		local, relay, failed bool
		punch, want          time.Duration
	}{
		{"local before no relay", "", "", true, false, false, 2 * time.Second, time.Second},
		{"peer symmetric before no relay", NATSymmetric, "", false, false, false, 2 * time.Second, time.Second},
		{"no relay", "", NATSymmetric, false, false, true, 2 * time.Second, 18 * time.Second},
		{"both asymmetric", NATAsymmetric, NATAsymmetric, false, true, false, 2 * time.Second, 18 * time.Second},
		{"asymmetric previous failure", NATAsymmetric, NATAsymmetric, false, true, true, 2 * time.Second, 12 * time.Second},
		{"own symmetric", NATAsymmetric, NATSymmetric, false, true, false, 2 * time.Second, time.Second},
		{"unknown peer own symmetric", "", NATSymmetric, false, true, false, 2 * time.Second, 12 * time.Second},
		{"unknown previous failure", "", "", false, true, true, 2 * time.Second, 6 * time.Second},
		{"minimum", "", "", false, true, true, time.Millisecond, time.Second},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := directConnectTimeout(tc.punch, tc.peer, tc.own, tc.local, tc.relay, tc.failed); got != tc.want {
				t.Fatalf("timeout = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestCoordinateTCPRetriesSameConnectionWithoutLosingPartialFrame(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	msg := wire.Message{Type: wire.TypeTCPPunch, Role: wire.RoleViewer, HostID: "peer", Nonce: "one-nonce"}
	got := make(chan tcpCoordinationReply, 1)
	go func() {
		reply, err := coordinateTCP(ctx, client, msg, 30*time.Millisecond)
		got <- tcpCoordinationReply{reply, err}
	}()
	br := bufio.NewReader(server)
	first, err := readHandshake(br)
	if err != nil {
		t.Fatal(err)
	}
	// Start the response header but deliver the remainder after a retry.
	framedClient, framedServer := net.Pipe()
	defer framedClient.Close()
	go func() {
		defer framedServer.Close()
		_ = writeHandshake(framedServer, wire.Message{Type: wire.TypeTCPPunch, OK: true, Nonce: msg.Nonce})
	}()
	var header [1]byte
	if _, err := framedClient.Read(header[:]); err != nil {
		t.Fatal(err)
	}
	if _, err := server.Write(header[:]); err != nil {
		t.Fatal(err)
	}
	second, err := readHandshake(br)
	if err != nil || first.Nonce != second.Nonce || first.Timestamp != second.Timestamp || second.HostID != msg.HostID {
		t.Fatalf("retry replaced request: %+v %+v %v", first, second, err)
	}
	buf := make([]byte, 4096)
	for {
		n, err := framedClient.Read(buf)
		if n > 0 {
			if _, e := server.Write(buf[:n]); e != nil {
				t.Fatal(e)
			}
		}
		if err != nil {
			break
		}
	}
	select {
	case result := <-got:
		if result.err != nil || !result.msg.OK {
			t.Fatalf("partial response lost: %+v", result)
		}
	case <-ctx.Done():
		t.Fatal("coordination did not finish")
	}
}

func TestDirectHistorySurvivesNewViewerAndClearsOnSuccess(t *testing.T) {
	cfg := ViewerConfig{ServerURL: "https://server.example", HostID: "../peer", DirectHistoryDir: t.TempDir()}
	newViewer := func() *ViewerAgent { v := &ViewerAgent{cfg: cfg, log: quietLogger()}; v.initDirectHistory(); return v }
	v := newViewer()
	v.recordDirectResult(true)
	v = newViewer()
	if !v.directFailed {
		t.Fatal("new viewer forgot direct failure")
	}
	v.recordDirectResult(false)
	if newViewer().directFailed {
		t.Fatal("direct success did not clear history")
	}
	cfg.HostID = "different-peer"
	if newViewer().directFailed {
		t.Fatal("history crossed peer boundaries")
	}
}

func TestUDPWinnerCancelsPendingTCPAndCannotSwitchAgain(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	v := &ViewerAgent{ctx: context.Background(), directCtx: ctx, directCancel: cancel,
		cfg: ViewerConfig{Mode: ModeAuto}, log: quietLogger()}
	first := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40001}
	v.onDirectPacket(first)
	if ctx.Err() == nil {
		t.Fatal("TCP attempt was not cancelled")
	}
	v.onDirectPacket(&net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002})
	if !sameAddr(v.remote, first) {
		t.Fatal("late losing candidate changed the selected path")
	}
}
