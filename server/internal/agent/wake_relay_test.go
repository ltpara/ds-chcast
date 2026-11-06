package agent

import (
	"bytes"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/wake"
	"github.com/ltpara/ds-chcast/server/internal/wol"
)

func wakeListener(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func TestRelayWakeJobsBroadcastsOnce(t *testing.T) {
	listener := wakeListener(t)
	h := &HostAgent{
		cfg: HostConfig{WakeRelay: true, WakeTargets: []string{listener.LocalAddr().String()}},
		log: quietLogger(),
	}
	job := wake.Job{ID: "job-1", Target: "targetbox", MAC: "AA:BB:CC:DD:EE:FF", ExpiresInSec: 60}
	h.relayWakeJobs([]wake.Job{job})

	want, err := wol.MagicPacket(job.MAC)
	if err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 2048)
	listener.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err := listener.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("wake packet: %v", err)
	}
	if !bytes.Equal(buf[:n], want) {
		t.Fatalf("wake packet = %x, want %x", buf[:n], want)
	}

	h.relayWakeJobs([]wake.Job{job})
	listener.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if n, _, err := listener.ReadFromUDP(buf); err == nil {
		t.Fatalf("duplicate job was relayed again: %x", buf[:n])
	}
}

func TestRelayWakeJobsDisabledByDefault(t *testing.T) {
	listener := wakeListener(t)
	h := &HostAgent{
		cfg: HostConfig{WakeTargets: []string{listener.LocalAddr().String()}},
		log: quietLogger(),
	}
	h.relayWakeJobs([]wake.Job{{ID: "job-2", Target: "targetbox", MAC: "AA:BB:CC:DD:EE:FF"}})
	listener.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 2048)
	if n, _, err := listener.ReadFromUDP(buf); err == nil {
		t.Fatalf("relay sent a packet while disabled: %x", buf[:n])
	}
}

func TestRelayWakeJobsSkipsForeignLink(t *testing.T) {
	listener := wakeListener(t)
	h := &HostAgent{cfg: HostConfig{WakeRelay: true}, log: quietLogger()}
	job := wake.Job{
		ID:         "job-3",
		Target:     "targetbox",
		MAC:        "AA:BB:CC:DD:EE:FF",
		Broadcasts: []string{"203.0.113.255:9"},
	}
	h.relayWakeJobs([]wake.Job{job})

	listener.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 2048)
	if n, _, err := listener.ReadFromUDP(buf); err == nil {
		t.Fatalf("relay sent a packet for a foreign link: %x", buf[:n])
	}
}

func TestSharedTargets(t *testing.T) {
	got := sharedTargets(
		[]string{"192.168.1.255:9", "10.0.0.255:9"},
		[]string{"10.0.0.255:9", "172.16.0.255:9"},
	)
	if len(got) != 1 || got[0] != "10.0.0.255:9" {
		t.Fatalf("sharedTargets = %v, want [10.0.0.255:9]", got)
	}
	if got := sharedTargets(nil, []string{"10.0.0.255:9"}); len(got) != 0 {
		t.Fatalf("sharedTargets with an empty job = %v", got)
	}
	if got := sharedTargets([]string{"10.0.0.255:9"}, nil); len(got) != 0 {
		t.Fatalf("sharedTargets with an empty local list = %v", got)
	}
	if got := sharedTargets([]string{"10.0.0.255:9"}, []string{"10.0.0.255:9"}); len(got) != 1 {
		t.Fatalf("sharedTargets matching = %v", got)
	}
}

func TestWakeTargetsReportOverride(t *testing.T) {
	h := &HostAgent{cfg: HostConfig{WakeTargets: []string{
		" 127.0.0.1:9 ", "255.255.255.255:9", "127.0.0.1:9",
	}}}
	got := h.wakeTargets()
	if len(got) != 1 || got[0] != "127.0.0.1:9" {
		t.Fatalf("wakeTargets override = %v, want [127.0.0.1:9]", got)
	}
	auto := (&HostAgent{}).wakeTargets()
	if auto == nil {
		t.Fatal("automatic wake targets must be a non-nil list")
	}
	for _, target := range auto {
		if target == "255.255.255.255:9" {
			t.Fatalf("automatic wake targets include the limited broadcast: %v", auto)
		}
	}
}
