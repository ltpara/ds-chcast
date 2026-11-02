package identity

import (
	"crypto/ed25519"
	"crypto/rand"
	"os"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

func TestHostSecretDerivesStableIdentity(t *testing.T) {
	a := PrivateKeyFromSecret("host-secret")
	b := PrivateKeyFromSecret("host-secret")
	c := PrivateKeyFromSecret("another-secret")
	if !a.Equal(b) {
		t.Fatal("the same secret derived different keys")
	}
	if a.Equal(c) {
		t.Fatal("different secrets derived the same key")
	}
	if len(a.Public().(ed25519.PublicKey)) != ed25519.PublicKeySize {
		t.Fatal("unexpected public key size")
	}
}

func TestMessageSignatureRejectsTamper(t *testing.T) {
	priv := PrivateKeyFromSecret("host-secret")
	pub := priv.Public().(ed25519.PublicKey)
	msg := RegisterMessage("devbox", "AB12", 1700000000)
	sig := Sign(priv, msg)
	if !Verify(pub, msg, sig) {
		t.Fatal("valid signature rejected")
	}
	if Verify(pub, RegisterMessage("devbox", "AB13", 1700000000), sig) {
		t.Fatal("signature accepted for a different fingerprint")
	}
	if Verify(pub, RegisterMessage("other", "AB12", 1700000000), sig) {
		t.Fatal("signature accepted for a different id")
	}
	if Verify(pub, msg, "not-base64") {
		t.Fatal("malformed signature accepted")
	}
}

func TestMigrateSignatureBindsBothIDs(t *testing.T) {
	priv := PrivateKeyFromSecret("host-secret")
	pub := priv.Public().(ed25519.PublicKey)
	sig := Sign(priv, MigrateMessage("oldbox", "newbox", 1700000000))
	if !Verify(pub, MigrateMessage("oldbox", "newbox", 1700000000), sig) {
		t.Fatal("valid migration signature rejected")
	}
	if Verify(pub, MigrateMessage("otherbox", "newbox", 1700000000), sig) {
		t.Fatal("migration signature accepted for a different old id")
	}
	if Verify(pub, MigrateMessage("oldbox", "otherbox", 1700000000), sig) {
		t.Fatal("migration signature accepted for a different new id")
	}
	if Verify(pub, RegisterMessage("newbox", "", 1700000000), sig) {
		t.Fatal("migration signature doubled as a registration proof")
	}
}

func TestHostMetadataSignatureBindsReportedFields(t *testing.T) {
	priv := PrivateKeyFromSecret("host-secret")
	pub := priv.Public().(ed25519.PublicKey)
	sig := Sign(priv, HostMetadataMessage("devbox", "STUDIO-PC", "windows/amd64", "0.36.0", 1700000000))
	if !Verify(pub, HostMetadataMessage("devbox", "STUDIO-PC", "windows/amd64", "0.36.0", 1700000000), sig) {
		t.Fatal("valid metadata signature rejected")
	}
	for name, tampered := range map[string][]byte{
		"hostname":  HostMetadataMessage("devbox", "ATTACKER", "windows/amd64", "0.36.0", 1700000000),
		"platform":  HostMetadataMessage("devbox", "STUDIO-PC", "linux/amd64", "0.36.0", 1700000000),
		"version":   HostMetadataMessage("devbox", "STUDIO-PC", "windows/amd64", "9.9.9", 1700000000),
		"id":        HostMetadataMessage("otherbox", "STUDIO-PC", "windows/amd64", "0.36.0", 1700000000),
		"timestamp": HostMetadataMessage("devbox", "STUDIO-PC", "windows/amd64", "0.36.0", 1700000001),
	} {
		if Verify(pub, tampered, sig) {
			t.Fatalf("metadata signature accepted after tampering with %s", name)
		}
	}
	if Verify(pub, RegisterMessage("devbox", "", 1700000000), sig) {
		t.Fatal("metadata signature doubled as a registration proof")
	}
	if Verify(pub, HostMetadataMessage("devbox", "STUDIO-PC", "windows/amd64", "0.36.0", 1700000000),
		Sign(priv, RegisterMessage("devbox", "", 1700000000))) {
		t.Fatal("registration signature verified under the metadata domain")
	}
}

func TestFieldBoundariesCannotBeShifted(t *testing.T) {
	priv := PrivateKeyFromSecret("host-secret")
	pub := priv.Public().(ed25519.PublicKey)
	sig := Sign(priv, KeepaliveMessage("a", "b:c", "nonce"))
	if Verify(pub, KeepaliveMessage("a:b", "c", "nonce"), sig) {
		t.Fatal("length prefixes did not separate fields")
	}
}

func TestServerBinding(t *testing.T) {
	server, err := GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	host := PrivateKeyFromSecret("host-secret")
	hostPub := host.Public().(ed25519.PublicKey)
	binding, err := server.SignHostBinding("devbox", hostPub, "AB12", time.Unix(1700000000, 0))
	if err != nil {
		t.Fatal(err)
	}
	got, err := VerifyHostBinding(binding, server.PublicKeyText())
	if err != nil {
		t.Fatal(err)
	}
	if got.ID != "devbox" || got.Fingerprint != "AB12" || got.PublicKey != PublicKeyText(hostPub) {
		t.Fatalf("binding = %+v", got)
	}
	other, _ := GenerateServer()
	if _, err := VerifyHostBinding(binding, other.PublicKeyText()); err == nil {
		t.Fatal("binding verified with the wrong server key")
	}
	if _, err := VerifyHostBinding("x"+binding, server.PublicKeyText()); err == nil {
		t.Fatal("tampered binding verified")
	}
}

func TestSoftwareSignature(t *testing.T) {
	server, err := GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	sig, err := server.SignSoftware("2.1.0", "https://downloads.example.com/software/chast-2.1.0.exe", "ab12", 4096)
	if err != nil {
		t.Fatal(err)
	}
	pub, err := ParsePublicKey(server.PublicKeyText())
	if err != nil {
		t.Fatal(err)
	}
	message := SoftwareMessage("2.1.0", "https://downloads.example.com/software/chast-2.1.0.exe", "ab12", 4096)
	if !Verify(pub, message, sig) {
		t.Fatal("software signature did not verify")
	}
	tampered := SoftwareMessage("2.1.1", "https://downloads.example.com/software/chast-2.1.0.exe", "ab12", 4096)
	if Verify(pub, tampered, sig) {
		t.Fatal("signature covered the wrong version")
	}
	if _, err := server.SignSoftware("", "https://example.com/x", "ab12", 1); err == nil {
		t.Fatal("empty version was signed")
	}
	if _, err := server.SignSoftware("2.1.0", "https://example.com/x", "ab12", 0); err == nil {
		t.Fatal("zero size was signed")
	}
	var missing *Server
	if _, err := missing.SignSoftware("2.1.0", "https://example.com/x", "ab12", 1); err == nil {
		t.Fatal("nil identity signed a notice")
	}
}

func TestPublisherSignatureIsDomainSeparated(t *testing.T) {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	pubText := PublicKeyText(pub)
	if got := PublisherKeyID(pubText); len(got) != 16 {
		t.Fatalf("PublisherKeyID = %q, want 16 hex characters", got)
	}
	if PublisherKeyID("not a key") != "" {
		t.Fatal("PublisherKeyID accepted an invalid key")
	}
	sig := Sign(priv, PublisherMessage("2.1.0", "https://example.com/chast-2.1.0.exe", "ab12", 4096))
	if !Verify(pub, PublisherMessage("2.1.0", "https://example.com/chast-2.1.0.exe", "ab12", 4096), sig) {
		t.Fatal("publisher signature did not verify")
	}
	if Verify(pub, SoftwareMessage("2.1.0", "https://example.com/chast-2.1.0.exe", "ab12", 4096), sig) {
		t.Fatal("publisher signature verified under the server domain")
	}
	if Verify(pub, PublisherMessage("2.1.1", "https://example.com/chast-2.1.0.exe", "ab12", 4096), sig) {
		t.Fatal("publisher signature did not bind the version")
	}
}

func TestLoadOrCreateServerPersistsSeed(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server-identity")
	first, err := LoadOrCreateServer(path)
	if err != nil {
		t.Fatal(err)
	}
	second, err := LoadOrCreateServer(path)
	if err != nil {
		t.Fatal(err)
	}
	if first.PublicKeyText() != second.PublicKeyText() {
		t.Fatal("server identity changed after reload")
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o077 != 0 {
		t.Fatalf("identity file permissions = %o, want owner-only", info.Mode().Perm())
	}
}
