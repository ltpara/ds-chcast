package api_test

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

// These benchmarks measure the control plane an agent and a viewer actually
// drive: register stays the same HTTP route the tunnel uses, lookups reuse one
// keep-alive connection, and the batch case shares one request across ids.

func benchClient(b *testing.B) *http.Client {
	b.Helper()
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.MaxIdleConnsPerHost = 4
	b.Cleanup(transport.CloseIdleConnections)
	return &http.Client{Transport: transport}
}

func benchDo(b *testing.B, client *http.Client, method, url, body, key string) []byte {
	b.Helper()
	var reader io.Reader
	if body != "" {
		reader = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		b.Fatal(err)
	}
	if key != "" {
		req.Header.Set("Authorization", "Bearer "+key)
	}
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := client.Do(req)
	if err != nil {
		b.Fatal(err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		b.Fatal(err)
	}
	if resp.StatusCode != http.StatusOK {
		b.Fatalf("%s %s = %d: %s", method, url, resp.StatusCode, data)
	}
	return data
}

func benchRegisterBody(b *testing.B, id, secret, fingerprint string) string {
	b.Helper()
	priv, pub := hostKey(secret)
	ts := time.Now().Unix()
	body, err := json.Marshal(map[string]any{
		"id":          id,
		"public_key":  pub,
		"fingerprint": fingerprint,
		"timestamp":   ts,
		"signature":   identity.Sign(priv, identity.RegisterMessage(id, fingerprint, ts)),
	})
	if err != nil {
		b.Fatal(err)
	}
	return string(body)
}

// BenchmarkRegister measures a renewal: the host is already known, its relay
// port stays leased, and the request still verifies the signed registration.
func BenchmarkRegister(b *testing.B) {
	s := testutil.Start(b)
	client := benchClient(b)
	const id = "bench-host"
	body := benchRegisterBody(b, id, "bench secret", "AB12")
	url := s.URL + "/v1/hosts"
	benchDo(b, client, http.MethodPost, url, body, testutil.ServerKey)

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		benchDo(b, client, http.MethodPost, url, body, testutil.ServerKey)
	}
}

// BenchmarkLookup measures one online lookup, the request a viewer issues
// immediately before punching.
func BenchmarkLookup(b *testing.B) {
	s := testutil.Start(b)
	client := benchClient(b)
	const id = "bench-host"
	body := benchRegisterBody(b, id, "bench secret", "AB12")
	benchDo(b, client, http.MethodPost, s.URL+"/v1/hosts", body, testutil.ServerKey)
	if err := s.Registry.Touch(id, "198.51.100.9:5000", []string{"192.168.1.9:5000"}, "port-restricted"); err != nil {
		b.Fatal(err)
	}
	url := s.URL + "/v1/hosts/" + id

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		benchDo(b, client, http.MethodGet, url, "", testutil.ServerKey)
	}
}

// BenchmarkBatchLookup measures a 50-device address-book refresh, the largest
// realistic batch under maxBatchIDs.
func BenchmarkBatchLookup(b *testing.B) {
	s := testutil.Start(b)
	client := benchClient(b)
	ids := make([]string, 0, 50)
	for i := 0; i < 50; i++ {
		id := fmt.Sprintf("bench-%02d", i)
		body := benchRegisterBody(b, id, "bench secret "+id, "AB12")
		benchDo(b, client, http.MethodPost, s.URL+"/v1/hosts", body, testutil.ServerKey)
		if err := s.Registry.Touch(id, "198.51.100.9:5000", nil, "port-restricted"); err != nil {
			b.Fatal(err)
		}
		ids = append(ids, id)
	}
	url := s.URL + "/v1/hosts?ids=" + strings.Join(ids, ",")

	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		benchDo(b, client, http.MethodGet, url, "", testutil.ServerKey)
	}
}

// BenchmarkRegisterParallel measures registrations arriving from separate
// agents at once; the registry serializes them, so the result is the renewal
// capacity of the process rather than of one connection.
func BenchmarkRegisterParallel(b *testing.B) {
	s := testutil.Start(b)
	client := benchClient(b)
	body := []byte(benchRegisterBody(b, "bench-par", "bench secret", "AB12"))
	url := s.URL + "/v1/hosts"
	benchDo(b, client, http.MethodPost, url, string(body), testutil.ServerKey)

	b.ReportAllocs()
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			benchDo(b, client, http.MethodPost, url, string(body), testutil.ServerKey)
		}
	})
}

// BenchmarkLookupParallel measures viewers looking up hosts at once. The
// registry read lock is shared, so this value is close to the serial cost.
func BenchmarkLookupParallel(b *testing.B) {
	s := testutil.Start(b)
	client := benchClient(b)
	const id = "bench-par"
	body := benchRegisterBody(b, id, "bench secret", "AB12")
	benchDo(b, client, http.MethodPost, s.URL+"/v1/hosts", body, testutil.ServerKey)
	if err := s.Registry.Touch(id, "198.51.100.9:5000", nil, "port-restricted"); err != nil {
		b.Fatal(err)
	}
	url := s.URL + "/v1/hosts/" + id

	b.ReportAllocs()
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		for pb.Next() {
			benchDo(b, client, http.MethodGet, url, "", testutil.ServerKey)
		}
	})
}
