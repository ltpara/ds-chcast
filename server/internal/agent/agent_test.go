package agent

import (
	"context"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/lanmask"
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
	return startHostAgentInfo(t, ctx, serverURL, echoAddr, nil)
}

func startHostAgentInfo(t *testing.T, ctx context.Context, serverURL, echoAddr string,
	onInfo func(HostInfo)) *HostAgent {
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
		OnInfo:     onInfo,
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
	assertRoundTrip(t, viewer, mode)
}

func assertRoundTrip(t *testing.T, viewer *ViewerAgent, mode string) {
	t.Helper()
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

func TestHostIDMigrationThroughAgent(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)

	first := startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())
	first.Close()

	host, err := StartHost(ctx, HostConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		ID:         "newbox",
		OldID:      "devbox",
		Key:        "host-secret",
		LocalAddr:  echo.LocalAddr().String(),
		ListenAddr: "127.0.0.1:0",
		Keepalive:  300 * time.Millisecond,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start migrated host: %v", err)
	}
	t.Cleanup(host.Close)
	readyCtx, readyCancel := context.WithTimeout(ctx, 5*time.Second)
	defer readyCancel()
	if err := host.WaitReady(readyCtx); err != nil {
		t.Fatalf("host readiness: %v", err)
	}
	if info := host.Info(); info.ID != "newbox" || info.MigratedFrom != "devbox" {
		t.Fatalf("host info after migration = %+v", info)
	}
	if _, ok := s.Registry.Identity("devbox"); ok {
		t.Fatal("the server kept the old identity")
	}
	if _, ok := s.Registry.Identity("newbox"); !ok {
		t.Fatal("the server did not create the new identity")
	}

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "newbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeDirect,
		DirectTimeout: 1 * time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer: %v", err)
	}
	t.Cleanup(viewer.Close)
	assertRoundTrip(t, viewer, ModeDirect)
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

// Both agents run on loopback, so the hbbs LAN rule sees a same-network pair
// and must leave it on the direct path rather than forcing the relay.
func TestLANMaskKeepsSameNetworkDirect(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	mask, err := lanmask.Parse("127.0.0.0/8")
	if err != nil {
		t.Fatal(err)
	}
	s := testutil.StartWith(t, testutil.Options{LANMask: mask, LANLocalIP: "127.0.0.1"})
	testRoundTrip(t, ctx, s, ModeAuto)
}

// TestViewerDetectsOwnNATType runs the same two-port observation the host
// uses. On loopback the observed port does not change per destination, so the
// classification must be asymmetric.
func TestViewerDetectsOwnNATType(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeAuto,
		DirectTimeout: time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	deadline := time.Now().Add(5 * time.Second)
	for {
		info := viewer.PathInfo()
		if info.OwnNAT == NATAsymmetric {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("viewer NAT type = %q, want %q", info.OwnNAT, NATAsymmetric)
		}
		time.Sleep(20 * time.Millisecond)
	}
}

// NAT observations do not spend or restart the coordination budget.
func TestViewerSymmetricNATDoesNotPreemptCoordination(t *testing.T) {
	v := &ViewerAgent{cfg: ViewerConfig{Mode: ModeAuto}, ctx: context.Background(),
		log: quietLogger(), pendingDirect: 2, natType: NATAsymmetric,
		relay: &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002}}
	v.setOwnNAT(NATSymmetric)
	if v.fellBackNow() || v.timer != nil {
		t.Fatal("NAT observation started fallback before coordination")
	}
	if wait := v.connectWait(2*time.Second, false); wait != time.Second {
		t.Fatalf("asymmetric peer / symmetric own connect window = %v", wait)
	}
	v.directAttemptDone()
	if v.fellBackNow() {
		t.Fatal("fallback preempted the remaining direct path")
	}
	v.directAttemptDone()
	if !v.fellBackNow() {
		t.Fatal("exhausted paths did not fall back")
	}
}

