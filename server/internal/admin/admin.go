// Package admin serves the hbbs-style plain-text control console. It binds a
// loopback TCP address, reads one command line per connection and writes a
// plain-text reply. No data-plane work ever depends on this package.
package admin

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
)

const (
	maxCommand   = 1024
	readTimeout  = 2 * time.Second
	writeTimeout = 5 * time.Second
)

const helpText = `stats
hosts [count]
peers [count]
alias <id> [text]
tags <id> [tag1,tag2]
punch-requests(pr) [start] [count] | pr -
ip-blocker(ib) [ip] | ib -
ip-changes(ic) [id|start] [count] | ic <id> - | ic -
relay-servers(rs) [host1,host2 | -]
test-geo(tg) <ip1> [ip2]
relay-only(aur) [Y|N]
user list | user add <name> [password] | user passwd <name> [password] | user del <name>
user 2fa <name> off | user unlock <name>
ab <username> [count]
audit [count]
connections [count]
transfers [count]
relay bandwidth: sb|tb|ls [Mbit/s], dt [0..1], t [seconds]
relay lists: ba|br|b and Ba|Br|B
relay usage: u
`

type Config struct {
	Listen     string
	Registry   *registry.Registry
	Relay      *relay.Manager
	Punch      *punchlog.Log
	Blocker    *abuse.Blocker
	Accounts   *account.Store
	IPChanges  *ipchange.Tracker
	RelayAddrs *relayaddr.List
	RelayPool  *relaypool.Pool
	RelayOnly  *atomic.Bool
	// PolicyOnly trims the help text to the relay console used by chast-relay.
	PolicyOnly bool
	Logger     *slog.Logger
}

type Console struct {
	cfg       Config
	listener  net.Listener
	done      chan struct{}
	closeOnce sync.Once
	wg        sync.WaitGroup
}

func New(cfg Config) (*Console, error) {
	if cfg.Relay == nil {
		return nil, errors.New("admin console needs a relay manager")
	}
	if cfg.Logger == nil {
		cfg.Logger = slog.Default()
	}
	listener, err := net.Listen("tcp", cfg.Listen)
	if err != nil {
		return nil, fmt.Errorf("admin listen %q: %w", cfg.Listen, err)
	}
	tcpAddr, ok := listener.Addr().(*net.TCPAddr)
	if !ok || !tcpAddr.IP.IsLoopback() {
		listener.Close()
		return nil, fmt.Errorf("admin listen %q must be a loopback address", cfg.Listen)
	}
	return &Console{cfg: cfg, listener: listener, done: make(chan struct{})}, nil
}

func (c *Console) Addr() string { return c.listener.Addr().String() }

func (c *Console) Serve() {
	for {
		conn, err := c.listener.Accept()
		if err != nil {
			select {
			case <-c.done:
				return
			default:
			}
			if errors.Is(err, net.ErrClosed) {
				return
			}
			c.cfg.Logger.Warn("admin accept", "error", err)
			continue
		}
		c.wg.Add(1)
		go func() {
			defer c.wg.Done()
			c.handle(conn)
		}()
	}
}

func (c *Console) Close() {
	c.closeOnce.Do(func() {
		close(c.done)
		c.listener.Close()
		c.wg.Wait()
	})
}

func (c *Console) handle(conn net.Conn) {
	defer conn.Close()
	ip, ok := remoteIP(conn.RemoteAddr())
	if !ok || !ip.IsLoopback() {
		c.cfg.Logger.Warn("admin connection rejected", "remote", conn.RemoteAddr())
		return
	}
	conn.SetReadDeadline(time.Now().Add(readTimeout))
	reader := bufio.NewReaderSize(conn, maxCommand)
	line, err := reader.ReadString('\n')
	if err != nil && line == "" {
		return
	}
	reply := c.execute(strings.TrimSpace(line))
	if reply == "" {
		return
	}
	conn.SetWriteDeadline(time.Now().Add(writeTimeout))
	_, _ = io.WriteString(conn, reply)
}

