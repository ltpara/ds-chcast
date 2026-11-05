// Package software hosts the client/host update artifact advertised through the
// rendezvous API. The operator drops release files into --software-dir; the
// server publishes them read-only and binds the digest into the signed update
// notice so a client can reject a swapped binary.
package software

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// RoutePrefix is where hosted artifacts are published.
const RoutePrefix = "/software/"

// PublisherSuffix names the detached publisher signature beside a release.
const PublisherSuffix = ".pubsig"

// PublisherSignature is the sidecar written by chast-publish. The signature
// itself binds the key id, version, URL, digest and size, so the sidecar only
// has to carry the selector and signature.
type PublisherSignature struct {
	KeyID     string `json:"key_id"`
	Signature string `json:"signature"`
}

// PublisherPath returns the sidecar path for a hosted artifact.
func PublisherPath(dir, name string) string {
	return filepath.Join(dir, name+PublisherSuffix)
}

// LoadPublisherSignature reads and validates the sidecar. A missing or
// malformed sidecar is an error when the operator pinned a publisher key.
func LoadPublisherSignature(dir, name string) (PublisherSignature, error) {
	if !ValidName(name) {
		return PublisherSignature{}, fmt.Errorf("invalid software file name %q", name)
	}
	raw, err := os.ReadFile(PublisherPath(dir, name))
	if err != nil {
		return PublisherSignature{}, err
	}
	var sig PublisherSignature
	if err := json.Unmarshal(raw, &sig); err != nil {
		return PublisherSignature{}, fmt.Errorf("publisher signature %s: %w", name+PublisherSuffix, err)
	}
	sig.KeyID = strings.TrimSpace(sig.KeyID)
	sig.Signature = strings.TrimSpace(sig.Signature)
	if sig.KeyID == "" || len(sig.KeyID) > 64 || sig.Signature == "" || len(sig.Signature) > 256 {
		return PublisherSignature{}, fmt.Errorf("publisher signature %s is malformed", name+PublisherSuffix)
	}
	return sig, nil
}

// SavePublisherSignature writes the sidecar atomically. The caller has already
// signed the release.
func SavePublisherSignature(path string, sig PublisherSignature) error {
	raw, err := json.MarshalIndent(sig, "", "  ")
	if err != nil {
		return err
	}
	raw = append(raw, '\n')
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".chast-pubsig-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if _, err := tmp.Write(raw); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}

// Artifact is a hosted update file with its content digest.
type Artifact struct {
	Name    string
	Path    string
	SHA256  string
	Size    int64
	ModTime time.Time
}

// ValidName reports whether name is a single portable path element that may be
// served from the hosting directory. Names must start with a letter or digit
// and contain only [A-Za-z0-9._-], which rules out separators, dot entries and
// hidden files.
func ValidName(name string) bool {
	if name == "" || len(name) > 128 {
		return false
	}
	for i, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
		case r == '.' || r == '_' || r == '-':
			if i == 0 {
				return false
			}
		default:
			return false
		}
	}
	return true
}

// HostedName returns the file name for a download URL published under
// RoutePrefix. It returns an empty name for URLs outside that prefix and an
// error for a hosted path that does not name a single valid file.
func HostedName(rawURL string) (string, error) {
	u, err := url.Parse(rawURL)
	if err != nil {
		return "", fmt.Errorf("software URL: %w", err)
	}
	if !strings.HasPrefix(u.Path, RoutePrefix) {
		return "", nil
	}
	name := strings.TrimPrefix(u.Path, RoutePrefix)
	if !ValidName(name) {
		return "", fmt.Errorf("software URL path %q must name a file directly under %s", u.Path, RoutePrefix)
	}
	return name, nil
}

// Load reads a hosted artifact and computes its SHA-256 and size.
func Load(dir, name string) (*Artifact, error) {
	if !ValidName(name) {
		return nil, fmt.Errorf("invalid software file name %q", name)
	}
	file := filepath.Join(dir, name)
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("software file %s is not a regular file", file)
	}
	sum := sha256.New()
	if _, err := io.Copy(sum, f); err != nil {
		return nil, err
	}
	return &Artifact{
		Name:    name,
		Path:    file,
		SHA256:  hex.EncodeToString(sum.Sum(nil)),
		Size:    info.Size(),
		ModTime: info.ModTime(),
	}, nil
}

// Handler serves files from dir at GET/HEAD /software/{name}. There is no
// directory listing and unsafe names are answered with 404.
func Handler(dir string) http.Handler {
	return handler{dir: dir}
}

type handler struct {
	dir string
}

func (h handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	if !ValidName(name) {
		http.NotFound(w, r)
		return
	}
	f, err := os.Open(filepath.Join(h.dir, name))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil || !info.Mode().IsRegular() {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", name))
	w.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(w, r, name, info.ModTime(), f)
}
