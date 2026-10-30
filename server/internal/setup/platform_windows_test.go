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
