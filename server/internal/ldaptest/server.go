// Package ldaptest serves an in-process LDAP directory for tests. It answers
// simple binds and subtree searches, so a test can exercise the real
// bind-and-search client instead of a fake one.
package ldaptest

import (
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/hashicorp/go-hclog"
	"github.com/jimlambrt/gldap"
)

// BaseDN is the search root every test directory serves.
const BaseDN = "ou=people,dc=example,dc=com"

// ServiceDN and ServicePassword are the service account tests can pass in
// ldapauth.Config. The bind handler accepts only this pair for searches.
const (
	ServiceDN       = "cn=service," + BaseDN
	ServicePassword = "service-secret"
)

type entry struct {
	dn       string
	password string
	email    string
	disabled bool
}

// Directory is a running LDAP server plus the mutable identities it serves.
type Directory struct {
	URL    string
	server *gldap.Server

	mu    sync.Mutex
	users map[string]entry
}

// Start listens on a loopback port and shuts the server down when the test
// ends. The directory starts with alice/secret and bob/hunter2.
func Start(t testing.TB) *Directory {
	t.Helper()
	d := &Directory{users: map[string]entry{}}
	d.AddUser("alice", "secret", "alice@example.com")
	d.AddUser("bob", "hunter2", "bob@example.com")

	server, err := gldap.NewServer(gldap.WithLogger(hclog.NewNullLogger()))
	if err != nil {
		t.Fatalf("ldaptest: new server: %v", err)
	}
	mux, err := gldap.NewMux()
	if err != nil {
		t.Fatalf("ldaptest: new mux: %v", err)
	}
	if err := mux.Bind(d.bind); err != nil {
		t.Fatalf("ldaptest: bind route: %v", err)
	}
	// No route base DN: every search reaches the handler, which answers
	// noSuchObject itself so a wrong base DN fails fast instead of hanging.
	if err := mux.Search(d.search); err != nil {
		t.Fatalf("ldaptest: search route: %v", err)
	}
	if err := server.Router(mux); err != nil {
		t.Fatalf("ldaptest: router: %v", err)
	}

	addr := reserveAddr(t)
	go func() { _ = server.Run(addr) }()
	deadline := time.Now().Add(5 * time.Second)
	for !server.Ready() {
		if time.Now().After(deadline) {
			t.Fatalf("ldaptest: server did not start on %s", addr)
		}
		time.Sleep(5 * time.Millisecond)
	}
	d.server = server
	d.URL = "ldap://" + addr
	t.Cleanup(d.Close)
	return d
}

// Close stops the server. It is called automatically by Start.
func (d *Directory) Close() {
	if d.server != nil {
		_ = d.server.Stop()
	}
}

// AddUser creates or replaces an identity. The entry DN is
// uid=<name>,ou=people,dc=example,dc=com.
func (d *Directory) AddUser(name, password, email string) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.users[name] = entry{
		dn:       fmt.Sprintf("uid=%s,%s", name, BaseDN),
		password: password,
		email:    email,
	}
}

// SetDisabled makes the directory refuse the user's bind, the way an AD or
// OpenLDAP account lock looks to a client: result code 49 for a correct
// password.
func (d *Directory) SetDisabled(name string, disabled bool) {
	d.mu.Lock()
	defer d.mu.Unlock()
	user := d.users[name]
	user.disabled = disabled
	d.users[name] = user
}

func (d *Directory) bind(w *gldap.ResponseWriter, r *gldap.Request) {
	response := r.NewBindResponse(gldap.WithResponseCode(gldap.ResultInvalidCredentials))
	defer func() { _ = w.Write(response) }()
	message, err := r.GetSimpleBindMessage()
	if err != nil {
		return
	}
	password := string(message.Password)
	if strings.EqualFold(message.UserName, ServiceDN) && password == ServicePassword {
		response.SetResultCode(gldap.ResultSuccess)
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, user := range d.users {
		if user.disabled || !strings.EqualFold(user.dn, message.UserName) {
			continue
		}
		if user.password == password {
			response.SetResultCode(gldap.ResultSuccess)
			return
		}
	}
}

func (d *Directory) search(w *gldap.ResponseWriter, r *gldap.Request) {
	response := r.NewSearchDoneResponse(gldap.WithResponseCode(gldap.ResultSuccess))
	defer func() { _ = w.Write(response) }()
	message, err := r.GetSearchMessage()
	if err != nil {
		response.SetResultCode(gldap.ResultProtocolError)
		return
	}
	if !strings.EqualFold(message.BaseDN, BaseDN) {
		response.SetResultCode(gldap.ResultNoSuchObject)
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	for name, user := range d.users {
		// The client escapes the login name, so a wildcard login arrives as
		// (uid=\2a) and matches nothing.
		if !strings.Contains(message.Filter, "(uid="+name+")") {
			continue
		}
		_ = w.Write(r.NewSearchResponseEntry(user.dn, gldap.WithAttributes(map[string][]string{
			"uid":  {name},
			"mail": {user.email},
		})))
	}
}

func reserveAddr(t testing.TB) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("ldaptest: reserve port: %v", err)
	}
	addr := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatalf("ldaptest: release port: %v", err)
	}
	return addr
}