func TestHostDetectsNATType(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	host := startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	deadline := time.Now().Add(5 * time.Second)
	for host.Info().NAT == "" {
		if time.Now().After(deadline) {
			t.Fatalf("host never classified its NAT: %+v", host.Info())
		}
		time.Sleep(50 * time.Millisecond)
	}
	if got := host.Info().NAT; got != NATAsymmetric {
		t.Fatalf("loopback NAT type = %q, want %q", got, NATAsymmetric)
	}

	for {
		record, ok := s.Registry.Lookup("devbox")
		if ok && record.NATType == NATAsymmetric {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("registry NAT type = %q, ok=%v", record.NATType, ok)
		}
		time.Sleep(50 * time.Millisecond)
	}

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     s.URL,
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeAuto,
		DirectTimeout: time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	info := viewer.PathInfo()
	if info.NAT != NATAsymmetric {
		t.Fatalf("viewer NAT type = %q", info.NAT)
	}
	if info.Candidates < 1 || info.Direct == "" {
		t.Fatalf("viewer did not receive direct candidates: %+v", info)
	}
	if info.Carrier == "" || info.CarrierToken == "" {
		t.Fatalf("viewer did not receive the shared carrier: %+v", info)
	}
	if info.Carrier != s.Carrier {
		t.Fatalf("viewer carrier = %q, want %q", info.Carrier, s.Carrier)
	}
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

func TestPathInfoReportsInitialPathAndFallback(t *testing.T) {
	direct := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40001}
	relay := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40002}
	var got []PathInfo
	v := &ViewerAgent{
		cfg: ViewerConfig{
			Mode:   ModeAuto,
			OnPath: func(info PathInfo) { got = append(got, info) },
		},
		ctx:    context.Background(),
		direct: direct,
		remote: direct,
		relay:  relay,
		log:    quietLogger(),
	}
	v.reportPath()
	if len(got) != 1 || got[0].Path != "connecting" || got[0].Remote != direct.String() {
		t.Fatalf("initial path = %+v", got)
	}
	v.onDirectTimeout()
	if len(got) != 2 || got[1].Path != "relay" || got[1].Remote != relay.String() {
		t.Fatalf("paths after fallback = %+v", got)
	}
	v.onDirectPacket(direct)
	if len(got) != 2 {
		t.Fatalf("a late direct packet reported another path change: %+v", got)
	}
}

func TestHostInfoReportsViewerPath(t *testing.T) {
	for _, mode := range []string{ModeDirect, ModeRelay} {
		t.Run(mode, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			s := testutil.Start(t)
			echo := startEcho(t)
			events := make(chan HostInfo, 16)
			host := startHostAgentInfo(t, ctx, s.URL, echo.LocalAddr().String(),
				func(info HostInfo) { events <- info })
			initial := host.Info()
			if initial.ID != "devbox" || initial.Listen == "" || initial.Relay == "" {
				t.Fatalf("initial info = %+v", initial)
			}
			if initial.Viewer != "" || initial.Path != "" {
				t.Fatalf("a fresh host already reports a viewer: %+v", initial)
			}

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
			payload := []byte("host-path-" + mode)
			deadline := time.Now().Add(4 * time.Second)
			buf := make([]byte, 2048)
			for {
				client.Write(payload)
				client.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
				if n, err := client.Read(buf); err == nil && string(buf[:n]) == string(payload) {
					break
				}
				if time.Now().After(deadline) {
					t.Fatalf("%s path did not echo", mode)
				}
			}

			wait := time.After(2 * time.Second)
			for {
				select {
				case info := <-events:
					if info.Path == mode && info.Viewer != "" {
						return
					}
				case <-wait:
					t.Fatalf("host never reported the %s viewer path", mode)
				}
			}
		})
	}
}

// TestRelayMuxViewerRenewsRelayAuth keeps an active relay-path session alive
// past the relay's 60-second viewer authorization window. Without a periodic
// token proof the relay silently drops viewer->host datagrams while
// host->viewer keeps flowing, so the echo stalls even though the session
// still looks alive.
func TestRelayMuxViewerRenewsRelayAuth(t *testing.T) {
	if testing.Short() {
		t.Skip("waits out the relay viewer authorization window")
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	s := testutil.Start(t)
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:  s.URL,
		ServerKey:  testutil.ServerKey,
		HostID:     "devbox",
		ListenAddr: "127.0.0.1:0",
		Mode:       ModeRelay,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer agent: %v", err)
	}
	t.Cleanup(viewer.Close)
	if info := viewer.PathInfo(); info.Path != "relay" {
		t.Fatalf("path info = %+v, want relay", info)
	}

	start := time.Now()
	for _, at := range []time.Duration{time.Second, 20 * time.Second, 40 * time.Second, 62 * time.Second} {
		if wait := time.Until(start.Add(at)); wait > 0 {
			time.Sleep(wait)
		}
		assertRoundTrip(t, viewer, ModeRelay)
	}
}
