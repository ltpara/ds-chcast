package api_test

import (
	"encoding/json"
	"net/http"
	"net/url"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type identityEntry struct {
	ID         string   `json:"id"`
	Hostname   string   `json:"hostname"`
	Platform   string   `json:"platform"`
	Version    string   `json:"version"`
	Alias      string   `json:"alias"`
	Tags       []string `json:"tags"`
	Online     bool     `json:"online"`
	Relay      string   `json:"relay"`
	Candidate  string   `json:"candidate"`
	LastSeenMS int64    `json:"last_seen_ms"`
}

type identityList struct {
	Identities []identityEntry `json:"identities"`
	Total      int             `json:"total"`
	Online     int             `json:"online"`
}

func listIdentities(t *testing.T, base, query, key string) (int, identityList) {
	t.Helper()
	status, body := request(t, http.MethodGet, base+"/v1/identities"+query, "", key)
	var out identityList
	if status == http.StatusOK {
		if err := json.Unmarshal(body, &out); err != nil {
			t.Fatalf("decode identities: %v (%s)", err, body)
		}
	}
	return status, out
}

func TestListIdentities(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	for _, host := range []struct{ id, secret, fingerprint string }{
		{"alpha", "alpha secret", "AB12"},
		{"beta", "beta secret", "CD34"},
	} {
		status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
			registerBody(t, host.id, host.secret, host.fingerprint, now), testutil.ServerKey)
		if status != http.StatusOK {
			t.Fatalf("register %s: %d %s", host.id, status, body)
		}
	}
	if err := s.Registry.Touch("alpha", "198.51.100.7:5000", nil, ""); err != nil {
		t.Fatal(err)
	}
	if status, body := request(t, http.MethodPatch, s.URL+"/v1/identities/alpha",
		`{"alias":"办公室","tags":["lab","vip"]}`, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("labels: %d %s", status, body)
	}
	// Removing the lease keeps the durable identity, so beta stays listed but
	// offline.
	if status, body := request(t, http.MethodDelete, s.URL+"/v1/hosts/beta", "", testutil.ServerKey); status != http.StatusNoContent {
		t.Fatalf("unregister beta: %d %s", status, body)
	}

	if status, _ := listIdentities(t, s.URL, "", ""); status != http.StatusUnauthorized {
		t.Fatalf("list without a key: %d", status)
	}
	if status, _ := listIdentities(t, s.URL, "", "wrong-key"); status != http.StatusUnauthorized {
		t.Fatalf("list with a wrong key: %d", status)
	}

	status, list := listIdentities(t, s.URL, "", testutil.ServerKey)
	if status != http.StatusOK || list.Total != 2 || list.Online != 1 || len(list.Identities) != 2 {
		t.Fatalf("list = %d %+v", status, list)
	}
	if list.Identities[0].ID != "alpha" || !list.Identities[0].Online ||
		list.Identities[0].Alias != "办公室" || len(list.Identities[0].Tags) != 2 {
		t.Fatalf("alpha entry = %+v", list.Identities[0])
	}
	if list.Identities[0].Relay == "" || list.Identities[0].Candidate != "198.51.100.7:5000" ||
		list.Identities[0].LastSeenMS < 0 {
		t.Fatalf("alpha live fields = %+v", list.Identities[0])
	}
	if list.Identities[1].ID != "beta" || list.Identities[1].Online ||
		list.Identities[1].Relay != "" || list.Identities[1].LastSeenMS < 0 {
		t.Fatalf("beta entry = %+v", list.Identities[1])
	}

	if _, list := listIdentities(t, s.URL, "?q="+url.QueryEscape("办公"), testutil.ServerKey); list.Total != 1 || list.Identities[0].ID != "alpha" {
		t.Fatalf("alias search = %+v", list)
	}
	if _, list := listIdentities(t, s.URL, "?q=LAB", testutil.ServerKey); list.Total != 1 || list.Identities[0].ID != "alpha" {
		t.Fatalf("tag search = %+v", list)
	}
	if _, list := listIdentities(t, s.URL, "?q=CD34", testutil.ServerKey); list.Total != 1 || list.Identities[0].ID != "beta" {
		t.Fatalf("fingerprint search = %+v", list)
	}
	if _, list := listIdentities(t, s.URL, "?online=1", testutil.ServerKey); list.Total != 1 || !list.Identities[0].Online {
		t.Fatalf("online filter = %+v", list)
	}
	if _, list := listIdentities(t, s.URL, "?limit=1&offset=1", testutil.ServerKey); list.Total != 2 || list.Online != 1 || len(list.Identities) != 1 || list.Identities[0].ID != "beta" {
		t.Fatalf("paged list = %+v", list)
	}
	for _, query := range []string{"?limit=0", "?limit=1001", "?offset=-1", "?online=maybe"} {
		if status, _ := listIdentities(t, s.URL, query, testutil.ServerKey); status != http.StatusBadRequest {
			t.Fatalf("%s = %d, want 400", query, status)
		}
	}
}
