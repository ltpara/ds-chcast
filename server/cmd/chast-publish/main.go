// Command chast-publish signs one release artifact with the publisher's
// Ed25519 key and writes the detached .pubsig sidecar chast-server verifies at
// startup. The private seed is read from a file; it is never accepted on the
// command line or printed.
package main

import (
	"flag"
	"fmt"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/software"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "chast-publish:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet("chast-publish", flag.ExitOnError)
	keyPath := fs.String("key", "", "publisher Ed25519 seed file (base64, 32 bytes)")
	release := fs.String("file", "", "release file to sign")
	version := fs.String("version", "", "release version advertised by chast-server")
	downloadURL := fs.String("url", "", "absolute http(s) download URL of the release")
	out := fs.String("out", "", "sidecar path (default: <file>.pubsig)")
	create := fs.Bool("create", false, "create the key file when it does not exist")
	fs.Parse(args)

	if strings.TrimSpace(*keyPath) == "" {
		return fmt.Errorf("--key is required")
	}
	if strings.TrimSpace(*release) == "" {
		return fmt.Errorf("--file is required")
	}
	if strings.TrimSpace(*version) == "" {
		return fmt.Errorf("--version is required")
	}
	if strings.TrimSpace(*downloadURL) == "" {
		return fmt.Errorf("--url is required")
	}
	if _, err := os.Stat(*keyPath); err != nil {
		if !os.IsNotExist(err) {
			return fmt.Errorf("publisher key %s: %w", *keyPath, err)
		}
		if !*create {
			return fmt.Errorf("publisher key %s does not exist; pass --create to generate it", *keyPath)
		}
	}

	file := filepath.Clean(*release)
	dir := filepath.Dir(file)
	name := filepath.Base(file)
	if !software.ValidName(name) {
		return fmt.Errorf("release file name %q must use [A-Za-z0-9._-]", name)
	}
	u, err := url.Parse(strings.TrimSpace(*downloadURL))
	if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return fmt.Errorf("--url must be an absolute http or https URL")
	}
	if u.User != nil {
		return fmt.Errorf("--url must not carry credentials")
	}
	if path.Base(u.Path) != name {
		return fmt.Errorf("--url must end with the release file name %q", name)
	}
	artifact, err := software.Load(dir, name)
	if err != nil {
		return err
	}

	publisher, err := identity.LoadOrCreateServer(*keyPath)
	if err != nil {
		return fmt.Errorf("publisher key %s: %w", *keyPath, err)
	}
	publicKey := publisher.PublicKeyText()
	keyID, err := identity.PublisherKeyID(publicKey)
	if err != nil {
		return err
	}
	signature, err := publisher.SignSoftwarePublisher(keyID, *version, *downloadURL,
		artifact.SHA256, artifact.Size)
	if err != nil {
		return err
	}
	sidecar := *out
	if strings.TrimSpace(sidecar) == "" {
		sidecar = software.PublisherPath(dir, name)
	}
	if err := software.SavePublisherSignature(sidecar, software.PublisherSignature{
		KeyID:     keyID,
		Signature: signature,
	}); err != nil {
		return err
	}

	fmt.Printf("publisher public key: %s\n", publicKey)
	fmt.Printf("publisher key id: %s\n", keyID)
	fmt.Printf("release: %s %s %d\n", name, artifact.SHA256, artifact.Size)
	fmt.Printf("sidecar: %s\n", sidecar)
	fmt.Printf("chast-server: --software-publisher-key %s\n", publicKey)
	fmt.Printf("host/viewer: --publisher-pubkey %s\n", publicKey)
	return nil
}
