// chast-tunnel connects a local chast QUIC endpoint to the rendezvous and relay
// service. It runs next to chast-host (host mode) or next to the viewer (viewer
// mode) and moves opaque UDP datagrams, so the chast protocol is unchanged.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/agent"
	"github.com/ltpara/ds-chcast/server/internal/lan"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "chast-tunnel:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: chast-tunnel host|viewer|discover|apply-update [flags]")
	}
	switch args[0] {
	case "host":
		return runHost(args[1:])
	case "viewer":
		return runViewer(args[1:])
	case "discover":
		return runDiscover(args[1:])
	case "apply-update":
		return runApplyUpdate(args[1:])
	default:
		return fmt.Errorf("unknown mode %q; use host, viewer, discover or apply-update", args[0])
	}
}

func runHost(args []string) error {
	fs := flag.NewFlagSet("host", flag.ExitOnError)
	server := fs.String("server", "", "HTTP API base, e.g. http://127.0.0.1:8443")
	serverKey := fs.String("server-key", "", "server key required by the API")
	serverPubKey := fs.String("server-pubkey", "", "pin the server Ed25519 public key returned by /v1/server")
	rendezvousList := fs.String("rendezvous-servers", "", "extra rendezvous HTTP API bases tried on failure; the server's pushed list replaces them")
	id := fs.String("id", "", "host id registered with the service")
	oldID := fs.String("old-id", "", "previous host id to migrate on the server (used after changing --id)")
	key := fs.String("key", "", "host key, at least 8 characters")
	fingerprint := fs.String("fingerprint", "", "certificate fingerprint shown in lookups")
	local := fs.String("local", "127.0.0.1:4433", "chast-host QUIC listener")
	carrierAddr := fs.String("carrier", "", "chast-host TCP carrier listener (127.0.0.1:P+1); empty disables the relay's TCP fallback")
	auditSpool := fs.String("audit-spool", "", "chast-host JSONL audit spool; the agent signs and forwards it")
	listen := fs.String("listen", "0.0.0.0:14434", "public-facing UDP address of this agent")
	keepalive := fs.Duration("keepalive", 10*time.Second, "rendezvous keepalive interval")
	upnpEnabled := fs.Bool("upnp", false, "map the UDP port on the local UPnP gateway and report it")
	autoUpdate := fs.Bool("auto-update", false, "stage and launch the installer advertised by the server; needs --server-pubkey")
	autoUpdateDir := fs.String("auto-update-dir", "", "directory for the downloaded installer (default: system temp)")
	lanDiscovery := fs.Bool("lan-discovery", true, "answer LAN discovery pings; exposes id, hostname, username, platform and version on the local link")
	lanPort := fs.Int("lan-port", lan.DefaultPort, "UDP port for LAN discovery")
	tlsCA := fs.String("tls-ca", "", "PEM CA bundle for an HTTPS rendezvous server using a private CA")
	tlsCert := fs.String("tls-client-cert", "", "client certificate presented when the rendezvous server requires mTLS")
	tlsKey := fs.String("tls-client-key", "", "private key for --tls-client-cert")
	verbose := fs.Bool("verbose", false, "debug logging")
	fs.Parse(args)

	if *lanPort < 1 || *lanPort > 65535 {
		return fmt.Errorf("invalid --lan-port %d", *lanPort)
	}
	tlsConfig, err := netutil.ClientTLSConfig(*tlsCA, *tlsCert, *tlsKey)
	if err != nil {
		return err
	}

	log := newLogger(*verbose)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// Stdout carries machine events for the tray. Registration and path
	// changes are dropped until the ready snapshot is written, so an embedding
	// UI never sees a partial state.
	var emitMu sync.Mutex
	ready := false
	writeEvent := func(event agentEvent) {
		_ = json.NewEncoder(os.Stdout).Encode(event)
	}
	emit := func(event agentEvent) {
		if event.Event != "ready" && !ready {
			return
		}
		writeEvent(event)
	}

	h, err := agent.StartHost(ctx, agent.HostConfig{
		ServerURL:       *server,
		Servers:         splitCSV(*rendezvousList),
		ServerKey:       *serverKey,
		ServerPublicKey: *serverPubKey,
		ID:              *id,
		OldID:           *oldID,
		Key:             *key,
		Fingerprint:     *fingerprint,
		LocalAddr:       *local,
		CarrierAddr:     *carrierAddr,
		AuditSpool:      *auditSpool,
		ListenAddr:      *listen,
		Keepalive:       *keepalive,
		UPnP:            *upnpEnabled,
		AutoUpdate:      *autoUpdate,
		UpdateDir:       *autoUpdateDir,
		TLS:             tlsConfig,
		Logger:          log,
		OnInfo: func(info agent.HostInfo) {
			emitMu.Lock()
			defer emitMu.Unlock()
			event := agentEvent{
				Event:        "relay",
				Listen:       info.Listen,
				Host:         info.ID,
				Path:         info.Path,
				Remote:       info.Viewer,
				Punch:        info.Punch,
				Relay:        info.Relay,
				NAT:          info.NAT,
				MigratedFrom: info.MigratedFrom,
			}
			if info.Viewer != "" {
				event.Event = "viewer"
			}
			emit(event)
		},
	})
	if err != nil {
		return err
	}
	defer h.Close()
	readyCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	err = h.WaitReady(readyCtx)
	cancel()
	if err != nil {
		return fmt.Errorf("become reachable: %w", err)
	}
	info := h.Info()
	emitMu.Lock()
	ready = true
	writeEvent(agentEvent{
		Event:        "ready",
		Listen:       info.Listen,
		Host:         info.ID,
		Path:         info.Path,
		Remote:       info.Viewer,
		Punch:        info.Punch,
		Relay:        info.Relay,
		NAT:          info.NAT,
		MigratedFrom: info.MigratedFrom,
	})
	emitMu.Unlock()
	log.Info("host agent online", "id", *id)
	if *lanDiscovery {
		profile := lan.LocalInfo(info.ID, agent.Version)
		if _, err := lan.Start(ctx, fmt.Sprintf("0.0.0.0:%d", *lanPort), profile, log); err != nil {
			// Discovery is a convenience; a busy port must not take the host
			// offline.
			log.Warn("LAN discovery unavailable", "port", *lanPort, "error", err)
		} else {
			log.Info("LAN discovery listening", "port", *lanPort, "id", info.ID)
		}
	}
	<-ctx.Done()
	return nil
}

