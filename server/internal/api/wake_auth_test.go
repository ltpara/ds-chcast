package api

import (
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

func TestWakeAuthenticationWithoutAccounts(t *testing.T) {
	for _, tc := range []struct {
		name, key, token string
		allowed          bool
	}{
		{"open", "", "", true},
		{"missing key", "secret", "", false},
		{"wrong key", "secret", "wrong", false},
		{"valid key", "secret", "secret", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := &server{cfg: Config{ServerKey: tc.key}}
			req := httptest.NewRequest(http.MethodPost, "/", nil)
			if tc.token != "" {
				req.Header.Set("Authorization", "Bearer "+tc.token)
			}
			response := httptest.NewRecorder()
			_, allowed := s.wakeActor(response, req, "target")
			if allowed != tc.allowed || (!allowed && response.Code != http.StatusUnauthorized) {
				t.Fatalf("allowed=%v status=%d", allowed, response.Code)
			}
		})
	}
}

func TestWakeRequiresAccountWhenServerKeyIsEmpty(t *testing.T) {
	store, err := account.Open(filepath.Join(t.TempDir(), "accounts.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	s := &server{cfg: Config{Accounts: store}}
	req := httptest.NewRequest(http.MethodPost, "/", nil)
	response := httptest.NewRecorder()
	if _, allowed := s.wakeActor(response, req, "target"); allowed || response.Code != http.StatusUnauthorized {
		t.Fatalf("anonymous wake allowed=%v status=%d", allowed, response.Code)
	}
}
