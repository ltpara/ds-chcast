package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/software"
	"github.com/ltpara/ds-chcast/server/internal/updater"
)

func TestPublishWritesVerifiableSidecar(t *testing.T) {
	dir := t.TempDir()
	keyPath := filepath.Join(dir, "publisher.key")
	release := filepath.Join(dir, "chast-2.2.0.exe")
	body := []byte("installer bytes for the publisher signature")
	if err := os.WriteFile(release, body, 0o644); err != nil {
		t.Fatal(err)
	}
	downloadURL := "https://downloads.example.com/software/chast-2.2.0.exe"
	if err := run([]string{
		"-key", keyPath,
		"-file", release,
		"-version", "2.2.0",
		"-url", downloadURL,
		"-create",
	}); err != nil {
		t.Fatalf("run: %v", err)
	}

	sidecar, err := software.LoadPublisherSignature(dir, "chast-2.2.0.exe")
	if err != nil {
		t.Fatalf("load sidecar: %v", err)
	}
	publisher, err := identity.LoadOrCreateServer(keyPath)
	if err != nil {
		t.Fatalf("load publisher key: %v", err)
	}
	sum := sha256.Sum256(body)
	notice := updater.Notice{
		Version:            "2.2.0",
		URL:                downloadURL,
		SHA256:             hex.EncodeToString(sum[:]),
		Size:               int64(len(body)),
		PublisherKeyID:     sidecar.KeyID,
		PublisherSignature: sidecar.Signature,
	}
	if err := updater.VerifyPublisher(notice, publisher.PublicKeyText()); err != nil {
		t.Fatalf("VerifyPublisher: %v", err)
	}
	notice.Size++
	if err := updater.VerifyPublisher(notice, publisher.PublicKeyText()); err == nil {
		t.Fatal("a sidecar verified a changed release size")
	}
}

func TestPublishRefusesMissingKeyAndNameMismatch(t *testing.T) {
	dir := t.TempDir()
	release := filepath.Join(dir, "chast-2.2.0.exe")
	if err := os.WriteFile(release, []byte("installer"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{
		"-key", filepath.Join(dir, "publisher.key"),
		"-file", release,
		"-version", "2.2.0",
		"-url", "https://downloads.example.com/software/chast-2.2.0.exe",
	}); err == nil {
		t.Fatal("run accepted a missing key without --create")
	}
	if _, err := os.Stat(filepath.Join(dir, "chast-2.2.0.exe.pubsig")); !os.IsNotExist(err) {
		t.Fatal("a failed run left a sidecar behind")
	}
	if err := run([]string{
		"-key", filepath.Join(dir, "publisher.key"),
		"-file", release,
		"-version", "2.2.0",
		"-url", "https://downloads.example.com/software/other.exe",
		"-create",
	}); err == nil {
		t.Fatal("run accepted a URL whose file name differs from the release")
	}
}
