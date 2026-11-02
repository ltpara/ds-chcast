package software

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func signedManifest(t *testing.T, body []byte) (*Manifest, ed25519.PrivateKey, ed25519.PublicKey) {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	m := &Manifest{
		Version: "2.1.0",
		URL:     "https://downloads.example.com/software/chast-2.1.0.exe",
		SHA256:  hex.EncodeToString(sum[:]),
		Size:    int64(len(body)),
	}
	m.PublisherKeyID = identity.PublisherKeyID(identity.PublicKeyText(pub))
	m.PublisherSignature = identity.Sign(priv, identity.PublisherMessage(m.Version, m.URL, m.SHA256, m.Size))
	return m, priv, pub
}

func TestManifestVerify(t *testing.T) {
	body := []byte("release bytes")
	m, _, pub := signedManifest(t, body)
	if err := m.Verify(identity.PublicKeyText(pub)); err != nil {
		t.Fatalf("Verify: %v", err)
	}
}

func TestManifestRejectsTampering(t *testing.T) {
	body := []byte("release bytes")
	base, _, pub := signedManifest(t, body)
	otherPub, _, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name   string
		mutate func(*Manifest)
		key    ed25519.PublicKey
		want   string
	}{
		{"version", func(m *Manifest) { m.Version = "9.9.9" }, pub, "signature"},
		{"url", func(m *Manifest) { m.URL = "https://evil.example.com/chast-2.1.0.exe" }, pub, "signature"},
		{"digest", func(m *Manifest) { m.SHA256 = strings.Repeat("0", 64) }, pub, "signature"},
		{"size", func(m *Manifest) { m.Size++ }, pub, "signature"},
		{"missing signature", func(m *Manifest) { m.PublisherSignature = "" }, pub, "no publisher signature"},
		{"key id", func(m *Manifest) { m.PublisherKeyID = "0123456789abcdef" }, pub, "pinned key"},
		{"other key", func(m *Manifest) {}, otherPub, "pinned key"},
		{"missing version", func(m *Manifest) { m.Version = "" }, pub, "needs version"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			m := *base
			tc.mutate(&m)
			err := m.Verify(identity.PublicKeyText(tc.key))
			if err == nil {
				t.Fatal("Verify accepted a tampered manifest")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error %q does not mention %q", err, tc.want)
			}
		})
	}
}

func TestManifestMatchesArtifact(t *testing.T) {
	body := []byte("release bytes")
	m, _, pub := signedManifest(t, body)
	sum := sha256.Sum256(body)
	artifact := &Artifact{
		Name:   "chast-2.1.0.exe",
		SHA256: hex.EncodeToString(sum[:]),
		Size:   int64(len(body)),
	}
	if err := m.Matches("2.1.0", m.URL, artifact); err != nil {
		t.Fatalf("Matches: %v", err)
	}
	if err := m.Verify(identity.PublicKeyText(pub)); err != nil {
		t.Fatalf("Verify: %v", err)
	}
	if err := m.Matches("2.2.0", m.URL, artifact); err == nil || !strings.Contains(err.Error(), "version") {
		t.Fatalf("Matches version = %v", err)
	}
	if err := m.Matches("2.1.0", m.URL+"x", artifact); err == nil || !strings.Contains(err.Error(), "URL") {
		t.Fatalf("Matches URL = %v", err)
	}
	badDigest := *artifact
	badDigest.SHA256 = strings.Repeat("a", 64)
	if err := m.Matches("2.1.0", m.URL, &badDigest); err == nil || !strings.Contains(err.Error(), "sha256") {
		t.Fatalf("Matches digest = %v", err)
	}
	badSize := *artifact
	badSize.Size++
	if err := m.Matches("2.1.0", m.URL, &badSize); err == nil || !strings.Contains(err.Error(), "size") {
		t.Fatalf("Matches size = %v", err)
	}
}

func TestLoadVerifiedManifest(t *testing.T) {
	body := []byte("release bytes")
	m, _, pub := signedManifest(t, body)
	dir := t.TempDir()
	path := filepath.Join(dir, ManifestName("chast-2.1.0.exe"))
	raw, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(body)
	artifact := &Artifact{Name: "chast-2.1.0.exe", SHA256: hex.EncodeToString(sum[:]), Size: int64(len(body))}
	pubText := identity.PublicKeyText(pub)
	got, err := LoadVerifiedManifest(path, pubText, "2.1.0", m.URL, artifact)
	if err != nil {
		t.Fatalf("LoadVerifiedManifest: %v", err)
	}
	if got.PublisherKeyID != m.PublisherKeyID {
		t.Fatalf("manifest key id = %q", got.PublisherKeyID)
	}
	if _, err := LoadVerifiedManifest(path, pubText, "2.2.0", m.URL, artifact); err == nil {
		t.Fatal("accepted a manifest for another version")
	}
	bad := *artifact
	bad.SHA256 = strings.Repeat("a", 64)
	if _, err := LoadVerifiedManifest(path, pubText, "2.1.0", m.URL, &bad); err == nil {
		t.Fatal("accepted a manifest for another artifact")
	}
	other, _, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := LoadVerifiedManifest(path, identity.PublicKeyText(other), "2.1.0", m.URL, artifact); err == nil {
		t.Fatal("accepted a manifest from another publisher")
	}
}

func TestLoadManifest(t *testing.T) {
	body := []byte("release bytes")
	m, _, pub := signedManifest(t, body)
	dir := t.TempDir()
	path := filepath.Join(dir, ManifestName("chast-2.1.0.exe"))
	raw, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := LoadManifest(path)
	if err != nil {
		t.Fatalf("LoadManifest: %v", err)
	}
	if err := got.Verify(identity.PublicKeyText(pub)); err != nil {
		t.Fatalf("Verify after load: %v", err)
	}
	if err := os.WriteFile(path, make([]byte, maxManifestBytes+1), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadManifest(path); err == nil || !strings.Contains(err.Error(), "exceeds") {
		t.Fatalf("LoadManifest oversize = %v", err)
	}
	if _, err := LoadManifest(filepath.Join(dir, "missing.json")); err == nil {
		t.Fatal("LoadManifest accepted a missing file")
	}
}
