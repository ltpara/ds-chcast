package api_test

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/ldapauth"
	"github.com/ltpara/ds-chcast/server/internal/ldaptest"
)

// newLDAPServer starts a directory and an API that accepts its users.
func newLDAPServer(t *testing.T, autoCreate bool) (*account.Store, *ldaptest.Directory, *httptest.Server) {
	t.Helper()
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { store.Close() })
	directory := ldaptest.Start(t)
	auth, err := ldapauth.New(ldapauth.Config{
		URL:          directory.URL,
		BindDN:       ldaptest.ServiceDN,
		BindPassword: ldaptest.ServicePassword,
		BaseDN:       ldaptest.BaseDN,
		Filter:       "(uid=%s)",
	})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(api.New(api.Config{
		Accounts:       store,
		LDAP:           auth,
		LDAPAutoCreate: autoCreate,
	}))
	t.Cleanup(srv.Close)
	return store, directory, srv
}

func directoryLogin(t *testing.T, base, username, password string) (int, loginReply) {
	t.Helper()
	body, err := json.Marshal(map[string]string{"username": username, "password": password})
	if err != nil {
		t.Fatal(err)
	}
	status, data := request(t, http.MethodPost, base+"/v1/account/login", string(body), "")
	var reply loginReply
	if status == http.StatusOK {
		if err := json.Unmarshal(data, &reply); err != nil {
			t.Fatalf("login reply: %v", err)
		}
	}
	return status, reply
}

func TestLoginLDAPCreatesAccountAndSignsIn(t *testing.T) {
	store, directory, srv := newLDAPServer(t, true)
	status, reply := directoryLogin(t, srv.URL, "alice", "secret")
	if status != http.StatusOK {
		t.Fatalf("login: %d %+v", status, reply)
	}
	if reply.User.Username != "alice" || reply.User.Role != account.RoleAdmin || reply.Token == "" {
		t.Fatalf("reply = %+v", reply)
	}
	user, err := store.FindUser("alice")
	if err != nil || user.Email != "alice@example.com" {
		t.Fatalf("created account = %+v, %v", user, err)
	}
	if linked, err := store.LDAPLinked("alice"); err != nil || !linked {
		t.Fatalf("LDAPLinked = %v, %v", linked, err)
	}
	_, again := directoryLogin(t, srv.URL, "alice", "secret")
	if again.User.ID != reply.User.ID {
		t.Fatalf("second login created a different account: %q vs %q", again.User.ID, reply.User.ID)
	}

	directory.SetDisabled("alice", true)
	if status, _ := directoryLogin(t, srv.URL, "alice", "secret"); status != http.StatusUnauthorized {
		t.Fatalf("disabled directory account: %d", status)
	}
}

func TestLoginLDAPRejectsWrongPassword(t *testing.T) {
	_, _, srv := newLDAPServer(t, true)
	if status, _ := directoryLogin(t, srv.URL, "alice", "wrong"); status != http.StatusUnauthorized {
		t.Fatalf("wrong password: %d", status)
	}
	if status, _ := directoryLogin(t, srv.URL, "nobody", "secret"); status != http.StatusUnauthorized {
		t.Fatalf("unknown user: %d", status)
	}
}

func TestLoginLDAPLinkedAccountIgnoresLocalPassword(t *testing.T) {
	store, _, srv := newLDAPServer(t, true)
	if status, _ := directoryLogin(t, srv.URL, "alice", "secret"); status != http.StatusOK {
		t.Fatalf("first login: %d", status)
	}
	if err := store.SetPassword("alice", "local-password"); err != nil {
		t.Fatal(err)
	}
	// The directory is authoritative for a linked account, so the local
	// password must not open a session even though it is valid.
	if status, _ := directoryLogin(t, srv.URL, "alice", "local-password"); status != http.StatusUnauthorized {
		t.Fatalf("local password on a linked account: %d", status)
	}
	if status, _ := directoryLogin(t, srv.URL, "alice", "secret"); status != http.StatusOK {
		t.Fatalf("directory login after local password change: %d", status)
	}
}

func TestLoginLDAPLocalAccountKeepsItsPassword(t *testing.T) {
	store, _, srv := newLDAPServer(t, true)
	if _, err := store.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	// bob exists in the directory and not locally, so the directory signs him
	// in; a later local account with the same name is never adopted.
	if status, reply := directoryLogin(t, srv.URL, "bob", "hunter2"); status != http.StatusOK || reply.User.Username != "bob" {
		t.Fatalf("directory login: %d %+v", status, reply)
	}
	if status, _ := directoryLogin(t, srv.URL, "admin", "wrong"); status != http.StatusUnauthorized {
		t.Fatalf("local admin with a wrong directory password: %d", status)
	}
	if status, reply := directoryLogin(t, srv.URL, "admin", "correct horse"); status != http.StatusOK || reply.User.Username != "admin" {
		t.Fatalf("local admin login: %d %+v", status, reply)
	}
}

func TestLoginLDAPAutoCreateDisabled(t *testing.T) {
	_, _, srv := newLDAPServer(t, false)
	if status, _ := directoryLogin(t, srv.URL, "alice", "secret"); status != http.StatusUnauthorized {
		t.Fatalf("auto-create disabled: %d", status)
	}
}

func TestLoginLDAPBannedLinkedAccount(t *testing.T) {
	store, _, srv := newLDAPServer(t, true)
	if _, err := store.CreateUser("admin", "correct horse"); err != nil {
		t.Fatal(err)
	}
	if status, _ := directoryLogin(t, srv.URL, "alice", "secret"); status != http.StatusOK {
		t.Fatalf("first login: %d", status)
	}
	if _, err := store.SetBanned("alice", true); err != nil {
		t.Fatal(err)
	}
	if status, _ := directoryLogin(t, srv.URL, "alice", "secret"); status != http.StatusForbidden {
		t.Fatalf("banned linked account: %d", status)
	}
}

func TestLoginLDAPDirectoryUnavailable(t *testing.T) {
	_, directory, srv := newLDAPServer(t, true)
	directory.Close()
	status, _ := directoryLogin(t, srv.URL, "alice", "secret")
	if status != http.StatusServiceUnavailable {
		t.Fatalf("directory down: %d", status)
	}
}
