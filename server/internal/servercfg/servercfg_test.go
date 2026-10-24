package servercfg

import (
	"flag"
	"os"
	"path/filepath"
	"testing"
)

func writeFile(t *testing.T, name, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatalf("write %s: %v", name, err)
	}
	return path
}

func TestLoadINIIgnoresSectionsAndComments(t *testing.T) {
	path := writeFile(t, "hbbs.ini", `
; comment
[server]
relay-ports = 41000-41010
RELAY_TOTAL_BANDWIDTH = 2048
# trailing comment
api-rate = "75"
`)
	layer, err := LoadINI(path)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	for key, want := range map[string]string{
		"relay-ports":           "41000-41010",
		"relay-total-bandwidth": "2048",
		"api-rate":              "75",
	} {
		got, ok := layer.Lookup(key)
		if !ok || got != want {
			t.Fatalf("%s = %q (ok=%t), want %q", key, got, ok, want)
		}
	}
}

func TestLoadEnvFileStripsExportAndPrefix(t *testing.T) {
	path := writeFile(t, ".env", "export CHAST_HOST_TTL=90s\nALWAYS_USE_RELAY=Y\n")
	layer, err := LoadEnvFile(path)
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if got, _ := layer.Lookup("host-ttl"); got != "90s" {
		t.Fatalf("host-ttl = %q", got)
	}
	if got, _ := layer.Lookup("always-use-relay"); got != "Y" {
		t.Fatalf("always-use-relay = %q", got)
	}
}

func TestApplyPrecedence(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	port := fs.String("port", "8443", "")
	ttl := fs.Duration("host-ttl", 0, "")
	mode := fs.Bool("relay-only", false, "")
	if err := fs.Parse([]string{"--port", "9000"}); err != nil {
		t.Fatalf("parse: %v", err)
	}

	ini, _ := LoadINI(writeFile(t, "hbbs.ini", "port=7000\nhost-ttl=90s\nrelay-only=Y\n"))
	env, _ := LoadEnvFile(writeFile(t, ".env", "PORT=6000\nHOST_TTL=30s\n"))

	if err := Apply(fs, ini, env); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *port != "9000" {
		t.Fatalf("explicit flag was overwritten: %q", *port)
	}
	if ttl.String() != "1m30s" {
		t.Fatalf("host-ttl = %s", ttl.String())
	}
	if !*mode {
		t.Fatal("relay-only=Y was not applied")
	}
}

func TestApplyFallsBackToEnvFile(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	port := fs.String("port", "8443", "")
	env, _ := LoadEnvFile(writeFile(t, ".env", "PORT=6000\n"))
	if err := Apply(fs, env); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *port != "6000" {
		t.Fatalf("port = %q", *port)
	}
}

func TestApplyReportsBadValue(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	fs.Int("relay-ports", 0, "")
	ini, _ := LoadINI(writeFile(t, "bad.ini", "relay-ports=not-a-number\n"))
	if err := Apply(fs, ini); err == nil {
		t.Fatal("expected a conversion error")
	}
}

func TestFromEnvPrefixedKeyWins(t *testing.T) {
	t.Setenv("PORT", "5000")
	t.Setenv("CHAST_PORT", "5001")
	layer := FromEnv()
	if got, _ := layer.Lookup("port"); got != "5001" {
		t.Fatalf("port = %q", got)
	}
}
