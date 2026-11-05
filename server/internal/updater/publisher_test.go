package updater

import (
	"strings"
	"testing"

	"github.com/ltpara/ds-chcast/server/internal/identity"
)

func signedPublisherNotice(t *testing.T, publisher *identity.Server, version,
	downloadURL, digest string, size int64) Notice {
	t.Helper()
	keyID, err := identity.PublisherKeyID(publisher.PublicKeyText())
	if err != nil {
		t.Fatal(err)
	}
	sig, err := publisher.SignSoftwarePublisher(keyID, version, downloadURL, digest, size)
	if err != nil {
		t.Fatal(err)
	}
	return Notice{
		Version:            version,
		URL:                downloadURL,
		SHA256:             digest,
		Size:               size,
		PublisherKeyID:     keyID,
		PublisherSignature: sig,
	}
}

func TestVerifyPublisherAcceptsSignedRelease(t *testing.T) {
	publisher, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	digest := digestOf([]byte("release body"))
	n := signedPublisherNotice(t, publisher, "2.2.0",
		"https://downloads.example.com/software/chast-2.2.0.exe", digest, 12)
	if err := VerifyPublisher(n, publisher.PublicKeyText()); err != nil {
		t.Fatalf("VerifyPublisher: %v", err)
	}
}

func TestVerifyPublisherRejectsTamperedOrMissingNotice(t *testing.T) {
	publisher, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	digest := digestOf([]byte("release body"))
	base := signedPublisherNotice(t, publisher, "2.2.0",
		"https://downloads.example.com/software/chast-2.2.0.exe", digest, 12)
	other, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}

	cases := []struct {
		name string
		n    Notice
		key  string
		want string
	}{
		{"version", withVersion(base, "9.9.9"), publisher.PublicKeyText(), "signature"},
		{"url", withURL(base, "https://evil.example.com/software/chast-2.2.0.exe"), publisher.PublicKeyText(), "signature"},
		{"digest", withDigest(base, digestOf([]byte("other"))), publisher.PublicKeyText(), "signature"},
		{"size", withSize(base, 13), publisher.PublicKeyText(), "signature"},
		{"missing", withPublisherSignature(base, ""), publisher.PublicKeyText(), "no publisher signature"},
		{"wrong pin", base, other.PublicKeyText(), "key id"},
		{"no pin", base, "", "pinned publisher key"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := VerifyPublisher(tc.n, tc.key)
			if err == nil {
				t.Fatal("VerifyPublisher accepted an invalid notice")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("error %q does not mention %q", err, tc.want)
			}
		})
	}
}

func withVersion(n Notice, version string) Notice {
	n.Version = version
	return n
}

func withURL(n Notice, url string) Notice {
	n.URL = url
	return n
}

func withDigest(n Notice, digest string) Notice {
	n.SHA256 = digest
	return n
}

func withSize(n Notice, size int64) Notice {
	n.Size = size
	return n
}

func withPublisherSignature(n Notice, signature string) Notice {
	n.PublisherSignature = signature
	return n
}
