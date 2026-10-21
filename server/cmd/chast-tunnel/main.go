// chast-tunnel connects a local chast QUIC endpoint to the rendezvous and relay
// service. It runs next to chast-host (host mode) or next to the viewer (viewer
// mode) and moves opaque UDP datagrams, so the chast protocol is unchanged.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/agent"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "chast-tunnel:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 {
		return fmt.Errorf("usage: chast-tunnel host|viewer [flags]")
	}
	switch args[0] {
	case "host":
		return runHost(args[1:])
	case "viewer":
		return runViewer(args[1:])
	default:
		return fmt.Errorf("unknown mode %q; use host or viewer", args[0])
	}
}

func runHost(args []string) error {
	fs := flag.NewFlagSet("host", flag.ExitOnError)
	server := fs.String("server", "", "HTTP API base, e.g. http://127.0.0.1:8443")
	serverKey := fs.String("server-key", "", "server key required by the API")
	id := fs.String("id", "", "host id registered with the service")
	key := fs.String("key", "", "host key, at least 8 characters")
	fingerprint := fs.String("fingerprint", "", "certificate fingerprint shown in lookups")
	local := fs.String("local", "127.0.0.1:4433", "chast-host QUIC listener")
	listen := fs.String("listen", "0.0.0.0:14434", "public-facing UDP address of this agent")
	keepalive := fs.Duration("keepalive", 10*time.Second, "rendezvous keepalive interval")
	verbose := fs.Bool("verbose", false, "debug logging")
	fs.Parse(args)

	log := newLogger(*verbose)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	h, err := agent.StartHost(ctx, agent.HostConfig{
		ServerURL:   *server,
		ServerKey:   *serverKey,
		ID:          *id,
		Key:         *key,
		Fingerprint: *fingerprint,
		LocalAddr:   *local,
		ListenAddr:  *listen,
		Keepalive:   *keepalive,
		Logger:      log,
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
	log.Info("host agent online", "id", *id)
	<-ctx.Done()
	return nil
}

func runViewer(args []string) error {
	fs := flag.NewFlagSet("viewer", flag.ExitOnError)
	server := fs.String("server", "", "HTTP API base, e.g. http://127.0.0.1:8443")
	serverKey := fs.String("server-key", "", "server key required by the API")
	id := fs.String("id", "", "host id to reach")
	listen := fs.String("listen", "127.0.0.1:24434", "local UDP address for the QUIC client")
	mode := fs.String("mode", agent.ModeAuto, "auto: direct then relay; direct; relay")
	directTimeout := fs.Duration("direct-timeout", 2*time.Second, "how long auto waits for the direct path")
	verbose := fs.Bool("verbose", false, "debug logging")
	fs.Parse(args)

	log := newLogger(*verbose)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	v, err := agent.StartViewer(ctx, agent.ViewerConfig{
		ServerURL:     *server,
		ServerKey:     *serverKey,
		HostID:        *id,
		ListenAddr:    *listen,
		Mode:          *mode,
		DirectTimeout: *directTimeout,
		Logger:        log,
	})
	if err != nil {
		return err
	}
	defer v.Close()
	log.Info("viewer agent online", "host", *id)
	<-ctx.Done()
	return nil
}

func newLogger(verbose bool) *slog.Logger {
	level := slog.LevelInfo
	if verbose {
		level = slog.LevelDebug
	}
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
}
