package agent

import (
	"context"
	"crypto/x509"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestViewerStartErrorClassification(t *testing.T) {
	for _, tc := range []struct {
		name, body string
		status     int
		retryable  bool
	}{
		{"offline", `{"online":false}`, 200, true},
		{"registering-again", `{"error":"missing"}`, 404, true},
		{"request-timeout", `{"error":"timeout"}`, 408, true},
		{"rate-limited", `{"error":"slow down"}`, 429, true},
		{"server-down", "unavailable", 503, true},
		{"unauthorized", `{"error":"bad credential"}`, 401, false},
		{"forbidden", `{"error":"policy"}`, 403, false},
		{"invalid-reply", `{broken`, 200, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(tc.status)
				_, _ = w.Write([]byte(tc.body))
			}))
			defer s.Close()
			v, err := StartViewer(context.Background(), ViewerConfig{
				ServerURL: s.URL, HostID: "devbox", ListenAddr: "127.0.0.1:0", Logger: quietLogger(),
			})
			if v != nil {
				v.Close()
				t.Fatal("unexpected viewer")
			}
			if err == nil || IsRetryableStartError(err) != tc.retryable {
				t.Fatalf("start error = %v, retryable = %v", err, IsRetryableStartError(err))
			}
		})
	}
	for _, tc := range []struct {
		err  error
		want bool
	}{
		{context.DeadlineExceeded, true},
		{context.Canceled, false},
		{&net.OpError{Op: "dial", Err: errors.New("refused")}, true},
		{x509.UnknownAuthorityError{}, false},
		{errors.New("identity signature mismatch"), false},
	} {
		if got := retryableLookupError(tc.err); got != tc.want {
			t.Errorf("%v: retryable = %v, want %v", tc.err, got, tc.want)
		}
	}
}
