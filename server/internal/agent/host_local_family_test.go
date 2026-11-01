package agent

import (
	"context"
	"net"
	"testing"
	"time"
)

// The punch socket may be bound to one address family while the local control
// address uses the other (for example --listen [::1] with the default IPv4
// control address). The agent must still carry both directions of the viewer
// session across that mismatch.
func TestHostForwardsAcrossLocalFamilies(t *testing.T) {
	punch, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Skipf("IPv6 loopback unavailable: %v", err)
	}
	defer punch.Close()
	echo, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer echo.Close()
	go func() {
		buf := make([]byte, 2048)
		for {
			n, src, err := echo.ReadFromUDP(buf)
			if err != nil {
				return
			}
			echo.WriteToUDP(buf[:n], src)
		}
	}()

	local, err := resolveUDP(echo.LocalAddr().String())
	if err != nil || local == nil {
		t.Fatalf("resolve local address: %v", err)
	}
	localOut, err := localForwardConn(punch, local)
	if err != nil {
		t.Fatal(err)
	}
	if localOut == nil {
		t.Fatal("expected a second socket for the cross-family local address")
	}
	defer localOut.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	h := &HostAgent{
		conn:     punch,
		localOut: localOut,
		local:    local,
		ctx:      ctx,
		log:      quietLogger(),
	}
	go h.readLoop()
	go h.localLoop()

	viewer, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Fatal(err)
	}
	defer viewer.Close()
	payload := []byte("cross-family-viewer-data")
	deadline := time.Now().Add(3 * time.Second)
	buf := make([]byte, 2048)
	for {
		viewer.WriteToUDP(payload, punch.LocalAddr().(*net.UDPAddr))
		viewer.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
		n, _, err := viewer.ReadFromUDP(buf)
		if err == nil {
			if string(buf[:n]) != string(payload) {
				t.Fatalf("reply = %q, want %q", buf[:n], payload)
			}
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("cross-family forwarding did not echo: %v", err)
		}
	}
}

func TestLocalForwardConnMatchesFamilies(t *testing.T) {
	v4, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer v4.Close()
	v6, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Skipf("IPv6 loopback unavailable: %v", err)
	}
	defer v6.Close()
	v4Local := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 18434}
	v6Local := &net.UDPAddr{IP: net.IPv6loopback, Port: 18434}

	cases := []struct {
		name  string
		conn  *net.UDPConn
		local *net.UDPAddr
		want  bool
	}{
		{"same v4 family", v4, v4Local, false},
		{"same v6 family", v6, v6Local, false},
		{"v6 punch, v4 local", v6, v4Local, true},
		{"v4 punch, v6 local", v4, v6Local, true},
	}
	for _, tc := range cases {
		conn, err := localForwardConn(tc.conn, tc.local)
		if err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		if conn != nil {
			conn.Close()
		}
		if got := conn != nil; got != tc.want {
			t.Fatalf("%s: second socket = %v, want %v", tc.name, got, tc.want)
		}
	}
}
