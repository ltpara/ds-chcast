package software

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

// maxManifestBytes keeps a malformed or hostile sidecar from being read into
// memory. A real manifest is a few hundred bytes.
const maxManifestBytes = 64 << 10

// Manifest is the publisher-signed release description stored next to an
// artifact. The server checks it before advertising the update, and a client
// that pins the publisher key can verify the same fields without trusting the
// rendezvous server alone.
type Manifest struct {
	Version            string `json:"version"`
	URL                string `json:"url"`
	SHA256             string `json:"sha256"`
	Size               int64  `json:"size"`
	PublisherKeyID     string `json:"publisher_key_id"`
	PublisherSignature string `json:"publisher_sig"`
}

// ManifestName is the sidecar file name for a hosted artifact.
func ManifestName(artifact string) string {
	return artifact + ".manifest.json"
}

// ManifestPath returns the sidecar path for a hosted artifact.
func ManifestPath(dir, artifact string) string {
	return filepath.Join(dir, ManifestName(artifact))
}

// LoadManifest reads a publisher manifest and rejects oversized sidecars.
func LoadManifest(path string) (*Manifest, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("manifest %s is not a regular file", path)
	}
	if info.Size() > maxManifestBytes {
		return nil, fmt.Errorf("manifest %s exceeds %d bytes", path, maxManifestBytes)
	}
	raw, err := io.ReadAll(io.LimitReader(f, maxManifestBytes+1))
	if err != nil {
		return nil, err
	}
	if len(raw) > maxManifestBytes {
		return nil, fmt.Errorf("manifest %s exceeds %d bytes", path, maxManifestBytes)
	}
	var m Manifest
	if err := json.Unmarshal(raw, &m); err != nil {
		return nil, fmt.Errorf("manifest %s: %w", path, err)
	}
	return &m, nil
}

// Verify checks the publisher signature and the announced key id against the
// pinned publisher key.
func (m *Manifest) Verify(publisherPublicKey string) error {
	if strings.TrimSpace(publisherPublicKey) == "" {
		return errors.New("manifest verification needs a publisher public key")
	}
	pub, err := identity.ParsePublicKey(publisherPublicKey)
	if err != nil {
		return err
	}
	if m.Version == "" || m.URL == "" || m.SHA256 == "" || m.Size <= 0 {
		return errors.New("manifest needs version, url, sha256 and a positive size")
	}
	if m.PublisherSignature == "" {
		return errors.New("manifest carries no publisher signature")
	}
	wantID := identity.PublisherKeyID(publisherPublicKey)
	if m.PublisherKeyID != wantID {
		return fmt.Errorf("manifest names publisher key %q, but the pinned key is %q", m.PublisherKeyID, wantID)
	}
	if !identity.Verify(pub, identity.PublisherMessage(m.Version, m.URL, m.SHA256, m.Size), m.PublisherSignature) {
		return errors.New("manifest publisher signature was not accepted")
	}
	return nil
}

// LoadVerifiedManifest loads a manifest, checks the publisher proof and
// requires it to describe the advertised release and hosted artifact.
func LoadVerifiedManifest(path, publisherPublicKey, version, downloadURL string, artifact *Artifact) (*Manifest, error) {
	manifest, err := LoadManifest(path)
	if err != nil {
		return nil, err
	}
	if err := manifest.Verify(publisherPublicKey); err != nil {
		return nil, err
	}
	if err := manifest.Matches(version, downloadURL, artifact); err != nil {
		return nil, err
	}
	return manifest, nil
}

// Matches requires the manifest to describe exactly the advertised release.
// The digest comparison is case-sensitive because the signature covers the
// manifest's own text: rewriting the case would invalidate it.
func (m *Manifest) Matches(version, downloadURL string, artifact *Artifact) error {
	if m.Version != version {
		return fmt.Errorf("manifest version %q does not match --software-version %q", m.Version, version)
	}
	if m.URL != downloadURL {
		return fmt.Errorf("manifest URL %q does not match --software-url %q", m.URL, downloadURL)
	}
	if artifact == nil {
		return nil
	}
	if m.SHA256 != artifact.SHA256 {
		return fmt.Errorf("manifest sha256 %s does not match the hosted file %s", m.SHA256, artifact.SHA256)
	}
	if m.Size != artifact.Size {
		return fmt.Errorf("manifest size %d does not match the hosted file %d", m.Size, artifact.Size)
	}
	return nil
}