func (c *Console) execute(line string) string {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return ""
	}
	switch fields[0] {
	case "h", "help":
		if c.cfg.PolicyOnly {
			return "stats\n" + c.cfg.Relay.Policy().Help()
		}
		return helpText + c.cfg.Relay.Policy().Help()
	case "stats":
		return c.statsText()
	case "hosts":
		return c.hostsText(parseCount(fields, 20))
	case "peers":
		return c.peersText(parseCount(fields, 20))
	case "alias":
		return c.setAlias(fields)
	case "tags":
		return c.setTags(fields)
	case "punch-requests", "pr":
		return c.punchText(fields[1:])
	case "ip-blocker", "ib":
		return c.blockerText(fields[1:])
	case "ip-changes", "ic":
		return c.ipChangesText(fields[1:])
	case "relay-servers", "rs":
		return c.relayText(fields[1:])
	case "test-geo", "tg":
		return c.testGeoText(fields[1:])
	case "relay-only", "aur":
		return c.relayOnlyText(fields[1:])
	case "user", "users":
		return c.accountText(fields[1:])
	case "ab":
		return c.addressBookText(fields[1:])
	case "audit", "audits":
		return c.auditText(fields[1:])
	case "connections", "conns":
		return c.connectionsText(fields[1:])
	case "transfers", "files":
		return c.transfersText(fields[1:])
	default:
		if reply, ok := c.cfg.Relay.Policy().Command(line); ok {
			return reply
		}
		return "unknown command\n"
	}
}

func (c *Console) statsText() string {
	var b strings.Builder
	if c.cfg.Registry != nil {
		fmt.Fprintf(&b, "hosts=%d identities=%d\n", c.cfg.Registry.Count(), c.cfg.Registry.IdentityCount())
	}
	fmt.Fprintf(&b, "relay_only=%t\n", c.relayOnly())
	stats := c.cfg.Relay.Stats()
	var assigned, busy int
	var toHost, toViewer, dropped, udpBusy uint64
	var tcpToHost, tcpToViewer, tcpViewers, tcpBusy uint64
	for _, p := range stats.Ports {
		if p.HostID != "" {
			assigned++
		}
		if p.Viewer != "" {
			busy++
		}
		toHost += p.ToHost
		toViewer += p.ToViewer
		dropped += p.Dropped
		udpBusy += p.Busy
		tcpToHost += p.TCPToHost
		tcpToViewer += p.TCPToViewer
		tcpViewers += p.TCPViewers
		tcpBusy += p.TCPBusy
	}
	fmt.Fprintf(&b, "relay_ports=%d assigned=%d busy=%d\n", len(stats.Ports), assigned, busy)
	fmt.Fprintf(&b, "udp to_host=%d to_viewer=%d dropped=%d busy=%d\n", toHost, toViewer, dropped, udpBusy)
	fmt.Fprintf(&b, "tcp to_host=%d to_viewer=%d viewers=%d busy=%d\n", tcpToHost, tcpToViewer, tcpViewers, tcpBusy)
	policy := stats.Policy
	fmt.Fprintf(&b, "relay_policy single_bps=%d total_bps=%d blacklist_bps=%d blacklist=%d blocklist=%d sessions=%d\n",
		policy.SingleBandwidthBps, policy.TotalBandwidthBps, policy.BlacklistSpeedBps,
		policy.Blacklist, policy.Blocklist, policy.Sessions)
	fmt.Fprintf(&b, "relay_policy limited_bytes=%d rate_dropped=%d rate_dropped_bytes=%d blocked=%d downgraded=%d\n",
		policy.LimitedBytes, policy.RateDroppedDatagrams, policy.RateDroppedBytes,
		policy.BlockedSessions, policy.DowngradedSessions)
	if shared := stats.SharedCarrier; shared != nil {
		fmt.Fprintf(&b, "carrier addr=%s hosts=%d links=%d viewers=%d busy=%d rejected=%d to_host=%d to_viewer=%d\n",
			shared.Addr, shared.Hosts, shared.HostLinks, shared.Viewers, shared.Busy, shared.Rejected,
			shared.ToHostBytes, shared.ToViewerBytes)
	}
	if punch := c.punchStats(); punch != nil {
		fmt.Fprintf(&b, "punch_log stored=%d total=%d deduped=%d\n", punch.Stored, punch.Total, punch.Deduped)
	}
	if blocked := c.blockerStats(); blocked != nil {
		fmt.Fprintf(&b, "registration_blocked tracked=%d rate=%d ids=%d\n",
			blocked.TrackedIPs, blocked.RateBlocked, blocked.IDBlocked)
	}
	if changes := c.ipChangeStats(); changes != nil {
		fmt.Fprintf(&b, "ip_changes tracked=%d changes=%d\n", changes.Tracked, changes.Changes)
	}
	if c.cfg.RelayAddrs != nil {
		fmt.Fprintf(&b, "relay_servers=%d\n", c.cfg.RelayAddrs.Len())
	}
	if c.cfg.RelayPool != nil {
		status := c.cfg.RelayPool.Status()
		fmt.Fprintf(&b, "relay_nodes=%d healthy=%d\n", len(status.Nodes), status.Healthy)
	}
	return b.String()
}

