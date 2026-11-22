package api

import (
	"crypto/tls"
	"net/http/httptest"
	"testing"
)

func TestSecureHostRecordingTransport(t *testing.T) {
	tests := []struct {
		name       string
		remoteAddr string
		useTLS     bool
		want       bool
	}{
		{name: "ipv4 loopback HTTP", remoteAddr: "127.0.0.1:1234", want: true},
		{name: "ipv6 loopback HTTP", remoteAddr: "[::1]:1234", want: true},
		{name: "remote HTTP", remoteAddr: "203.0.113.4:1234", want: false},
		{name: "TLS remote", remoteAddr: "203.0.113.4:1234", useTLS: true, want: true},
		{name: "invalid remote address", remoteAddr: "not-an-address", want: false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			r := httptest.NewRequest("POST", "http://example.test/upload", nil)
			r.RemoteAddr = test.remoteAddr
			if test.useTLS {
				r.TLS = &tls.ConnectionState{}
			}
			if got := secureHostRecordingTransport(r); got != test.want {
				t.Fatalf("secureHostRecordingTransport() = %v, want %v", got, test.want)
			}
		})
	}
}
