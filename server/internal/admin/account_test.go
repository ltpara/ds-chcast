package admin

import (
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestAccountConsoleCommands(t *testing.T) {
	s := testutil.Start(t)
	c := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts})

	help := send(t, c.Addr(), "h")
	for _, want := range []string{"user list", "user add", "user passwd", "user del", "user 2fa", "user unlock", "ab <username>", "audit", "connections", "transfers"} {
		if !strings.Contains(help, want) {
			t.Fatalf("help missing %q:\n%s", want, help)
		}
	}

	out := send(t, c.Addr(), "user add alice secret123")
	if !strings.Contains(out, "alice created") {
		t.Fatalf("user add: %q", out)
	}
	if strings.Contains(out, "password:") {
		t.Fatalf("explicit password must not be echoed: %q", out)
	}

	out = send(t, c.Addr(), "user add bob")
	password := generatedPassword(t, out)
	bob, err := s.Accounts.FindUser("bob")
	if err != nil {
		t.Fatalf("find bob: %v", err)
	}
	if _, _, _, err := s.Accounts.Login("bob", password); err != nil {
		t.Fatalf("login with generated password: %v", err)
	}

	users := send(t, c.Addr(), "user list")
	for _, want := range []string{"alice id=", "bob id="} {
		if !strings.Contains(users, want) {
			t.Fatalf("user list missing %q:\n%s", want, users)
		}
	}

	out = send(t, c.Addr(), "user passwd alice newsecret123")
	if !strings.Contains(out, "alice password updated") {
		t.Fatalf("user passwd: %q", out)
	}
	if _, _, _, err := s.Accounts.Login("alice", "newsecret123"); err != nil {
		t.Fatalf("login with new password: %v", err)
	}

	// A lost authenticator is recovered from the loopback console.
	alice, err := s.Accounts.FindUser("alice")
	if err != nil {
		t.Fatalf("find alice: %v", err)
	}
	secret, err := s.Accounts.StartTwoFactor(alice.ID)
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	code, err := account.TOTPCode(secret, time.Now())
	if err != nil {
		t.Fatalf("generate two-factor code: %v", err)
	}
	if err := s.Accounts.EnableTwoFactor(alice.ID, code); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	if out = send(t, c.Addr(), "user 2fa alice off"); !strings.Contains(out, "alice two-factor disabled") {
		t.Fatalf("user 2fa off: %q", out)
	}
	if enabled, err := s.Accounts.TwoFactorEnabled(alice.ID); err != nil || enabled {
		t.Fatalf("two-factor after reset = %v, %v", enabled, err)
	}
	if _, _, _, err := s.Accounts.Login("alice", "newsecret123"); err != nil {
		t.Fatalf("login after two-factor reset: %v", err)
	}
	if out = send(t, c.Addr(), "user 2fa alice"); !strings.Contains(out, "user 2fa") {
		t.Fatalf("two-factor usage: %q", out)
	}
	if out = send(t, c.Addr(), "user 2fa nobody off"); !strings.Contains(out, "could not reset two-factor") {
		t.Fatalf("two-factor reset for unknown user: %q", out)
	}

	locked := time.Date(2026, 10, 25, 12, 0, 0, 0, time.UTC)
	record := authlimit.Record{
		Failures:    5,
		WindowStart: locked,
		LockedUntil: locked.Add(time.Minute),
		LastSeen:    locked,
	}
	for _, key := range []string{"alice|10.0.0.1", "alice|10.0.0.2", "alicebob|10.0.0.3"} {
		if err := s.Accounts.SaveLoginLock(key, record); err != nil {
			t.Fatalf("save login lock %s: %v", key, err)
		}
	}
	if out = send(t, c.Addr(), "user unlock Alice"); !strings.Contains(out, "Alice login lockout cleared (2 keys)") {
		t.Fatalf("user unlock: %q", out)
	}
	for _, key := range []string{"alice|10.0.0.1", "alice|10.0.0.2"} {
		if _, found, err := s.Accounts.LoadLoginLock(key); err != nil || found {
			t.Fatalf("%s survived unlock: found=%v, %v", key, found, err)
		}
	}
	if _, found, err := s.Accounts.LoadLoginLock("alicebob|10.0.0.3"); err != nil || !found {
		t.Fatalf("other username was unlocked: found=%v, %v", found, err)
	}
	if out = send(t, c.Addr(), "user unlock alice"); !strings.Contains(out, "alice login lockout cleared (0 keys)") {
		t.Fatalf("repeat unlock: %q", out)
	}
	if out = send(t, c.Addr(), "user unlock"); !strings.Contains(out, "user unlock") {
		t.Fatalf("unlock usage: %q", out)
	}

	if err := s.Accounts.AppendAudit(account.AuditEvent{
		Username: "alice",
		Action:   account.AuditLogin,
		Detail:   "password",
		ClientIP: "127.0.0.1",
		Success:  true,
	}); err != nil {
		t.Fatalf("append audit: %v", err)
	}
	audit := send(t, c.Addr(), "audit 5")
	for _, want := range []string{
		"user=alice", "action=login", "ok", "ip=127.0.0.1", "detail=password",
		"action=2fa_reset", "ip=admin-console", "detail=reset from admin console",
		"action=login_unlock", "detail=cleared 2 lockout keys from admin console",
	} {
		if !strings.Contains(audit, want) {
			t.Fatalf("audit missing %q:\n%s", want, audit)
		}
	}

	started := time.Date(2026, 10, 25, 9, 0, 0, 0, time.UTC)
	if _, err := s.Accounts.RecordConnection(
		bob.ID, bob.Username, "devbox", "viewer", "127.0.0.1",
		started, started.Add(time.Minute), true, "",
	); err != nil {
		t.Fatalf("record connection: %v", err)
	}
	connections := send(t, c.Addr(), "connections 5")
	for _, want := range []string{
		"user=bob", "peer=devbox", "ok", "ip=127.0.0.1", "duration=1m0s", "detail=-",
	} {
		if !strings.Contains(connections, want) {
			t.Fatalf("connections missing %q:\n%s", want, connections)
		}
	}
	if out := send(t, c.Addr(), "connections nope"); !strings.Contains(out, "usage: connections") {
		t.Fatalf("connections usage: %q", out)
	}

	if _, err := s.Accounts.RecordFileTransfer(
		bob.ID, bob.Username, "devbox", "upload", "report.pdf", "127.0.0.1", 4096, 2,
		started.Add(time.Minute), true, "",
	); err != nil {
		t.Fatalf("record file transfer: %v", err)
	}
	transfers := send(t, c.Addr(), "transfers 5")
	for _, want := range []string{
		"user=bob", "peer=devbox", "upload", "name=report.pdf", "size=4096",
		"files=2", "ip=127.0.0.1", "ok", "detail=-",
	} {
		if !strings.Contains(transfers, want) {
			t.Fatalf("transfers missing %q:\n%s", want, transfers)
		}
	}
	if out := send(t, c.Addr(), "transfers nope"); !strings.Contains(out, "usage: transfers") {
		t.Fatalf("transfers usage: %q", out)
	}

	name := "办公室"
	tags := []string{"lab", "vip"}
	if _, err := s.Accounts.UpdatePeer(bob.ID, "peer123", &name, &tags); err != nil {
		t.Fatalf("seed address book: %v", err)
	}
	ab := send(t, c.Addr(), "ab bob")
	for _, want := range []string{"peer123 name=办公室 tags=lab,vip"} {
		if !strings.Contains(ab, want) {
			t.Fatalf("address book missing %q:\n%s", want, ab)
		}
	}
	empty := send(t, c.Addr(), "ab alice")
	if !strings.Contains(empty, "alice has no address book peers") {
		t.Fatalf("empty address book: %q", empty)
	}

	if out = send(t, c.Addr(), "user del bob"); !strings.Contains(out, "bob deleted") {
		t.Fatalf("user del: %q", out)
	}
	if out = send(t, c.Addr(), "ab bob"); !strings.Contains(out, "unknown user bob") {
		t.Fatalf("deleted user address book: %q", out)
	}
	if out = send(t, c.Addr(), "user add bad!name secret123"); !strings.Contains(out, "could not create user") {
		t.Fatalf("invalid username: %q", out)
	}
	if out = send(t, c.Addr(), "user add weak short"); !strings.Contains(out, "could not create user") {
		t.Fatalf("weak password: %q", out)
	}
	if out = send(t, c.Addr(), "user bogus"); !strings.Contains(out, "user list") {
		t.Fatalf("user usage: %q", out)
	}
}

func TestAccountConsoleNeedsStore(t *testing.T) {
	s := testutil.Start(t)
	c := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay})
	for _, command := range []string{"user list", "user add alice secret123", "ab alice", "audit", "connections", "transfers"} {
		out := send(t, c.Addr(), command)
		if !strings.Contains(out, "accounts require persistent state") {
			t.Fatalf("%q without an account store: %q", command, out)
		}
	}
}

func generatedPassword(t *testing.T, out string) string {
	t.Helper()
	for _, line := range strings.Split(out, "\n") {
		if password, ok := strings.CutPrefix(line, "password: "); ok {
			password = strings.TrimSpace(password)
			if password == "" {
				t.Fatalf("empty generated password: %q", out)
			}
			return password
		}
	}
	t.Fatalf("no generated password in %q", out)
	return ""
}
