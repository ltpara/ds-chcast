package ldapauth_test

import (
	"context"
	"errors"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/ldapauth"
	"github.com/ltpara/ds-chcast/server/internal/ldaptest"
)

func newAuthenticator(t *testing.T, url string) *ldapauth.Authenticator {
	t.Helper()
	auth, err := ldapauth.New(ldapauth.Config{
		URL:          url,
		BindDN:       ldaptest.ServiceDN,
		BindPassword: ldaptest.ServicePassword,
		BaseDN:       ldaptest.BaseDN,
		Filter:       "(uid=%s)",
	})
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	return auth
}

func TestAuthenticateFindsEntryAndReadsMail(t *testing.T) {
	directory := ldaptest.Start(t)
	auth := newAuthenticator(t, directory.URL)
	identity, err := auth.Authenticate(context.Background(), "alice", "secret")
	if err != nil {
		t.Fatalf("authenticate: %v", err)
	}
	if identity.DN != "uid=alice,"+ldaptest.BaseDN {
		t.Errorf("DN = %q", identity.DN)
	}
	if identity.Email != "alice@example.com" {
		t.Errorf("email = %q", identity.Email)
	}
}

func TestAuthenticateAnonymousSearch(t *testing.T) {
	directory := ldaptest.Start(t)
	auth, err := ldapauth.New(ldapauth.Config{
		URL:    directory.URL,
		BaseDN: ldaptest.BaseDN,
		Filter: "(uid=%s)",
	})
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	if _, err := auth.Authenticate(context.Background(), "bob", "hunter2"); err != nil {
		t.Fatalf("authenticate without service bind: %v", err)
	}
}

func TestAuthenticateRejectsWrongPassword(t *testing.T) {
	directory := ldaptest.Start(t)
	auth := newAuthenticator(t, directory.URL)
	_, err := auth.Authenticate(context.Background(), "alice", "wrong")
	if !errors.Is(err, ldapauth.ErrInvalidCredentials) {
		t.Fatalf("err = %v, want ErrInvalidCredentials", err)
	}
}

func TestAuthenticateRejectsUnknownUser(t *testing.T) {
	directory := ldaptest.Start(t)
	auth := newAuthenticator(t, directory.URL)
	_, err := auth.Authenticate(context.Background(), "carol", "secret")
	if !errors.Is(err, ldapauth.ErrInvalidCredentials) {
		t.Fatalf("err = %v, want ErrInvalidCredentials", err)
	}
}

func TestAuthenticateEscapesFilterMetacharacters(t *testing.T) {
	directory := ldaptest.Start(t)
	auth := newAuthenticator(t, directory.URL)
	// An unescaped wildcard would match every entry and could sign in as the
	// first one; the escaped login must match nothing at all.
	for _, login := range []string{"*", "alice)(uid=*", "alice*"} {
		if _, err := auth.Authenticate(context.Background(), login, "secret"); !errors.Is(err, ldapauth.ErrInvalidCredentials) {
			t.Errorf("login %q: err = %v, want ErrInvalidCredentials", login, err)
		}
	}
}

func TestAuthenticateRefusesEmptyPasswordWithoutDialing(t *testing.T) {
	// The URL is deliberately unreachable: an empty password must be refused
	// before any network operation.
	auth := newAuthenticator(t, "ldap://127.0.0.1:1")
	_, err := auth.Authenticate(context.Background(), "alice", "")
	if !errors.Is(err, ldapauth.ErrInvalidCredentials) {
		t.Fatalf("err = %v, want ErrInvalidCredentials", err)
	}
}

func TestAuthenticateDisabledAccount(t *testing.T) {
	directory := ldaptest.Start(t)
	directory.SetDisabled("alice", true)
	auth := newAuthenticator(t, directory.URL)
	_, err := auth.Authenticate(context.Background(), "alice", "secret")
	if !errors.Is(err, ldapauth.ErrInvalidCredentials) {
		t.Fatalf("err = %v, want ErrInvalidCredentials", err)
	}
}

func TestAuthenticateUnreachableDirectory(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve port: %v", err)
	}
	addr := listener.Addr().String()
	listener.Close()
	auth, err := ldapauth.New(ldapauth.Config{
		URL:     "ldap://" + addr,
		BaseDN:  ldaptest.BaseDN,
		Filter:  "(uid=%s)",
		Timeout: 500 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	if _, err := auth.Authenticate(context.Background(), "alice", "secret"); !errors.Is(err, ldapauth.ErrDirectoryUnavailable) {
		t.Fatalf("err = %v, want ErrDirectoryUnavailable", err)
	}
}

func TestAuthenticateWrongBaseDN(t *testing.T) {
	directory := ldaptest.Start(t)
	auth, err := ldapauth.New(ldapauth.Config{
		URL:          directory.URL,
		BindDN:       ldaptest.ServiceDN,
		BindPassword: ldaptest.ServicePassword,
		BaseDN:       "ou=elsewhere,dc=example,dc=com",
		Filter:       "(uid=%s)",
	})
	if err != nil {
		t.Fatalf("new authenticator: %v", err)
	}
	if _, err := auth.Authenticate(context.Background(), "alice", "secret"); !errors.Is(err, ldapauth.ErrDirectoryUnavailable) {
		t.Fatalf("err = %v, want ErrDirectoryUnavailable", err)
	}
}

func TestNewRejectsUnsafeConfig(t *testing.T) {
	cases := []struct {
		name string
		cfg  ldapauth.Config
		want string
	}{
		{"relative url", ldapauth.Config{URL: "ldap:///", BaseDN: "dc=x", Filter: "(uid=%s)"}, "absolute"},
		{"scheme", ldapauth.Config{URL: "http://host:389", BaseDN: "dc=x", Filter: "(uid=%s)"}, "scheme"},
		{"credentials in url", ldapauth.Config{URL: "ldap://user:pass@host:389", BaseDN: "dc=x", Filter: "(uid=%s)"}, "credentials"},
		{"missing base", ldapauth.Config{URL: "ldap://host:389", Filter: "(uid=%s)"}, "base DN"},
		{"no placeholder", ldapauth.Config{URL: "ldap://host:389", BaseDN: "dc=x", Filter: "(uid=alice)"}, "placeholder"},
		{"two placeholders", ldapauth.Config{URL: "ldap://host:389", BaseDN: "dc=x", Filter: "(|(uid=%s)(mail=%s))"}, "placeholder"},
		{"password without dn", ldapauth.Config{URL: "ldap://host:389", BaseDN: "dc=x", Filter: "(uid=%s)", BindPassword: "x"}, "bind password"},
		{"dn without password", ldapauth.Config{URL: "ldap://host:389", BaseDN: "dc=x", Filter: "(uid=%s)", BindDN: "cn=svc,dc=x"}, "bind password"},
		{"starttls on ldaps", ldapauth.Config{URL: "ldaps://host:636", BaseDN: "dc=x", Filter: "(uid=%s)", StartTLS: true}, "StartTLS"},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			_, err := ldapauth.New(testCase.cfg)
			if err == nil || !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("err = %v, want containing %q", err, testCase.want)
			}
		})
	}
}
