// chast-relay is a standalone relay node for chast, the hbbr equivalent.
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
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/admin"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relaynode"
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
		portRange   = flag.String("relay-ports", "41100-41199", "relay UDP/TCP ports, e.g. 41100-41199")
		carrierAddr = flag.String("carrier-listen", "0.0.0.0:8448", "shared TCP/WebSocket carrier address; empty disables it")
		advertise   = flag.String("advertise", "", "host name or IP published to hbbs and peers")
		nodeKey     = flag.String("key", "", "control key shared with hbbs --relay-node-key")
		nodeID      = flag.String("node-id", "", "node name shown in health and stats")
		janitor     = flag.Duration("lease-janitor", 5*time.Second, "how often expired leases are released")
		tlsCert     = flag.String("tls-cert", "", "PEM certificate for the control API; requires --tls-key")
		tlsKey      = flag.String("tls-key", "", "PEM private key for the control API; requires --tls-cert")
		adminListen = flag.String("admin-listen", "127.0.0.1:8449", "loopback-only relay console; empty disables it")

		singleBandwidth = flag.Float64("relay-single-bandwidth", 128, "per-session relay bandwidth in Mbit/s; 0 disables the limit")
		totalBandwidth  = flag.Float64("relay-total-bandwidth", 1024, "total relay bandwidth in Mbit/s; 0 disables the limit")
		blacklistSpeed  = flag.Float64("relay-blacklist-speed", 32, "relay bandwidth for blacklisted IPs in Mbit/s")
		downgradeAfter  = flag.Duration("relay-downgrade-after", 30*time.Minute, "session age before a sustained relay is downgraded")
		downgradeRatio  = flag.Float64("relay-downgrade-threshold", 0.66, "speed/limit ratio that triggers downgrade, 0..1")
		blacklistFile   = flag.String("relay-blacklist-file", "", "IP list file that receives the slower relay rate")
		blocklistFile   = flag.String("relay-blocklist-file", "", "IP list file whose viewers are disconnected")
		verbose         = flag.Bool("verbose", false, "debug logging")
	)
	flag.Parse()

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	if *nodeKey == "" {
		return errors.New("--key is required; hbbs passes the same value as --relay-node-key")
	}
	if *janitor < time.Second {
		return errors.New("--lease-janitor must be at least 1s")
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
		return errors.New("--advertise is required; it is the address hbbs hands to hosts and viewers")
	}

	auth := relaynode.NewAuthenticator()
	manager, err := relay.NewManager(*relayListen, ports, auth, log)
	if err != nil {
		return err
	}
	defer manager.Close()
	if err := manager.SetPolicyConfig(relay.ConfigFromMbit(
		*singleBandwidth, *totalBandwidth, *blacklistSpeed,
		*downgradeRatio, *downgradeAfter, *blacklistFile, *blocklistFile,
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
		log.Info("shared carrier listening", "on", manager.CarrierAddr())
	}

	node, err := relaynode.New(relaynode.Config{
		NodeID:    *nodeID,
		Advertise: advertised,
		Key:       *nodeKey,
		Logger:    log,
	}, manager, auth)
	if err != nil {
		return err
	}
	defer node.Close()
	node.StartJanitor(*janitor)

	httpServer := &http.Server{
		Addr:              *httpListen,
		Handler:           node.Handler(),
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
	)
	<-ctx.Done()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
	log.Info("chast-relay stopped")
	return nil
}
