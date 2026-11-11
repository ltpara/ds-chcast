// Package ldapauth authenticates a person against an LDAP or Active Directory
// server with the bind-and-search pattern: a service account finds the entry
// for a login name, then the connection rebinds as that entry with the
// supplied password. The directory decides whether the account is enabled,
// unlocked and its password valid, so a directory-side disable takes effect on
// the next login without touching the local account database.
package ldapauth

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/url"
	"strings"
	"time"

	ldap "github.com/go-ldap/ldap/v3"
)

var (
	// ErrInvalidCredentials covers an unknown login name and a rejected
	// password alike, so a caller cannot tell them apart.
	ErrInvalidCredentials = errors.New("ldap: invalid credentials")
	// ErrDirectoryUnavailable covers dial, TLS, service-bind and search
	// failures. It is a configuration or availability problem, not a verdict
	// on the supplied password, so it must not count as a failed login.
	ErrDirectoryUnavailable = errors.New("ldap: directory unavailable")
)

const (
	defaultTimeout   = 8 * time.Second
	defaultEmailAttr = "mail"
	// resultInvalidCredentials is LDAP result code 49, returned by a bind that
	// the directory refused.
	resultInvalidCredentials = 49
	// resultNoSuchObject is result code 32, normally a wrong base DN.
	resultNoSuchObject = 32
)

// Config describes one directory. URL is ldap:// (optionally upgraded with
// StartTLS) or ldaps://. Filter must contain exactly one %s, which receives
// the escaped login name; BaseDN scopes the search. BindDN and BindPassword
// are the service account used for the search; both empty performs an
// anonymous search, which only works on directories that allow it.
type Config struct {
	URL                string
	BindDN             string
	BindPassword       string
	BaseDN             string
	Filter             string
	EmailAttr          string
	StartTLS           bool
	InsecureSkipVerify bool
	Timeout            time.Duration
}

// Identity is the directory entry that proved the password.
type Identity struct {
	DN    string
	Email string
}

// Authenticator is safe for concurrent use.
type Authenticator struct {
	url          string
	scheme       string
	serverName   string
	bindDN       string
	bindPassword string
	baseDN       string
	filter       string
	emailAttr    string
	startTLS     bool
	insecure     bool
	timeout      time.Duration
}

// New validates the configuration. It rejects a template that cannot be used
// safely instead of failing at the first login.
func New(cfg Config) (*Authenticator, error) {
	raw := strings.TrimSpace(cfg.URL)
	parsed, err := url.Parse(raw)
	if err != nil || parsed.Host == "" {
		return nil, fmt.Errorf("ldap url %q is not an absolute URL", raw)
	}
	if parsed.Scheme != "ldap" && parsed.Scheme != "ldaps" {
		return nil, fmt.Errorf("ldap url scheme %q must be ldap or ldaps", parsed.Scheme)
	}
	if parsed.User != nil {
		return nil, errors.New("ldap url must not carry credentials; use the bind DN flags")
	}
	baseDN := strings.TrimSpace(cfg.BaseDN)
	if baseDN == "" {
		return nil, errors.New("ldap base DN is required")
	}
	filter := strings.TrimSpace(cfg.Filter)
	if strings.Count(filter, "%s") != 1 {
		return nil, errors.New("ldap filter must contain exactly one %s placeholder for the login name")
	}
	if cfg.BindDN == "" && cfg.BindPassword != "" {
		return nil, errors.New("ldap bind password requires a bind DN")
	}
	if cfg.BindDN != "" && cfg.BindPassword == "" {
		return nil, errors.New("ldap bind DN requires a bind password")
	}
	if cfg.StartTLS && parsed.Scheme == "ldaps" {
		return nil, errors.New("ldap StartTLS cannot be combined with an ldaps url")
	}
	emailAttr := strings.TrimSpace(cfg.EmailAttr)
	if emailAttr == "" {
		emailAttr = defaultEmailAttr
	}
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = defaultTimeout
	}
	return &Authenticator{
		url:          raw,
		scheme:       parsed.Scheme,
		serverName:   parsed.Hostname(),
		bindDN:       cfg.BindDN,
		bindPassword: cfg.BindPassword,
		baseDN:       baseDN,
		filter:       filter,
		emailAttr:    emailAttr,
		startTLS:     cfg.StartTLS,
		insecure:     cfg.InsecureSkipVerify,
		timeout:      timeout,
	}, nil
}