func (c *Console) hostsText(count int) string {
	if c.cfg.Registry == nil {
		return "host tracking is unavailable on this node\n"
	}
	hosts := c.cfg.Registry.Hosts()
	if len(hosts) == 0 {
		return "no hosts\n"
	}
	if count > len(hosts) {
		count = len(hosts)
	}
	var b strings.Builder
	for _, host := range hosts[:count] {
		linked := c.carrierLinked(host)
		online := c.cfg.Registry.Online(host) || linked
		candidate := host.Candidate
		if candidate == "" {
			candidate = "-"
		}
		relayHost := host.RelayHost
		if relayHost == "" {
			relayHost = "-"
		}
		node := host.RelayNode
		if node == "" {
			node = "-"
		}
		lastSeen := "-"
		if !host.LastSeen.IsZero() {
			lastSeen = time.Since(host.LastSeen).Truncate(time.Second).String()
		}
		fmt.Fprintf(&b, "%s online=%t candidate=%s relay=%d relay_host=%s node=%s carrier=%t last_seen=%s nat=%s\n",
			host.ID, online, candidate, host.RelayPort, relayHost, node, linked, lastSeen, host.NATType)
	}
	return b.String()
}

// setAlias changes the operator-facing name of a durable peer. An empty value
// clears it; host-reported hostname is never modified.
func (c *Console) setAlias(fields []string) string {
	if c.cfg.Registry == nil {
		return "peer tracking is unavailable on this node\n"
	}
	if len(fields) < 2 {
		return "usage: alias <id> [text]\n"
	}
	id := fields[1]
	alias := strings.Join(fields[2:], " ")
	record, found, err := c.cfg.Registry.SetLabels(id, &alias, nil)
	if err != nil {
		return fmt.Sprintf("could not persist alias: %v\n", err)
	}
	if !found {
		return fmt.Sprintf("unknown peer %s\n", id)
	}
	if record.Alias == "" {
		return fmt.Sprintf("%s alias cleared\n", id)
	}
	return fmt.Sprintf("%s alias=%s\n", id, record.Alias)
}

// setTags replaces the peer tag set with a comma-separated list. No tags
// clears the set.
func (c *Console) setTags(fields []string) string {
	if c.cfg.Registry == nil {
		return "peer tracking is unavailable on this node\n"
	}
	if len(fields) < 2 {
		return "usage: tags <id> [tag1,tag2]\n"
	}
	id := fields[1]
	var tags []string
	if len(fields) > 2 {
		tags = strings.Split(strings.Join(fields[2:], " "), ",")
	}
	record, found, err := c.cfg.Registry.SetLabels(id, nil, &tags)
	if err != nil {
		return fmt.Sprintf("could not persist tags: %v\n", err)
	}
	if !found {
		return fmt.Sprintf("unknown peer %s\n", id)
	}
	if len(record.Tags) == 0 {
		return fmt.Sprintf("%s tags cleared\n", id)
	}
	return fmt.Sprintf("%s tags=%s\n", id, strings.Join(record.Tags, ","))
}

// peersText lists the durable peer records, including offline peers and their
// last reported metadata. hostsText only covers live leases.
func (c *Console) peersText(count int) string {
	if c.cfg.Registry == nil {
		return "peer tracking is unavailable on this node\n"
	}
	peers := c.cfg.Registry.Identities()
	if len(peers) == 0 {
		return "no peers\n"
	}
	if count > len(peers) {
		count = len(peers)
	}
	var b strings.Builder
	for _, peer := range peers[:count] {
		online := false
		if host, live := c.cfg.Registry.Lookup(peer.ID); live {
			online = c.cfg.Registry.Online(host) || c.carrierLinked(host)
		}
		hostname, platform, version := peer.Hostname, peer.Platform, peer.Version
		if hostname == "" {
			hostname = "-"
		}
		if platform == "" {
			platform = "-"
		}
		if version == "" {
			version = "-"
		}
		lastSeen := "-"
		if !peer.LastSeen.IsZero() {
			lastSeen = time.Since(peer.LastSeen).Truncate(time.Second).String()
		}
		alias := peer.Alias
		if alias == "" {
			alias = "-"
		}
		tags := strings.Join(peer.Tags, ",")
		if tags == "" {
			tags = "-"
		}
		fmt.Fprintf(&b, "%s online=%t hostname=%s platform=%s version=%s alias=%s tags=%s last_seen=%s\n",
			peer.ID, online, hostname, platform, version, alias, tags, lastSeen)
	}
	return b.String()
}

