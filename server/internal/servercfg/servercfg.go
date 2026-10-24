// Package servercfg fills command-line flags from INI files, .env files and
// environment variables.
//
// It mirrors the configuration sources of hbbs/hbbr: an explicit flag always
// wins, then the INI file passed with --config (hbbs only), then the .env file,
// then inherited environment variables. Keys are matched case-insensitively
// with '-' and '_' treated the same, and either CHAST_<KEY> or <KEY> is
// accepted.
package servercfg

import (
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"os"
	"strings"
)

// Layer is one configuration source. Earlier layers passed to Apply take
// precedence over later ones.
type Layer struct {
	values map[string]string
}

func newLayer() *Layer {
	return &Layer{values: make(map[string]string)}
}

func normalizeKey(key string) string {
	key = strings.TrimSpace(strings.Trim(key, `"'`))
	key = strings.ReplaceAll(key, "-", "_")
	return strings.ToUpper(key)
}

// Lookup returns the value for a flag name. CHAST_-prefixed keys win over the
// unprefixed form so a deployment can namespace an override.
func (l *Layer) Lookup(flagName string) (string, bool) {
	if l == nil {
		return "", false
	}
	key := normalizeKey(flagName)
	if value, ok := l.values["CHAST_"+key]; ok {
		return value, true
	}
	value, ok := l.values[key]
	return value, ok
}

// FromEnv snapshots the process environment.
func FromEnv() *Layer {
	l := newLayer()
	for _, entry := range os.Environ() {
		key, value, ok := strings.Cut(entry, "=")
		if !ok || key == "" {
			continue
		}
		l.values[normalizeKey(key)] = value
	}
	return l
}

// LoadINI reads a key=value file. Section headers and lines starting with '#'
// or ';' are ignored; the last duplicate key wins.
func LoadINI(path string) (*Layer, error) {
	return load(path, false)
}

// LoadEnvFile reads a dotenv-style file. It accepts an optional "export "
// prefix and ignores comments and blank lines.
func LoadEnvFile(path string) (*Layer, error) {
	return load(path, true)
}

func load(path string, dotenv bool) (*Layer, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return nil, err
		}
		return nil, fmt.Errorf("read config %s: %w", path, err)
	}
	l := newLayer()
	for _, raw := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			continue
		}
		if dotenv {
			line = strings.TrimPrefix(line, "export ")
			line = strings.TrimSpace(line)
		}
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		if key == "" {
			continue
		}
		value = strings.TrimSpace(value)
		if len(value) >= 2 {
			if (value[0] == '"' && value[len(value)-1] == '"') ||
				(value[0] == '\'' && value[len(value)-1] == '\'') {
				value = value[1 : len(value)-1]
			}
		}
		l.values[normalizeKey(key)] = value
	}
	return l, nil
}

// Apply sets every flag that was not given on the command line from the first
// layer containing it. It returns the first conversion error.
func Apply(fs *flag.FlagSet, layers ...*Layer) error {
	explicit := make(map[string]bool)
	fs.Visit(func(f *flag.Flag) { explicit[f.Name] = true })
	var firstErr error
	fs.VisitAll(func(f *flag.Flag) {
		if explicit[f.Name] || firstErr != nil {
			return
		}
		if f.Name == "config" || f.Name == "env-file" {
			return
		}
		for _, layer := range layers {
			value, ok := layer.Lookup(f.Name)
			if !ok {
				continue
			}
			err := fs.Set(f.Name, value)
			if err != nil {
				// hbbs accepts Y/N for boolean switches; keep those working.
				if b, ok := parseBool(value); ok {
					err = fs.Set(f.Name, b)
				}
			}
			if err != nil {
				firstErr = fmt.Errorf("config %s=%q: %w", f.Name, value, err)
			}
			return
		}
	})
	return firstErr
}

func parseBool(value string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "y", "yes", "on":
		return "true", true
	case "n", "no", "off":
		return "false", true
	}
	return "", false
}
