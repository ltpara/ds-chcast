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

func TestApplyRustDeskAliases(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	punch := fs.String("punch-listen", "0.0.0.0:8444", "")
	relayOnly := fs.Bool("relay-only", false, "")
	testPunch := fs.String("test-punch", "", "")
	rateMax := fs.Int("punch-rate-max", 30, "")
	punchLog := fs.Int("punch-log-max", 100000, "")
	rmem := fs.Int("rmem", 0, "")
	ini, _ := LoadINI(writeFile(t, "hbbs.ini", `
BIND=0.0.0.0
PORT=21116
ALWAYS_USE_RELAY=Y
TEST_HBBS=auto
MAX_PUNCH_REQUESTS=12
RMEM=1048576
RELAY-SERVERS=relay.example.com
`))
	relayServers := fs.String("relay-servers", "", "")
	if err := Apply(fs, ini); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *punch != "0.0.0.0:21116" {
		t.Fatalf("punch-listen = %q", *punch)
	}
	if !*relayOnly {
		t.Fatal("ALWAYS_USE_RELAY was not applied")
	}
	if *testPunch != "auto" {
		t.Fatalf("test-punch = %q", *testPunch)
	}
	if *rateMax != 30 {
		t.Fatalf("punch-rate-max = %d, want the default 30", *rateMax)
	}
	if *punchLog != 12 {
		t.Fatalf("punch-log-max = %d", *punchLog)
	}
	if *rmem != 1048576 {
		t.Fatalf("rmem = %d", *rmem)
	}
	if *relayServers != "relay.example.com" {
		t.Fatalf("relay-servers = %q", *relayServers)
	}
}

func TestPunchListenPortKeepsCurrentHost(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	punch := fs.String("punch-listen", "127.0.0.1:8444", "")
	ini, _ := LoadINI(writeFile(t, "hbbs.ini", "PORT=21116\n"))
	if err := Apply(fs, ini); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *punch != "127.0.0.1:21116" {
		t.Fatalf("punch-listen = %q", *punch)
	}
}

func TestAliasCanonicalAndExplicitWin(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	punch := fs.String("punch-listen", "0.0.0.0:8444", "")
	if err := fs.Parse([]string{"--punch-listen", "127.0.0.1:9001"}); err != nil {
		t.Fatalf("parse: %v", err)
	}
	ini, _ := LoadINI(writeFile(t, "hbbs.ini", "BIND=0.0.0.0\nPORT=21116\n"))
	if err := Apply(fs, ini); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *punch != "127.0.0.1:9001" {
		t.Fatalf("explicit flag was overwritten: %q", *punch)
	}

	fs = flag.NewFlagSet("test", flag.ContinueOnError)
	punch = fs.String("punch-listen", "0.0.0.0:8444", "")
	canonical, _ := LoadINI(writeFile(t, "canonical.ini", "punch-listen=127.0.0.1:9000\nBIND=0.0.0.0\nPORT=21116\n"))
	if err := Apply(fs, canonical); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *punch != "127.0.0.1:9000" {
		t.Fatalf("canonical key lost to an alias: %q", *punch)
	}
}

func TestAliasFollowsLayerPrecedence(t *testing.T) {
	fs := flag.NewFlagSet("test", flag.ContinueOnError)
	punch := fs.String("punch-listen", "0.0.0.0:8444", "")
	ini, _ := LoadINI(writeFile(t, "hbbs.ini", "PORT=21116\n"))
	env, _ := LoadEnvFile(writeFile(t, ".env", "PUNCH_LISTEN=127.0.0.1:9999\n"))
	if err := Apply(fs, ini, env); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if *punch != "0.0.0.0:21116" {
		t.Fatalf("higher-priority alias lost: %q", *punch)
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
