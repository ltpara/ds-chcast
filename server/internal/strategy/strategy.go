// Package strategy defines the versioned, non-secret client configuration
// contract. It does not grant session permissions or replace access policies.
package strategy

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"sort"
)

const (
	Host     = "host"
	Viewer   = "viewer"
	Locked   = "locked"
	Default  = "default"
	MaxBytes = 16 << 10
	// JSON numbers must remain exact in the web console and Dart clients.
	MaxRevision int64 = 1<<53 - 1
)

var ErrInvalid = errors.New("invalid client strategy")

// Definition is also the console's schema. Apply describes when a consumer
// may acknowledge a setting; receiving a document is not applying it.
type Definition struct {
	Key     string   `json:"key"`
	Target  string   `json:"target"`
	Type    string   `json:"type"`
	Apply   string   `json:"apply"`
	Min     int64    `json:"min,omitempty"`
	Max     int64    `json:"max,omitempty"`
	Choices []string `json:"choices,omitempty"`
}

var definitions = []Definition{
	{Key: "host.lock_after_session_end", Target: Host, Type: "boolean", Apply: "next_session"},
	{Key: "host.keep_awake", Target: Host, Type: "boolean", Apply: "restart"},
	{Key: "host.auto_disconnect_minutes", Target: Host, Type: "integer", Apply: "next_session", Max: 10080},
	{Key: "host.max_viewers", Target: Host, Type: "integer", Apply: "restart", Min: 1, Max: 8},
	{Key: "host.carrier", Target: Host, Type: "boolean", Apply: "restart"},
	{Key: "host.lan_discovery", Target: Host, Type: "boolean", Apply: "restart"},
	{Key: "viewer.tunnel_mode", Target: Viewer, Type: "string", Apply: "next_session", Choices: []string{"auto", "direct", "relay"}},
	{Key: "viewer.use_websocket", Target: Viewer, Type: "boolean", Apply: "next_session"},
	{Key: "viewer.upload_recordings", Target: Viewer, Type: "boolean", Apply: "next_session"},
	{Key: "viewer.sync_initial_clipboard", Target: Viewer, Type: "boolean", Apply: "next_session"},
	{Key: "viewer.view_fit", Target: Viewer, Type: "integer", Apply: "next_session", Max: 1},
	{Key: "viewer.default_view_only", Target: Viewer, Type: "boolean", Apply: "next_session"},
}

func Catalog() []Definition {
	out := append([]Definition(nil), definitions...)
	for i := range out {
		out[i].Choices = append([]string(nil), out[i].Choices...)
	}
	return out
}

func ValidTarget(target string) bool { return target == Host || target == Viewer }

type Setting struct {
	Mode  string          `json:"mode"`
	Value json.RawMessage `json:"value"`
}

// Settings is a complete overlay. Missing keys mean inherit the local value,
// default keys apply only without an explicit local preference, and locked
// keys override preferences. Removing an overlay restores the local value.
type Settings map[string]Setting

// UnmarshalJSON rejects ambiguous duplicate keys and unexpected setting fields.
// Silent last-wins decoding is inappropriate for managed configuration.
func (s *Settings) UnmarshalJSON(data []byte) error {
	if len(data) > MaxBytes {
		return fmt.Errorf("%w: exceeds %d bytes", ErrInvalid, MaxBytes)
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	token, err := dec.Token()
	if err != nil || token != json.Delim('{') {
		return fmt.Errorf("%w: settings must be an object", ErrInvalid)
	}
	out := Settings{}
	for dec.More() {
		token, err = dec.Token()
		if err != nil {
			return fmt.Errorf("%w: %v", ErrInvalid, err)
		}
		key, ok := token.(string)
		if !ok {
			return fmt.Errorf("%w: key must be a string", ErrInvalid)
		}
		if _, exists := out[key]; exists {
			return fmt.Errorf("%w: duplicate key %s", ErrInvalid, key)
		}
		var raw json.RawMessage
		if err := dec.Decode(&raw); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalid, err)
		}
		setting, err := decodeSetting(raw)
		if err != nil {
			return fmt.Errorf("%w: %s: %v", ErrInvalid, key, err)
		}
		out[key] = setting
	}
	if _, err := dec.Token(); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalid, err)
	}
	var extra any
	if err := dec.Decode(&extra); err != io.EOF {
		return fmt.Errorf("%w: trailing JSON", ErrInvalid)
	}
	*s = out
	return nil
}

