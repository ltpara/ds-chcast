// chast-server is the rendezvous, punch coordinator and UDP relay for chast.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/admin"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/ratelimit"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
	"github.com/ltpara/ds-chcast/server/internal/rendezvous"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "chast-server:", err)
		os.Exit(1)
	}
}

func run() error {
	var (
		httpListen   = flag.String("http-listen", "0.0.0.0:8443", "HTTP registration and lookup address")
		punchListen  = flag.String("punch-listen", "0.0.0.0:8444", "UDP rendezvous and punch address")
		relayListen  = flag.String("relay-listen", "0.0.0.0", "IP address for relay UDP ports")
		portRange    = flag.String("relay-ports", "41000-41099", "relay UDP ports, e.g. 41000-41099")
		carrierAddr  = flag.String("carrier-listen", "0.0.0.0:8445", "shared TCP/WebSocket carrier address; empty disables it")
		advertise    = flag.String("advertise", "", "public IP or DNS name returned to peers")
		relayServers = flag.String("relay-servers", "", "comma-separated relay host names or IPs without ports; empty uses --advertise")
		relayNodes   = flag.String("relay-nodes", "", "comma-separated standalone relay node URLs, e.g. http://relay1:8447")
		relayNodeKey = flag.String("relay-node-key", "", "control key shared with every --relay-nodes entry")
		relayHealth  = flag.Duration("relay-node-health", 10*time.Second, "how often standalone relay nodes are probed")
		serverKey    = flag.String("server-key", "", "required for registration unless empty")
		hostTTL      = flag.Duration("host-ttl", 60*time.Second, "how long a registration survives without renewal")
		offline      = flag.Duration("offline-after", 30*time.Second, "keepalive silence that makes a host offline")
		stateFile    = flag.String("state", "chast-server-state.json", "persistent ID/public-key bindings; empty disables persistence")
		identityFile = flag.String("identity", "chast-server-identity.key", "Ed25519 server identity seed; created on first run, empty for ephemeral")
		tlsCert      = flag.String("tls-cert", "", "PEM certificate for the HTTP API; requires --tls-key")
		tlsKey       = flag.String("tls-key", "", "PEM private key for the HTTP API; requires --tls-cert")
		apiRate      = flag.Float64("api-rate", 50, "sustained HTTP requests per second per client IP; 0 disables the limiter")
		apiBurst     = flag.Int("api-burst", 100, "HTTP burst allowance per client IP")
		proxyHeader  = flag.String("proxy-header", "", "header carrying the client IP behind a trusted proxy, e.g. X-Forwarded-For")
		relayOnly    = flag.Bool("relay-only", false, "hide direct candidates and force viewers onto the relay or carrier")
		adminListen  = flag.String("admin-listen", "127.0.0.1:8446", "loopback-only plain-text admin console; empty disables it")

		relaySingle    = flag.Float64("relay-single-bandwidth", 128, "per-session relay bandwidth in Mbit/s; 0 disables the limit")
		relayTotal     = flag.Float64("relay-total-bandwidth", 1024, "total relay bandwidth in Mbit/s; 0 disables the limit")
		relayBlacklist = flag.Float64("relay-blacklist-speed", 32, "relay bandwidth for blacklisted IPs in Mbit/s")
		relayDowngrade = flag.Duration("relay-downgrade-after", 30*time.Minute, "session age before a sustained relay is downgraded")
		relayRatio     = flag.Float64("relay-downgrade-threshold", 0.66, "speed/limit ratio that triggers downgrade, 0..1")
		relayBlackFile = flag.String("relay-blacklist-file", "", "IP list file that receives the slower relay rate")
		relayBlockFile = flag.String("relay-blocklist-file", "", "IP list file whose viewers are disconnected")
		punchLogMax    = flag.Int("punch-log-max", 100000, "retained punch request records; 0 disables the log")
		regMax         = flag.Int("register-max", 30, "registration attempts per IP per window; 0 disables the rate check")
		regWindow      = flag.Duration("register-window", time.Minute, "registration rate window")
		regMaxIDs      = flag.Int("register-max-ids", 300, "distinct IDs per IP per ID window; 0 disables the check")
		regIDWindow    = flag.Duration("register-id-window", 24*time.Hour, "distinct ID window per IP")
		verbose        = flag.Bool("verbose", false, "debug logging")
	)
	flag.Parse()

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	if *apiRate < 0 {
		return errors.New("--api-rate must not be negative")
	}
	if *apiBurst < 1 {
		return errors.New("--api-burst must be at least 1")
	}
	if *punchLogMax < 0 {
		return errors.New("--punch-log-max must not be negative")
	}
	if *regMax < 0 || *regMaxIDs < 0 {
		return errors.New("--register-max and --register-max-ids must not be negative")
	}
	if *regMax > 0 && *regWindow < time.Second {
		return errors.New("--register-window must be at least 1s")
	}
	if *regMaxIDs > 0 && *regIDWindow < time.Minute {
		return errors.New("--register-id-window must be at least 1m")
	}
	tlsConfig, err := netutil.LoadTLSConfig(*tlsCert, *tlsKey)
	if err != nil {
		return err
	}

	ports, err := netutil.ParsePorts(*portRange)
	if err != nil {
		return err
	}
	advertised := *advertise
	if advertised == "" {
		advertised, err = netutil.DetectAdvertise()
		if err != nil {
			return err
		}
		log.Info("advertising the detected address; pass --advertise to override", "host", advertised)
	}
	if *hostTTL < 10*time.Second {
		return errors.New("--host-ttl must be at least 10s")
	}
	relayHosts := []string{advertised}
	if *relayServers != "" {
		relayHosts = strings.Split(*relayServers, ",")
	}
	relayAddrs, err := relayaddr.New(relayHosts)
	if err != nil {
		return fmt.Errorf("--relay-servers: %w", err)
	}
	if *relayNodes != "" && *relayNodeKey == "" {
		return errors.New("--relay-nodes requires --relay-node-key")
	}
	if *relayNodes == "" && *relayNodeKey != "" {
		return errors.New("--relay-node-key requires --relay-nodes")
	}
	if *relayHealth < time.Second {
		return errors.New("--relay-node-health must be at least 1s")
	}

	serverIdentity, err := identity.LoadOrCreateServer(*identityFile)
	if err != nil {
		return err
	}
	if *identityFile == "" {
		log.Warn("running with an ephemeral server identity; host bindings will not verify across restarts")
	}
	log.Info("server identity", "public_key", serverIdentity.PublicKeyText())

	reg, err := registry.NewPersistent(*offline, *stateFile)
	if err != nil {
		return err
	}
	relayOnlyFlag := &atomic.Bool{}
	relayOnlyFlag.Store(*relayOnly)
	punchLog := punchlog.New(*punchLogMax, 60*time.Second)
	ipChanges := ipchange.New(0)
	var blocker *abuse.Blocker
	if *regMax > 0 || *regMaxIDs > 0 {
		blocker = abuse.New(abuse.Config{
			Window:       *regWindow,
			MaxPerWindow: *regMax,
			IDWindow:     *regIDWindow,
			MaxIDs:       *regMaxIDs,
		})
	}
	relayAddr := func(port int) string {
		return net.JoinHostPort(advertised, strconv.Itoa(port))
	}
	relayManager, err := relay.NewManager(*relayListen, ports, reg, log)
	if err != nil {
		return err
	}
	defer relayManager.Close()
	if err := relayManager.SetPolicyConfig(relay.ConfigFromMbit(
		*relaySingle, *relayTotal, *relayBlacklist,
		*relayRatio, *relayDowngrade, *relayBlackFile, *relayBlockFile,
	)); err != nil {
		return fmt.Errorf("relay policy: %w", err)
	}
	if *carrierAddr != "" {
		if err := relayManager.EnableSharedCarrier(*carrierAddr); err != nil {
			return err
		}
		log.Info("shared carrier listening", "on", relayManager.CarrierAddr())
	}

	var pool *relaypool.Pool
	if *relayNodes != "" {
		pool, err = relaypool.New(relaypool.Config{
			Nodes:    strings.Split(*relayNodes, ","),
			Key:      *relayNodeKey,
			Interval: *relayHealth,
			Logger:   log,
		})
		if err != nil {
			return fmt.Errorf("--relay-nodes: %w", err)
		}
		pool.Start()
		defer pool.Close()
		log.Info("relay nodes configured", "nodes", pool.NodeURLs(), "health_interval", *relayHealth)
	}

	meet, err := rendezvous.New(*punchListen, reg, relayAddr, log)
	if err != nil {
		return err
	}
	meet.SetIdentity(serverIdentity)
	meet.SetPunchLog(punchLog)
	meet.SetIPChanges(ipChanges)
	defer meet.Close()
	_, punchPortText, err := net.SplitHostPort(meet.Addr())
	if err != nil {
		return err
	}
	punchAddr := net.JoinHostPort(advertised, punchPortText)

	var console *admin.Console
	if *adminListen != "" {
		console, err = admin.New(admin.Config{
			Listen:     *adminListen,
			Registry:   reg,
			Relay:      relayManager,
			Punch:      punchLog,
			Blocker:    blocker,
			IPChanges:  ipChanges,
			RelayAddrs: relayAddrs,
			RelayPool:  pool,
			RelayOnly:  relayOnlyFlag,
			Logger:     log,
		})
		if err != nil {
			return err
		}
		defer console.Close()
		go console.Serve()
		log.Info("admin console listening", "on", console.Addr())
	}

	handler := api.New(api.Config{
		Registry:    reg,
		Relay:       relayManager,
		Identity:    serverIdentity,
		PunchAddr:   punchAddr,
		RelayHost:   advertised,
		ServerKey:   *serverKey,
		TTL:         *hostTTL,
		RelayOnly:   relayOnlyFlag,
		Blocker:     blocker,
		PunchLog:    punchLog,
		IPChanges:   ipChanges,
		RelayAddrs:  relayAddrs,
		RelayPool:   pool,
		Logger:      log,
		Limiter:     ratelimit.New(*apiRate, float64(*apiBurst)),
		ProxyHeader: *proxyHeader,
		TLS:         tlsConfig != nil,
	})
	httpServer := &http.Server{
		Addr:              *httpListen,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}
	listener, err := net.Listen("tcp", *httpListen)
	if err != nil {
		meet.Close()
		relayManager.Close()
		return fmt.Errorf("HTTP listen %q: %w", *httpListen, err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go meet.Serve()
	go func() {
		var serveErr error
		if tlsConfig != nil {
			httpServer.TLSConfig = tlsConfig
			log.Info("HTTPS API listening", "on", listener.Addr().String())
			serveErr = httpServer.ServeTLS(listener, "", "")
		} else {
			log.Info("HTTP API listening", "on", listener.Addr().String())
			serveErr = httpServer.Serve(listener)
		}
		if serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			log.Error("HTTP server stopped", "error", serveErr)
			stop()
		}
	}()
	go expireLoop(ctx, reg, relayManager, pool, log)

	log.Info("chast-server ready",
		"punch", punchAddr,
		"relay_ports", *portRange,
		"carrier", relayManager.CarrierAddr(),
		"server_key", *serverKey != "",
		"tls", tlsConfig != nil,
		"api_rate", *apiRate,
		"relay_only", relayOnlyFlag.Load(),
		"relay_servers", relayAddrs.Addrs(),
		"relay_nodes", *relayNodes,
		"admin", *adminListen,
		"punch_log_max", *punchLogMax,
		"register_max", *regMax,
		"register_max_ids", *regMaxIDs,
	)
	<-ctx.Done()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
	log.Info("chast-server stopped")
	return nil
}

func expireLoop(ctx context.Context, reg *registry.Registry, relayManager *relay.Manager, pool *relaypool.Pool, log *slog.Logger) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			for _, host := range reg.Expire() {
				if host.RelayNode != "" {
					if pool != nil {
						pool.ReleaseAsync(host.RelayNode, host.ID)
					}
				} else {
					relayManager.Release(host.ID)
				}
				log.Info("host registration expired", "id", host.ID, "relay_node", host.RelayNode)
			}
		}
	}
}
