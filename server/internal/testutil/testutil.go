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
	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/configpush"
	"github.com/ltpara/ds-chcast/server/internal/geoip"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/oidc"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/ratelimit"
	"github.com/ltpara/ds-chcast/server/internal/recordings"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
	"github.com/ltpara/ds-chcast/server/internal/rendezvous"
	"github.com/ltpara/ds-chcast/server/internal/software"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
)

const ServerKey = "test-server-key"

// RelayNodeKey is the control key shared by test relay nodes.
const RelayNodeKey = "test-relay-node-key"

// Options tunes the HTTP handler for tests that exercise operations features.
type Options struct {
	RateLimit   float64
	RateBurst   float64
	ProxyHeader string
	TLS         bool
	// RequireHostMetadata mirrors --require-host-metadata-signature.
	RequireHostMetadata bool
	RelayOnly           bool
	WakeRelays          map[string][]string
	Registration        *abuse.Blocker
	RelayServers        []string
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
	// SoftwareDir publishes artifacts under /software/. StartWith hashes the
	// file named by SoftwareURL and signs the notice with its server identity,
	// matching the chast-server wiring.
	SoftwareDir string
	// SoftwarePublisher signs the release with a second, publisher identity,
	// mirroring --software-publisher-key and the .pubsig sidecar.
	SoftwarePublisher *identity.Server
	// RendezvousConfig pushes alternative rendezvous servers to clients.
	RendezvousConfig *configpush.Config
	// NoAccounts leaves login and address-book routes unavailable, matching a
	// server started with --state "".
	NoAccounts bool
	// RecordingsDir enables the account recording archive at this directory.
	// RecordingsQuota and RecordingsMax default to 8 GiB and 2 GiB when zero,
	// so a test only names the directory.
	RecordingsDir   string
	RecordingsQuota int64
	RecordingsMax   int64
	// LoginLimiter locks repeated wrong passwords out. Nil disables it.
	LoginLimiter *authlimit.Limiter
	// LoginPersistent enables the default 3-failures/1-minute lockout backed
	// by the account database instead of process memory.
	LoginPersistent bool
	// Mailer delivers password-reset mail. Nil leaves recovery routes at 503.
	Mailer api.Mailer
	// SMS delivers password-reset codes. Nil leaves SMS recovery at 503.
	SMS api.SMSer
	// RecoveryURL and RecoveryTTL shape reset links and token lifetime.
	RecoveryURL string
	RecoveryTTL time.Duration
	// SMSTTL bounds SMS reset codes.
	SMSTTL time.Duration
	// WebAuthnRPID and WebAuthnOrigins enable the security-key routes.
	WebAuthnRPID    string
	WebAuthnOrigins []string
	// OIDC enables viewer single sign-on routes. Nil leaves them at 503.
	OIDC *oidc.Provider
	// GeoIP labels login addresses in audit events and session lists.
	GeoIP *geoip.Locator
	// RelayExtra advertises additional relay hosts (for example the IPv6
	// listener of the same relay) without making them the primary.
	RelayExtra []string
	// CarrierV6 adds an IPv6 shared-carrier listener on ::1 and advertises it,
	// so tests can cover dual-stack carrier endpoints.
	CarrierV6 bool
	// RegistryOfflineAfter controls how long a host remains UDP-online after
	// its last rendezvous keepalive. Zero keeps the production default.
	RegistryOfflineAfter time.Duration
	// Tracer exports request spans. Nil disables tracing.
	Tracer *tracing.Tracer
	// LinkKey and HostKey split viewer lookups and host registration from the
	// management ServerKey. Empty keeps the legacy single-key behavior.
	LinkKey string
	HostKey string
	// AccessIssuer signs short-lived access tickets. Nil disables the endpoint.
	AccessIssuer *acl.Issuer
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
	TCPPunch   string
	RelayHost  string
	Carrier    string
	RelayOnly  *atomic.Bool
	PunchLog   *punchlog.Log
	IPChanges  *ipchange.Tracker
	RelayAddrs *relayaddr.List
	Pool       *relaypool.Pool
	CarrierV6  string
}

func Start(t testing.TB) *Server {
	t.Helper()
	return StartWith(t, Options{})
}

