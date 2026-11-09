package api_test

import (
	"net/http"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

const (
	roleLinkKey = "test-link-key"
	roleHostKey = "test-host-key"
)

// TestRoleKeysSeparateViewerAndHostCredentials pins the three-key model: the
// management key is the root credential, the link key opens viewer lookups
// only, and the host key opens registration and host-signed audit only.
func TestRoleKeysSeparateViewerAndHostCredentials(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{LinkKey: roleLinkKey, HostKey: roleHostKey})
	now := time.Now().Unix()

	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "host secret", "AB12", now), roleHostKey); status != http.StatusOK {
		t.Fatalf("host key registration: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "link-registered", "link secret", "", now), roleLinkKey); status != http.StatusUnauthorized {
		t.Fatalf("a link key registered a host: %d %s", status, body)
	}
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "root-registered", "root secret", "", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("management key registration: %d %s", status, body)
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", roleLinkKey); status != http.StatusOK {
		t.Fatalf("link key lookup: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", roleHostKey); status != http.StatusUnauthorized {
		t.Fatalf("a host key opened lookup: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("management key lookup: %d %s", status, body)
	}

	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts?ids=devbox", "", roleLinkKey); status != http.StatusOK {
		t.Fatalf("link key batch lookup: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts?ids=devbox", "", roleHostKey); status != http.StatusUnauthorized {
		t.Fatalf("a host key opened batch lookup: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts?ids=devbox", "", testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("management key batch lookup: %d %s", status, body)
	}

	auditBody := hostAuditSessionBody(t, "host secret", "devbox", "aabbccdd",
		1700000000000, 1700000001000, true, "", time.Now().Unix())
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		auditBody, roleHostKey); status != http.StatusCreated {
		t.Fatalf("host key audit upload: %d %s", status, body)
	}
	linkAuditBody := hostAuditSessionBody(t, "host secret", "devbox", "aabbccde",
		1700000002000, 1700000003000, true, "", time.Now().Unix())
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		linkAuditBody, roleLinkKey); status != http.StatusUnauthorized {
		t.Fatalf("a link key wrote host audit: %d %s", status, body)
	}

	for _, key := range []string{roleLinkKey, roleHostKey} {
		if status, body := request(t, http.MethodGet, s.URL+"/metrics", "", key); status != http.StatusUnauthorized {
			t.Fatalf("a role key opened metrics: %d %s", status, body)
		}
		if status, body := request(t, http.MethodGet, s.URL+"/v1/audit", "", key); status != http.StatusUnauthorized {
			t.Fatalf("a role key opened the console audit log: %d %s", status, body)
		}
		for _, tc := range []struct{ method, path, body string }{
			{http.MethodPost, "/v1/accounts", `{"username":"role-admin"}`},
			{http.MethodDelete, "/v1/identities/devbox", ""},
			{http.MethodPatch, "/v1/identities/devbox", `{"alias":"x"}`},
			{http.MethodGet, "/v1/recordings", ""},
			{http.MethodPost, "/v1/hosts/devbox/wake", `{"mac":"001122334455"}`},
			{http.MethodGet, "/v1/groups", ""},
		} {
			if status, body := request(t, tc.method, s.URL+tc.path, tc.body, key); status != http.StatusUnauthorized {
				t.Fatalf("a role key reached %s %s: %d %s", tc.method, tc.path, status, body)
			}
		}
	}
	if status, body := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("management key metrics: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/audit", "", testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("management key audit log: %d %s", status, body)
	}
}

// TestRoleKeysFallBackToManagementKey keeps an existing deployment working:
// with no role keys configured, a host registered with the server key still
// looks up and audits with the same key.
func TestRoleKeysFallBackToManagementKey(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{})
	now := time.Now().Unix()

	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "devbox", "host secret", "AB12", now), testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	if status, body := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	auditBody := hostAuditSessionBody(t, "host secret", "devbox", "aabbccdf",
		1700000000000, 1700000001000, true, "", time.Now().Unix())
	if status, body := request(t, http.MethodPost, s.URL+"/v1/hosts/devbox/audit",
		auditBody, testutil.ServerKey); status != http.StatusCreated {
		t.Fatalf("audit upload: %d %s", status, body)
	}
}
