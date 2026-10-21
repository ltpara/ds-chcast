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
	"os"
	"os/signal"
	"sync"
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
		ServerURL:   *server,
		ServerKey:   *serverKey,
		ID:          *id,
		Key:         *key,
		Fingerprint: *fingerprint,
		LocalAddr:   *local,
		ListenAddr:  *listen,
		Keepalive:   *keepalive,
		Logger:      log,
		OnInfo: func(info agent.HostInfo) {
			emitMu.Lock()
			defer emitMu.Unlock()
			event := agentEvent{
				Event:  "relay",
				Listen: info.Listen,
				Host:   info.ID,
				Path:   info.Path,
				Remote: info.Viewer,
				Punch:  info.Punch,
				Relay:  info.Relay,
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
		Event:  "ready",
		Listen: info.Listen,
		Host:   info.ID,
		Path:   info.Path,
		Remote: info.Viewer,
		Punch:  info.Punch,
		Relay:  info.Relay,
	})
	emitMu.Unlock()
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
		ServerURL:     *server,
		ServerKey:     *serverKey,
		HostID:        *id,
		ListenAddr:    *listen,
		Mode:          *mode,
		DirectTimeout: *directTimeout,
		Logger:        log,
		OnPath: func(info agent.PathInfo) {
			emit(agentEvent{
				Event:  "path",
				Path:   info.Path,
				Remote: info.Remote,
				Direct: info.Direct,
				Relay:  info.Relay,
			})
		},
	})
	if err != nil {
		return err
	}
	defer v.Close()
	info := v.PathInfo()
	emit(agentEvent{
		Event:  "ready",
		Listen: v.LocalAddr().String(),
		Host:   *id,
		Path:   info.Path,
		Remote: info.Remote,
		Direct: info.Direct,
		Relay:  info.Relay,
	})
	log.Info("viewer agent online", "host", *id)
	<-ctx.Done()
	return nil
}

type agentEvent struct {
	Event  string `json:"event"`
	Listen string `json:"listen,omitempty"`
	Host   string `json:"host,omitempty"`
	Path   string `json:"path,omitempty"`
	Remote string `json:"remote,omitempty"`
	Punch  string `json:"punch,omitempty"`
	Direct string `json:"direct,omitempty"`
	Relay  string `json:"relay,omitempty"`
}

func newLogger(verbose bool) *slog.Logger {
	level := slog.LevelInfo
	if verbose {
		level = slog.LevelDebug
	}
	return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))
}
