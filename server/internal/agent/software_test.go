package agent

import (
	"context"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func TestSoftwareUpdateAvailability(t *testing.T) {
	previous := Version
	Version = "1.0.0"
	t.Cleanup(func() { Version = previous })

	cases := []struct {
		name  string
		offer *softwareInfo
		want  bool
	}{
		{"nil offer", nil, false},
		{"same version", &softwareInfo{Version: "1.0.0", URL: "https://example.com/a.exe"}, false},
		{"new version", &softwareInfo{Version: "1.2.0", URL: "https://example.com/a.exe"}, true},
		{"missing url", &softwareInfo{Version: "1.2.0"}, false},
		{"missing version", &softwareInfo{URL: "https://example.com/a.exe"}, false},
	}
	for _, tc := range cases {
		if got := tc.offer.updateAvailable(); got != tc.want {
			t.Fatalf("%s: updateAvailable = %t, want %t", tc.name, got, tc.want)
		}
	}
}

func TestRegistrationRemembersSoftwareOffer(t *testing.T) {
	previous := Version
	Version = "1.0.0"
	t.Cleanup(func() { Version = previous })

	h, _ := testHostAgent(t)
	offer := &softwareInfo{Version: "1.2.0", URL: "https://example.com/chast-1.2.0.exe"}
	h.applyRegistration(registerResult{Software: offer})
	h.mu.Lock()
	seen := h.softwareSeen
	h.mu.Unlock()
	if seen != "1.2.0" {
		t.Fatalf("softwareSeen = %q", seen)
	}

	h.applyRegistration(registerResult{})
	h.mu.Lock()
	seen = h.softwareSeen
	h.mu.Unlock()
	if seen != "" {
		t.Fatalf("softwareSeen after the server stopped advertising = %q", seen)
	}
}

func TestStartHostRejectsAutoUpdateWithoutPin(t *testing.T) {
	_, err := StartHost(context.Background(), HostConfig{
		ServerURL:  "http://127.0.0.1:1",
		ID:         "devbox",
		Key:        "host-secret-key",
		AutoUpdate: true,
	})
	if err == nil || !strings.Contains(err.Error(), "--auto-update needs --server-pubkey") {
		t.Fatalf("StartHost with --auto-update and no pin = %v", err)
	}
}

func TestStartUpdateIgnoredWhenDisabled(t *testing.T) {
	h, _ := testHostAgent(t)
	h.startUpdate(softwareInfo{Version: "9.9.9", URL: "https://example.com/a.exe"})
	h.mu.Lock()
	tried := len(h.updatesTried)
	h.mu.Unlock()
	if tried != 0 {
		t.Fatalf("auto-update off still recorded %d attempts", tried)
	}
}

func TestStartUpdateStagesOncePerVersion(t *testing.T) {
	srv, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	h, _ := testHostAgent(t)
	h.ctx = context.Background()
	h.cfg.AutoUpdate = true
	h.cfg.ServerPublicKey = srv.PublicKeyText()

	offer := softwareInfo{Version: "9.9.9", URL: "https://example.com/chast-9.9.9.exe"}
	h.startUpdate(offer)
	h.startUpdate(offer)

	h.mu.Lock()
	tried := len(h.updatesTried)
	h.mu.Unlock()
	if tried != 1 {
		t.Fatalf("updatesTried holds %d versions, want 1", tried)
	}
}
