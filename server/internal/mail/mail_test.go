package mail

import (
	"bufio"
	"encoding/base64"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type fakeSMTP struct {
	addr     string
	auth     bool
	messages chan string
	auths    chan string
}

// startFakeSMTP serves one SMTP session with optional AUTH PLAIN. It records
// the DATA body and the base64 AUTH argument for assertions.
func startFakeSMTP(t *testing.T, auth bool) *fakeSMTP {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	server := &fakeSMTP{
		addr: listener.Addr().String(), auth: auth,
		messages: make(chan string, 1), auths: make(chan string, 1),
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		reader := bufio.NewReader(conn)
		write := func(line string) { _, _ = io.WriteString(conn, line+"\r\n") }
		write("220 fake ESMTP ready")
		for {
			line, err := reader.ReadString('\n')
			if err != nil {
				return
			}
			line = strings.TrimRight(line, "\r\n")
			upper := strings.ToUpper(line)
			switch {
			case strings.HasPrefix(upper, "EHLO "):
				write("250-fake")
				if auth {
					write("250-AUTH PLAIN")
				}
				write("250 OK")
			case strings.HasPrefix(upper, "HELO "):
				write("250 fake")
			case strings.HasPrefix(upper, "AUTH PLAIN "):
				server.auths <- strings.TrimSpace(line[len("AUTH PLAIN "):])
				write("235 2.7.0 authentication succeeded")
			case strings.HasPrefix(upper, "MAIL FROM:"), strings.HasPrefix(upper, "RCPT TO:"):
				write("250 OK")
			case upper == "DATA":
				write("354 end with .")
				var body strings.Builder
				for {
					row, err := reader.ReadString('\n')
					if err != nil {
						return
					}
					row = strings.TrimRight(row, "\r\n")
					if row == "." {
						break
					}
					if strings.HasPrefix(row, "..") {
						row = row[1:]
					}
					body.WriteString(row + "\n")
				}
				server.messages <- body.String()
				write("250 queued")
			case upper == "QUIT":
				write("221 bye")
				return
			default:
				write("250 OK")
			}
		}
	}()
	return server
}

func TestSendPasswordReset(t *testing.T) {
	server := startFakeSMTP(t, true)
	sender, err := New(Config{
		Addr: server.addr, From: "chast@example.com",
		User: "chast", Pass: "smtp-secret", TLS: TLSOff,
	})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	link := "https://chast.example.com/reset?token=reset-token-1"
	if err := sender.SendPasswordReset(
		"alice@example.com", "alice", "reset-token-1", link, 30*time.Minute,
	); err != nil {
		t.Fatalf("send: %v", err)
	}
	select {
	case body := <-server.messages:
		for _, want := range []string{
			"To: alice@example.com",
			"From: chast@example.com",
			"Subject: chast password reset",
			`account "alice"`,
			"reset-token-1",
			link,
			"30m0s",
		} {
			if !strings.Contains(body, want) {
				t.Fatalf("message missing %q:\n%s", want, body)
			}
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no SMTP message")
	}
	select {
	case encoded := <-server.auths:
		raw, err := base64.StdEncoding.DecodeString(encoded)
		if err != nil || string(raw) != "\x00chast\x00smtp-secret" {
			t.Fatalf("AUTH PLAIN = %q (%v)", string(raw), err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no AUTH command")
	}
}

func TestSendLoginCode(t *testing.T) {
	server := startFakeSMTP(t, false)
	sender, err := New(Config{Addr: server.addr, From: "chast@example.com", TLS: TLSOff})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendLoginCode(
		"alice@example.com", "alice", "1234 5678", 10*time.Minute,
	); err != nil {
		t.Fatalf("send: %v", err)
	}
	select {
	case body := <-server.messages:
		for _, want := range []string{
			"To: alice@example.com",
			"Subject: chast login code",
			`account "alice"`,
			"1234 5678",
			"10m0s",
			"change your password",
		} {
			if !strings.Contains(body, want) {
				t.Fatalf("message missing %q:\n%s", want, body)
			}
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no SMTP message")
	}
}

func TestSendEmailVerification(t *testing.T) {
	server := startFakeSMTP(t, false)
	sender, err := New(Config{Addr: server.addr, From: "chast@example.com", TLS: TLSOff})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendEmailVerification(
		"alice@example.com", "alice", "verify-token-1", 30*time.Minute,
	); err != nil {
		t.Fatalf("send: %v", err)
	}
	select {
	case body := <-server.messages:
		for _, want := range []string{
			"To: alice@example.com",
			"Subject: chast email verification",
			`account "alice"`,
			"verify-token-1",
			"30m0s",
		} {
			if !strings.Contains(body, want) {
				t.Fatalf("message missing %q:\n%s", want, body)
			}
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no SMTP message")
	}
}

func TestSendAccountInvitation(t *testing.T) {
	server := startFakeSMTP(t, false)
	sender, err := New(Config{Addr: server.addr, From: "chast@example.com", TLS: TLSOff})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	link := "https://console.example.com/admin/invite.html#token=invite-code"
	if err := sender.SendAccountInvitation(
		"new.person@example.com", "invite-code", link, 48*time.Hour,
	); err != nil {
		t.Fatalf("send: %v", err)
	}
	select {
	case body := <-server.messages:
		for _, want := range []string{
			"To: new.person@example.com",
			"Subject: chast account invitation",
			"invite-code",
			link,
			"48h0m0s",
			"viewer access",
		} {
			if !strings.Contains(body, want) {
				t.Fatalf("message missing %q:\n%s", want, body)
			}
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no SMTP message")
	}
}

func TestSendSecurityAlarm(t *testing.T) {
	server := startFakeSMTP(t, false)
	sender, err := New(Config{Addr: server.addr, From: "chast@example.com", TLS: TLSOff})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	alarm := account.AlarmEvent{
		ID: 7, CreatedAt: time.Date(2026, 11, 24, 12, 0, 0, 0, time.UTC),
		Username: "alice", HostID: "desk-01", Category: "security", Severity: "warning",
		Code: "host_access_denied", Summary: "主机按访问策略拒绝了连接",
		Detail: "private audit payload",
	}
	if err := sender.SendSecurityAlarm("alice@example.com", alarm); err != nil {
		t.Fatalf("send security alarm: %v", err)
	}
	select {
	case body := <-server.messages:
		for _, want := range []string{
			"To: alice@example.com", "Subject: chast security alert: host_access_denied",
			"Category: security", "Severity: warning", "Summary: 主机按访问策略拒绝了连接",
			"Account: alice", "Host: desk-01", "2026-11-24T12:00:00Z",
		} {
			if !strings.Contains(body, want) {
				t.Fatalf("message missing %q:\n%s", want, body)
			}
		}
		if strings.Contains(body, alarm.Detail) {
			t.Fatalf("free-form audit detail leaked into email:\n%s", body)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no SMTP message")
	}
	if err := sender.SendSecurityAlarm("alice@example.com", account.AlarmEvent{Code: "bad\r\nBcc: x"}); err == nil {
		t.Fatal("alarm header injection accepted")
	}
}

func TestSendRejectsHeaderInjection(t *testing.T) {
	server := startFakeSMTP(t, false)
	sender, err := New(Config{Addr: server.addr, From: "chast@example.com", TLS: TLSOff})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendPasswordReset("alice@example.com", "alice\r\nBcc: x", "token", "", time.Minute); err == nil {
		t.Fatal("injected username accepted")
	}
	if err := sender.SendPasswordReset("alice@example.com", "alice", "tok\r\nen", "", time.Minute); err == nil {
		t.Fatal("injected token accepted")
	}
	if err := sender.SendPasswordReset("not-an-address", "alice", "token", "", time.Minute); err == nil {
		t.Fatal("bad recipient accepted")
	}
}

func TestNewValidation(t *testing.T) {
	cases := []Config{
		{From: "chast@example.com"},
		{Addr: "127.0.0.1:25"},
		{Addr: "127.0.0.1:25", From: "Chast <chast@example.com>"},
		{Addr: "127.0.0.1:25", From: "chast@example.com", User: "chast"},
		{Addr: "127.0.0.1:25", From: "chast@example.com", TLS: "ssl"},
	}
	for _, cfg := range cases {
		if _, err := New(cfg); err == nil {
			t.Fatalf("config %+v accepted", cfg)
		}
	}
	sender, err := New(Config{Addr: "127.0.0.1:25", From: "chast@example.com"})
	if err != nil || sender.cfg.TLS != TLSStart || sender.timeout != 10*time.Second {
		t.Fatalf("defaults = %+v, %v", sender, err)
	}
}
