// Command chast-release signs an update manifest with a publisher key. The
// rendezvous server verifies the manifest with --software-publisher-key before
// advertising a release, and clients that pin the same key can verify it again
// without trusting the server. The private key never has to reach the server.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/software"
	"github.com/ltpara/ds-chcast/server/internal/updater"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "chast-release: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 {
		return usage()
	}
	switch args[0] {
	case "keygen":
		return runKeygen(args[1:])
	case "sign":
		return runSign(args[1:])
	case "verify":
		return runVerify(args[1:])
	case "keyid":
		return runKeyID(args[1:])
	default:
		return usage()
	}
}

func usage() error {
	return errors.New("usage: chast-release keygen -out publisher.key | " +
		"sign -key publisher.key -file release.exe -version V -url URL [-out manifest.json] | " +
		"verify -manifest manifest.json -file release.exe -publisher-key BASE64 | " +
		"keyid -publisher-key BASE64")
}

func runKeygen(args []string) error {
	fs := flag.NewFlagSet("keygen", flag.ExitOnError)
	out := fs.String("out", "", "file that receives the private key seed; required")
	fs.Parse(args)
	if strings.TrimSpace(*out) == "" {
		return errors.New("keygen needs -out <private key file>")
	}
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	seed := priv.Seed()
	file, err := os.OpenFile(*out, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	if err != nil {
		return fmt.Errorf("create %s: %w", *out, err)
	}
	if _, err := file.WriteString(base64.RawURLEncoding.EncodeToString(seed) + "\n"); err != nil {
		file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	pubText := identity.PublicKeyText(pub)
	fmt.Printf("publisher public key: %s\n", pubText)
	fmt.Printf("publisher key id: %s\n", identity.PublisherKeyID(pubText))
	fmt.Printf("private key written to: %s\n", *out)
	fmt.Println("keep the private key offline; give the server only the public key")
	return nil
}

func runSign(args []string) error {
	fs := flag.NewFlagSet("sign", flag.ExitOnError)
	keyPath := fs.String("key", "", "private key seed file produced by keygen; required")
	filePath := fs.String("file", "", "release binary to describe; required")
	version := fs.String("version", "", "release version; required")
	downloadURL := fs.String("url", "", "absolute http(s) download URL; required")
	out := fs.String("out", "", "manifest output path; default <file>.manifest.json")
	fs.Parse(args)
	versionText := strings.TrimSpace(*version)
	urlText := strings.TrimSpace(*downloadURL)
	if strings.TrimSpace(*keyPath) == "" || strings.TrimSpace(*filePath) == "" {
		return errors.New("sign needs -key and -file")
	}
	if versionText == "" || urlText == "" {
		return errors.New("sign needs -version and -url")
	}
	if len(versionText) > 64 || len(urlText) > 1024 {
		return errors.New("version is limited to 64 characters and url to 1024")
	}
	parsed, err := url.Parse(urlText)
	if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
		return errors.New("-url must be an absolute http or https URL")
	}
	priv, err := loadPrivateKey(*keyPath)
	if err != nil {
		return err
	}
	digest, size, err := hashFile(*filePath)
	if err != nil {
		return err
	}
	if size <= 0 || size > int64(updater.MaxSize) {
		return fmt.Errorf("release size %d is outside 1..%d", size, int64(updater.MaxSize))
	}
	pubText := identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
	manifest := software.Manifest{
		Version:            versionText,
		URL:                urlText,
		SHA256:             digest,
		Size:               size,
		PublisherKeyID:     identity.PublisherKeyID(pubText),
		PublisherSignature: identity.Sign(priv, identity.PublisherMessage(versionText, urlText, digest, size)),
	}
	manifestPath := strings.TrimSpace(*out)
	if manifestPath == "" {
		manifestPath = *filePath + ".manifest.json"
	}
	if err := writeManifest(manifestPath, &manifest); err != nil {
		return err
	}
	fmt.Printf("manifest: %s\n", manifestPath)
	fmt.Printf("publisher key id: %s\n", manifest.PublisherKeyID)
	fmt.Printf("sha256: %s\n", digest)
	fmt.Printf("size: %d\n", size)
	return nil
}

func runVerify(args []string) error {
	fs := flag.NewFlagSet("verify", flag.ExitOnError)
	manifestPath := fs.String("manifest", "", "manifest to verify; required")
	filePath := fs.String("file", "", "release binary the manifest describes; required")
	publisherKey := fs.String("publisher-key", "", "pinned publisher Ed25519 public key; required")
	fs.Parse(args)
	if strings.TrimSpace(*manifestPath) == "" || strings.TrimSpace(*filePath) == "" || strings.TrimSpace(*publisherKey) == "" {
		return errors.New("verify needs -manifest, -file and -publisher-key")
	}
	manifest, err := software.LoadManifest(*manifestPath)
	if err != nil {
		return err
	}
	if err := manifest.Verify(*publisherKey); err != nil {
		return err
	}
	digest, size, err := hashFile(*filePath)
	if err != nil {
		return err
	}
	artifact := &software.Artifact{SHA256: digest, Size: size}
	if err := manifest.Matches(manifest.Version, manifest.URL, artifact); err != nil {
		return err
	}
	fmt.Printf("manifest verified: version %s, %d bytes, sha256 %s\n", manifest.Version, manifest.Size, manifest.SHA256)
	return nil
}

func runKeyID(args []string) error {
	fs := flag.NewFlagSet("keyid", flag.ExitOnError)
	publisherKey := fs.String("publisher-key", "", "publisher Ed25519 public key; required")
	fs.Parse(args)
	if strings.TrimSpace(*publisherKey) == "" {
		return errors.New("keyid needs -publisher-key")
	}
	id := identity.PublisherKeyID(strings.TrimSpace(*publisherKey))
	if id == "" {
		return identity.ErrBadPublicKey
	}
	fmt.Println(id)
	return nil
}

func loadPrivateKey(path string) (ed25519.PrivateKey, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	seed, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil {
		return nil, fmt.Errorf("publisher key %s: %w", path, err)
	}
	if len(seed) != ed25519.SeedSize {
		return nil, fmt.Errorf("publisher key %s must contain a %d-byte seed", path, ed25519.SeedSize)
	}
	return ed25519.NewKeyFromSeed(seed), nil
}

func hashFile(path string) (string, int64, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return "", 0, err
	}
	if !info.Mode().IsRegular() {
		return "", 0, fmt.Errorf("%s is not a regular file", path)
	}
	sum := sha256.New()
	written, err := io.Copy(sum, file)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(sum.Sum(nil)), written, nil
}

func writeManifest(path string, manifest *software.Manifest) error {
	raw, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".chast-manifest-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if _, err := tmp.Write(append(raw, '\n')); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}
