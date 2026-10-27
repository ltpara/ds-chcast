package geoip

import (
	"os"
	"path/filepath"
	"testing"
)

func writeTable(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "geoip.txt")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLookupUsesLongestPrefix(t *testing.T) {
	path := writeTable(t, "\ufeff# test table\n"+
		"0.0.0.0/0\tInternet\n"+
		"203.0.113.0/24 Tokyo, JP\n"+
		"203.0.113.7/32\tLab rack 3\n"+
		"2001:db8::/32\tDocumentation network\n"+
		"\n")
	locator, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := locator.Len(); got != 4 {
		t.Fatalf("entries = %d, want 4", got)
	}
	cases := map[string]string{
		"203.0.113.7":        "Lab rack 3",
		"203.0.113.9":        "Tokyo, JP",
		"198.51.100.4":       "Internet",
		"2001:db8::42":       "Documentation network",
		"...":                "",
		"::ffff:203.0.113.9": "Tokyo, JP",
	}
	for addr, want := range cases {
		if got := locator.Lookup(addr); got != want {
			t.Fatalf("Lookup(%q) = %q, want %q", addr, got, want)
		}
	}
}

func TestLoadRejectsMalformedLines(t *testing.T) {
	for name, body := range map[string]string{
		"missing label": "203.0.113.0/24\n",
		"bad prefix":    "203.0.113.0/99\tSomewhere\n",
		"not a prefix":  "example.com\tSomewhere\n",
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := Load(writeTable(t, body)); err == nil {
				t.Fatal("Load accepted a malformed table")
			}
		})
	}
}

func TestLoadMissingFile(t *testing.T) {
	if _, err := Load(filepath.Join(t.TempDir(), "absent.txt")); err == nil {
		t.Fatal("Load accepted a missing file")
	}
}

func TestNilLocator(t *testing.T) {
	var locator *Locator
	if got := locator.Lookup("203.0.113.7"); got != "" {
		t.Fatalf("nil lookup = %q", got)
	}
	if got := locator.Len(); got != 0 {
		t.Fatalf("nil len = %d", got)
	}
}
