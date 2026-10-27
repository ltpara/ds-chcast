package admin

import (
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
	"github.com/ltpara/ds-chcast/server/internal/authlimit"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

type consoleReset struct {
	to       string
	username string
	token    string
	link     string
	ttl      time.Duration
}

type consoleMailer struct {
	mu   sync.Mutex
	sent []consoleReset
}

func (m *consoleMailer) SendPasswordReset(to, username, token, link string, ttl time.Duration) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.sent = append(m.sent, consoleReset{to: to, username: username, token: token, link: link, ttl: ttl})
	return nil
}

func (m *consoleMailer) messages() []consoleReset {
	m.mu.Lock()
	defer m.mu.Unlock()
	return append([]consoleReset(nil), m.sent...)
}

type consoleSMS struct {
	mu   sync.Mutex
	sent []consoleReset
}

func (m *consoleSMS) SendPasswordReset(to, username, code string, ttl time.Duration) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.sent = append(m.sent, consoleReset{to: to, username: username, token: code, ttl: ttl})
	return nil
}

func (m *consoleSMS) messages() []consoleReset {
	m.mu.Lock()
	defer m.mu.Unlock()
	return append([]consoleReset(nil), m.sent...)
}

func TestAccountConsoleCommands(t *testing.T) {
	s := testutil.Start(t)
	c := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts})

	help := send(t, c.Addr(), "h")
	for _, want := range []string{"user list", "user add", "user passwd", "user del", "user role", "user ban", "user unban", "user email", "user recover", "user phone", "user sms", "user 2fa", "user unlock", "ab <username>", "ab prune <days>", "audit prune", "connections", "transfers"} {
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
	for _, want := range []string{"alice role=admin id=", "bob role=user id="} {
		if !strings.Contains(users, want) {
			t.Fatalf("user list missing %q:\n%s", want, users)
		}
	}

	if out = send(t, c.Addr(), "user role bob admin"); !strings.Contains(out, "bob role=admin") {
		t.Fatalf("user role promote: %q", out)
	}
	if out = send(t, c.Addr(), "user role alice user"); !strings.Contains(out, "alice role=user") {
		t.Fatalf("user role demote: %q", out)
	}
	if out = send(t, c.Addr(), "user role bob user"); !strings.Contains(out, "at least one admin account must remain") {
		t.Fatalf("user role last admin: %q", out)
	}
	if out = send(t, c.Addr(), "user role alice admin"); !strings.Contains(out, "alice role=admin") {
		t.Fatalf("user role restore: %q", out)
	}
	if out = send(t, c.Addr(), "user role nobody admin"); !strings.Contains(out, "could not set role") {
		t.Fatalf("user role unknown: %q", out)
	}
	if out = send(t, c.Addr(), "user role bob"); !strings.Contains(out, "user role") {
		t.Fatalf("user role usage: %q", out)
	}

	if out = send(t, c.Addr(), "user ban bob"); !strings.Contains(out, "bob banned") {
		t.Fatalf("user ban: %q", out)
	}
	users = send(t, c.Addr(), "user list")
	if !strings.Contains(users, "bob role=admin") || !strings.Contains(users, "banned=true") {
		t.Fatalf("user list after ban:\n%s", users)
	}
	if _, _, _, err := s.Accounts.Login("bob", password); err == nil {
		t.Fatal("banned account still logs in")
	}
	if out = send(t, c.Addr(), "user ban alice"); !strings.Contains(out, "at least one admin account must remain") {
		t.Fatalf("ban last usable admin: %q", out)
	}
	if out = send(t, c.Addr(), "user unban bob"); !strings.Contains(out, "bob unbanned") {
		t.Fatalf("user unban: %q", out)
	}
	if _, _, _, err := s.Accounts.Login("bob", password); err != nil {
		t.Fatalf("login after unban: %v", err)
	}
	if out = send(t, c.Addr(), "user ban nobody"); !strings.Contains(out, "could not change ban state") {
		t.Fatalf("ban unknown user: %q", out)
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
	audit := send(t, c.Addr(), "audit 10")
	for _, want := range []string{
		"user=alice", "action=login", "ok", "ip=127.0.0.1", "detail=password",
		"action=role_change", "detail=role=admin from admin console",
		"action=2fa_reset", "ip=admin-console", "detail=reset from admin console",
		"action=login_unlock", "detail=cleared 2 lockout keys from admin console",
		"action=account_ban", "detail=banned from admin console",
		"action=account_unban", "detail=unbanned from admin console",
	} {
		if !strings.Contains(audit, want) {
			t.Fatalf("audit missing %q:\n%s", want, audit)
		}
	}

	started := time.Date(2026, 10, 25, 9, 0, 0, 0, time.UTC)
	if _, err := s.Accounts.RecordConnection(
		bob.ID, bob.Username, "devbox", "viewer", "127.0.0.1", "",
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
		bob.ID, bob.Username, "devbox", "upload", "report.pdf", "127.0.0.1", "", 4096, 2,
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

func TestAuditConsolePrune(t *testing.T) {
	s := testutil.Start(t)
	c := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts})
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	old := time.Now().Add(-30 * 24 * time.Hour)
	fresh := time.Now().Add(-time.Hour)
	for _, at := range []time.Time{old, fresh, fresh} {
		if err := s.Accounts.AppendAudit(account.AuditEvent{
			Username: "alice", Action: account.AuditLogin, Detail: at.Format(time.RFC3339),
			CreatedAt: at, Success: true,
		}); err != nil {
			t.Fatalf("append audit: %v", err)
		}
	}
	for _, at := range []time.Time{old, fresh} {
		if _, err := s.Accounts.RecordConnection(
			alice.ID, alice.Username, "devbox", "viewer", "127.0.0.1", "",
			at, at.Add(time.Minute), true, "",
		); err != nil {
			t.Fatalf("record connection: %v", err)
		}
		if _, err := s.Accounts.RecordFileTransfer(
			alice.ID, alice.Username, "devbox", "upload", at.Format("150405")+".pdf",
			"127.0.0.1", "", 1, 1, at, true, "",
		); err != nil {
			t.Fatalf("record transfer: %v", err)
		}
	}

	for _, usage := range []string{"audit prune", "audit prune 0", "audit prune -3", "audit prune abc"} {
		if out := send(t, c.Addr(), usage); !strings.Contains(out, "usage: audit prune <days>") {
			t.Fatalf("%q: %q", usage, out)
		}
	}
	out := send(t, c.Addr(), "audit prune 7")
	if !strings.Contains(out, "pruned older than 7 days: audit=1 connections=1 transfers=1") {
		t.Fatalf("audit prune: %q", out)
	}
	if events, err := s.Accounts.AuditEvents(50); err != nil || len(events) != 2 {
		t.Fatalf("audit after prune = %d, %v", len(events), err)
	}
	connections, err := s.Accounts.Connections(alice.ID, 50)
	if err != nil || len(connections) != 1 || connections[0].StartedAt.Before(fresh.Add(-time.Minute)) {
		t.Fatalf("connections after prune = %+v, %v", connections, err)
	}
	transfers, err := s.Accounts.FileTransfers(alice.ID, 50)
	if err != nil || len(transfers) != 1 || transfers[0].Name != fresh.Format("150405")+".pdf" {
		t.Fatalf("transfers after prune = %+v, %v", transfers, err)
	}
}

func TestAddressBookConsolePrune(t *testing.T) {
	s := testutil.Start(t)
	c := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts})
	alice, err := s.Accounts.CreateUser("alice", "correct horse")
	if err != nil {
		t.Fatalf("create alice: %v", err)
	}
	old := time.Now().Add(-30 * 24 * time.Hour)
	fresh := time.Now().Add(-time.Hour)
	s.Accounts.SetClock(func() time.Time { return old })
	name := "old peer"
	if _, err := s.Accounts.UpdatePeer(alice.ID, "peer-old", &name, nil); err != nil {
		t.Fatalf("add old peer: %v", err)
	}
	if _, err := s.Accounts.DeletePeer(alice.ID, "peer-old"); err != nil {
		t.Fatalf("delete old peer: %v", err)
	}
	if _, err := s.Accounts.UpsertGroup(alice.ID, "group-old", "Old"); err != nil {
		t.Fatalf("add old group: %v", err)
	}
	if _, err := s.Accounts.DeleteGroup(alice.ID, "group-old"); err != nil {
		t.Fatalf("delete old group: %v", err)
	}
	s.Accounts.SetClock(func() time.Time { return fresh })
	freshName := "fresh peer"
	if _, err := s.Accounts.UpdatePeer(alice.ID, "peer-fresh", &freshName, nil); err != nil {
		t.Fatalf("add fresh peer: %v", err)
	}
	if _, err := s.Accounts.DeletePeer(alice.ID, "peer-fresh"); err != nil {
		t.Fatalf("delete fresh peer: %v", err)
	}
	if _, err := s.Accounts.UpdatePeer(alice.ID, "peer-live", &freshName, nil); err != nil {
		t.Fatalf("add live peer: %v", err)
	}

	for _, usage := range []string{"ab prune", "ab prune 0", "ab prune -3", "ab prune abc"} {
		if out := send(t, c.Addr(), usage); !strings.Contains(out, "usage: ab prune <days>") {
			t.Fatalf("%q: %q", usage, out)
		}
	}
	out := send(t, c.Addr(), "ab prune 7")
	if !strings.Contains(out, "pruned address book tombstones older than 7 days: peers=1 groups=1") {
		t.Fatalf("ab prune: %q", out)
	}
	peers, err := s.Accounts.Peers(alice.ID)
	if err != nil || len(peers) != 1 || peers[0].ID != "peer-live" {
		t.Fatalf("live peers after prune = %+v, %v", peers, err)
	}
	snapshot, err := s.Accounts.Snapshot(alice.ID, 0)
	if err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	found := 0
	for _, change := range snapshot.Changes {
		if change.Deleted && change.ID == "peer-fresh" {
			found++
		}
		if change.Deleted && change.ID == "peer-old" {
			t.Fatalf("expired tombstone survived: %+v", change)
		}
	}
	if found != 1 {
		t.Fatalf("recent tombstone missing from changes: %+v", snapshot.Changes)
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

func TestAccountEmailAndRecoveryConsole(t *testing.T) {
	s := testutil.Start(t)
	mailer := &consoleMailer{}
	c := startConsole(t, Config{
		Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts,
		Mailer: mailer, RecoveryURL: "https://chast.example/reset?lang=en", RecoveryTTL: 10 * time.Minute,
	})
	if _, err := s.Accounts.CreateUser("alice", "secret123"); err != nil {
		t.Fatalf("create alice: %v", err)
	}

	out := send(t, c.Addr(), "user email alice alice@example.com")
	if !strings.Contains(out, "alice recovery email=alice@example.com") {
		t.Fatalf("set email: %q", out)
	}
	if users := send(t, c.Addr(), "user list"); !strings.Contains(users, "email=alice@example.com") {
		t.Fatalf("user list lacks email:\n%s", users)
	}
	if out = send(t, c.Addr(), "user email alice not-an-address"); !strings.Contains(out, "could not set recovery email") {
		t.Fatalf("invalid email: %q", out)
	}
	if out = send(t, c.Addr(), "user email ghost ghost@example.com"); !strings.Contains(out, "could not set recovery email") {
		t.Fatalf("unknown user email: %q", out)
	}

	out = send(t, c.Addr(), "user recover alice")
	if !strings.Contains(out, "password reset email sent to alice@example.com") {
		t.Fatalf("recover: %q", out)
	}
	msgs := mailer.messages()
	if len(msgs) != 1 {
		t.Fatalf("reset mails = %d", len(msgs))
	}
	msg := msgs[0]
	if msg.to != "alice@example.com" || msg.username != "alice" || msg.token == "" || msg.ttl != 10*time.Minute {
		t.Fatalf("reset mail = %+v", msg)
	}
	if !strings.Contains(msg.link, "lang=en") || !strings.Contains(msg.link, "token="+msg.token) {
		t.Fatalf("reset link = %q", msg.link)
	}
	if _, err := s.Accounts.ConsumePasswordReset(msg.token, "brand new secret"); err != nil {
		t.Fatalf("console token does not reset the password: %v", err)
	}

	if out = send(t, c.Addr(), "user email alice -"); !strings.Contains(out, "recovery email cleared") {
		t.Fatalf("clear email: %q", out)
	}
	if out = send(t, c.Addr(), "user recover alice"); !strings.Contains(out, "could not create password reset") {
		t.Fatalf("recover without email: %q", out)
	}
}

func TestAccountPhoneAndSMSConsole(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUser("alice", "secret123"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := s.Accounts.CreateUser("bob", "secret123"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	smsSender := &consoleSMS{}
	c := startConsole(t, Config{
		Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts,
		SMS: smsSender, SMSTTL: 5 * time.Minute,
	})

	out := send(t, c.Addr(), "user phone alice +1 555 010 0001")
	if !strings.Contains(out, "alice recovery phone=15550100001") {
		t.Fatalf("set phone: %q", out)
	}
	if users := send(t, c.Addr(), "user list"); !strings.Contains(users, "phone=15550100001") {
		t.Fatalf("user list lacks phone:\n%s", users)
	}
	if out = send(t, c.Addr(), "user phone alice 12345"); !strings.Contains(out, "could not set recovery phone") {
		t.Fatalf("invalid phone: %q", out)
	}
	if out = send(t, c.Addr(), "user phone ghost 15550100002"); !strings.Contains(out, "could not set recovery phone") {
		t.Fatalf("unknown user phone: %q", out)
	}
	if out = send(t, c.Addr(), "user phone bob 15550100001"); !strings.Contains(out, "could not set recovery phone") {
		t.Fatalf("duplicate phone: %q", out)
	}

	out = send(t, c.Addr(), "user sms alice")
	if !strings.Contains(out, "password reset SMS sent to 15550100001") {
		t.Fatalf("send SMS: %q", out)
	}
	msgs := smsSender.messages()
	if len(msgs) != 1 || msgs[0].to != "15550100001" || len(msgs[0].token) != 8 || msgs[0].ttl != 5*time.Minute {
		t.Fatalf("SMS = %+v", msgs)
	}
	if _, err := s.Accounts.ConsumePhoneReset("alice", msgs[0].token, "brand new secret"); err != nil {
		t.Fatalf("console SMS code does not reset the password: %v", err)
	}

	if out = send(t, c.Addr(), "user phone alice -"); !strings.Contains(out, "recovery phone cleared") {
		t.Fatalf("clear phone: %q", out)
	}
	if out = send(t, c.Addr(), "user sms alice"); !strings.Contains(out, "could not create SMS reset") {
		t.Fatalf("SMS without phone: %q", out)
	}

	plain := startConsole(t, Config{Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts})
	if out = send(t, plain.Addr(), "user phone bob 15550100002"); !strings.Contains(out, "bob recovery phone=15550100002") {
		t.Fatalf("set phone without SMS gateway: %q", out)
	}
	if out = send(t, plain.Addr(), "user sms bob"); !strings.Contains(out, "SMS recovery is not configured") {
		t.Fatalf("SMS without gateway: %q", out)
	}
}

func TestAccountRecoverWithoutSMTP(t *testing.T) {
	s := testutil.Start(t)
	if _, err := s.Accounts.CreateUserWithEmail("alice", "secret123", "alice@example.com"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	c := startConsole(t, Config{
		Registry: s.Registry, Relay: s.Relay, Accounts: s.Accounts,
		RecoveryURL: "https://chast.example/reset", RecoveryTTL: time.Minute,
	})
	out := send(t, c.Addr(), "user recover alice")
	for _, want := range []string{"deliver this token out of band", "token: ", "expires: ", "link: https://chast.example/reset?token="} {
		if !strings.Contains(out, want) {
			t.Fatalf("manual recovery missing %q:\n%s", want, out)
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
