// Package updater stages a server-advertised release on the host. The notice
// is signed with the server identity and binds the version, download URL,
// SHA-256 and size, so an agent that pins the server key can reject both a
// swapped notice and a swapped binary. Nothing here executes anything until
// the digest matches; Launch is a separate step so a caller can stage first.
package updater

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

// MaxSize caps a staged release. The server refuses to advertise larger files
// and the agent refuses to write one even if the notice is forged.
const MaxSize = 512 << 20

// Notice is the subset of the server's software update reply needed to stage
// a release.
type Notice struct {
	Version   string
	URL       string
	SHA256    string
	Size      int64
	Signature string
}

// Complete reports whether the server hosted the file itself and signed the
// notice. A notice without a digest can still be shown to an operator, but it
// cannot be verified before running.
func (n Notice) Complete() bool {
	return n.SHA256 != "" && n.Size > 0 && n.Signature != ""
}

// Verify checks the notice signature against the pinned server key.
func Verify(n Notice, serverPublicKey string) error {
	if strings.TrimSpace(serverPublicKey) == "" {
		return errors.New("automatic update needs a pinned server key")
	}
	pub, err := identity.ParsePublicKey(serverPublicKey)
	if err != nil {
		return err
	}
	if !n.Complete() {
		return errors.New("the update notice carries no digest; the server does not host the file")
	}
	if n.Size > MaxSize {
		return fmt.Errorf("release size %d exceeds the %d byte limit", n.Size, int64(MaxSize))
	}
	if _, err := downloadName(n.URL); err != nil {
		return err
	}
	if !identity.Verify(pub, identity.SoftwareMessage(n.Version, n.URL, n.SHA256, n.Size), n.Signature) {
		return errors.New("update notice signature was not accepted")
	}
	return nil
}

// Fetch downloads the advertised release into dir and returns its path. The
// body is streamed once, capped at the advertised size plus one byte, and the
// digest is checked before the file is renamed into place; a failed download
// never leaves a partial file behind.
func Fetch(ctx context.Context, client *http.Client, n Notice, dir string) (string, error) {
	if n.SHA256 == "" {
		return "", errors.New("the update notice carries no digest")
	}
	if n.Size <= 0 || n.Size > MaxSize {
		return "", fmt.Errorf("release size %d is outside 1..%d", n.Size, int64(MaxSize))
	}
	name, err := downloadName(n.URL)
	if err != nil {
		return "", err
	}
	if strings.TrimSpace(dir) == "" {
		dir = os.TempDir()
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	if client == nil {
		client = &http.Client{Timeout: 10 * time.Minute}
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, n.URL, nil)
	if err != nil {
		return "", err
	}
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("download answered %s", resp.Status)
	}
	tmp, err := os.CreateTemp(dir, ".chast-update-*.part")
	if err != nil {
		return "", err
	}
	tmpName := tmp.Name()
	cleanup := func() {
		tmp.Close()
		os.Remove(tmpName)
	}
	digest := sha256.New()
	written, err := io.Copy(io.MultiWriter(tmp, digest), io.LimitReader(resp.Body, n.Size+1))
	if err != nil {
		cleanup()
		return "", err
	}
	if written != n.Size {
		cleanup()
		return "", fmt.Errorf("download size %d does not match the advertised %d", written, n.Size)
	}
	got := hex.EncodeToString(digest.Sum(nil))
	if !strings.EqualFold(got, n.SHA256) {
		cleanup()
		return "", fmt.Errorf("download digest %s does not match the advertised %s", got, n.SHA256)
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmpName)
		return "", err
	}
	final := filepath.Join(dir, name)
	if err := os.Rename(tmpName, final); err != nil {
		// Windows refuses to replace an existing file; a previous run may have
		// staged the same release. Drop that copy once and retry.
		if os.Remove(final) == nil {
			err = os.Rename(tmpName, final)
		}
		if err != nil {
			os.Remove(tmpName)
			return "", err
		}
	}
	return final, nil
}

// downloadName derives the staged file name from the download URL. Only a
// single portable path element is accepted so a hostile notice cannot escape
// the staging directory.
func downloadName(rawURL string) (string, error) {
	u, err := url.Parse(strings.TrimSpace(rawURL))
	if err != nil || u.Host == "" {
		return "", errors.New("update URL must be an absolute http or https URL")
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return "", errors.New("update URL must be an absolute http or https URL")
	}
	if u.User != nil {
		return "", errors.New("update URL must not carry credentials")
	}
	name := path.Base(u.Path)
	if name == "" || name == "." || name == "/" || len(name) > 128 {
		return "", errors.New("update URL must name a file")
	}
	for i, r := range name {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
		case r == '.' || r == '_' || r == '-':
			if i == 0 {
				return "", errors.New("update URL file name must start with a letter or digit")
			}
		default:
			return "", errors.New("update URL file name must use [A-Za-z0-9._-]")
		}
	}
	return name, nil
}
