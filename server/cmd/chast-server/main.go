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
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
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
		httpListen  = flag.String("http-listen", "0.0.0.0:8443", "HTTP registration and lookup address")
		punchListen = flag.String("punch-listen", "0.0.0.0:8444", "UDP rendezvous and punch address")
		relayListen = flag.String("relay-listen", "0.0.0.0", "IP address for relay UDP ports")
		portRange   = flag.String("relay-ports", "41000-41099", "relay UDP ports, e.g. 41000-41099")
		advertise   = flag.String("advertise", "", "public IP or DNS name returned to peers")
		serverKey   = flag.String("server-key", "", "required for registration unless empty")
		hostTTL     = flag.Duration("host-ttl", 60*time.Second, "how long a registration survives without renewal")
		offline     = flag.Duration("offline-after", 30*time.Second, "keepalive silence that makes a host offline")
		verbose     = flag.Bool("verbose", false, "debug logging")
	)
	flag.Parse()

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	ports, err := parsePorts(*portRange)
	if err != nil {
		return err
	}
	advertised := *advertise
	if advertised == "" {
		advertised, err = detectAdvertise()
		if err != nil {
			return err
		}
		log.Info("advertising the detected address; pass --advertise to override", "host", advertised)
	}
	if *hostTTL < 10*time.Second {
		return errors.New("--host-ttl must be at least 10s")
	}

	reg := registry.New(*offline)
	relayAddr := func(port int) string {
		return net.JoinHostPort(advertised, strconv.Itoa(port))
	}
	relayManager, err := relay.NewManager(*relayListen, ports, reg, log)
	if err != nil {
		return err
	}
	defer relayManager.Close()

	meet, err := rendezvous.New(*punchListen, reg, relayAddr, log)
	if err != nil {
		return err
	}
	defer meet.Close()
	_, punchPortText, err := net.SplitHostPort(meet.Addr())
	if err != nil {
		return err
	}
	punchAddr := net.JoinHostPort(advertised, punchPortText)

	handler := api.New(api.Config{
		Registry:  reg,
		Relay:     relayManager,
		PunchAddr: punchAddr,
		RelayHost: advertised,
		ServerKey: *serverKey,
		TTL:       *hostTTL,
		Logger:    log,
	})
	httpServer := &http.Server{
		Addr:              *httpListen,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go meet.Serve()
	go func() {
		log.Info("HTTP API listening", "on", *httpListen)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Error("HTTP server stopped", "error", err)
			stop()
		}
	}()
	go expireLoop(ctx, reg, relayManager, log)

	log.Info("chast-server ready",
		"punch", punchAddr,
		"relay_ports", *portRange,
		"server_key", *serverKey != "",
	)
	<-ctx.Done()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
	log.Info("chast-server stopped")
	return nil
}

func expireLoop(ctx context.Context, reg *registry.Registry, relayManager *relay.Manager, log *slog.Logger) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			for _, host := range reg.Expire() {
				relayManager.Release(host.ID)
				log.Info("host registration expired", "id", host.ID)
			}
		}
	}
}

func parsePorts(text string) ([]int, error) {
	var ports []int
	for _, part := range strings.Split(text, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if start, end, ok := strings.Cut(part, "-"); ok {
			from, err := strconv.Atoi(strings.TrimSpace(start))
			if err != nil {
				return nil, fmt.Errorf("relay-ports %q: %w", part, err)
			}
			to, err := strconv.Atoi(strings.TrimSpace(end))
			if err != nil {
				return nil, fmt.Errorf("relay-ports %q: %w", part, err)
			}
			if from < 1 || to > 65535 || from > to {
				return nil, fmt.Errorf("relay-ports %q is not an ascending range within 1..65535", part)
			}
			for p := from; p <= to; p++ {
				ports = append(ports, p)
			}
		} else {
			p, err := strconv.Atoi(part)
			if err != nil || p < 1 || p > 65535 {
				return nil, fmt.Errorf("relay port %q is not 1..65535", part)
			}
			ports = append(ports, p)
		}
	}
	if len(ports) == 0 {
		return nil, errors.New("--relay-ports must name at least one port")
	}
	if len(ports) > 4096 {
		return nil, errors.New("too many relay ports (limit 4096)")
	}
	return ports, nil
}

func detectAdvertise() (string, error) {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return "", fmt.Errorf("discover a public address: %w; pass --advertise", err)
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok || ipnet.IP.IsLoopback() {
			continue
		}
		if ip4 := ipnet.IP.To4(); ip4 != nil {
			return ip4.String(), nil
		}
	}
	return "", errors.New("no non-loopback IPv4 address found; pass --advertise <ip-or-name>")
}
