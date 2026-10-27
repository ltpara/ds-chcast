package identity

import (
	"crypto/ed25519"
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
