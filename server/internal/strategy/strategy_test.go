package strategy

import (
	"encoding/json"
	"errors"
	"testing"
)

func TestSettingsRejectAmbiguousAndInvalidInput(t *testing.T) {
	for _, raw := range []string{
		`null`, `[]`,
		`{"host.keep_awake":{"mode":"locked","value":true},"host.keep_awake":{"mode":"locked","value":false}}`,
		`{"host.keep_awake":{"mode":"locked","value":true,"value":false}}`,
		`{"host.keep_awake":{"mode":"locked","value":true,"extra":1}}`,
		`{"host.keep_awake":{"value":true}}`,
		`{"host.keep_awake":{"mode":"locked","value":null}}`,
		`{"host.keep_awake":{"mode":"suggested","value":true}}`,
		`{"host.keep_awake":{"mode":"locked","value":"true"}}`,
		`{"host.max_viewers":{"mode":"locked","value":0}}`,
		`{"host.max_viewers":{"mode":"locked","value":9}}`,
		`{"host.max_viewers":{"mode":"locked","value":1.5}}`,
		`{"host.max_viewers":{"mode":"locked","value":1e0}}`,
		`{"host.auto_disconnect_minutes":{"mode":"locked","value":-1}}`,
		`{"viewer.tunnel_mode":{"mode":"locked","value":"proxy"}}`,
		`{"host.password":{"mode":"locked","value":"secret"}}`,
	} {
		t.Run(raw, func(t *testing.T) {
			var settings Settings
			err := json.Unmarshal([]byte(raw), &settings)
			if err == nil {
				_, err = Normalize(settings)
			}
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("input accepted or wrong error: %v", err)
			}
		})
	}
}

func TestSettingsCanonicalCopyAndTargetIsolation(t *testing.T) {
	var settings Settings
	if err := json.Unmarshal([]byte(`{"viewer.tunnel_mode":{"value":"relay","mode":"locked"},"host.keep_awake":{"value":false,"mode":"default"}}`), &settings); err != nil {
		t.Fatal(err)
	}
	host, err := ForTarget(settings, Host)
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := Encode(host)
	if err != nil || encoded != `{"host.keep_awake":{"mode":"default","value":false}}` {
		t.Fatalf("host = %s %v", encoded, err)
	}
	host["host.keep_awake"].Value[0] = 'x'
	if string(settings["host.keep_awake"].Value) != "false" {
		t.Fatal("caller mutated source")
	}
	if Digest(Host, "{}") == Digest(Viewer, "{}") {
		t.Fatal("digest not scoped to target")
	}
	if _, err := ForTarget(settings, "other"); !errors.Is(err, ErrInvalid) {
		t.Fatal(err)
	}
	if encoded, err := Encode(nil); err != nil || encoded != "{}" {
		t.Fatalf("empty = %q %v", encoded, err)
	}
	catalog := Catalog()
	for i := range catalog {
		if len(catalog[i].Choices) > 0 {
			catalog[i].Choices[0] = "corrupted"
		}
	}
	if _, err := Normalize(settings); err != nil {
		t.Fatalf("catalog leaked mutable schema: %v", err)
	}
}
