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
