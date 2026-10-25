package admin

import (
	"crypto/ed25519"
	"io"
	"log/slog"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func startConsole(t *testing.T, cfg Config) *Console {
	t.Helper()
	if cfg.Listen == "" {
		cfg.Listen = "127.0.0.1:0"
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.New(slog.NewTextHandler(io.Discard, nil))
	}
	c, err := New(cfg)
	if err != nil {
		t.Fatalf("admin console: %v", err)
	}
	go c.Serve()
	t.Cleanup(c.Close)
	return c
}

func send(t *testing.T, addr, command string) string {
	t.Helper()
	conn, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatalf("dial admin console: %v", err)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err := io.WriteString(conn, command+"\n"); err != nil {
		t.Fatalf("write %q: %v", command, err)
	}
	out, err := io.ReadAll(conn)
	if err != nil {
		t.Fatalf("read %q: %v", command, err)
	}
	return string(out)
}

func TestConsoleCommands(t *testing.T) {
	s := testutil.Start(t)
	priv := identity.PrivateKeyFromSecret("admin console host")
	if _, err := s.Registry.Register(registry.RegisterRequest{
		ID:        "devbox",
		PublicKey: identity.PublicKeyText(priv.Public().(ed25519.PublicKey)),
		Hostname:  "Dev Box",
		Platform:  "windows/amd64",
		Version:   "1.0.0",
		TTL:       time.Minute,
	}); err != nil {
		t.Fatalf("register host: %v", err)
	}
	if err := s.Registry.Touch("devbox", "198.51.100.9:5000", nil, "port-restricted"); err != nil {
		t.Fatalf("touch host: %v", err)
	}

	blocker := abuse.New(abuse.Config{Window: time.Minute, MaxPerWindow: 30, IDWindow: time.Hour, MaxIDs: 10})
	if ok, reason := blocker.Allow("10.0.0.1", "devbox"); !ok {
		t.Fatalf("seed blocker: %s", reason)
	}
	s.PunchLog.Record("10.0.0.1", "198.51.100.9", "devbox")
	s.PunchLog.Record("10.0.0.2", "198.51.100.9", "devbox")

	changes := ipchange.New(0)
	changes.Observe("devbox", "203.0.113.1")
	changes.Observe("devbox", "203.0.113.2")
	changes.Observe("devbox", "203.0.113.3")
	relays, err := relayaddr.New([]string{"127.0.0.1", "relay.example"})
	if err != nil {
		t.Fatalf("relay addresses: %v", err)
	}

	c := startConsole(t, Config{
		Registry:   s.Registry,
		Relay:      s.Relay,
		Punch:      s.PunchLog,
		Blocker:    blocker,
		IPChanges:  changes,
		RelayAddrs: relays,
		RelayOnly:  s.RelayOnly,
	})

	help := send(t, c.Addr(), "h")
	for _, want := range []string{"stats", "hosts", "peers", "alias", "tags", "punch-requests", "ip-blocker", "ip-changes", "relay-servers", "test-geo", "relay-only"} {
		if !strings.Contains(help, want) {
			t.Fatalf("help missing %q:\n%s", want, help)
		}
	}

	stats := send(t, c.Addr(), "stats")
	for _, want := range []string{
		"hosts=1", "relay_only=false", "punch_log stored=2", "registration_blocked tracked=1",
		"ip_changes tracked=1 changes=2", "relay_servers=2",
		"relay_policy single_bps=134217728", "relay_policy limited_bytes=0",
	} {
		if !strings.Contains(stats, want) {
			t.Fatalf("stats missing %q:\n%s", want, stats)
		}
	}

	peers := send(t, c.Addr(), "peers")
	for _, want := range []string{"devbox online=true", "hostname=Dev Box", "platform=windows/amd64", "version=1.0.0", "alias=-", "tags=-"} {
		if !strings.Contains(peers, want) {
			t.Fatalf("peers missing %q:\n%s", want, peers)
		}
	}
	if out := send(t, c.Addr(), "alias devbox Studio PC"); out != "devbox alias=Studio PC\n" {
		t.Fatalf("alias output = %q", out)
	}
	if out := send(t, c.Addr(), "tags devbox office,prod,office"); out != "devbox tags=office,prod\n" {
		t.Fatalf("tags output = %q", out)
	}
	peers = send(t, c.Addr(), "peers")
	if !strings.Contains(peers, "alias=Studio PC tags=office,prod") {
		t.Fatalf("labels missing from peers:\n%s", peers)
	}
	if out := send(t, c.Addr(), "tags devbox"); out != "devbox tags cleared\n" {
		t.Fatalf("clear tags output = %q", out)
	}
	if out := send(t, c.Addr(), "alias missing name"); !strings.Contains(out, "unknown peer missing") {
		t.Fatalf("unknown alias output = %q", out)
	}

	if out := send(t, c.Addr(), "sb 32"); out != "32.00Mb/s\n" {
		t.Fatalf("sb output = %q", out)
	}
	if out := send(t, c.Addr(), "sb"); out != "32.00Mb/s\n" {
		t.Fatalf("sb query = %q", out)
	}
	if out := send(t, c.Addr(), "ba 203.0.113.9"); !strings.HasPrefix(out, "added 1, total=1") {
		t.Fatalf("ba output = %q", out)
	}
	if out := send(t, c.Addr(), "b"); !strings.Contains(out, "203.0.113.9") {
		t.Fatalf("b output = %q", out)
	}
	if out := send(t, c.Addr(), "u"); out != "no relay sessions\n" {
		t.Fatalf("u output = %q", out)
	}

	hosts := send(t, c.Addr(), "hosts")
	if !strings.Contains(hosts, "devbox online=true candidate=198.51.100.9:5000") ||
		!strings.Contains(hosts, "relay_host=-") {
		t.Fatalf("hosts output:\n%s", hosts)
	}

	punch := send(t, c.Addr(), "pr")
	if !strings.Contains(punch, "10.0.0.1 -> devbox@198.51.100.9") {
		t.Fatalf("punch output:\n%s", punch)
	}
	if got := send(t, c.Addr(), "pr 1 1"); strings.Contains(got, "10.0.0.1") || !strings.Contains(got, "10.0.0.2") {
		t.Fatalf("punch pagination:\n%s", got)
	}
	if got := send(t, c.Addr(), "pr -"); !strings.Contains(got, "cleared") {
		t.Fatalf("punch clear: %q", got)
	}
	if got := send(t, c.Addr(), "punch-requests"); !strings.Contains(got, "no punch requests") {
		t.Fatalf("punch after clear: %q", got)
	}

	if got := send(t, c.Addr(), "aur"); !strings.Contains(got, "relay_only=false") {
		t.Fatalf("aur get: %q", got)
	}
	if got := send(t, c.Addr(), "aur Y"); !strings.Contains(got, "relay_only=true") {
		t.Fatalf("aur set: %q", got)
	}
	if !s.RelayOnly.Load() {
		t.Fatal("aur Y did not flip the shared switch")
	}
	send(t, c.Addr(), "relay-only N")

	if got := send(t, c.Addr(), "ib"); !strings.Contains(got, "tracked=1") {
		t.Fatalf("ip-blocker summary: %q", got)
	}
	if got := send(t, c.Addr(), "ib 10.0.0.1"); !strings.Contains(got, "count=1") || !strings.Contains(got, "ids=1") {
		t.Fatalf("ip-blocker entry: %q", got)
	}
	if got := send(t, c.Addr(), "ib 192.0.2.1"); !strings.Contains(got, "not tracked") {
		t.Fatalf("ip-blocker unknown entry: %q", got)
	}
	if got := send(t, c.Addr(), "ib -"); !strings.Contains(got, "cleared 1") {
		t.Fatalf("ip-blocker clear: %q", got)
	}

	if got := send(t, c.Addr(), "rs"); !strings.Contains(got, "127.0.0.1\n") || !strings.Contains(got, "relay.example\n") {
		t.Fatalf("relay list: %q", got)
	}
	if got := send(t, c.Addr(), "rs relay2.example,relay3.example"); !strings.Contains(got, "relay2.example") || strings.Contains(got, "127.0.0.1") {
		t.Fatalf("relay set: %q", got)
	}
	if got := send(t, c.Addr(), "rs relay.example:41000"); !strings.Contains(got, "must not include a port") {
		t.Fatalf("relay invalid set: %q", got)
	}
	if got := send(t, c.Addr(), "rs -"); !strings.Contains(got, "reset") || !strings.Contains(got, "relay.example") {
		t.Fatalf("relay reset: %q", got)
	}
	if got := send(t, c.Addr(), "tg nope"); !strings.Contains(got, "usage") {
		t.Fatalf("test-geo invalid: %q", got)
	}
	if got := send(t, c.Addr(), "tg 203.0.113.1 203.0.113.2"); !strings.Contains(got, "next=127.0.0.1") {
		t.Fatalf("test-geo preview: %q", got)
	}

	if got := send(t, c.Addr(), "ic"); !strings.Contains(got, "tracked=1 changes=2") ||
		!strings.Contains(got, "devbox age=") || !strings.Contains(got, "203.0.113.2:1") ||
		!strings.Contains(got, "203.0.113.3:1") {
		t.Fatalf("ip-changes list: %q", got)
	}
	if got := send(t, c.Addr(), "ic devbox"); !strings.Contains(got, "devbox age=") || !strings.Contains(got, "203.0.113.3:1") {
		t.Fatalf("ip-changes detail: %q", got)
	}
	if got := send(t, c.Addr(), "ic devbox -"); !strings.Contains(got, "removed") {
		t.Fatalf("ip-changes remove: %q", got)
	}
	if got := send(t, c.Addr(), "ic"); !strings.Contains(got, "tracked=0 changes=2") {
		t.Fatalf("ip-changes after remove: %q", got)
	}
	changes.Observe("devbox", "203.0.113.4")
	changes.Observe("devbox", "203.0.113.5")
	changes.Observe("devbox", "203.0.113.6")
	if got := send(t, c.Addr(), "ic 0 1"); !strings.Contains(got, "tracked=1") || !strings.Contains(got, "203.0.113.5:1") {
		t.Fatalf("ip-changes pagination: %q", got)
	}
	if got := send(t, c.Addr(), "ic -"); !strings.Contains(got, "cleared 1") {
		t.Fatalf("ip-changes clear: %q", got)
	}

	if got := send(t, c.Addr(), "nonsense"); got != "unknown command\n" {
		t.Fatalf("unknown command: %q", got)
	}
}

func TestRejectsNonLoopbackBind(t *testing.T) {
	s := testutil.Start(t)
	if _, err := New(Config{Listen: "0.0.0.0:0", Registry: s.Registry, Relay: s.Relay}); err == nil {
		t.Fatal("wildcard bind was accepted")
	}
}

type fakeConn struct {
	net.Conn
	remote net.Addr
}

func (f fakeConn) RemoteAddr() net.Addr { return f.remote }

func TestPolicyOnlyConsole(t *testing.T) {
	s := testutil.Start(t)
	c := startConsole(t, Config{Relay: s.Relay, PolicyOnly: true})

	help := send(t, c.Addr(), "h")
	if !strings.Contains(help, "stats") || !strings.Contains(help, "single-bandwidth") {
		t.Fatalf("relay help:\n%s", help)
	}
	if strings.Contains(help, "punch-requests") {
		t.Fatalf("relay help lists hbbs commands:\n%s", help)
	}
	stats := send(t, c.Addr(), "stats")
	if !strings.Contains(stats, "relay_policy single_bps=") || strings.HasPrefix(stats, "hosts=") {
		t.Fatalf("relay stats:\n%s", stats)
	}
	if out := send(t, c.Addr(), "hosts"); !strings.Contains(out, "unavailable") {
		t.Fatalf("hosts output = %q", out)
	}
	if out := send(t, c.Addr(), "tb 256"); out != "256.00Mb/s\n" {
		t.Fatalf("tb output = %q", out)
	}
	if out := send(t, c.Addr(), "no-such-command"); !strings.Contains(out, "unknown command") {
		t.Fatalf("unknown output = %q", out)
	}
}

func TestHandleRejectsRemotePeer(t *testing.T) {
	server, client := net.Pipe()
	defer client.Close()
	c := &Console{cfg: Config{Logger: slog.New(slog.NewTextHandler(io.Discard, nil))}, done: make(chan struct{})}
	done := make(chan struct{})
	go func() {
		c.handle(fakeConn{Conn: server, remote: &net.TCPAddr{IP: net.ParseIP("203.0.113.5"), Port: 5555}})
		close(done)
	}()
	client.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := client.Read(make([]byte, 1)); err == nil {
		t.Fatal("non-loopback connection received a reply")
	}
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("non-loopback connection was not closed")
	}
}
