package relay

import (
	"bufio"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
)

func TestFirstForwardedIP(t *testing.T) {
	cases := []struct {
		value string
		want  string
	}{
		{"203.0.113.9", "203.0.113.9"},
		{" 203.0.113.9 ", "203.0.113.9"},
		{"198.51.100.4, 10.0.0.1", "198.51.100.4"},
		{"unknown, 203.0.113.9", "203.0.113.9"},
		{"203.0.113.9:443", "203.0.113.9"},
		{"[2001:db8::1]", "2001:db8::1"},
		{"[2001:db8::1]:8443", "2001:db8::1"},
		{"", ""},
		{"not-an-ip", ""},
	}
	for _, tc := range cases {
		got := firstForwardedIP(tc.value)
		if tc.want == "" {
			if got != nil {
				t.Fatalf("firstForwardedIP(%q) = %v, want nil", tc.value, got)
			}
			continue
		}
		if got == nil || got.String() != tc.want {
			t.Fatalf("firstForwardedIP(%q) = %v, want %s", tc.value, got, tc.want)
		}
	}
}

// openProxyWSViewer upgrades a WebSocket viewer with optional extra handshake
// headers of the form "Name: value".
func openProxyWSViewer(t *testing.T, addr string, extra ...string) (net.Conn, *bufio.Reader) {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial carrier: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	request := wsRequest
	if len(extra) > 0 {
		request = strings.Replace(request, "Host: relay\r\n",
			"Host: relay\r\n"+strings.Join(extra, "\r\n")+"\r\n", 1)
	}
	if _, err := io.WriteString(conn, request); err != nil {
		t.Fatalf("write upgrade: %v", err)
	}
	br := bufio.NewReader(conn)
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		t.Fatalf("read upgrade: %v", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("upgrade status = %s", resp.Status)
	}
	conn.SetReadDeadline(time.Time{})
	return conn, br
}

func TestWSViewerProxyHeaderFeedsPolicy(t *testing.T) {
	const secret = "host-secret-ws-proxy"
	m, _, relayAddr, _ := startRelayWith(t, "devbox", secret,
		WithViewerTokenRequired(false), WithProxyHeader("X-Real-IP"))
	if err := m.SetPolicyConfig(PolicyConfig{BlocklistFile: blocklistFile(t, "203.0.113.9")}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	openHostLink(t, relayAddr, "devbox", secret)

	conn, _ := openProxyWSViewer(t, relayAddr.String(), "X-Real-IP: 203.0.113.9")
	expectClosed(t, conn)
	waitForStats(t, m, func(s PortStats) bool { return s.Blocked == 1 })
}

func TestWSViewerProxyHeaderIgnoredByDefault(t *testing.T) {
	const secret = "host-secret-ws-proxy-off"
	m, _, relayAddr, _ := startRelayWith(t, "devbox", secret, WithViewerTokenRequired(false))
	if err := m.SetPolicyConfig(PolicyConfig{BlocklistFile: blocklistFile(t, "203.0.113.9")}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	openHostLink(t, relayAddr, "devbox", secret)

	openProxyWSViewer(t, relayAddr.String(), "X-Real-IP: 203.0.113.9")
	stats := waitForStats(t, m, func(s PortStats) bool { return s.TCPViewers == 1 })
	if !strings.HasPrefix(stats.Viewer, "127.0.0.1:") {
		t.Fatalf("untrusted header changed the viewer address: %q", stats.Viewer)
	}
	if stats.Blocked != 0 {
		t.Fatalf("blocked sessions = %d", stats.Blocked)
	}
}

func TestSharedCarrierWSViewerProxyHeaderFeedsPolicy(t *testing.T) {
	m, _, addr, token, secret := startSharedRelay(t, "devbox", "host-secret-shared-ws-proxy",
		WithProxyHeader("X-Real-IP"))
	if err := m.SetPolicyConfig(PolicyConfig{BlocklistFile: blocklistFile(t, "203.0.113.9")}); err != nil {
		t.Fatalf("policy: %v", err)
	}
	openSharedHost(t, addr, "devbox", secret)

	conn, _ := openProxyWSViewer(t, addr, "X-Real-IP: 203.0.113.9")
	hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: token})
	if _, err := conn.Write(clientBinaryFrame(hello)); err != nil {
		t.Fatalf("write hello: %v", err)
	}
	expectClosed(t, conn)
	stats := waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Blocked == 1 })
	if stats.Viewers != 0 {
		t.Fatalf("viewers = %d", stats.Viewers)
	}
}
