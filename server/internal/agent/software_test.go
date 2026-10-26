package agent

import "testing"

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
