// chast-server is the rendezvous, punch coordinator and UDP relay for chast.
package main

import (
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/abuse"
	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/acmecert"
	"github.com/ltpara/ds-chcast/server/internal/admin"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/auditarchive"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/configpush"
	"github.com/ltpara/ds-chcast/server/internal/geoip"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/ipchange"
	"github.com/ltpara/ds-chcast/server/internal/logging"
	"github.com/ltpara/ds-chcast/server/internal/mail"
	"github.com/ltpara/ds-chcast/server/internal/netutil"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/ratelimit"
	"github.com/ltpara/ds-chcast/server/internal/recordings"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/relay"
	"github.com/ltpara/ds-chcast/server/internal/relayaddr"
	"github.com/ltpara/ds-chcast/server/internal/relaypool"
	"github.com/ltpara/ds-chcast/server/internal/rendezvous"
	"github.com/ltpara/ds-chcast/server/internal/servercfg"
	"github.com/ltpara/ds-chcast/server/internal/sms"
	"github.com/ltpara/ds-chcast/server/internal/software"
	"github.com/ltpara/ds-chcast/server/internal/tracing"
	"github.com/ltpara/ds-chcast/server/internal/webauthn"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "chast-server:", err)
		os.Exit(1)
	}
}

func run() error {
	var (
		httpListen      = flag.String("http-listen", "0.0.0.0:8443", "HTTP registration and lookup address")
		punchListen     = flag.String("punch-listen", "0.0.0.0:8444", "UDP rendezvous and punch address")
		relayListen     = flag.String("relay-listen", "0.0.0.0", "IP address for relay UDP ports")
		relayV6         = flag.String("relay-listen-v6", "", "optional IPv6 address for relay UDP ports, same port numbers as --relay-listen")
		portRange       = flag.String("relay-ports", "41000-41099", "relay UDP ports, e.g. 41000-41099")
		carrierAddr     = flag.String("carrier-listen", "0.0.0.0:8445", "shared TCP/WebSocket carrier address; empty disables it")
		carrierV6       = flag.String("carrier-listen-v6", "", "optional IPv6 address for the shared carrier, same port as --carrier-listen")
		advertise       = flag.String("advertise", "", "public IP or DNS name returned to peers")
		advertiseV6     = flag.String("advertise-v6", "", "public IPv6 address for the rendezvous port; empty derives it from the registration connection")
		relayServers    = flag.String("relay-servers", "", "comma-separated relay host names or IPs without ports; empty uses --advertise")
		relayNodes      = flag.String("relay-nodes", "", "comma-separated standalone relay node URLs, e.g. http://relay1:8447")
		relayNodeKey    = flag.String("relay-node-key", "", "control key shared with every --relay-nodes entry")
		relayNodeCA     = flag.String("relay-node-ca", "", "PEM CA bundle for HTTPS relay nodes; empty uses the system roots")
		relayNodeCert   = flag.String("relay-node-client-cert", "", "client certificate presented to relay nodes requiring mTLS")
		relayNodeKeyID  = flag.String("relay-node-client-key", "", "private key for --relay-node-client-cert")
		relayHealth     = flag.Duration("relay-node-health", 10*time.Second, "how often standalone relay nodes are probed")
		serverKey       = flag.String("server-key", "", "required for registration unless empty")
		softwareVer     = flag.String("software-version", "", "latest client/host version advertised to peers; requires --software-url")
		softwareURL     = flag.String("software-url", "", "download URL advertised with --software-version")
		softwareDir     = flag.String("software-dir", "", "release directory published read-only at /software/{name}; the file named by --software-url is hashed and signed in the notice")
		smtpAddr        = flag.String("smtp-addr", "", "SMTP host:port used to send password-reset mail; empty disables email recovery")
		smtpFrom        = flag.String("smtp-from", "", "From address for password-reset mail; required with --smtp-addr")
		smtpUser        = flag.String("smtp-user", "", "SMTP username for PLAIN authentication; empty sends without auth")
		smtpPass        = flag.String("smtp-pass", "", "SMTP password for PLAIN authentication; prefer an env file over the command line")
		smtpTLS         = flag.String("smtp-tls", mail.TLSStart, "SMTP security: starttls, implicit or none")
		recoveryURL     = flag.String("recovery-url", "", "page password-reset links point at; the token is always included in the mail")
		recoveryTTL     = flag.Duration("recovery-ttl", 30*time.Minute, "password-reset token lifetime")
		smsURL          = flag.String("sms-url", "", "HTTP SMS gateway endpoint for password-reset codes; empty disables SMS recovery")
		smsToken        = flag.String("sms-token", "", "bearer token sent to --sms-url; prefer an env file over the command line")
		smsFrom         = flag.String("sms-from", "", "optional sender ID passed to the SMS gateway")
		smsTTL          = flag.Duration("sms-ttl", 10*time.Minute, "SMS password-reset code lifetime, at most 1h")
		webauthnRPID    = flag.String("webauthn-rp-id", "", "relying party id (bare host name) that enables security-key registration and login")
		webauthnOrigin  = flag.String("webauthn-origin", "", "comma-separated origins (scheme://host[:port]) accepted in security-key ceremonies")
		auditRetention  = flag.Int("audit-retention-days", 0, "delete audit, connection and file-transfer history older than this many days; 0 keeps the row caps only")
		auditArchive    = flag.String("audit-archive-dir", "", "append audit, connection and file-transfer history to JSONL files in this directory; empty disables the archive")
		archiveEvery    = flag.Duration("audit-archive-interval", time.Hour, "how often new history rows are appended to the archive")
		archiveGzip     = flag.Bool("audit-archive-compress", true, "gzip archive files once their UTC day is closed")
		archiveKeep     = flag.Int("audit-archive-retention-days", 0, "delete archive files whose UTC day is older than this many days; 0 keeps them")
		tombstoneDays   = flag.Int("addressbook-tombstone-days", 0, "delete address-book tombstones older than this many days; 0 keeps them")
		recordingsDir   = flag.String("recordings-dir", "", "directory for viewer recordings uploaded to accounts; empty disables the archive")
		recordingsQuota = flag.Int("recordings-quota-mb", 8192, "per-account recording storage limit in MiB; 0 disables the quota")
		recordingsMax   = flag.Int("recordings-max-mb", 2048, "largest single recording upload in MiB; 0 disables the limit")
		recordingsDays  = flag.Int("recordings-retention-days", 30, "delete uploaded recordings older than this many days; 0 keeps them forever")
		rendezvousList  = flag.String("rendezvous-servers", "", "comma-separated HTTP API base URLs pushed to clients for failover; empty disables the push")
		configSerial    = flag.Int64("config-serial", 0, "serial of the pushed rendezvous list; 0 becomes 1 when a list is set")
		hostTTL         = flag.Duration("host-ttl", 60*time.Second, "how long a registration survives without renewal")
		offline         = flag.Duration("offline-after", 30*time.Second, "keepalive silence that makes a host offline")
		stateFile       = flag.String("state", "chast-server-state.db", "persistent SQLite peer database; a legacy .json next to it is imported once; empty disables persistence")
		identityFile    = flag.String("identity", "chast-server-identity.key", "Ed25519 server identity seed; created on first run, empty for ephemeral")
		tlsCert         = flag.String("tls-cert", "", "PEM certificate for the HTTP API; requires --tls-key")
		tlsKey          = flag.String("tls-key", "", "PEM private key for the HTTP API; requires --tls-cert")
		tlsClientCA     = flag.String("tls-client-ca", "", "PEM CA bundle; when set, HTTPS clients must present a certificate signed by it (mTLS)")
		acmeDomain      = flag.String("acme-domain", "", "comma-separated DNS names obtained automatically over ACME HTTP-01; replaces --tls-cert/--tls-key")
		acmeEmail       = flag.String("acme-email", "", "contact address registered with the ACME CA")
		acmeCache       = flag.String("acme-cache", "", "directory for ACME account keys and certificates; empty uses an acme-cache directory next to --state")
		acmeHTTPListen  = flag.String("acme-http-listen", ":80", "HTTP-01 challenge and HTTPS redirect listener; must be reachable from the CA")
		acmeDirectory   = flag.String("acme-directory-url", "", "ACME directory URL; empty uses Let's Encrypt production")
		acmeEABKID      = flag.String("acme-eab-kid", "", "external account binding key id for CAs that require EAB")
		acmeEABKey      = flag.String("acme-eab-key", "", "external account binding HMAC key in base64; paired with --acme-eab-kid")
		apiRate         = flag.Float64("api-rate", 50, "sustained HTTP requests per second per client IP; 0 disables the limiter")
		apiBurst        = flag.Int("api-burst", 100, "HTTP burst allowance per client IP")
		loginFails      = flag.Int("login-max-failures", 5, "failed account logins per username and client IP before a lockout; 0 disables it")
		loginWindow     = flag.Duration("login-window", 10*time.Minute, "window for counting failed account logins")
		loginLockout    = flag.Duration("login-lockout", time.Minute, "how long a locked username and client IP stay refused")
		proxyHeader     = flag.String("proxy-header", "", "header carrying the client IP behind a trusted proxy, e.g. X-Forwarded-For")
		geoIPFile       = flag.String("geoip-file", "", "CIDR-to-location table used to label login IPs; empty records IPs only")
		relayOnly       = flag.Bool("relay-only", false, "hide direct candidates and force viewers onto the relay or carrier")
		adminListen     = flag.String("admin-listen", "127.0.0.1:8446", "loopback-only plain-text admin console; empty disables it")

		relaySingle    = flag.Float64("relay-single-bandwidth", 128, "per-session relay bandwidth in Mbit/s; 0 disables the limit")
		relayTotal     = flag.Float64("relay-total-bandwidth", 1024, "total relay bandwidth in Mbit/s; 0 disables the limit")
		relayBlacklist = flag.Float64("relay-blacklist-speed", 32, "relay bandwidth for blacklisted IPs in Mbit/s")
		relayDowngrade = flag.Duration("relay-downgrade-after", 30*time.Minute, "session age before a sustained relay is downgraded")
		relayRatio     = flag.Float64("relay-downgrade-threshold", 0.66, "speed/limit ratio that triggers downgrade, 0..1")
		relayBlackFile = flag.String("relay-blacklist-file", "", "IP list file that receives the slower relay rate; empty probes ./blacklist.txt")
		relayBlockFile = flag.String("relay-blocklist-file", "", "IP list file whose viewers are disconnected; empty probes ./blocklist.txt")
		configFile     = flag.String("config", "", "INI file with flag values; an explicit flag still wins")
		envFile        = flag.String("env-file", ".env", "dotenv file with flag values; empty disables it")
		rmem           = flag.Int("rmem", 0, "UDP receive buffer in bytes for rendezvous and relay sockets; 0 keeps the system default")
		punchRateMax   = flag.Int("punch-rate-max", rendezvous.DefaultRateMax, "punch requests per client IP per 10s; 0 disables the limiter")
		testPunch      = flag.String("test-punch", "auto", "startup UDP self-check: auto probes a local endpoint, no disables it, or an explicit host:port")
		punchLogMax    = flag.Int("punch-log-max", 100000, "retained punch request records; 0 disables the log")
		regMax         = flag.Int("register-max", 30, "registration attempts per IP per window; 0 disables the rate check")
		regWindow      = flag.Duration("register-window", time.Minute, "registration rate window")
		regMaxIDs      = flag.Int("register-max-ids", 300, "distinct IDs per IP per ID window; 0 disables the check")
		regIDWindow    = flag.Duration("register-id-window", 24*time.Hour, "distinct ID window per IP")
		verbose        = flag.Bool("verbose", false, "debug logging")
		logFile        = flag.String("log-file", "", "also append logs to this file; empty writes to stderr only")
		logMaxSize     = flag.Int64("log-max-size", logging.DefaultMaxSize>>20, "rotate --log-file after this many MiB")
		logMaxBackups  = flag.Int("log-max-backups", logging.DefaultMaxBackups, "rotated log files to keep; 0 keeps all")
		logMaxAge      = flag.Duration("log-max-age", 0, "delete rotated logs older than this; 0 keeps all")
		logCompress    = flag.Bool("log-compress", true, "gzip rotated log files")
		traceEndpoint  = flag.String("trace-endpoint", "", "OTLP/HTTP collector base URL; spans post to <url>/v1/traces; empty disables tracing")
		traceSample    = flag.Float64("trace-sample", 1, "fraction of HTTP requests traced, 0..1")
		traceService   = flag.String("trace-service", "chast-server", "service.name attached to exported spans")
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
	softwareVersion := strings.TrimSpace(*softwareVer)
	softwareDownload := strings.TrimSpace(*softwareURL)
	if (softwareVersion == "") != (softwareDownload == "") {
		return fmt.Errorf("--software-version and --software-url must be set together")
	}
	if softwareDownload != "" {
		parsed, err := url.Parse(softwareDownload)
		if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
			return fmt.Errorf("--software-url must be an absolute http or https URL")
		}
		if len(softwareDownload) > 1024 || len(softwareVersion) > 64 {
			return fmt.Errorf("--software-url is limited to 1024 characters and --software-version to 64")
		}
	}
	softwareName := ""
	if *softwareDir != "" {
		if softwareVersion == "" {
			return errors.New("--software-dir requires --software-version and --software-url")
		}
		name, err := software.HostedName(softwareDownload)
		if err != nil {
			return fmt.Errorf("--software-url: %w", err)
		}
		if name == "" {
			return fmt.Errorf("--software-dir requires --software-url to point under %s", software.RoutePrefix)
		}
		softwareName = name
	}
	smtpServer := strings.TrimSpace(*smtpAddr)
	smtpFromAddr := strings.TrimSpace(*smtpFrom)
	if smtpServer == "" {
		if smtpFromAddr != "" || *smtpUser != "" || *smtpPass != "" {
			return errors.New("--smtp-from, --smtp-user and --smtp-pass require --smtp-addr")
		}
	} else if smtpFromAddr == "" {
		return errors.New("--smtp-addr requires --smtp-from")
	}
	if *recoveryTTL < time.Minute {
		return errors.New("--recovery-ttl must be at least 1m")
	}
	resetPage := strings.TrimSpace(*recoveryURL)
	if resetPage != "" {
		parsed, err := url.Parse(resetPage)
		if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
			return errors.New("--recovery-url must be an absolute http or https URL")
		}
	}
	smsEndpoint := strings.TrimSpace(*smsURL)
	if smsEndpoint == "" {
		if strings.TrimSpace(*smsToken) != "" || strings.TrimSpace(*smsFrom) != "" {
			return errors.New("--sms-token and --sms-from require --sms-url")
		}
	} else {
		parsed, err := url.Parse(smsEndpoint)
		if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
			return errors.New("--sms-url must be an absolute http or https URL")
		}
	}
	if *smsTTL < time.Minute || *smsTTL > time.Hour {
		return errors.New("--sms-ttl must be between 1m and 1h")
	}
	relyingParty := strings.TrimSpace(*webauthnRPID)
	var webauthnOrigins []string
	for _, origin := range strings.Split(*webauthnOrigin, ",") {
		if trimmed := strings.TrimSpace(origin); trimmed != "" {
			webauthnOrigins = append(webauthnOrigins, trimmed)
		}
	}
	if (relyingParty == "") != (len(webauthnOrigins) == 0) {
		return errors.New("--webauthn-rp-id and --webauthn-origin must be set together")
	}
	if relyingParty != "" {
		if err := webauthn.ValidateConfig(relyingParty, webauthnOrigins); err != nil {
			return err
		}
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
	if softwareName == "" && softwareDownload != "" {
		if name, _ := software.HostedName(softwareDownload); name != "" {
			log.Warn("software URL is under /software/ but --software-dir is not set; clients must fetch it elsewhere", "name", name)
		}
	}
	if relyingParty != "" {
		log.Info("security key login enabled", "rp_id", relyingParty, "origins", webauthnOrigins)
	}

	var (
		mailSender    *mail.Sender
		apiMailer     api.Mailer
		consoleMailer admin.Mailer
	)
	if smtpServer != "" {
		mailSender, err = mail.New(mail.Config{
			Addr:   smtpServer,
			From:   smtpFromAddr,
			User:   *smtpUser,
			Pass:   *smtpPass,
			TLS:    *smtpTLS,
			Logger: log,
		})
		if err != nil {
			return fmt.Errorf("--smtp-addr: %w", err)
		}
		apiMailer = mailSender
		consoleMailer = mailSender
		log.Info("email recovery enabled", "smtp", smtpServer, "from", smtpFromAddr,
			"tls", *smtpTLS, "auth", *smtpUser != "", "reset_ttl", *recoveryTTL)
	} else if resetPage != "" {
		log.Warn("--recovery-url has no effect without --smtp-addr; self-service recovery stays disabled")
	}

	var (
		smsSender  *sms.Sender
		apiSMS     api.SMSer
		consoleSMS admin.SMS
	)
	if smsEndpoint != "" {
		smsSender, err = sms.New(sms.Config{
			URL:    smsEndpoint,
			Token:  *smsToken,
			From:   *smsFrom,
			Logger: log,
		})
		if err != nil {
			return fmt.Errorf("--sms-url: %w", err)
		}
		apiSMS = smsSender
		consoleSMS = smsSender
		log.Info("SMS recovery enabled", "gateway", smsEndpoint, "from", *smsFrom,
			"auth", strings.TrimSpace(*smsToken) != "", "reset_ttl", *smsTTL)
	}

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

	if *apiRate < 0 {
		return errors.New("--api-rate must not be negative")
	}
	if *apiBurst < 1 {
		return errors.New("--api-burst must be at least 1")
	}
	if *loginFails < 0 {
		return errors.New("--login-max-failures must not be negative")
	}
	if *loginFails > 0 && *loginWindow < time.Second {
		return errors.New("--login-window must be at least 1s")
	}
	if *loginFails > 0 && *loginLockout < time.Second {
		return errors.New("--login-lockout must be at least 1s")
	}
	if *punchLogMax < 0 {
		return errors.New("--punch-log-max must not be negative")
	}
	if *rmem < 0 {
		return errors.New("--rmem must not be negative")
	}
	if *punchRateMax < 0 {
		return errors.New("--punch-rate-max must not be negative")
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
	if *recordingsQuota < 0 || *recordingsMax < 0 || *recordingsDays < 0 {
		return errors.New("--recordings-quota-mb, --recordings-max-mb and --recordings-retention-days must not be negative")
	}
	var acmeManager *acmecert.Manager
	acmeDomainsRaw := strings.TrimSpace(*acmeDomain)
	if acmeDomainsRaw == "" {
		if *acmeEmail != "" || *acmeCache != "" || *acmeDirectory != "" || *acmeEABKID != "" || *acmeEABKey != "" {
			return errors.New("--acme-domain is required by the other --acme-* flags")
		}
	} else {
		if *tlsCert != "" || *tlsKey != "" {
			return errors.New("--acme-domain replaces --tls-cert/--tls-key")
		}
		domains, err := acmecert.ParseDomains(acmeDomainsRaw)
		if err != nil {
			return fmt.Errorf("--acme-domain: %w", err)
		}
		if *acmeHTTPListen == "" {
			return errors.New("--acme-http-listen must not be empty when --acme-domain is set")
		}
		if sameListenPort(*acmeHTTPListen, *httpListen) {
			return errors.New("--acme-http-listen and --http-listen must not use the same TCP port")
		}
		if (*acmeEABKID == "") != (*acmeEABKey == "") {
			return errors.New("--acme-eab-kid and --acme-eab-key must be set together")
		}
		var eabKey []byte
		if *acmeEABKID != "" {
			eabKey, err = acmecert.ParseEABKey(*acmeEABKey)
			if err != nil {
				return fmt.Errorf("--acme-eab-key: %w", err)
			}
		}
		cacheDir := *acmeCache
		if cacheDir == "" {
			cacheDir = "acme-cache"
			if *stateFile != "" {
				cacheDir = filepath.Join(filepath.Dir(*stateFile), "acme-cache")
			}
		}
		acmeManager, err = acmecert.New(acmecert.Config{
			Domains:      domains,
			Email:        *acmeEmail,
			CacheDir:     cacheDir,
			DirectoryURL: *acmeDirectory,
			EABKID:       *acmeEABKID,
			EABKey:       eabKey,
			HTTPListen:   *acmeHTTPListen,
			Logger:       log,
		})
		if err != nil {
			return err
		}
		if err := acmeManager.StartHTTP(); err != nil {
			return err
		}
	}
	var tlsConfig *tls.Config
	if acmeManager != nil {
		tlsConfig, err = acmeManager.TLSConfig(*tlsClientCA, netutil.CertificateReloadInterval)
	} else {
		tlsConfig, err = netutil.LoadReloadingTLSConfig(*tlsCert, *tlsKey, *tlsClientCA, log,
			netutil.CertificateReloadInterval)
	}
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
	var relayExtra []string
	if *relayServers == "" && *relayV6 != "" {
		if *advertiseV6 == "" {
			log.Warn("IPv6 relay is listening but not advertised; pass --advertise-v6 or --relay-servers")
		} else {
			extra, err := relayaddr.New([]string{*advertiseV6})
			if err != nil {
				return fmt.Errorf("--advertise-v6: %w", err)
			}
			relayExtra = extra.Addrs()
		}
	}
	if *relayNodes != "" && *relayNodeKey == "" {
		return errors.New("--relay-nodes requires --relay-node-key")
	}
	if *relayNodes == "" && *relayNodeKey != "" {
		return errors.New("--relay-node-key requires --relay-nodes")
	}
	relayTLS, err := netutil.ClientTLSConfig(*relayNodeCA, *relayNodeCert, *relayNodeKeyID)
	if err != nil {
		return err
	}
	if relayTLS != nil && *relayNodes == "" {
		return errors.New("--relay-node-ca/--relay-node-client-cert require --relay-nodes")
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

	var (
		softwareSHA256    string
		softwareSize      int64
		softwareSignature string
	)
	if softwareName != "" {
		artifact, err := software.Load(*softwareDir, softwareName)
		if err != nil {
			return fmt.Errorf("--software-dir: %w", err)
		}
		softwareSHA256 = artifact.SHA256
		softwareSize = artifact.Size
		softwareSignature, err = serverIdentity.SignSoftware(softwareVersion, softwareDownload, softwareSHA256, softwareSize)
		if err != nil {
			return err
		}
		log.Info("software update artifact", "name", artifact.Name, "size", softwareSize, "sha256", softwareSHA256)
	}

	reg, err := registry.NewPersistent(*offline, *stateFile)
	if err != nil {
		return err
	}
	defer reg.Close()
	var accounts *account.Store
	if *stateFile != "" {
		accounts, err = account.Open(*stateFile)
		if err != nil {
			return fmt.Errorf("accounts: %w", err)
		}
		defer accounts.Close()
	} else {
		log.Warn("persistent state disabled; viewer accounts and address books are unavailable")
	}
	var geoLocator *geoip.Locator
	if *geoIPFile != "" {
		geoLocator, err = geoip.Load(*geoIPFile)
		if err != nil {
			return fmt.Errorf("--geoip-file: %w", err)
		}
		log.Info("geoip table loaded", "path", *geoIPFile, "prefixes", geoLocator.Len())
	}
	relayOnlyFlag := &atomic.Bool{}
	relayOnlyFlag.Store(*relayOnly)
	rendezvousConfig, err := configpush.New(*configSerial, strings.Split(*rendezvousList, ","))
	if err != nil {
		return fmt.Errorf("--rendezvous-servers: %w", err)
	}
	if serial, servers := rendezvousConfig.Snapshot(); len(servers) > 0 {
		log.Info("rendezvous failover list configured", "serial", serial, "servers", servers)
	}
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
	relayManager, err := relay.NewManager(*relayListen, ports, reg, log,
		relay.WithReadBuffer(*rmem),
		relay.WithViewerTokenRequired(*serverKey != ""),
		relay.WithBind6(*relayV6),
		relay.WithCarrierBind6(*carrierV6),
	)
	if err != nil {
		return err
	}
	defer relayManager.Close()
	blackFile := relay.DefaultListFile(*relayBlackFile, "blacklist.txt")
	if strings.TrimSpace(*relayBlackFile) == "" && blackFile != "" {
		log.Info("using the default relay blacklist file", "path", blackFile)
	}
	blockFile := relay.DefaultListFile(*relayBlockFile, "blocklist.txt")
	if strings.TrimSpace(*relayBlockFile) == "" && blockFile != "" {
		log.Info("using the default relay blocklist file", "path", blockFile)
	}
	if err := relayManager.SetPolicyConfig(relay.ConfigFromMbit(
		*relaySingle, *relayTotal, *relayBlacklist,
		*relayRatio, *relayDowngrade, blackFile, blockFile,
	)); err != nil {
		return fmt.Errorf("relay policy: %w", err)
	}
	if *carrierAddr != "" {
		if err := relayManager.EnableSharedCarrier(*carrierAddr); err != nil {
			return err
		}
		log.Info("shared carrier listening", "on", relayManager.CarrierAddr(),
			"addrs", relayManager.CarrierAddrs())
	}

	var pool *relaypool.Pool
	if *relayNodes != "" {
		pool, err = relaypool.New(relaypool.Config{
			Nodes:    strings.Split(*relayNodes, ","),
			Key:      *relayNodeKey,
			Interval: *relayHealth,
			Logger:   log,
			TLS:      relayTLS,
		})
		if err != nil {
			return fmt.Errorf("--relay-nodes: %w", err)
		}
		pool.Start()
		defer pool.Close()
		log.Info("relay nodes configured", "nodes", pool.NodeURLs(), "health_interval", *relayHealth)
	}

	meet, err := rendezvous.New(*punchListen, reg, relayAddr, log,
		rendezvous.WithReadBuffer(*rmem),
		rendezvous.WithPunchRateMax(*punchRateMax),
		rendezvous.WithServerKey(*serverKey),
	)
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
	punchAddrV6 := ""
	if *advertiseV6 != "" {
		punchAddrV6 = net.JoinHostPort(strings.Trim(*advertiseV6, "[]"), punchPortText)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	var recordingArchive *recordings.Archive
	if *recordingsDir != "" {
		if accounts == nil {
			return errors.New("--recordings-dir requires --state")
		}
		archive, err := recordings.New(recordings.Config{
			Dir:       *recordingsDir,
			Quota:     int64(*recordingsQuota) << 20,
			MaxUpload: int64(*recordingsMax) << 20,
			Accounts:  accounts,
			Logger:    log,
		})
		if err != nil {
			return fmt.Errorf("--recordings-dir: %w", err)
		}
		recordingArchive = archive
		log.Info("recording archive enabled", "dir", archive.Dir(),
			"quota_mb", *recordingsQuota, "max_mb", *recordingsMax,
			"retention_days", *recordingsDays)
		purge := func() {
			if *recordingsDays > 0 {
				age := time.Duration(*recordingsDays) * 24 * time.Hour
				purged, err := archive.PurgeExpired(time.Now().Add(-age))
				if err != nil {
					log.Error("recording retention", "error", err)
				} else if purged > 0 {
					log.Info("recording retention removed files", "count", purged)
				}
			}
			orphans, err := archive.PurgeOrphans(time.Now())
			if err != nil {
				log.Error("recording orphan sweep", "error", err)
			} else if orphans > 0 {
				log.Info("recording orphan sweep removed files", "count", orphans)
			}
		}
		purge()
		go func() {
			ticker := time.NewTicker(6 * time.Hour)
			defer ticker.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
					purge()
				}
			}
		}()
	}
	if (*auditRetention > 0 || *tombstoneDays > 0) && accounts != nil {
		auditAge := time.Duration(*auditRetention) * 24 * time.Hour
		tombstoneAge := time.Duration(*tombstoneDays) * 24 * time.Hour
		prune := func() {
			if auditAge > 0 {
				report, err := accounts.PruneBefore(time.Now().Add(-auditAge))
				if err != nil {
					log.Error("history retention", "error", err)
				} else if report.AuditEvents+report.Connections+report.FileTransfers > 0 {
					log.Info("history retention pruned rows", "audit", report.AuditEvents,
						"connections", report.Connections, "transfers", report.FileTransfers)
				}
			}
			if tombstoneAge > 0 {
				report, err := accounts.PruneTombstones(time.Now().Add(-tombstoneAge))
				if err != nil {
					log.Error("tombstone retention", "error", err)
				} else if report.Peers+report.Groups > 0 {
					log.Info("tombstone retention pruned rows", "peers", report.Peers, "groups", report.Groups)
				}
			}
		}
		prune()
		go func() {
			ticker := time.NewTicker(6 * time.Hour)
			defer ticker.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
					prune()
				}
			}
		}()
	}
	var archiveConsole *auditarchive.Archiver
	if *auditArchive != "" {
		if accounts == nil {
			return fmt.Errorf("--audit-archive-dir requires --state")
		}
		if *archiveEvery <= 0 {
			return fmt.Errorf("--audit-archive-interval must be positive")
		}
		if *archiveKeep < 0 {
			return fmt.Errorf("--audit-archive-retention-days must not be negative")
		}
		archiver, err := auditarchive.New(*auditArchive, accounts, log, auditarchive.Options{
			Compress:      *archiveGzip,
			RetentionDays: *archiveKeep,
		})
		if err != nil {
			return fmt.Errorf("--audit-archive-dir: %w", err)
		}
		archiveConsole = archiver
		archive := func() {
			result, err := archiver.Run()
			if err != nil {
				log.Error("audit archive", "error", err)
				return
			}
			if result.Audit+result.Connections+result.FileTransfers > 0 {
				log.Info("audit archive wrote rows", "audit", result.Audit,
					"connections", result.Connections, "transfers", result.FileTransfers,
					"files", result.Files)
			}
			if len(result.Compressed)+len(result.Pruned) > 0 {
				log.Info("audit archive compacted", "compressed", len(result.Compressed),
					"pruned", len(result.Pruned))
			}
		}
		log.Info("audit archive enabled", "dir", archiver.Dir(), "interval", *archiveEvery,
			"compress", *archiveGzip, "retention_days", *archiveKeep)
		archive()
		go func() {
			ticker := time.NewTicker(*archiveEvery)
			defer ticker.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
					archive()
				}
			}
		}()
	}
	go meet.Serve()

	target, skip, err := rendezvous.ProbeTarget(*testPunch, *advertise, meet.Addr())
	if err != nil {
		return err
	}
	if skip != "" {
		log.Info("startup punch self-check skipped", "reason", skip)
	} else {
		rtt, err := rendezvous.Probe(ctx, target, reg, log)
		if err != nil {
			return fmt.Errorf("--test-punch: %w", err)
		}
		log.Info("startup punch self-check passed", "target", target, "rtt", rtt.Round(time.Millisecond))
	}

	var console *admin.Console
	if *adminListen != "" {
		console, err = admin.New(admin.Config{
			Listen:      *adminListen,
			Registry:    reg,
			Relay:       relayManager,
			Punch:       punchLog,
			Blocker:     blocker,
			Accounts:    accounts,
			IPChanges:   ipChanges,
			RelayAddrs:  relayAddrs,
			RelayPool:   pool,
			RelayOnly:   relayOnlyFlag,
			Rendezvous:  rendezvousConfig,
			Mailer:      consoleMailer,
			RecoveryURL: resetPage,
			RecoveryTTL: *recoveryTTL,
			SMS:         consoleSMS,
			SMSTTL:      *smsTTL,
			Archive:     archiveConsole,
			Logger:      log,
		})
		if err != nil {
			return err
		}
		defer console.Close()
		go console.Serve()
		log.Info("admin console listening", "on", console.Addr())
	}

	carrierV6Host := ""
	if *carrierAddr != "" && *carrierV6 != "" {
		if *advertiseV6 == "" {
			log.Warn("IPv6 carrier is listening but not advertised; pass --advertise-v6")
		} else {
			carrierV6Host = strings.Trim(*advertiseV6, "[]")
		}
	}

	handler := api.New(api.Config{
		Registry:      reg,
		Relay:         relayManager,
		Identity:      serverIdentity,
		PunchAddr:     punchAddr,
		PunchAddrV6:   punchAddrV6,
		RelayHost:     advertised,
		ServerKey:     *serverKey,
		TTL:           *hostTTL,
		RelayOnly:     relayOnlyFlag,
		Blocker:       blocker,
		Accounts:      accounts,
		Recordings:    recordingArchive,
		GeoIP:         geoLocator,
		PunchLog:      punchLog,
		IPChanges:     ipChanges,
		RelayAddrs:    relayAddrs,
		RelayExtra:    relayExtra,
		CarrierV6Host: carrierV6Host,
		RelayPool:     pool,
		Logger:        log,
		Limiter:       ratelimit.New(*apiRate, float64(*apiBurst)),
		LoginLimiter: newLoginLimiter(accounts, authlimit.Config{
			Window:      *loginWindow,
			MaxFailures: *loginFails,
			Lockout:     *loginLockout,
		}, log),
		ProxyHeader:       *proxyHeader,
		TLS:               tlsConfig != nil,
		PunchReadBuffer:   meet.ReadBuffer(),
		SoftwareVersion:   softwareVersion,
		SoftwareURL:       softwareDownload,
		SoftwareDir:       *softwareDir,
		SoftwareSHA256:    softwareSHA256,
		SoftwareSize:      softwareSize,
		SoftwareSignature: softwareSignature,
		Rendezvous:        rendezvousConfig,
		Mailer:            apiMailer,
		SMS:               apiSMS,
		RecoveryURL:       resetPage,
		RecoveryTTL:       *recoveryTTL,
		SMSTTL:            *smsTTL,
		WebAuthnRPID:      relyingParty,
		WebAuthnOrigins:   webauthnOrigins,
		Tracer:            tracer,
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

	acmeHTTPAddr := ""
	if acmeManager != nil {
		acmeHTTPAddr = acmeManager.HTTPAddr()
	}
	log.Info("chast-server ready",
		"punch", punchAddr,
		"punch_v6", punchAddrV6,
		"relay_ports", *portRange,
		"carrier", relayManager.CarrierAddr(),
		"server_key", *serverKey != "",
		"tls", tlsConfig != nil,
		"acme", acmeDomainsRaw,
		"acme_http", acmeHTTPAddr,
		"mtls", *tlsClientCA != "",
		"api_rate", *apiRate,
		"relay_only", relayOnlyFlag.Load(),
		"relay_servers", relayAddrs.Addrs(),
		"relay_nodes", *relayNodes,
		"admin", *adminListen,
		"punch_log_max", *punchLogMax,
		"register_max", *regMax,
		"register_max_ids", *regMaxIDs,
		"rmem", *rmem,
		"punch_read_buffer", meet.ReadBuffer(),
		"relay_read_buffer", relayManager.ReadBuffer(),
		"punch_rate_max", *punchRateMax,
		"software_version", softwareVersion,
		"software_url", softwareDownload,
		"software_dir", *softwareDir,
		"software_sha256", softwareSHA256,
		"smtp_addr", smtpServer,
		"smtp_tls", *smtpTLS,
		"smtp_auth", *smtpUser != "",
		"recovery_url", resetPage,
		"recovery_ttl", *recoveryTTL,
		"trace_endpoint", *traceEndpoint,
	)
	<-ctx.Done()

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(shutdownCtx)
	if acmeManager != nil {
		_ = acmeManager.Shutdown(shutdownCtx)
	}
	log.Info("chast-server stopped")
	return nil
}

// sameListenPort reports whether two TCP listen addresses collide on the port
// number, regardless of the bind address.
func sameListenPort(a, b string) bool {
	_, portA, errA := net.SplitHostPort(a)
	_, portB, errB := net.SplitHostPort(b)
	if errA != nil || errB != nil {
		return a == b
	}
	return portA != "" && portA == portB
}

// newLoginLimiter persists lockout state when accounts are enabled, so a
// restart does not clear an active lockout. Without persistent state login is
// unavailable anyway, but the in-memory limiter keeps the config paths uniform.
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

func newLoginLimiter(accounts *account.Store, cfg authlimit.Config, log *slog.Logger) *authlimit.Limiter {
	if accounts == nil {
		return authlimit.New(cfg)
	}
	return authlimit.NewPersistent(cfg, accounts, func(err error) {
		log.Error("login lockout state", "error", err)
	})
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
