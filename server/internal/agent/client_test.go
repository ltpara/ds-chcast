package agent

import (
	"context"
	"os"
	"runtime"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/netproxy"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestRegisterSignsHostMetadata(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{RequireHostMetadataSig: true})
	priv := identity.PrivateKeyFromSecret("host-secret")
	client := newAPIClient(s.URL, testutil.ServerKey, nil, nil)
	res, err := client.register(context.Background(), "devbox", "", priv, "AB12")
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	if res.ID != "devbox" || res.PublicKey == "" {
		t.Fatalf("registration reply = %+v", res)
	}
	record, ok := s.Registry.Identity("devbox")
	if !ok || !record.MetadataSigned {
		t.Fatalf("identity = %+v (found %v)", record, ok)
	}
	hostname, _ := os.Hostname()
	if record.Hostname != hostname {
		t.Fatalf("hostname = %q, want %q", record.Hostname, hostname)
	}
	if record.Platform != runtime.GOOS+"/"+runtime.GOARCH {
		t.Fatalf("platform = %q", record.Platform)
	}
	if record.Version != Version {
		t.Fatalf("version = %q, want %q", record.Version, Version)
	}
}

func TestAPIClientUsesSocks5Proxy(t *testing.T) {
	s := testutil.StartWith(t, testutil.Options{})
	socks := testutil.StartSocks5(t, "agent", "s3cret")
	proxy, err := netproxy.Parse(socks.URL())
	if err != nil {
		t.Fatalf("parse proxy: %v", err)
	}
	priv := identity.PrivateKeyFromSecret("host-secret")
	client := newAPIClient(s.URL, testutil.ServerKey, nil, proxy)
	if _, err := client.register(context.Background(), "proxied", "", priv, "CD34"); err != nil {
		t.Fatalf("register through proxy: %v", err)
	}
	if len(socks.Targets()) == 0 {
		t.Fatal("proxy recorded no CONNECT; the API request bypassed it")
	}
}

func TestHostRejectsInvalidSocks5(t *testing.T) {
	_, err := StartHost(context.Background(), HostConfig{
		ServerURL: "http://127.0.0.1:1",
		ID:        "devbox",
		Key:       "12345678",
		Socks5:    "http://127.0.0.1:8080",
	})
	if err == nil || !strings.Contains(err.Error(), "--socks5") {
		t.Fatalf("StartHost error = %v, want a --socks5 rejection", err)
	}
}

func TestViewerRejectsInvalidSocks5(t *testing.T) {
	_, err := StartViewer(context.Background(), ViewerConfig{
		ServerURL: "http://127.0.0.1:1",
		HostID:    "devbox",
		Socks5:    "http://127.0.0.1:8080",
	})
	if err == nil || !strings.Contains(err.Error(), "--socks5") {
		t.Fatalf("StartViewer error = %v, want a --socks5 rejection", err)
	}
}
