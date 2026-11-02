//go:build windows

package setup

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDefaultRoot(t *testing.T) {
	root := DefaultRoot(false)
	if !strings.HasSuffix(strings.ToLower(root), `programs\chast`) {
		t.Fatalf("per-user root = %q", root)
	}
	if machine := DefaultRoot(true); !strings.HasSuffix(strings.ToLower(machine), `chast`) ||
		strings.Contains(strings.ToLower(machine), "programs") {
		t.Fatalf("machine root = %q", machine)
	}
}

func TestCheckElevation(t *testing.T) {
	if err := checkElevation(Options{Service: true}); err == nil {
		t.Fatal("--service without --machine was accepted")
	}
	err := checkElevation(Options{Machine: true})
	if (err == nil) != IsElevated() {
		t.Fatalf("machine check = %v, elevated = %v", err, IsElevated())
	}
	if err := checkElevation(Options{}); err != nil {
		t.Fatalf("per-user check: %v", err)
	}
}

func TestProtocolRegistrationRoundtrip(t *testing.T) {
	dir := t.TempDir()
	root := filepath.Join(dir, "chast")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, viewerExe), []byte("viewer"), 0o755); err != nil {
		t.Fatal(err)
	}
	opts := Options{
		Root:           root,
		AppID:          "chast-accept-unit",
		Protocol:       true,
		ProtocolScheme: "chast-accept-unit",
	}
	key, err := protocolKey(opts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		run("reg.exe", "delete", key, "/f")
	})
	if err := platformInstall(opts, 1); err != nil {
		t.Fatalf("platformInstall: %v", err)
	}
	query := func(args ...string) string {
		out, err := run("reg.exe", append([]string{"query", key}, args...)...)
		if err != nil {
			t.Fatalf("query %v: %v (%s)", args, err, out)
		}
		return out
	}
	if out := query("/ve"); !strings.Contains(out, "URL:chast Protocol") {
		t.Fatalf("default value: %s", out)
	}
	if out := query("/v", "URL Protocol"); !strings.Contains(out, "URL Protocol") {
		t.Fatalf("URL Protocol: %s", out)
	}
	if out := query("/v", "DefaultIcon"); !strings.Contains(out, viewerExe) || !strings.Contains(out, ",0") {
		t.Fatalf("DefaultIcon: %s", out)
	}
	command, err := run("reg.exe", "query", key+`\shell\open\command`, "/ve")
	if err != nil {
		t.Fatalf("query open command: %v (%s)", err, command)
	}
	if !strings.Contains(command, viewerExe) || !strings.Contains(command, `"%1"`) {
		t.Fatalf("open command: %s", command)
	}
	if out, err := run("reg.exe", "query", key, "/v", `shell\open\command`); err == nil {
		t.Fatalf("the open command was written as a value of the root key: %s", out)
	}
	if err := platformUninstall(opts); err != nil {
		t.Fatalf("platformUninstall: %v", err)
	}
	if _, err := run("reg.exe", "query", key); err == nil {
		t.Fatal("the URL scheme registration survived uninstall")
	}
}

func TestProtocolCommandQuotesPath(t *testing.T) {
	want := `"C:\Program Files\chast\chast_viewer.exe" "%1"`
	if got := protocolCommand(`C:\Program Files\chast`); got != want {
		t.Fatalf("protocolCommand = %q, want %q", got, want)
	}
}

func TestProtocolKey(t *testing.T) {
	key, err := protocolKey(Options{})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(key, `\Software\Classes\chast`) || !strings.HasPrefix(key, "HKCU") {
		t.Fatalf("per-user key = %q", key)
	}
	key, err = protocolKey(Options{Machine: true, ProtocolScheme: "chast-test"})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(key, "HKLM") || !strings.HasSuffix(key, `\Software\Classes\chast-test`) {
		t.Fatalf("machine key = %q", key)
	}
	for _, scheme := range []string{"1chast", "chast scheme", `chast\x`} {
		if _, err := protocolKey(Options{ProtocolScheme: scheme}); err == nil {
			t.Fatalf("scheme %q was accepted", scheme)
		}
	}
}

func TestUninstallEntryAndShortcutRoundtrip(t *testing.T) {
	dir := t.TempDir()
	root := filepath.Join(dir, "chast")
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, viewerExe), []byte("viewer"), 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("APPDATA", filepath.Join(dir, "appdata"))
	opts := Options{Root: root, AppID: "chast-accept-unit", Version: "9.9.9", Shortcut: true, Register: true}
	key, err := uninstallKey(opts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		run("reg.exe", "delete", key, "/f")
		os.Remove(shortcutPath(opts))
	})
	if err := platformInstall(opts, 2048); err != nil {
		t.Fatalf("platformInstall: %v", err)
	}
	query := func(value string) string {
		out, err := run("reg.exe", "query", key, "/v", value)
		if err != nil {
			t.Fatalf("query %s: %v (%s)", value, err, out)
		}
		return out
	}
	if out := query("DisplayVersion"); !strings.Contains(out, "9.9.9") {
		t.Fatalf("DisplayVersion: %s", out)
	}
	if out := query("UninstallString"); !strings.Contains(out, "--uninstall") || !strings.Contains(out, root) {
		t.Fatalf("UninstallString: %s", out)
	}
	if out := query("QuietUninstallString"); !strings.Contains(out, "--quiet") {
		t.Fatalf("QuietUninstallString: %s", out)
	}
	if out := query("EstimatedSize"); !strings.Contains(out, "0x2") {
		t.Fatalf("EstimatedSize: %s", out)
	}
	if out := query("DisplayIcon"); !strings.Contains(out, viewerExe) {
		t.Fatalf("DisplayIcon: %s", out)
	}
	shortcut := shortcutPath(opts)
	if _, err := os.Stat(shortcut); err != nil {
		t.Fatalf("shortcut missing: %v", err)
	}
	if err := platformUninstall(opts); err != nil {
		t.Fatalf("platformUninstall: %v", err)
	}
	if _, err := run("reg.exe", "query", key); err == nil {
		t.Fatal("the uninstall entry survived")
	}
	if _, err := os.Stat(shortcut); !os.IsNotExist(err) {
		t.Fatalf("shortcut survived: %v", err)
	}
}
