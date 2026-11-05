package api_test

import (
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/api"
)

func TestOpenRegistrationRequiresConsoleCredentials(t *testing.T) {
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	admin, err := store.CreateUser("admin", "correct horse")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := store.CreateUser("viewer", "correct horse"); err != nil {
		t.Fatal(err)
	}
	group, err := store.CreateAccessGroup("private", "", 0, 0, admin.ID)
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(api.New(api.Config{Accounts: store}))
	defer srv.Close()
	adminToken := login(t, srv.URL, "admin", "correct horse").Token
	viewerToken := login(t, srv.URL, "viewer", "correct horse").Token
	for _, tc := range []struct {
		name, token string
		want        int
	}{
		{"anonymous", "", http.StatusUnauthorized},
		{"invalid", "invalid", http.StatusUnauthorized},
		{"viewer", viewerToken, http.StatusForbidden},
		{"admin", adminToken, http.StatusOK},
	} {
		t.Run(tc.name, func(t *testing.T) {
			for _, path := range []string{"/v1/accounts", "/v1/access-groups/" + group.ID} {
				status, body := request(t, http.MethodGet, srv.URL+path, "", tc.token)
				if status != tc.want {
					t.Fatalf("%s: %d %s", path, status, body)
				}
			}
		})
	}
	status, body := request(t, http.MethodPost, srv.URL+"/v1/accounts",
		`{"username":"intruder","password":"correct horse","role":"admin"}`, "")
	if status != http.StatusUnauthorized {
		t.Fatalf("anonymous create: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, srv.URL+"/v1/access-groups", "", "")
	if status != http.StatusUnauthorized {
		t.Fatalf("anonymous groups: %d %s", status, body)
	}
	status, body = request(t, http.MethodGet, srv.URL+"/v1/access-groups", "", viewerToken)
	if status != http.StatusOK || string(body) != "{\"groups\":[]}\n" {
		t.Fatalf("viewer groups: %d %s", status, body)
	}
}
