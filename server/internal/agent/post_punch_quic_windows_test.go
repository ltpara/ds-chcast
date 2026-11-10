package agent

import (
	"bytes"
	"context"
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type quicTestOutput struct {
	sync.Mutex
	bytes.Buffer
}

func (b *quicTestOutput) Write(p []byte) (int, error) {
	b.Lock()
	defer b.Unlock()
	return b.Buffer.Write(p)
}
func (b *quicTestOutput) text() string { b.Lock(); defer b.Unlock(); return b.Buffer.String() }

// Opt-in: runs the real capture/encode/QUIC/decode harness on an interactive
// Windows desktop. No public network or firewall configuration is changed.
func TestNativeQUICAfterUDPBlackhole(t *testing.T) {
	binary := os.Getenv("CHAST_QUIC_TEST_BINARY")
	if binary == "" {
		t.Skip("set CHAST_QUIC_TEST_BINARY to the built chast-quic.exe")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 70*time.Second)
	defer cancel()
	free, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	port := free.LocalAddr().(*net.UDPAddr).Port
	free.Close()
	var serverLog, clientLog quicTestOutput
	server := exec.CommandContext(ctx, binary, "serve", "--bind", "127.0.0.1", "--port", strconv.Itoa(port), "--seconds", "32", "--encoder", "ffmpeg", "--framerate", "10", "--token", "post-punch-test-token")
	server.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	server.Stdout, server.Stderr = &serverLog, &serverLog
	if err := server.Start(); err != nil {
		t.Fatal(err)
	}
	serverDone := make(chan error, 1)
	go func() { serverDone <- server.Wait() }()
	defer func() {
		cancel()
		_ = server.Process.Kill()
		select {
		case <-serverDone:
		case <-time.After(3 * time.Second):
		}
	}()
	deadline := time.Now().Add(10 * time.Second)
	var fingerprint string
	for fingerprint == "" {
		for _, line := range strings.Split(serverLog.text(), "\n") {
			if suffix, ok := strings.CutPrefix(line, "quic: identity SHA256 "); ok {
				fingerprint = strings.TrimSpace(suffix)
			}
		}
		if time.Now().After(deadline) {
			t.Fatalf("native server not ready: %s", serverLog.text())
		}
		time.Sleep(20 * time.Millisecond)
	}
	s := testutil.Start(t)
	host := startHostAgent(t, ctx, s.URL, net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
	// A forwarding hop lets us drop only the direct path. Host registration
	// and the relay remain reachable throughout the fault.
	front, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer front.Close()
	back, err := net.DialUDP("udp4", nil, host.conn.LocalAddr().(*net.UDPAddr))
	if err != nil {
		t.Fatal(err)
	}
	defer back.Close()
	host.rememberPunchPeer(back.LocalAddr().(*net.UDPAddr))
	var drop atomic.Bool
	var peerMu sync.Mutex
	var viewerAddr *net.UDPAddr
	go func() {
		buf := make([]byte, 65535)
		for {
			n, src, err := front.ReadFromUDP(buf)
			if err != nil {
				return
			}
			peerMu.Lock()
			viewerAddr = src
			peerMu.Unlock()
			if !drop.Load() {
				_, _ = back.Write(buf[:n])
			}
		}
	}()
	go func() {
		buf := make([]byte, 65535)
		for {
			n, err := back.Read(buf)
			if err != nil {
				return
			}
			peerMu.Lock()
			dst := cloneAddr(viewerAddr)
			peerMu.Unlock()
			if !drop.Load() && dst != nil {
				_, _ = front.WriteToUDP(buf[:n], dst)
			}
		}
	}()
	api := newAPIClient(s.URL, testutil.ServerKey, nil, nil)
	lookup, err := api.lookup(ctx, "devbox", "")
	if err != nil {
		t.Fatal(err)
	}
	local, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	out, err := listenUpstream()
	if err != nil {
		local.Close()
		t.Fatal(err)
	}
	vctx, vcancel := context.WithCancel(ctx)
	v := &ViewerAgent{ctx: vctx, cancel: vcancel, cfg: ViewerConfig{Mode: ModeAuto, HostID: "devbox"},
		conn: local, out: out, log: quietLogger(), nonce: newNonce(), carrierToken: lookup.CarrierToken,
		direct: front.LocalAddr().(*net.UDPAddr), remote: front.LocalAddr().(*net.UDPAddr)}
	v.relay, _ = resolveUDP(lookup.Relay)
	v.candidates = []*net.UDPAddr{v.direct}
	t.Cleanup(v.Close)
	go v.readLoop()
	go v.localLoop()
	client := exec.CommandContext(ctx, binary, "connect", "--host", "127.0.0.1", "--port", strconv.Itoa(local.LocalAddr().(*net.UDPAddr).Port), "--fingerprint", fingerprint, "--token", "post-punch-test-token")
	client.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	client.Stdout, client.Stderr = &clientLog, &clientLog
	if err := client.Start(); err != nil {
		t.Fatal(err)
	}
	clientStarted := time.Now()
	clientDone := make(chan error, 1)
	go func() { clientDone <- client.Wait() }()
	clientWaited := false
	defer func() {
		_ = client.Process.Kill()
		if !clientWaited {
			select {
			case <-clientDone:
			case <-time.After(3 * time.Second):
			}
		}
	}()
	deadline = time.Now().Add(10 * time.Second)
	for !v.directOK.Load() {
		if time.Now().After(deadline) {
			t.Fatalf("no native direct traffic: %s", clientLog.text())
		}
		time.Sleep(20 * time.Millisecond)
	}
	time.Sleep(3 * time.Second)
	drop.Store(true)
	faultAt := time.Now()
	// At 10 fps this generously bounds frames captured before the fault.
	preFaultFrames := int(faultAt.Sub(clientStarted).Seconds())*10 + 30
	deadline = faultAt.Add(22 * time.Second)
	for !v.fellBackNow() {
		if time.Now().After(deadline) {
			t.Fatalf("no fallback after blackhole: %s", clientLog.text())
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Logf("UDP blackhole selected relay after %v", time.Since(faultAt))
	select {
	case err := <-clientDone:
		clientWaited = true
		if err != nil {
			t.Fatalf("native session failed: %v\nclient: %s\nserver: %s", err, clientLog.text(), serverLog.text())
		}
	case <-ctx.Done():
		t.Fatalf("native client did not complete: %s", clientLog.text())
	}
	text := clientLog.text()
	stats := make(map[string]string)
	for _, line := range strings.Split(text, "\n") {
		if !strings.HasPrefix(line, "receiver:") {
			continue
		}
		t.Log(line)
		for _, field := range strings.Fields(line) {
			if key, value, ok := strings.Cut(field, "="); ok {
				stats[key] = value
			}
		}
	}
	for _, key := range []string{"crc-errors", "decode-errors", "protocol-errors"} {
		if stats[key] != "0" {
			t.Fatalf("%s missing or failed: %s", key, text)
		}
	}
	decoded, _ := strconv.Atoi(stats["decoded"])
	elapsed, _ := strconv.Atoi(stats["elapsed"])
	age, err := strconv.ParseFloat(stats["frame-age"], 64)
	if decoded <= preFaultFrames || elapsed < 28000 || err != nil || age >= 1000 {
		t.Fatalf("no healthy video after fallback (pre-fault bound %d): %s", preFaultFrames, text)
	}
}