// Authenticate finds the entry for username and proves password by binding as
// it. An empty password is refused before dialing: a simple bind with no
// password is an unauthenticated bind, which some servers accept.
func (a *Authenticator) Authenticate(ctx context.Context, username, password string) (Identity, error) {
	username = strings.TrimSpace(username)
	if username == "" || password == "" {
		return Identity{}, ErrInvalidCredentials
	}
	conn, done, err := a.dial(ctx)
	if err != nil {
		return Identity{}, err
	}
	defer func() {
		close(done)
		conn.Close()
	}()
	if a.bindDN != "" {
		if err := conn.Bind(a.bindDN, a.bindPassword); err != nil {
			return Identity{}, fmt.Errorf("%w: service bind: %v", ErrDirectoryUnavailable, err)
		}
	}
	filter := fmt.Sprintf(a.filter, ldap.EscapeFilter(username))
	// Two entries are enough to detect an ambiguous filter; asking for more
	// would only widen the search.
	request := ldap.NewSearchRequest(a.baseDN, ldap.ScopeWholeSubtree,
		ldap.NeverDerefAliases, 2, 0, false, filter, []string{a.emailAttr}, nil)
	result, err := conn.Search(request)
	if err != nil {
		if ldapCode(err) == resultNoSuchObject {
			return Identity{}, fmt.Errorf("%w: search base %q does not exist", ErrDirectoryUnavailable, a.baseDN)
		}
		return Identity{}, fmt.Errorf("%w: search: %v", ErrDirectoryUnavailable, err)
	}
	switch len(result.Entries) {
	case 1:
	case 0:
		return Identity{}, ErrInvalidCredentials
	default:
		return Identity{}, fmt.Errorf("%w: %d entries match the login filter", ErrDirectoryUnavailable, len(result.Entries))
	}
	entry := result.Entries[0]
	if err := conn.Bind(entry.DN, password); err != nil {
		if ldapCode(err) == resultInvalidCredentials {
			return Identity{}, ErrInvalidCredentials
		}
		return Identity{}, fmt.Errorf("%w: user bind: %v", ErrDirectoryUnavailable, err)
	}
	identity := Identity{DN: entry.DN}
	if values := entry.GetAttributeValues(a.emailAttr); len(values) > 0 {
		identity.Email = strings.TrimSpace(values[0])
	}
	return identity, nil
}

// dial returns a connection whose operations all finish within the configured
// timeout and whose socket is closed when ctx is cancelled. done must be
// closed by the caller once it no longer uses the connection.
func (a *Authenticator) dial(ctx context.Context) (*ldap.Conn, chan struct{}, error) {
	dialer := &net.Dialer{Timeout: a.timeout}
	options := []ldap.DialOpt{ldap.DialWithDialer(dialer)}
	if a.scheme == "ldaps" && a.insecure {
		options = append(options, ldap.DialWithTLSConfig(&tls.Config{InsecureSkipVerify: true}))
	}
	conn, err := ldap.DialURL(a.url, options...)
	if err != nil {
		return nil, nil, fmt.Errorf("%w: %v", ErrDirectoryUnavailable, err)
	}
	if a.startTLS {
		tlsConfig := &tls.Config{ServerName: a.serverName, InsecureSkipVerify: a.insecure}
		if err := conn.StartTLS(tlsConfig); err != nil {
			conn.Close()
			return nil, nil, fmt.Errorf("%w: StartTLS: %v", ErrDirectoryUnavailable, err)
		}
	}
	conn.SetTimeout(a.timeout)
	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			conn.Close()
		case <-done:
		}
	}()
	return conn, done, nil
}

// ldapCode extracts the LDAP result code from an error, or 0 when the failure
// happened below the protocol (network, TLS).
func ldapCode(err error) uint16 {
	var ldapErr *ldap.Error
	if errors.As(err, &ldapErr) {
		return ldapErr.ResultCode
	}
	return 0
}
