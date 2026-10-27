package admin

import (
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/configpush"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestConsoleRendezvousServers(t *testing.T) {
	s := testutil.Start(t)
	push, err := configpush.New(1, []string{"https://one.example"})
	if err != nil {
		t.Fatalf("config: %v", err)
	}
	c := startConsole(t, Config{
		Registry:   s.Registry,
		Relay:      s.Relay,
		Rendezvous: push,
	})

	help := send(t, c.Addr(), "h")
	if !strings.Contains(help, "rendezvous-servers(rv)") {
		t.Fatalf("help missing rendezvous-servers:\n%s", help)
	}

	out := send(t, c.Addr(), "rv")
	if !strings.Contains(out, "serial=1") || !strings.Contains(out, "https://one.example") {
		t.Fatalf("rv = %q", out)
	}

	out = send(t, c.Addr(), "rv https://two.example,https://three.example")
	if !strings.Contains(out, "serial=2") {
		t.Fatalf("rv set = %q", out)
	}
	serial, servers := push.Snapshot()
	if serial != 2 || len(servers) != 2 || servers[0] != "https://two.example" {
		t.Fatalf("config after set = %d/%v", serial, servers)
	}

	out = send(t, c.Addr(), "rv serial 5 https://four.example")
	if !strings.Contains(out, "serial=5") {
		t.Fatalf("rv serial = %q", out)
	}
	if serial, servers := push.Snapshot(); serial != 5 || servers[0] != "https://four.example" {
		t.Fatalf("config after serial = %d/%v", serial, servers)
	}

	out = send(t, c.Addr(), "rv serial 4 https://five.example")
	if !strings.Contains(out, "must be greater") {
		t.Fatalf("stale serial accepted: %q", out)
	}
	out = send(t, c.Addr(), "rv bogus-url")
	if !strings.Contains(out, "rendezvous-servers:") {
		t.Fatalf("invalid url accepted: %q", out)
	}

	out = send(t, c.Addr(), "rv clear")
	if !strings.Contains(out, "serial=6 cleared") {
		t.Fatalf("rv clear = %q", out)
	}
	if _, servers := push.Snapshot(); len(servers) != 0 {
		t.Fatalf("clear left %v", servers)
	}

	without := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay})
	if out := send(t, without.Addr(), "rv"); !strings.Contains(out, "not enabled") {
		t.Fatalf("rv without config = %q", out)
	}
}
