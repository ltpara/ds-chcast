package agent

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// startEchoCarrier stands in for chast-host's CarrierServer: it accepts the
// agent's raw carrier connection and echoes every frame.
func startEchoCarrier(t *testing.T) net.Listener {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("carrier listener: %v", err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				br := bufio.NewReader(conn)
				line, err := carrier.ReadLine(br, 64)
				if err != nil || line+"\n" != carrier.MagicRaw {
					return
				}
				frames := carrier.NewFrameReader(br)
				send := carrier.NewFrameWriter(conn)
				for {
					payload, err := frames.ReadFrame()
					if err != nil {
						return
					}
					if len(payload) == 0 {
						continue
					}
					if err := send.WriteFrame(payload); err != nil {
						return
					}
				}
			}(conn)
		}
	}()
	return listener
}

func startHostAgentCarrier(t *testing.T, ctx context.Context, serverURL, echoAddr, carrierAddr string) *HostAgent {
	t.Helper()
	host, err := StartHost(ctx, HostConfig{
		ServerURL:   serverURL,
		ServerKey:   testutil.ServerKey,
		ID:          "devbox",
		Key:         "host-secret",
		LocalAddr:   echoAddr,
		CarrierAddr: carrierAddr,
		ListenAddr:  "127.0.0.1:0",
		Keepalive:   300 * time.Millisecond,
		Logger:      quietLogger(),
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

func relayPortFor(s *testutil.Server, hostID string) int {
	for _, p := range s.Relay.Stats().Ports {
		if p.HostID == hostID {
			return p.Port
		}
	}
	return 0
}

// TestCarrierLinkThroughAgent exercises the whole server-side carrier path: the
// host agent authenticates its link to the shared carrier, a viewer opens the
// carrier with the token from its lookup, and frames make a round trip through
// the local carrier.
func TestCarrierLinkThroughAgent(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	localCarrier := startEchoCarrier(t)
	startHostAgentCarrier(t, ctx, s.URL, echo.LocalAddr().String(), localCarrier.Addr().String())

	waitForSharedHost(t, s)
	token := carrierTokenFor(t, s, "devbox")

	payload := []byte("carrier-round-trip")
	deadline := time.Now().Add(5 * time.Second)
	for {
		conn, err := net.DialTimeout("tcp", s.Carrier, 500*time.Millisecond)
		if err == nil {
			conn.SetDeadline(time.Now().Add(time.Second))
			hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: token})
			_, err = io.WriteString(conn, carrier.MagicViewer+string(hello)+"\n")
			if err == nil {
				err = carrier.NewFrameWriter(conn).WriteFrame(payload)
			}
			var got []byte
			if err == nil {
				got, err = carrier.NewFrameReader(conn).ReadFrame()
			}
			conn.Close()
			if err == nil && string(got) == string(payload) {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatal("the carrier frame did not make a round trip")
		}
		time.Sleep(100 * time.Millisecond)
	}

	deadline = time.Now().Add(2 * time.Second)
	for {
		stats, ok := s.Relay.SharedCarrierStats()
		if ok && stats.ToHostBytes >= uint64(len(payload)) && stats.ToViewerBytes >= uint64(len(payload)) {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("shared carrier stats did not advance: %+v", stats)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func waitForSharedHost(t *testing.T, s *testutil.Server) {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		stats, ok := s.Relay.SharedCarrierStats()
		if ok && stats.Hosts >= 1 {
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("host link never reached the shared carrier: %+v", stats)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

func carrierTokenFor(t *testing.T, s *testutil.Server, id string) string {
	t.Helper()
	host, ok := s.Registry.Lookup(id)
	if !ok || host.CarrierToken == "" {
		t.Fatalf("registry has no carrier token for %s", id)
	}
	return host.CarrierToken
}

func viewerRoundTrip(t *testing.T, v *ViewerAgent, payload []byte) {
	t.Helper()
	client, err := net.DialUDP("udp", nil, v.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	deadline := time.Now().Add(6 * time.Second)
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
			t.Fatalf("the carrier path did not deliver a datagram: %v (%+v)", err, v.PathInfo())
		}
	}
}

func TestViewerCarrierModeRoundTrip(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	localCarrier := startEchoCarrier(t)
	startHostAgentCarrier(t, ctx, s.URL, echo.LocalAddr().String(), localCarrier.Addr().String())
	waitForSharedHost(t, s)

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeCarrier,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	if info := viewer.PathInfo(); info.Path != "carrier" || info.Carrier != s.Carrier {
		t.Fatalf("carrier path info: %+v", info)
	}
	viewerRoundTrip(t, viewer, []byte("quic-datagram-over-carrier"))

	// Drop the TCP path underneath the agent; the pump must reconnect and
	// keep forwarding.
	viewer.carrierMu.Lock()
	if viewer.carrierConn != nil {
		viewer.carrierConn.Close()
	}
	viewer.carrierMu.Unlock()
	viewerRoundTrip(t, viewer, []byte("quic-datagram-after-reconnect"))
}

// TestCarrierKeepsHostOnlineWithoutRendezvous stops the UDP rendezvous port and
// waits until the last keepalive is outside the online window. The carrier link
// must keep the host online, and the lookup must hide the stale UDP mapping.
func TestCarrierKeepsHostOnlineWithoutRendezvous(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	localCarrier := startEchoCarrier(t)
	startHostAgentCarrier(t, ctx, s.URL, echo.LocalAddr().String(), localCarrier.Addr().String())
	waitForSharedHost(t, s)

	s.Rendezvous.Close()
	deadline := time.Now().Add(10 * time.Second)
	for {
		host, ok := s.Registry.Lookup("devbox")
		if ok && !s.Registry.Online(host) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("the UDP mapping never aged out of the online window")
		}
		time.Sleep(50 * time.Millisecond)
	}

	api := newAPIClient(s.URL, testutil.ServerKey)
	lookupCtx, lookupCancel := context.WithTimeout(ctx, 5*time.Second)
	host, err := api.lookup(lookupCtx, "devbox")
	lookupCancel()
	if err != nil {
		t.Fatalf("lookup after the UDP path died: %v", err)
	}
	if !host.Online || host.Transport != "carrier" || host.CarrierToken == "" ||
		host.Candidate != "" || len(host.Candidates) != 0 || len(host.LocalAddrs) != 0 {
		t.Fatalf("carrier-backed lookup: %+v", host)
	}

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeAuto,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("viewer did not accept the carrier-only host: %v", err)
	}
	t.Cleanup(viewer.Close)
	if info := viewer.PathInfo(); info.Path != "carrier" || info.Candidates != 0 {
		t.Fatalf("carrier fallback path info: %+v", info)
	}
	viewerRoundTrip(t, viewer, []byte("quic-datagram-carrier-fallback"))
}

func TestRelayOnlyForcesRelayAndRefusesDirect(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.StartWith(t, testutil.Options{RelayOnly: true})
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeAuto,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	info := viewer.PathInfo()
	if info.Path != "relay" || info.Candidates != 0 || info.NAT != "symmetric" {
		t.Fatalf("relay-only path info: %+v", info)
	}
	viewerRoundTrip(t, viewer, []byte("quic-datagram-forced-relay"))

	_, err = StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeDirect,
		Logger:     quietLogger(),
	})
	if err == nil || !strings.Contains(err.Error(), "server forces relay") {
		t.Fatalf("direct mode under relay-only = %v", err)
	}
}
