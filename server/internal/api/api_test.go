package api_test

import (
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/api"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type hostReply struct {
	ID          string `json:"id"`
	Online      bool   `json:"online"`
	Candidate   string `json:"candidate"`
	Relay       string `json:"relay"`
	Punch       string `json:"punch"`
	Fingerprint string `json:"fingerprint"`
	LastSeenMS  int64  `json:"last_seen_ms"`
	ExpiresInMS int64  `json:"expires_in_ms"`
}

func request(t *testing.T, method, url, body, key string) (int, []byte) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatal(err)
	}
	if key != "" {
		req.Header.Set("Authorization", "Bearer "+key)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, data
}

func TestRegisterLookupAndDelete(t *testing.T) {
	s := testutil.Start(t)
	const key = "correct horse"

	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		`{"id":"devbox","key":"correct horse","fingerprint":"AB12"}`, testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}
	var reg struct {
		ID    string `json:"id"`
		Relay string `json:"relay"`
		Punch string `json:"punch"`
	}
	if err := json.Unmarshal(body, &reg); err != nil {
		t.Fatal(err)
	}
	if reg.ID != "devbox" || reg.Relay == "" || reg.Punch != s.Punch {
		t.Fatalf("register reply: %+v", reg)
	}

	status, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
	if status != http.StatusOK {
		t.Fatalf("lookup: %d %s", status, body)
	}
	var host hostReply
	json.Unmarshal(body, &host)
	if host.Online || host.Candidate != "" {
		t.Fatalf("host without keepalive: %+v", host)
	}
	if host.Relay != reg.Relay || host.Punch != reg.Punch || host.Fingerprint != "AB12" {
		t.Fatalf("lookup reply: %+v", host)
	}

	if err := s.Registry.Touch("devbox", api.HashKey(key), "198.51.100.9:5000"); err != nil {
		t.Fatal(err)
	}
	_, body = request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", "")
	json.Unmarshal(body, &host)
	if !host.Online || host.Candidate != "198.51.100.9:5000" || host.LastSeenMS < 0 {
		t.Fatalf("host after keepalive: %+v", host)
	}
	if host.ExpiresInMS <= 0 || host.ExpiresInMS > int64(time.Minute/time.Millisecond) {
		t.Fatalf("expiry = %d", host.ExpiresInMS)
	}

	status, body = request(t, http.MethodDelete, s.URL+"/v1/hosts/devbox", "", testutil.ServerKey)
	if status != http.StatusNoContent {
		t.Fatalf("delete: %d %s", status, body)
	}
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/hosts/devbox", "", ""); status != http.StatusNotFound {
		t.Fatalf("lookup after delete: %d", status)
	}
	for _, p := range s.Relay.Stats().Ports {
		if p.HostID == "devbox" {
			t.Fatalf("relay port still reserved: %+v", p)
		}
	}
}

func TestRegistrationValidation(t *testing.T) {
	s := testutil.Start(t)
	valid := `{"id":"devbox","key":"correct horse"}`
	cases := []struct {
		name   string
		body   string
		key    string
		status int
	}{
		{"no server key", valid, "", http.StatusUnauthorized},
		{"wrong server key", valid, "nope", http.StatusUnauthorized},
		{"bad id", `{"id":"two words","key":"correct horse"}`, testutil.ServerKey, http.StatusBadRequest},
		{"short key", `{"id":"devbox","key":"short"}`, testutil.ServerKey, http.StatusBadRequest},
		{"unknown field", `{"id":"devbox","key":"correct horse","extra":1}`, testutil.ServerKey, http.StatusBadRequest},
		{"broken json", `{`, testutil.ServerKey, http.StatusBadRequest},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			status, body := request(t, http.MethodPost, s.URL+"/v1/hosts", tc.body, tc.key)
			if status != tc.status {
				t.Fatalf("status %d, want %d: %s", status, tc.status, body)
			}
		})
	}

	if status, _ := request(t, http.MethodPost, s.URL+"/v1/hosts", valid, testutil.ServerKey); status != http.StatusOK {
		t.Fatalf("initial register: %d", status)
	}
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		`{"id":"devbox","key":"another horse"}`, testutil.ServerKey)
	if status != http.StatusForbidden {
		t.Fatalf("renew with a different key: %d %s", status, body)
	}
}

func TestHealthAndStats(t *testing.T) {
	s := testutil.Start(t)
	if status, _ := request(t, http.MethodGet, s.URL+"/healthz", "", ""); status != http.StatusOK {
		t.Fatalf("health: %d", status)
	}
	if status, _ := request(t, http.MethodGet, s.URL+"/v1/stats", "", ""); status != http.StatusUnauthorized {
		t.Fatalf("stats without key: %d", status)
	}
	status, body := request(t, http.MethodGet, s.URL+"/v1/stats", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("stats: %d %s", status, body)
	}
	var stats struct {
		Hosts int `json:"hosts"`
		Relay struct {
			Ports []struct {
				Port int `json:"port"`
			} `json:"ports"`
		} `json:"relay"`
	}
	if err := json.Unmarshal(body, &stats); err != nil {
		t.Fatal(err)
	}
	if len(stats.Relay.Ports) != 2 {
		t.Fatalf("relay ports in stats: %+v", stats)
	}
}
