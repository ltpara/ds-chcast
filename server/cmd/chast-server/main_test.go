package main

import (
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/logging"
)

func TestNewLoggerWritesToRotatingSink(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server.log")
	log, sink, err := newLogger(slog.LevelInfo, logging.Config{Path: path, MaxSize: 1024, MaxBackups: 2})
	if err != nil {
		t.Fatal(err)
	}
	log.Info("rotating sink probe", "key", "value")
	if err := sink.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), "rotating sink probe") {
		t.Fatalf("log file = %q", data)
	}
}

func TestCheckServerKey(t *testing.T) {
	cases := []struct {
		name     string
		key      string
		insecure bool
		listen   string
		wantErr  bool
	}{
		{"key set", "secret", false, "0.0.0.0:8443", false},
		{"key set with insecure switch", "secret", true, "127.0.0.1:8443", true},
		{"empty key", "", false, "127.0.0.1:8443", true},
		{"insecure loopback v4", "", true, "127.0.0.1:8443", false},
		{"insecure loopback v6", "", true, "[::1]:8443", false},
		{"insecure localhost", "", true, "localhost:8443", false},
		{"insecure exposed", "", true, "0.0.0.0:8443", true},
		{"insecure missing host", "", true, ":8443", true},
		{"insecure malformed listen", "", true, "8443", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := checkServerKey(tc.key, tc.insecure, tc.listen)
			if (err != nil) != tc.wantErr {
				t.Fatalf("checkServerKey(%q, %v, %q) = %v, want error %v", tc.key, tc.insecure, tc.listen, err, tc.wantErr)
			}
		})
	}
}

func TestNewLoggerWithoutFile(t *testing.T) {
	log, sink, err := newLogger(slog.LevelInfo, logging.Config{})
	if err != nil {
		t.Fatal(err)
	}
	if log == nil || sink != nil {
		t.Fatalf("stderr-only logger = %v, %v", log, sink)
	}
	if err := sink.Close(); err != nil {
		t.Fatal(err)
	}
}