// carrierLinked reports whether the host holds a shared-carrier link on the
// relay that owns its lease.
func (c *Console) carrierLinked(host registry.Host) bool {
	if host.RelayNode != "" {
		return c.cfg.RelayPool != nil && c.cfg.RelayPool.HostLinked(host.RelayNode, host.ID)
	}
	return c.cfg.Relay != nil && c.cfg.Relay.HostLinked(host.ID)
}

func (c *Console) punchText(args []string) string {
	if c.cfg.Punch == nil || !c.cfg.Punch.Enabled() {
		return "punch log is disabled\n"
	}
	if len(args) > 0 && args[0] == "-" {
		c.cfg.Punch.Clear()
		return "cleared\n"
	}
	start, count := 0, 10
	if len(args) > 0 {
		if n, err := strconv.Atoi(args[0]); err == nil && n >= 0 {
			start = n
		}
	}
	if len(args) > 1 {
		if n, err := strconv.Atoi(args[1]); err == nil && n >= 0 {
			count = n
		}
	}
	if count == 0 || count > 1000 {
		count = 10
	}
	entries := c.cfg.Punch.Snapshot(start, count)
	if len(entries) == 0 {
		return "no punch requests\n"
	}
	var b strings.Builder
	for _, e := range entries {
		fmt.Fprintf(&b, "%s %s -> %s@%s\n",
			e.Time.UTC().Format(time.RFC3339), e.FromIP, e.ToID, e.ToIP)
	}
	return b.String()
}

func (c *Console) blockerText(args []string) string {
	if c.cfg.Blocker == nil {
		return "registration blocker is disabled\n"
	}
	if len(args) > 0 && args[0] == "-" {
		return fmt.Sprintf("cleared %d\n", c.cfg.Blocker.Clear())
	}
	stats := c.cfg.Blocker.Stats()
	var b strings.Builder
	fmt.Fprintf(&b, "tracked=%d rate_blocked=%d id_blocked=%d\n", stats.TrackedIPs, stats.RateBlocked, stats.IDBlocked)
	if len(args) > 0 {
		if entry, ok := c.cfg.Blocker.IPEntry(args[0]); ok {
			fmt.Fprintf(&b, "%s count=%d window_age=%s ids=%d ids_age=%s\n",
				entry.IP, entry.WindowCount, entry.WindowAge.Truncate(time.Second), entry.IDs, entry.IDsAge.Truncate(time.Second))
		} else {
			fmt.Fprintf(&b, "%s is not tracked\n", args[0])
		}
	}
	return b.String()
}

func (c *Console) ipChangesText(args []string) string {
	if c.cfg.IPChanges == nil {
		return "ip-change audit is disabled\n"
	}
	if len(args) > 0 && args[0] == "-" {
		return fmt.Sprintf("cleared %d\n", c.cfg.IPChanges.Clear())
	}
	stats := c.cfg.IPChanges.Stats()
	var b strings.Builder
	fmt.Fprintf(&b, "tracked=%d changes=%d\n", stats.Tracked, stats.Changes)
	if len(args) == 0 {
		writeIPChanges(&b, c.cfg.IPChanges.Snapshot(0, 10))
		return b.String()
	}
	if len(args) > 1 && args[1] == "-" {
		if c.cfg.IPChanges.Remove(args[0]) {
			b.WriteString("removed\n")
		} else {
			fmt.Fprintf(&b, "%s is not tracked\n", args[0])
		}
		return b.String()
	}
	if start, err := strconv.Atoi(args[0]); err == nil && start >= 0 {
		count := 10
		if len(args) > 1 {
			if n, err := strconv.Atoi(args[1]); err == nil && n > 0 {
				count = n
			}
		}
		if count > 1000 {
			count = 1000
		}
		writeIPChanges(&b, c.cfg.IPChanges.Snapshot(start, count))
		return b.String()
	}
	entry, ok := c.cfg.IPChanges.Lookup(args[0])
	if !ok {
		fmt.Fprintf(&b, "%s is not tracked\n", args[0])
		return b.String()
	}
	b.WriteString(formatIPChange(entry))
	b.WriteString("\n")
	return b.String()
}

func writeIPChanges(b *strings.Builder, entries []ipchange.Entry) {
	for _, entry := range entries {
		b.WriteString(formatIPChange(entry))
		b.WriteString("\n")
	}
}

func formatIPChange(entry ipchange.Entry) string {
	var b strings.Builder
	fmt.Fprintf(&b, "%s age=%s ips=", entry.ID, entry.Age.Truncate(time.Second))
	for i, ip := range entry.IPs {
		if i > 0 {
			b.WriteString(",")
		}
		fmt.Fprintf(&b, "%s:%d", ip.IP, ip.Count)
	}
	return b.String()
}

