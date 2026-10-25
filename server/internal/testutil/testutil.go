// Package testutil starts an in-process copy of the server for integration
// tests. It is only imported by tests.
package testutil

import (
	"io"
	"log/slog"
	"net"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"sync/atomic"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/ratelimit"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
	"github.com/ltpara/ds-chcast/server/internal/rendezvous"
)

const ServerKey = "test-server-key"

// RelayNodeKey is the control key shared by test relay nodes.
const RelayNodeKey = "test-relay-node-key"

// Options tunes the HTTP handler for tests that exercise operations features.
type Options struct {
	RateLimit    float64
	RateBurst    float64
	ProxyHeader  string
	TLS          bool
	RelayOnly    bool
	Registration *abuse.Blocker
	RelayServers []string
	// RelayNodes are standalone relay node base URLs. RelayNodeKey is their
	// shared control key; empty falls back to RelayNodeKey.
	RelayNodes []string
	// RelayNodeKey authenticates control requests to RelayNodes.
	RelayNodeKey string
	// RelayNodeHealth overrides the node health probe interval.
	RelayNodeHealth time.Duration
	// SoftwareVersion and SoftwareURL advertise an update like hbbs.
	SoftwareVersion string
	SoftwareURL     string
	// NoAccounts leaves login and address-book routes unavailable, matching a
	// server started with --state "".
	NoAccounts bool
	// LoginLimiter locks repeated wrong passwords out. Nil disables it.
	LoginLimiter *authlimit.Limiter
	// LoginPersistent enables the default 3-failures/1-minute lockout backed
	// by the account database instead of process memory.
	LoginPersistent bool
}

type Server struct {
	Registry   *registry.Registry
	Accounts   *account.Store
	Relay      *relay.Manager
	Rendezvous *rendezvous.Server
	Identity   *identity.Server
	HTTP       *httptest.Server
	URL        string
	Punch      string
	RelayHost  string
	Carrier    string
	RelayOnly  *atomic.Bool
	PunchLog   *punchlog.Log
	IPChanges  *ipchange.Tracker
	RelayAddrs *relayaddr.List
	Pool       *relaypool.Pool
}

func Start(t testing.TB) *Server {
	t.Helper()
	return StartWith(t, Options{})
}

func StartWith(t testing.TB, opts Options) *Server {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	reg := registry.New(5 * time.Second)
	var accounts *account.Store
	if !opts.NoAccounts {
		var err error
		accounts, err = account.Open(filepath.Join(t.TempDir(), "accounts.db"))
		if err != nil {
			t.Fatalf("accounts: %v", err)
		}
	}
	serverIdentity, err := identity.GenerateServer()
	if err != nil {
		t.Fatalf("server identity: %v", err)
	}
	rm, err := relay.NewManager("127.0.0.1", []int{0, 0}, reg, log,
		relay.WithViewerTokenRequired(true),
	)
	if err != nil {
		t.Fatalf("relay: %v", err)
	}
	if err := rm.EnableSharedCarrier("127.0.0.1:0"); err != nil {
		t.Fatalf("shared carrier: %v", err)
	}
	relayAddr := func(port int) string {
		return net.JoinHostPort("127.0.0.1", strconv.Itoa(port))
	}
	meet, err := rendezvous.New("127.0.0.1:0", reg, relayAddr, log,
		rendezvous.WithServerKey(ServerKey),
	)
	if err != nil {
		rm.Close()
		t.Fatalf("rendezvous: %v", err)
	}
	meet.SetIdentity(serverIdentity)
	punchLog := punchlog.New(256, time.Minute)
	meet.SetPunchLog(punchLog)
	ipChanges := ipchange.New(0)
	meet.SetIPChanges(ipChanges)
	go meet.Serve()
	relayOnly := &atomic.Bool{}
	relayOnly.Store(opts.RelayOnly)
	relayServers := opts.RelayServers
	if len(relayServers) == 0 {
		relayServers = []string{"127.0.0.1"}
	}
	relayAddrs, err := relayaddr.New(relayServers)
	if err != nil {
		t.Fatalf("relay addresses: %v", err)
	}
	var pool *relaypool.Pool
	if len(opts.RelayNodes) > 0 {
		key := opts.RelayNodeKey
		if key == "" {
			key = RelayNodeKey
		}
		pool, err = relaypool.New(relaypool.Config{
			Nodes:    opts.RelayNodes,
			Key:      key,
			Interval: opts.RelayNodeHealth,
			Timeout:  time.Second,
			Logger:   log,
		})
		if err != nil {
			t.Fatalf("relay pool: %v", err)
		}
		pool.Start()
	}
	loginLimiter := opts.LoginLimiter
	if loginLimiter == nil && opts.LoginPersistent && accounts != nil {
		loginLimiter = authlimit.NewPersistent(authlimit.Config{
			Window:      time.Minute,
			MaxFailures: 3,
			Lockout:     time.Minute,
		}, accounts, nil)
	}
	handler := api.New(api.Config{
		Registry:        reg,
		Relay:           rm,
		Identity:        serverIdentity,
		PunchAddr:       meet.Addr(),
		RelayHost:       "127.0.0.1",
		ServerKey:       ServerKey,
		TTL:             60 * time.Second,
		Logger:          log,
		Limiter:         ratelimit.New(opts.RateLimit, opts.RateBurst),
		LoginLimiter:    loginLimiter,
		ProxyHeader:     opts.ProxyHeader,
		TLS:             opts.TLS,
		RelayOnly:       relayOnly,
		Blocker:         opts.Registration,
		PunchLog:        punchLog,
		IPChanges:       ipChanges,
		RelayAddrs:      relayAddrs,
		RelayPool:       pool,
		PunchReadBuffer: meet.ReadBuffer(),
		SoftwareVersion: opts.SoftwareVersion,
		SoftwareURL:     opts.SoftwareURL,
		Accounts:        accounts,
	})
	hs := httptest.NewServer(handler)
	s := &Server{
		Registry:   reg,
		Accounts:   accounts,
		Relay:      rm,
		Rendezvous: meet,
		Identity:   serverIdentity,
		HTTP:       hs,
		URL:        hs.URL,
		Punch:      meet.Addr(),
		RelayHost:  "127.0.0.1",
		Carrier:    rm.CarrierAddr(),
		RelayOnly:  relayOnly,
		PunchLog:   punchLog,
		IPChanges:  ipChanges,
		RelayAddrs: relayAddrs,
		Pool:       pool,
	}
	t.Cleanup(func() {
		hs.Close()
		if accounts != nil {
			accounts.Close()
		}
		meet.Close()
		rm.Close()
		if pool != nil {
			pool.Close()
		}
	})
	return s
}
