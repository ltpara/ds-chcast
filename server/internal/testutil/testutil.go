// Package testutil starts an in-process copy of the server for integration
// tests. It is only imported by tests.
package testutil

import (
	"io"
	"log/slog"
	"net"
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/rendezvous"
)

const ServerKey = "test-server-key"

type Server struct {
	Registry   *registry.Registry
	Relay      *relay.Manager
	Rendezvous *rendezvous.Server
	HTTP       *httptest.Server
	URL        string
	Punch      string
	RelayHost  string
}

func Start(t testing.TB) *Server {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	reg := registry.New(5 * time.Second)
	rm, err := relay.NewManager("127.0.0.1", []int{0, 0}, reg, log)
	if err != nil {
		t.Fatalf("relay: %v", err)
	}
	relayAddr := func(port int) string {
		return net.JoinHostPort("127.0.0.1", strconv.Itoa(port))
	}
	meet, err := rendezvous.New("127.0.0.1:0", reg, relayAddr, log)
	if err != nil {
		rm.Close()
		t.Fatalf("rendezvous: %v", err)
	}
	go meet.Serve()
	handler := api.New(api.Config{
		Registry:  reg,
		Relay:     rm,
		PunchAddr: meet.Addr(),
		RelayHost: "127.0.0.1",
		ServerKey: ServerKey,
		TTL:       60 * time.Second,
		Logger:    log,
	})
	hs := httptest.NewServer(handler)
	s := &Server{
		Registry:   reg,
		Relay:      rm,
		Rendezvous: meet,
		HTTP:       hs,
		URL:        hs.URL,
		Punch:      meet.Addr(),
		RelayHost:  "127.0.0.1",
	}
	t.Cleanup(func() {
		hs.Close()
		meet.Close()
		rm.Close()
	})
	return s
}
