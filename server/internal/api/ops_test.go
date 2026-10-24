package api_test

import (
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func doRequest(t *testing.T, method, url, body string, headers map[string]string) (*http.Response, string) {
	t.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatal(err)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	for key, value := range headers {
		req.Header.Set(key, value)
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
	return resp, string(data)
}

func TestRateLimitRejectsBurst(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RateLimit: 1, RateBurst: 1})
	resp, body := doRequest(t, http.MethodGet, s.URL+"/healthz", "", nil)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("first request: %d %s", resp.StatusCode, body)
	}
	resp, body = doRequest(t, http.MethodGet, s.URL+"/healthz", "", nil)
	if resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("second request = %d %s, want 429", resp.StatusCode, body)
	}
	if got := resp.Header.Get("Retry-After"); got != "1" {
		t.Fatalf("Retry-After = %q", got)
	}
	if !strings.Contains(body, "too many requests") {
		t.Fatalf("rate limit body = %s", body)
	}
}

func TestRateLimitUsesLastProxyHop(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{
		RateLimit:   1,
		RateBurst:   1,
		ProxyHeader: "X-Forwarded-For",
	})
	client := map[string]string{"X-Forwarded-For": "198.51.100.1, 203.0.113.9"}
	resp, body := doRequest(t, http.MethodGet, s.URL+"/healthz", "", client)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("first proxy request: %d %s", resp.StatusCode, body)
	}
	client["X-Forwarded-For"] = "198.51.100.2, 203.0.113.9"
	resp, body = doRequest(t, http.MethodGet, s.URL+"/healthz", "", client)
	if resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("same last hop = %d %s, want 429", resp.StatusCode, body)
	}
	client["X-Forwarded-For"] = "203.0.113.10"
	resp, body = doRequest(t, http.MethodGet, s.URL+"/healthz", "", client)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("different client = %d %s, want 200", resp.StatusCode, body)
	}
}

func TestMetricsExposeStats(t *testing.T) {
	s := testutil.Start(t)
	now := time.Now().Unix()
	status, body := request(t, http.MethodPost, s.URL+"/v1/hosts",
		registerBody(t, "metrics-host", "correct horse", "AB12", now), testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("register: %d %s", status, body)
	}

	status, raw := request(t, http.MethodGet, s.URL+"/metrics", "", testutil.ServerKey)
	if status != http.StatusOK {
		t.Fatalf("metrics: %d %s", status, raw)
	}
	text := string(raw)
	for _, want := range []string{
		`chast_server_info{version="chast-server/1",tls="false"} 1`,
		"chast_hosts 1",
		"chast_identities 1",
		"chast_relay_ports_assigned 1",
		`chast_carrier_bytes_total{direction="to_host"} 0`,
		`chast_carrier_hosts 0`,
		"chast_http_rate_limited_total 0",
		`chast_http_requests_total{endpoint="POST /v1/hosts",code="200"} 1`,
		`chast_http_request_duration_seconds_count{endpoint="POST /v1/hosts"}`,
	} {
		if !strings.Contains(text, want) {
			t.Fatalf("metrics missing %q in:\n%s", want, text)
		}
	}

	status, _ = request(t, http.MethodGet, s.URL+"/metrics", "", "")
	if status != http.StatusUnauthorized {
		t.Fatalf("unauthenticated metrics = %d, want 401", status)
	}
}

func TestServerInfoReportsTLS(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{TLS: true})
	status, body := request(t, http.MethodGet, s.URL+"/v1/server", "", "")
	if status != http.StatusOK {
		t.Fatalf("server info: %d %s", status, body)
	}
	if !strings.Contains(string(body), `"tls":true`) {
		t.Fatalf("server info does not report TLS: %s", body)
	}
}