func (c *Console) relayText(args []string) string {
	var b strings.Builder
	if c.cfg.RelayPool != nil {
		status := c.cfg.RelayPool.Status()
		fmt.Fprintf(&b, "relay_nodes=%d healthy=%d\n", len(status.Nodes), status.Healthy)
		for _, node := range status.Nodes {
			state := "down"
			if node.Healthy {
				state = "up"
			}
			fmt.Fprintf(&b, "node=%s status=%s leases=%d linked=%d rtt_ms=%d",
				node.URL, state, node.Leases, node.Linked, node.RTTMS)
			if node.Error != "" {
				fmt.Fprintf(&b, " error=%q", node.Error)
			}
			b.WriteString("\n")
		}
	}
	if c.cfg.RelayAddrs == nil {
		if c.cfg.RelayPool != nil {
			return b.String()
		}
		return "relay address list is unavailable\n"
	}
	if len(args) == 0 {
		b.WriteString(formatRelayAddrs(c.cfg.RelayAddrs.Addrs()))
		return b.String()
	}
	if len(args) == 1 && args[0] == "-" {
		b.WriteString("reset\n")
		b.WriteString(formatRelayAddrs(c.cfg.RelayAddrs.Reset()))
		return b.String()
	}
	if err := c.cfg.RelayAddrs.SetCSV(strings.Join(args, ",")); err != nil {
		fmt.Fprintf(&b, "relay-servers: %v\n", err)
		return b.String()
	}
	b.WriteString(formatRelayAddrs(c.cfg.RelayAddrs.Addrs()))
	return b.String()
}

func formatRelayAddrs(addrs []string) string {
	if len(addrs) == 0 {
		return "no relay servers\n"
	}
	var b strings.Builder
	for _, addr := range addrs {
		b.WriteString(addr)
		b.WriteString("\n")
	}
	return b.String()
}

func (c *Console) testGeoText(args []string) string {
	if len(args) == 0 || net.ParseIP(args[0]) == nil ||
		(len(args) > 1 && net.ParseIP(args[1]) == nil) {
		return "usage: test-geo <ip1> [ip2]\n"
	}
	if c.cfg.RelayPool != nil {
		if node, ok := c.cfg.RelayPool.Preview(); ok {
			return fmt.Sprintf("next=%s\n", node.URL)
		}
	}
	if c.cfg.RelayAddrs == nil {
		return "relay address list is unavailable\n"
	}
	next := c.cfg.RelayAddrs.Next()
	if next == "" {
		return "no relay servers\n"
	}
	return fmt.Sprintf("next=%s\n", next)
}

func (c *Console) relayOnlyText(args []string) string {
	if c.cfg.RelayOnly == nil {
		return "relay-only switch is unavailable\n"
	}
	if len(args) > 0 {
		switch strings.ToLower(args[0]) {
		case "y", "yes", "1", "true", "on":
			c.cfg.RelayOnly.Store(true)
		case "n", "no", "0", "false", "off":
			c.cfg.RelayOnly.Store(false)
		default:
			return "usage: relay-only Y|N\n"
		}
	}
	return fmt.Sprintf("relay_only=%t\n", c.relayOnly())
}

func (c *Console) relayOnly() bool {
	return c.cfg.RelayOnly != nil && c.cfg.RelayOnly.Load()
}

func (c *Console) punchStats() *punchlog.Stats {
	if c.cfg.Punch == nil {
		return nil
	}
	stats := c.cfg.Punch.Stats()
	return &stats
}

func (c *Console) blockerStats() *abuse.Stats {
	if c.cfg.Blocker == nil {
		return nil
	}
	stats := c.cfg.Blocker.Stats()
	return &stats
}

func (c *Console) ipChangeStats() *ipchange.Stats {
	if c.cfg.IPChanges == nil {
		return nil
	}
	stats := c.cfg.IPChanges.Stats()
	return &stats
}

func parseCount(fields []string, fallback int) int {
	if len(fields) < 2 {
		return fallback
	}
	n, err := strconv.Atoi(fields[1])
	if err != nil || n <= 0 {
		return fallback
	}
	if n > 200 {
		n = 200
	}
	return n
}

func remoteIP(addr net.Addr) (net.IP, bool) {
	tcpAddr, ok := addr.(*net.TCPAddr)
	if !ok {
		return nil, false
	}
	return tcpAddr.IP, true
}