func runViewer(args []string) error {
	fs := flag.NewFlagSet("viewer", flag.ExitOnError)
	server := fs.String("server", "", "HTTP API base, e.g. http://127.0.0.1:8443")
	serverKey := fs.String("server-key", "", "server key required by the API")
	serverPubKey := fs.String("server-pubkey", "", "pin the server Ed25519 public key returned by /v1/server")
	rendezvousList := fs.String("rendezvous-servers", "", "extra rendezvous HTTP API bases tried on failure; the server's pushed list replaces them")
	id := fs.String("id", "", "host id to reach")
	listen := fs.String("listen", "127.0.0.1:24434", "local UDP address for the QUIC client")
	mode := fs.String("mode", agent.ModeAuto, "auto: direct, relay or carrier; direct; relay; carrier")
	directTimeout := fs.Duration("direct-timeout", 2*time.Second, "how long auto waits for the direct path")
	upnpEnabled := fs.Bool("upnp", false, "map the UDP port on the local UPnP gateway and report it")
	tlsCA := fs.String("tls-ca", "", "PEM CA bundle for an HTTPS rendezvous server using a private CA")
	tlsCert := fs.String("tls-client-cert", "", "client certificate presented when the rendezvous server requires mTLS")
	tlsKey := fs.String("tls-client-key", "", "private key for --tls-client-cert")
	verbose := fs.Bool("verbose", false, "debug logging")
	fs.Parse(args)

	tlsConfig, err := netutil.ClientTLSConfig(*tlsCA, *tlsCert, *tlsKey)
	if err != nil {
		return err
	}

	log := newLogger(*verbose)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	// Stdout is a machine-readable event stream for an embedding client;
	// diagnostics stay on stderr. The first ready event tells the client which
	// local UDP port to point its QUIC connection at.
	var emitMu sync.Mutex
	emit := func(event agentEvent) {
		emitMu.Lock()
		defer emitMu.Unlock()
		_ = json.NewEncoder(os.Stdout).Encode(event)
	}

	v, err := agent.StartViewer(ctx, agent.ViewerConfig{
		ServerURL:       *server,
		Servers:         splitCSV(*rendezvousList),
		ServerKey:       *serverKey,
		ServerPublicKey: *serverPubKey,
		HostID:          *id,
		ListenAddr:      *listen,
		Mode:            *mode,
		DirectTimeout:   *directTimeout,
		UPnP:            *upnpEnabled,
		TLS:             tlsConfig,
		Logger:          log,
		OnPath: func(info agent.PathInfo) {
			emit(agentEvent{
				Event:        "path",
				Path:         info.Path,
				Remote:       info.Remote,
				Direct:       info.Direct,
				Relay:        info.Relay,
				NAT:          info.NAT,
				Candidates:   info.Candidates,
				Carrier:      info.Carrier,
				CarrierToken: info.CarrierToken,
			})
		},
	})
	if err != nil {
		return err
	}
	defer v.Close()
	info := v.PathInfo()
	emit(agentEvent{
		Event:        "ready",
		Listen:       v.LocalAddr().String(),
		Host:         *id,
		Path:         info.Path,
		Remote:       info.Remote,
		Direct:       info.Direct,
		Relay:        info.Relay,
		NAT:          info.NAT,
		Candidates:   info.Candidates,
		Carrier:      info.Carrier,
		CarrierToken: info.CarrierToken,
	})
	log.Info("viewer agent online", "host", *id)
	<-ctx.Done()
	return nil
}