func StartWith(t testing.TB, opts Options) *Server {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	offlineAfter := opts.RegistryOfflineAfter
	if offlineAfter <= 0 {
		offlineAfter = 5 * time.Second
	}
	reg := registry.New(offlineAfter)
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
	var softwareSHA256, softwareSignature string
	var softwarePublisherKeyID, softwarePublisherSignature string
	var softwareSize int64
	if opts.SoftwareDir != "" {
		name, err := software.HostedName(opts.SoftwareURL)
		if err != nil {
			t.Fatalf("software URL: %v", err)
		}
		if name == "" {
			t.Fatalf("SoftwareDir needs SoftwareURL under %s", software.RoutePrefix)
		}
		artifact, err := software.Load(opts.SoftwareDir, name)
		if err != nil {
			t.Fatalf("software artifact: %v", err)
		}
		softwareSHA256, softwareSize = artifact.SHA256, artifact.Size
		softwareSignature, err = serverIdentity.SignSoftware(
			opts.SoftwareVersion, opts.SoftwareURL, softwareSHA256, softwareSize)
		if err != nil {
			t.Fatalf("software signature: %v", err)
		}
		if opts.SoftwarePublisher != nil {
			softwarePublisherKeyID, err = identity.PublisherKeyID(opts.SoftwarePublisher.PublicKeyText())
			if err != nil {
				t.Fatalf("publisher key id: %v", err)
			}
			softwarePublisherSignature, err = opts.SoftwarePublisher.SignSoftwarePublisher(
				softwarePublisherKeyID, opts.SoftwareVersion, opts.SoftwareURL,
				softwareSHA256, softwareSize)
			if err != nil {
				t.Fatalf("publisher signature: %v", err)
			}
		}
	} else if opts.SoftwarePublisher != nil {
		t.Fatalf("SoftwarePublisher needs SoftwareDir")
	}
	var relayOpts []relay.Option
	relayOpts = append(relayOpts, relay.WithViewerTokenRequired(true))
	if opts.CarrierV6 {
		relayOpts = append(relayOpts, relay.WithCarrierBind6("::1"))
	}
	rm, err := relay.NewManager("127.0.0.1", []int{0, 0}, reg, log, relayOpts...)
	if err != nil {
		t.Fatalf("relay: %v", err)
	}
	if err := rm.EnableSharedCarrier("127.0.0.1:0"); err != nil {
		t.Fatalf("shared carrier: %v", err)
	}
	relayAddr := func(port int) string {
		return net.JoinHostPort("127.0.0.1", strconv.Itoa(port))
	}
	relayOnly := &atomic.Bool{}
	relayOnly.Store(opts.RelayOnly)
	var pool *relaypool.Pool
	meet, err := rendezvous.New("127.0.0.1:0", reg, relayAddr, log,
		rendezvous.WithServerKey(ServerKey),
		rendezvous.WithLinkKey(opts.LinkKey),
		rendezvous.WithRelayOnly(relayOnly.Load),
		rendezvous.WithHostCarrierLinked(func(id string) bool {
			host, ok := reg.Lookup(id)
			if !ok {
				return false
			}
			if host.RelayNode != "" {
				return pool != nil && pool.HostLinked(host.RelayNode, id)
			}
			return rm.HostLinked(id)
		}),
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
	relayServers := opts.RelayServers
	if len(relayServers) == 0 {
		relayServers = []string{"127.0.0.1"}
	}
	relayAddrs, err := relayaddr.New(relayServers)
	if err != nil {
		t.Fatalf("relay addresses: %v", err)
	}
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
	carrierV6Host := ""
	if opts.CarrierV6 {
		carrierV6Host = "::1"
	}
	var recordingArchive *recordings.Archive
	if opts.RecordingsDir != "" {
		if accounts == nil {
			t.Fatal("RecordingsDir needs accounts")
		}
		quota := opts.RecordingsQuota
		if quota == 0 {
			quota = 8 << 30
		}
		maxUpload := opts.RecordingsMax
		if maxUpload == 0 {
			maxUpload = 2 << 30
		}
		var err error
		recordingArchive, err = recordings.New(recordings.Config{
			Dir:       opts.RecordingsDir,
			Quota:     quota,
			MaxUpload: maxUpload,
			Accounts:  accounts,
			Logger:    log,
		})
		if err != nil {
			t.Fatalf("recordings: %v", err)
		}
	}
	handler := api.New(api.Config{
		Registry:                   reg,
		Relay:                      rm,
		Identity:                   serverIdentity,
		PunchAddr:                  meet.Addr(),
		TCPPunchAddr:               meet.TCPAddr(),
		HostControlLinked:          meet.HostControlLinked,
		RelayHost:                  "127.0.0.1",
		ServerKey:                  ServerKey,
		LinkKey:                    opts.LinkKey,
		HostKey:                    opts.HostKey,
		AccessIssuer:               opts.AccessIssuer,
		WakeRelays:                 opts.WakeRelays,
		TTL:                        60 * time.Second,
		Logger:                     log,
		Limiter:                    ratelimit.New(opts.RateLimit, opts.RateBurst),
		LoginLimiter:               loginLimiter,
		ProxyHeader:                opts.ProxyHeader,
		RequireHostMetadata:        opts.RequireHostMetadata,
		TLS:                        opts.TLS,
		GeoIP:                      opts.GeoIP,
		RelayOnly:                  relayOnly,
		Blocker:                    opts.Registration,
		PunchLog:                   punchLog,
		IPChanges:                  ipChanges,
		RelayAddrs:                 relayAddrs,
		RelayExtra:                 opts.RelayExtra,
		CarrierV6Host:              carrierV6Host,
		RelayPool:                  pool,
		PunchReadBuffer:            meet.ReadBuffer(),
		SoftwareVersion:            opts.SoftwareVersion,
		SoftwareURL:                opts.SoftwareURL,
		SoftwareDir:                opts.SoftwareDir,
		SoftwareSHA256:             softwareSHA256,
		SoftwareSize:               softwareSize,
		SoftwareSignature:          softwareSignature,
		SoftwarePublisherKeyID:     softwarePublisherKeyID,
		SoftwarePublisherSignature: softwarePublisherSignature,
		Rendezvous:                 opts.RendezvousConfig,
		Accounts:                   accounts,
		Recordings:                 recordingArchive,
		Mailer:                     opts.Mailer,
		SMS:                        opts.SMS,
		RecoveryURL:                opts.RecoveryURL,
		RecoveryTTL:                opts.RecoveryTTL,
		SMSTTL:                     opts.SMSTTL,
		WebAuthnRPID:               opts.WebAuthnRPID,
		WebAuthnOrigins:            opts.WebAuthnOrigins,
		OIDC:                       opts.OIDC,
		Tracer:                     opts.Tracer,
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
		TCPPunch:   meet.TCPAddr(),
		RelayHost:  "127.0.0.1",
		Carrier:    rm.CarrierAddr(),
		RelayOnly:  relayOnly,
		PunchLog:   punchLog,
		IPChanges:  ipChanges,
		RelayAddrs: relayAddrs,
		Pool:       pool,
		CarrierV6:  rm.CarrierV6Addr(),
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
