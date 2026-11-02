package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/software"
)

func TestSignThenVerifyManifest(t *testing.T) {
	dir := t.TempDir()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	keyPath := filepath.Join(dir, "publisher.key")
	seed := base64.RawURLEncoding.EncodeToString(priv.Seed()) + "\n"
	if err := os.WriteFile(keyPath, []byte(seed), 0o600); err != nil {
		t.Fatal(err)
	}
	filePath := filepath.Join(dir, "chast-2.1.0.exe")
	if err := os.WriteFile(filePath, []byte("release payload"), 0o644); err != nil {
		t.Fatal(err)
	}
	manifestPath := filepath.Join(dir, "manifest.json")
	url := "https://downloads.example.com/software/chast-2.1.0.exe"
	if err := runSign([]string{
		"-key", keyPath,
		"-file", filePath,
		"-version", "2.1.0",
		"-url", url,
		"-out", manifestPath,
	}); err != nil {
		t.Fatalf("runSign: %v", err)
	}
	manifest, err := software.LoadManifest(manifestPath)
	if err != nil {
		t.Fatalf("LoadManifest: %v", err)
	}
	pubText := identity.PublicKeyText(pub)
	if manifest.PublisherKeyID != identity.PublisherKeyID(pubText) {
		t.Fatalf("key id = %q", manifest.PublisherKeyID)
	}
	if err := runVerify([]string{"-manifest", manifestPath, "-file", filePath, "-publisher-key", pubText}); err != nil {
		t.Fatalf("runVerify: %v", err)
	}
	other, _, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	if err := runVerify([]string{"-manifest", manifestPath, "-file", filePath, "-publisher-key", identity.PublicKeyText(other)}); err == nil {
		t.Fatal("runVerify accepted another publisher key")
	}
	if err := os.WriteFile(filePath, []byte("release payload!"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := runVerify([]string{"-manifest", manifestPath, "-file", filePath, "-publisher-key", pubText}); err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Fatalf("runVerify after tampering = %v", err)
	}
}