func runDiscover(args []string) error {
	fs := flag.NewFlagSet("discover", flag.ExitOnError)
	port := fs.Int("port", lan.DefaultPort, "discovery UDP port")
	timeout := fs.Duration("timeout", lan.DefaultTimeout, "how long to wait for answers")
	selfID := fs.String("id", "", "local host id to exclude from the results")
	target := fs.String("target", "", "comma-separated host:port unicast probes in addition to broadcast")
	asJSON := fs.Bool("json", false, "print a JSON array instead of a table")
	fs.Parse(args)
	if *port < 1 || *port > 65535 {
		return fmt.Errorf("invalid --port %d", *port)
	}
	var targets []string
	for _, part := range strings.Split(*target, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if _, _, err := net.SplitHostPort(part); err != nil {
			return fmt.Errorf("invalid --target %q: %w", part, err)
		}
		targets = append(targets, part)
	}

	peers, err := lan.Discover(context.Background(), lan.DiscoverOptions{
		Port:    *port,
		Timeout: *timeout,
		SelfID:  *selfID,
		Targets: targets,
	})
	if err != nil {
		return err
	}
	if peers == nil {
		peers = []lan.Peer{}
	}
	if *asJSON {
		enc := json.NewEncoder(os.Stdout)
		enc.SetIndent("", "  ")
		return enc.Encode(peers)
	}
	if len(peers) == 0 {
		fmt.Printf("no chast hosts answered on UDP %d\n", *port)
		return nil
	}
	fmt.Printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
		"ID", "ADDRESS", "HOSTNAME", "USERNAME", "PLATFORM", "VERSION", "MAC")
	for _, peer := range peers {
		fmt.Printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			peer.ID, peer.Address, display(peer.Hostname), display(peer.Username),
			display(peer.Platform), display(peer.Version), display(peer.MAC))
	}
	return nil
}

func display(value string) string {
	if value == "" {
		return "-"
	}
	return value
}

func splitCSV(value string) []string {
	var out []string
	for _, part := range strings.Split(value, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, part)
		}
	}
	return out
}

type agentEvent struct {
	Event        string `json:"event"`
	Listen       string `json:"listen,omitempty"`
	Host         string `json:"host,omitempty"`
	Path         string `json:"path,omitempty"`
	Remote       string `json:"remote,omitempty"`
	Punch        string `json:"punch,omitempty"`
	Direct       string `json:"direct,omitempty"`
	Relay        string `json:"relay,omitempty"`
	NAT          string `json:"nat,omitempty"`
	Candidates   int    `json:"candidates,omitempty"`
	Carrier      string `json:"carrier,omitempty"`
	CarrierToken string `json:"carrier_token,omitempty"`
	MigratedFrom string `json:"migrated_from,omitempty"`
}

func newLogger(verbose bool) *slog.Logger {
	level := slog.LevelInfo
	if verbose {
		level = slog.LevelDebug
	}
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
}