func decodeSetting(raw []byte) (Setting, error) {
	dec := json.NewDecoder(bytes.NewReader(raw))
	token, err := dec.Token()
	if err != nil || token != json.Delim('{') {
		return Setting{}, errors.New("setting must be an object")
	}
	var out Setting
	seen := map[string]bool{}
	for dec.More() {
		token, err = dec.Token()
		if err != nil {
			return Setting{}, err
		}
		key, ok := token.(string)
		if !ok || (key != "mode" && key != "value") || seen[key] {
			return Setting{}, errors.New("unknown or duplicate setting field")
		}
		seen[key] = true
		if key == "mode" {
			err = dec.Decode(&out.Mode)
		} else {
			err = dec.Decode(&out.Value)
		}
		if err != nil {
			return Setting{}, err
		}
	}
	if _, err := dec.Token(); err != nil {
		return Setting{}, err
	}
	if !seen["mode"] || !seen["value"] {
		return Setting{}, errors.New("mode and value are required")
	}
	return out, nil
}

// Normalize validates and copies settings. Unknown keys are rejected by the
// authoring server; an older consumer must instead report unsupported keys
// without claiming the complete document was applied.
func Normalize(settings Settings) (Settings, error) {
	if len(settings) > len(definitions) {
		return nil, fmt.Errorf("%w: too many settings", ErrInvalid)
	}
	byKey := make(map[string]Definition, len(definitions))
	for _, def := range definitions {
		byKey[def.Key] = def
	}
	out := Settings{}
	keys := make([]string, 0, len(settings))
	for key := range settings {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		setting := settings[key]
		def, ok := byKey[key]
		if !ok {
			return nil, fmt.Errorf("%w: unknown key %s", ErrInvalid, key)
		}
		if setting.Mode != Locked && setting.Mode != Default {
			return nil, fmt.Errorf("%w: %s mode must be locked or default", ErrInvalid, key)
		}
		if len(setting.Value) == 0 || len(setting.Value) > MaxBytes || bytes.Equal(bytes.TrimSpace(setting.Value), []byte("null")) {
			return nil, fmt.Errorf("%w: %s requires a non-null value", ErrInvalid, key)
		}
		var value any
		switch def.Type {
		case "boolean":
			var v bool
			if err := json.Unmarshal(setting.Value, &v); err != nil {
				return nil, fmt.Errorf("%w: %s requires a boolean", ErrInvalid, key)
			}
			value = v
		case "integer":
			var v int64
			if err := json.Unmarshal(setting.Value, &v); err != nil || v < def.Min || v > def.Max {
				return nil, fmt.Errorf("%w: %s requires an integer in %d..%d", ErrInvalid, key, def.Min, def.Max)
			}
			value = v
		case "string":
			var v string
			if err := json.Unmarshal(setting.Value, &v); err != nil {
				return nil, fmt.Errorf("%w: %s requires a string", ErrInvalid, key)
			}
			found := false
			for _, choice := range def.Choices {
				if v == choice {
					found = true
				}
			}
			if !found {
				return nil, fmt.Errorf("%w: %s has an unsupported value", ErrInvalid, key)
			}
			value = v
		}
		data, err := json.Marshal(value)
		if err != nil {
			return nil, err
		}
		out[key] = Setting{Mode: setting.Mode, Value: data}
	}
	data, err := json.Marshal(out)
	if err != nil {
		return nil, err
	}
	if len(data) > MaxBytes {
		return nil, fmt.Errorf("%w: exceeds %d bytes", ErrInvalid, MaxBytes)
	}
	return out, nil
}

func ForTarget(settings Settings, target string) (Settings, error) {
	if !ValidTarget(target) {
		return nil, fmt.Errorf("%w: target must be host or viewer", ErrInvalid)
	}
	normalized, err := Normalize(settings)
	if err != nil {
		return nil, err
	}
	out := Settings{}
	for _, def := range definitions {
		if setting, exists := normalized[def.Key]; exists && def.Target == target {
			out[def.Key] = setting
		}
	}
	return out, nil
}

// Encode is canonical for supported values and distinguishes false from a
// missing override. Hashes are scoped to the consumer target.
func Encode(settings Settings) (string, error) {
	normalized, err := Normalize(settings)
	if err != nil {
		return "", err
	}
	data, err := json.Marshal(normalized)
	return string(data), err
}

func Digest(target, encoded string) string {
	sum := sha256.Sum256([]byte(target + "\x00" + encoded))
	return hex.EncodeToString(sum[:])
}
