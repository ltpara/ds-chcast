package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestLoadWakeRelayPolicy(t *testing.T) {
	if got, err := loadWakeRelayPolicy(""); err != nil || len(got) != 0 {
		t.Fatalf("disabled policy = %v, %v", got, err)
	}
	path := filepath.Join(t.TempDir(), "wake.json")
	if _, err := loadWakeRelayPolicy(path); err == nil {
		t.Fatal("missing policy file accepted")
	}
	for _, invalid := range []string{`null`, `[]`, `{`, `{"bad id":["proxy"]}`, `{"target":["bad id"]}`, `{"target":"proxy"}`, `{"target":["target"]}`, `{} {}`} {
		if err := os.WriteFile(path, []byte(invalid), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadWakeRelayPolicy(path); err == nil {
			t.Fatalf("invalid policy accepted: %s", invalid)
		}
	}
	if err := os.WriteFile(path, []byte(`{"target":["proxy-a","proxy-b"],"disabled":[]}`), 0600); err != nil {
		t.Fatal(err)
	}
	got, err := loadWakeRelayPolicy(path)
	want := map[string][]string{"target": {"proxy-a", "proxy-b"}, "disabled": {}}
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("policy = %v, %v; want %v", got, err, want)
	}

	// The startup line reports how many targets can actually be woken and how
	// many distinct relays serve them; an empty allowance is not a target.
	targets, relays := wakeRelayPolicyCounts(got)
	if targets != 1 || relays != 2 {
		t.Fatalf("counts = %d targets, %d relays; want 1 and 2", targets, relays)
	}
	if targets, relays := wakeRelayPolicyCounts(nil); targets != 0 || relays != 0 {
		t.Fatalf("disabled policy counts = %d, %d; want 0, 0", targets, relays)
	}
}
