package configpush

import (
	"reflect"
	"strings"
	"testing"
)

func TestNormalize(t *testing.T) {
	got, err := Normalize([]string{
		" https://one.example:8443/ ",
		"http://two.example",
		"https://one.example:8443",
		"",
	})
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	want := []string{"https://one.example:8443", "http://two.example"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("normalize = %v, want %v", got, want)
	}

	for _, bad := range []string{
		"one.example",
		"ftp://one.example",
		"https://",
		"https://one.example/path?x=1",
		"https://one.example/#frag",
	} {
		if _, err := Normalize([]string{bad}); err == nil {
			t.Fatalf("normalize accepted %q", bad)
		}
	}

	tooMany := make([]string, MaxServers+1)
	for i := range tooMany {
		tooMany[i] = "https://s" + strings.Repeat("x", i+1) + ".example"
	}
	if _, err := Normalize(tooMany); err == nil {
		t.Fatal("normalize accepted more than MaxServers entries")
	}
}

func TestNewAssignsFirstSerial(t *testing.T) {
	cfg, err := New(0, []string{"https://one.example"})
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	if serial, servers := cfg.Snapshot(); serial != 1 || len(servers) != 1 {
		t.Fatalf("new = %d/%v, want serial 1", serial, servers)
	}

	cfg, err = New(7, []string{"https://one.example"})
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	if serial, _ := cfg.Snapshot(); serial != 7 {
		t.Fatalf("serial = %d, want 7", serial)
	}

	cfg, err = New(0, nil)
	if err != nil {
		t.Fatalf("new empty: %v", err)
	}
	if serial, servers := cfg.Snapshot(); serial != 0 || len(servers) != 0 {
		t.Fatalf("empty new = %d/%v, want 0", serial, servers)
	}

	if _, err := New(-1, nil); err == nil {
		t.Fatal("negative serial accepted")
	}
	if _, err := New(0, []string{"not-a-url"}); err == nil {
		t.Fatal("invalid server accepted")
	}
}

func TestUpdateRequiresForwardSerial(t *testing.T) {
	cfg, err := New(3, []string{"https://one.example"})
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	if err := cfg.Update(3, []string{"https://two.example"}); err == nil {
		t.Fatal("equal serial accepted")
	}
	if err := cfg.Update(2, []string{"https://two.example"}); err == nil {
		t.Fatal("older serial accepted")
	}
	if err := cfg.Update(4, []string{"bad"}); err == nil {
		t.Fatal("invalid list accepted")
	}
	if serial, servers := cfg.Snapshot(); serial != 3 || servers[0] != "https://one.example" {
		t.Fatalf("failed update changed state: %d/%v", serial, servers)
	}
	if err := cfg.Update(4, []string{"https://two.example"}); err != nil {
		t.Fatalf("update: %v", err)
	}
	serial, servers := cfg.Snapshot()
	if serial != 4 || len(servers) != 1 || servers[0] != "https://two.example" {
		t.Fatalf("update = %d/%v", serial, servers)
	}
}

func TestSetIncrementsAndClones(t *testing.T) {
	cfg, err := New(0, nil)
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	serial, err := cfg.Set([]string{"https://one.example"})
	if err != nil {
		t.Fatalf("set: %v", err)
	}
	if serial != 1 {
		t.Fatalf("serial = %d, want 1", serial)
	}
	if serial, err = cfg.Set(nil); err != nil || serial != 2 {
		t.Fatalf("clear = %d/%v, want serial 2", serial, err)
	}
	if _, servers := cfg.Snapshot(); len(servers) != 0 {
		t.Fatalf("clear left %v", servers)
	}
	if _, err := cfg.Set([]string{"relative"}); err == nil {
		t.Fatal("invalid list accepted")
	}
	if cfg.Serial() != 2 {
		t.Fatalf("failed set moved serial to %d", cfg.Serial())
	}

	if _, err := cfg.Set([]string{"https://one.example", "https://two.example"}); err != nil {
		t.Fatalf("set: %v", err)
	}
	_, servers := cfg.Snapshot()
	servers[0] = "https://mutated.example"
	if _, again := cfg.Snapshot(); again[0] != "https://one.example" {
		t.Fatalf("snapshot shares its slice: %v", again)
	}
}

func TestNilConfig(t *testing.T) {
	var cfg *Config
	if serial, servers := cfg.Snapshot(); serial != 0 || servers != nil {
		t.Fatalf("nil snapshot = %d/%v", serial, servers)
	}
	if cfg.Serial() != 0 {
		t.Fatal("nil serial")
	}
}
