// chast-relay is a standalone relay node for chast, the hbbr equivalent.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/admin"
	"github.com/ltpara/ds-chcast/server/internal/logging"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relaynode"
	"github.com/ltpara/ds-chcast/server/internal/servercfg"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "chast-relay:", err)
		os.Exit(1)
	}
}

func run() error {
	var (
		httpListen  = flag.String("http-listen", "0.0.0.0:8447", "control API address used by hbbs")
		relayListen = flag.String("relay-listen", "0.0.0.0", "IP address for relay UDP/TCP ports")
		relayV6     = flag.String("relay-listen-v6", "", "optional IPv6 address for relay UDP/TCP ports, same port numbers as --relay-listen")
		portRange   = flag.String("relay-ports", "41100-41199", "relay UDP/TCP ports, e.g. 41100-41199")
		carrierAddr = flag.String("carrier-listen", "0.0.0.0:8448", "shared TCP/WebSocket carrier address; empty disables it")
		carrierV6   = flag.String("carrier-listen-v6", "", "optional IPv6 address for the shared carrier, same port as --carrier-listen")
		advertise   = flag.String("advertise", "", "host name or IP published to hbbs and peers")
		advertiseV6 = flag.String("advertise-v6", "", "optional public IPv6 address of the shared carrier; requires --carrier-listen-v6")
		nodeKey     = flag.String("key", "", "control key shared with hbbs --relay-node-key")
		nodeID      = flag.String("node-id", "", "node name shown in health and stats")
		janitor     = flag.Duration("lease-janitor", 5*time.Second, "how often expired leases are released")
		tlsCert     = flag.String("tls-cert", "", "PEM certificate for the control API; requires --tls-key")
		tlsKey      = flag.String("tls-key", "", "PEM private key for the control API; requires --tls-cert")
		tlsClientCA = flag.String("tls-client-ca", "", "PEM CA bundle; when set, HTTPS clients must present a certificate signed by it (mTLS)")
		adminListen = flag.String("admin-listen", "127.0.0.1:8449", "loopback-only relay console; empty disables it")

		singleBandwidth = flag.Float64("relay-single-bandwidth", 128, "per-session relay bandwidth in Mbit/s; 0 disables the limit")
		totalBandwidth  = flag.Float64("relay-total-bandwidth", 1024, "total relay bandwidth in Mbit/s; 0 disables the limit")
		blacklistSpeed  = flag.Float64("relay-blacklist-speed", 32, "relay bandwidth for blacklisted IPs in Mbit/s")
		downgradeAfter  = flag.Duration("relay-downgrade-after", 30*time.Minute, "session age before a sustained relay is downgraded")
		downgradeRatio  = flag.Float64("relay-downgrade-threshold", 0.66, "speed/limit ratio that triggers downgrade, 0..1")
		blacklistFile   = flag.String("relay-blacklist-file", "", "IP list file that receives the slower relay rate; empty probes ./blacklist.txt")
		blocklistFile   = flag.String("relay-blocklist-file", "", "IP list file whose viewers are disconnected; empty probes ./blocklist.txt")
		configFile      = flag.String("config", "", "INI file with flag values; an explicit flag still wins")
		envFile         = flag.String("env-file", ".env", "dotenv file with flag values; empty disables it")
		rmem            = flag.Int("rmem", 0, "UDP receive buffer in bytes for relay sockets; 0 keeps the system default")
		verbose         = flag.Bool("verbose", false, "debug logging")
		logFile         = flag.String("log-file", "", "also append logs to this file; empty writes to stderr only")
		logMaxSize      = flag.Int64("log-max-size", logging.DefaultMaxSize>>20, "rotate --log-file after this many MiB")
		logMaxBackups   = flag.Int("log-max-backups", logging.DefaultMaxBackups, "rotated log files to keep; 0 keeps all")
		logMaxAge       = flag.Duration("log-max-age", 0, "delete rotated logs older than this; 0 keeps all")
		logCompress     = flag.Bool("log-compress", true, "gzip rotated log files")
		traceEndpoint   = flag.String("trace-endpoint", "", "OTLP/HTTP collector base URL; spans post to <url>/v1/traces; empty disables tracing")
		traceSample     = flag.Float64("trace-sample", 1, "fraction of HTTP requests traced, 0..1")
		traceService    = flag.String("trace-service", "chast-relay", "service.name attached to exported spans")
	)
	flag.Parse()

	var layers []*servercfg.Layer
	if *envFile != "" {
		layer, err := servercfg.LoadEnvFile(*envFile)
		if err != nil && !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("--env-file: %w", err)
		}
		if layer != nil {
			layers = append(layers, layer)
		}
	}
	if *configFile != "" {
		layer, err := servercfg.LoadINI(*configFile)
		if err != nil {
			return fmt.Errorf("--config: %w", err)
		}
		layers = append([]*servercfg.Layer{layer}, layers...)
	}
	layers = append(layers, servercfg.FromEnv())
	if err := servercfg.Apply(flag.CommandLine, layers...); err != nil {
		return err
	}

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	log, logSink, err := newLogger(level, logging.Config{
		Path:       *logFile,
		MaxSize:    *logMaxSize << 20,
		MaxBackups: *logMaxBackups,
		MaxAge:     *logMaxAge,
		Compress:   *logCompress,
	})
	if err != nil {
		return err
	}
	defer logSink.Close()

	tracer := tracing.New(tracing.Config{
		Endpoint: *traceEndpoint,
		Service:  *traceService,
		Sample:   *traceSample,
	})
	defer func() {
		closeCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		if err := tracer.Close(closeCtx); err != nil {
			log.Warn("tracer shutdown", "error", err)
		}
	}()
	if tracer != nil {
		log.Info("tracing enabled", "endpoint", *traceEndpoint, "sample", *traceSample, "service", *traceService)
	}

	if *nodeKey == "" {
		return errors.New("--key is required; hbbs passes the same value as --relay-node-key")
	}
	if *janitor < time.Second {
		return errors.New("--lease-janitor must be at least 1s")
	}
	if *rmem < 0 {
		return errors.New("--rmem must not be negative")
	}
	tlsConfig, err := netutil.LoadReloadingTLSConfig(*tlsCert, *tlsKey, *tlsClientCA, log,
		netutil.CertificateReloadInterval)
	if err != nil {
		return err
	}
	ports, err := netutil.ParsePorts(*portRange)
	if err != nil {
		return err
	}
	advertised := *advertise
	if advertised == "" {
		return errors.New("--advertise is required; it is the address hbbs hands to hosts and viewers")
	}

	auth := relaynode.NewAuthenticator()
	manager, err := relay.NewManager(*relayListen, ports, auth, log,
		relay.WithReadBuffer(*rmem),
		relay.WithViewerTokenRequired(true),
		relay.WithBind6(*relayV6),
		relay.WithCarrierBind6(*carrierV6),
	)
	if err != nil {
		return err
	}
	defer manager.Close()
	blackFile := relay.DefaultListFile(*blacklistFile, "blacklist.txt")
	if strings.TrimSpace(*blacklistFile) == "" && blackFile != "" {
		log.Info("using the default relay blacklist file", "path", blackFile)
	}
	blockFile := relay.DefaultListFile(*blocklistFile, "blocklist.txt")
	if strings.TrimSpace(*blocklistFile) == "" && blockFile != "" {
		log.Info("using the default relay blocklist file", "path", blockFile)
	}
	if err := manager.SetPolicyConfig(relay.ConfigFromMbit(
		*singleBandwidth, *totalBandwidth, *blacklistSpeed,
		*downgradeRatio, *downgradeAfter, blackFile, blockFile,
	)); err != nil {
		return fmt.Errorf("relay policy: %w", err)
	}
	var console *admin.Console
	if *adminListen != "" {
		console, err = admin.New(admin.Config{
			Listen:     *adminListen,
			Relay:      manager,
			PolicyOnly: true,
			Logger:     log,
		})
		if err != nil {
			return err
		}
		go console.Serve()
		defer console.Close()
		log.Info("relay console listening", "on", console.Addr())
	}
	if *carrierAddr != "" {
		if err := manager.EnableSharedCarrier(*carrierAddr); err != nil {
			return err
		}
		log.Info("shared carrier listening", "on", manager.CarrierAddr(),
			"addrs", manager.CarrierAddrs())
	}

	node, err := relaynode.New(relaynode.Config{
		NodeID:      *nodeID,
		Advertise:   advertised,
		AdvertiseV6: *advertiseV6,
		Key:         *nodeKey,
		Logger:      log,
	}, manager, auth)
	if err != nil {
		return err
	}
	defer node.Close()
	node.StartJanitor(*janitor)

	httpServer := &http.Server{
		Addr:              *httpListen,
		Handler:           tracer.Handler(node.Handler()),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}
	listener, err := net.Listen("tcp", *httpListen)
	if err != nil {
		return fmt.Errorf("control listen %q: %w", *httpListen, err)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go func() {
		var serveErr error
		if tlsConfig != nil {
			httpServer.TLSConfig = tlsConfig
			log.Info("HTTPS control API listening", "on", listener.Addr().String())
			serveErr = httpServer.ServeTLS(listener, "", "")
		} else {
			log.Info("HTTP control API listening", "on", listener.Addr().String())
			serveErr = httpServer.Serve(listener)
		}
		if serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			log.Error("control API stopped", "error", serveErr)
			stop()
		}
	}()

	log.Info("chast-relay ready",
		"node", *nodeID,
		"advertise", advertised,
		"relay_ports", *portRange,
		"carrier", manager.CarrierAddr(),
		"tls", tlsConfig != nil,
		"janitor", *janitor,
		"rmem", *rmem,
		"relay_read_buffer", manager.ReadBuffer(),
	)
	<-ctx.Done()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
	log.Info("chast-relay stopped")
	return nil
}

// newLogger tees slog output to stderr and the optional rotating file sink.
func newLogger(level slog.Level, cfg logging.Config) (*slog.Logger, *logging.Writer, error) {
	sink, err := logging.Open(cfg)
	if err != nil {
		return nil, nil, err
	}
	var out io.Writer = os.Stderr
	if sink != nil {
		out = io.MultiWriter(os.Stderr, sink)
	}
	return slog.New(slog.NewTextHandler(out, &slog.HandlerOptions{Level: level})), sink, nil
}
