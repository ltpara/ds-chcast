package agent

import (
	"context"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func startEcho(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	go func() {
		buf := make([]byte, 65535)
		for {
			n, src, err := conn.ReadFromUDP(buf)
			if err != nil {
				return
			}
			conn.WriteToUDP(buf[:n], src)
		}
	}()
	return conn
}

func startHostAgent(t *testing.T, ctx context.Context, serverURL, echoAddr string) *HostAgent {
	t.Helper()
	host, err := StartHost(ctx, HostConfig{
		ServerURL:  serverURL,
		ServerKey:  testutil.ServerKey,
		ID:         "devbox",
		Key:        "host-secret",
		LocalAddr:  echoAddr,
		ListenAddr: "127.0.0.1:0",
		Keepalive:  300 * time.Millisecond,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start host agent: %v", err)
	}
	t.Cleanup(host.Close)
	readyCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	if err := host.WaitReady(readyCtx); err != nil {
		t.Fatalf("host agent readiness: %v", err)
	}
	return host
}

func testRoundTrip(t *testing.T, ctx context.Context, s *testutil.Server, mode string) {
	t.Helper()
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          mode,
		DirectTimeout: 1 * time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)

	client, err := net.DialUDP("udp", nil, viewer.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	payload := []byte("chast-quic-datagram-" + mode)
	deadline := time.Now().Add(4 * time.Second)
	buf := make([]byte, 2048)
	for {
		client.Write(payload)
		client.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		n, err := client.Read(buf)
		if err == nil {
			if string(buf[:n]) != string(payload) {
				t.Fatalf("echo = %q, want %q", buf[:n], payload)
			}
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("%s path did not deliver a datagram: %v", mode, err)
		}
	}
}

func TestDirectPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	testRoundTrip(t, ctx, s, ModeDirect)
}

func TestRelayPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	testRoundTrip(t, ctx, s, ModeRelay)
}

func TestAutoPathThroughAgents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	testRoundTrip(t, ctx, s, ModeAuto)
}

func TestViewerRequiresOnlineHost(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	_, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "ghost",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeAuto,
		Logger:     quietLogger(),
	})
	if err == nil {
		t.Fatal("viewer started for an unknown host")
	}
}

func TestAutoFallbackDoesNotFlap(t *testing.T) {
	direct := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40001}
	relay := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002}
	v := &ViewerAgent{
		cfg:    ViewerConfig{Mode: ModeAuto},
		ctx:    context.Background(),
		direct: direct,
		remote: direct,
		relay:  relay,
		log:    quietLogger(),
	}
	v.onDirectTimeout()
	if !v.fellBack || !sameAddr(v.remote, relay) {
		t.Fatalf("timeout did not select the relay: %+v", v)
	}
	v.onDirectPacket(direct)
	if !sameAddr(v.remote, relay) {
		t.Fatalf("a late direct packet switched back to direct")
	}
}
