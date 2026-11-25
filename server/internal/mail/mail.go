// Package mail sends account recovery messages over SMTP. It deliberately
// supports only what a deployment needs for password resets: one recipient,
// optional STARTTLS or implicit TLS, optional PLAIN authentication, and a
// bounded timeout so a wedged relay cannot hold a request open.
package mail

import (
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"net/mail"
	"net/smtp"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

// TLS modes accepted by New. StartTLS is the default and requires the server
// to advertise it; there is no silent plaintext fallback.
const (
	TLSOff      = "none"
	TLSStart    = "starttls"
	TLSImplicit = "implicit"
)

type Config struct {
	// Addr is the SMTP server as host:port. It is required.
	Addr string
	// From is the bare envelope and header address. It is required.
	From string
	// User and Pass enable PLAIN authentication when both are set.
	User string
	Pass string
	// TLS selects none, starttls (default) or implicit.
	TLS string
	// Timeout bounds dialing, the TLS handshake and each SMTP command.
	Timeout time.Duration
	// Hostname is sent in EHLO. Empty defaults to localhost.
	Hostname string
	Logger   *slog.Logger
}

type Sender struct {
	cfg      Config
	host     string
	from     string
	timeout  time.Duration
	heloName string
	logger   *slog.Logger
}

func New(cfg Config) (*Sender, error) {
	addr := strings.TrimSpace(cfg.Addr)
	host, port, err := net.SplitHostPort(addr)
	if err != nil || host == "" || port == "" {
		return nil, fmt.Errorf("SMTP address %q must be host:port", cfg.Addr)
	}
	from, err := mail.ParseAddress(strings.TrimSpace(cfg.From))
	if err != nil || from.Name != "" || from.Address == "" {
		return nil, fmt.Errorf("SMTP from %q must be a bare email address", cfg.From)
	}
	user, pass := strings.TrimSpace(cfg.User), cfg.Pass
	if (user == "") != (pass == "") {
		return nil, errors.New("SMTP user and password must be given together")
	}
	mode := strings.ToLower(strings.TrimSpace(cfg.TLS))
	if mode == "" {
		mode = TLSStart
	}
	switch mode {
	case TLSOff, TLSStart, TLSImplicit:
	default:
		return nil, fmt.Errorf("SMTP TLS mode %q must be none, starttls or implicit", cfg.TLS)
	}
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	helo := strings.TrimSpace(cfg.Hostname)
	if helo == "" {
		helo = "localhost"
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}
	return &Sender{
		cfg: Config{
			Addr: addr, From: from.Address, User: user, Pass: pass,
			TLS: mode, Timeout: timeout, Hostname: helo, Logger: logger,
		},
		host: host, from: from.Address, timeout: timeout,
		heloName: helo, logger: logger,
	}, nil
}

// SendPasswordReset delivers a single-use recovery token. username and token
// are checked for header injection; link is an optional browser URL that
// already carries the token.
func (s *Sender) SendPasswordReset(to, username, token, link string, ttl time.Duration) error {
	recipient, err := mail.ParseAddress(strings.TrimSpace(to))
	if err != nil || recipient.Name != "" || recipient.Address == "" {
		return fmt.Errorf("recipient %q is not a bare email address", to)
	}
	if strings.ContainsAny(username, "\r\n") || strings.ContainsAny(token, "\r\n") {
		return errors.New("recovery message contained a line break")
	}
	if strings.ContainsAny(link, "\r\n") {
		return errors.New("recovery link contained a line break")
	}
	subject := "chast password reset"
	body := fmt.Sprintf(
		"A password reset was requested for the chast account %q.\r\n\r\n"+
			"Use this single-use code within %s:\r\n\r\n%s\r\n\r\n"+
			"If you did not request it, ignore this message; the password is unchanged.\r\n",
		username, ttl.Round(time.Minute), token,
	)
	if link != "" {
		body += "\r\nOpen this link to continue:\r\n" + link + "\r\n"
	}
	msg := s.headers(recipient.Address, subject) + body
	return s.deliver(recipient.Address, msg)
}

// SendLoginCode delivers the second-factor code for a password login. It uses
// the same validation and transport as SendPasswordReset so a deployment only
// configures one sender.
func (s *Sender) SendLoginCode(to, username, code string, ttl time.Duration) error {
	recipient, err := mail.ParseAddress(strings.TrimSpace(to))
	if err != nil || recipient.Name != "" || recipient.Address == "" {
		return fmt.Errorf("recipient %q is not a bare email address", to)
	}
	if strings.ContainsAny(username, "\r\n") || strings.ContainsAny(code, "\r\n") {
		return errors.New("login message contained a line break")
	}
	subject := "chast login code"
	body := fmt.Sprintf(
		"A sign-in to the chast account %q requires a second factor.\r\n\r\n"+
			"Use this single-use code within %s:\r\n\r\n%s\r\n\r\n"+
			"If you did not try to sign in, change your password.\r\n",
		username, ttl.Round(time.Minute), code,
	)
	return s.deliver(recipient.Address, s.headers(recipient.Address, subject)+body)
}

// SendEmailVerification delivers the single-use token that marks the saved
// recovery address as locally verified.
func (s *Sender) SendEmailVerification(to, username, token string, ttl time.Duration) error {
	recipient, err := mail.ParseAddress(strings.TrimSpace(to))
	if err != nil || recipient.Name != "" || recipient.Address == "" {
		return fmt.Errorf("recipient %q is not a bare email address", to)
	}
	if strings.ContainsAny(username, "\r\n") || strings.ContainsAny(token, "\r\n") {
		return errors.New("verification message contained a line break")
	}
	subject := "chast email verification"
	body := fmt.Sprintf(
		"A verification was requested for the chast account %q.\r\n\r\n"+
			"Enter this single-use token within %s to confirm ownership of this address:\r\n\r\n%s\r\n\r\n"+
			"If you did not request it, ignore this message.\r\n",
		username, ttl.Round(time.Minute), token,
	)
	return s.deliver(recipient.Address, s.headers(recipient.Address, subject)+body)
}

// SendAccountInvitation sends the single-use code that creates an ordinary
// viewer account. The recipient email becomes verified when the code is used.
func (s *Sender) SendAccountInvitation(to, token, link string, ttl time.Duration) error {
	recipient, err := mail.ParseAddress(strings.TrimSpace(to))
	if err != nil || recipient.Name != "" || recipient.Address == "" {
		return fmt.Errorf("recipient %q is not a bare email address", to)
	}
	if strings.ContainsAny(token+link, "\r\n") {
		return errors.New("account invitation message contained a line break")
	}
	subject := "chast account invitation"
	body := fmt.Sprintf(
		"You have been invited to create a chast viewer account.\r\n\r\n"+
			"Use this single-use invitation code within %s:\r\n\r\n%s\r\n\r\n"+
			"The account will start with viewer access.\r\n",
		ttl.Round(time.Minute), token,
	)
	if link != "" {
		body += "\r\nOpen this page to accept the invitation:\r\n" + link + "\r\n"
	} else {
		body += "\r\nOpen the chast server invitation page and enter the code above.\r\n"
	}
	return s.deliver(recipient.Address, s.headers(recipient.Address, subject)+body)
}

// SendSecurityAlarm sends the useful audit context without including free-form
// audit detail, which can contain values that should remain in the console.
func (s *Sender) SendSecurityAlarm(to string, alarm account.AlarmEvent) error {
	recipient, err := mail.ParseAddress(strings.TrimSpace(to))
	if err != nil || recipient.Name != "" || recipient.Address == "" {
		return fmt.Errorf("recipient %q is not a bare email address", to)
	}
	for _, value := range []string{alarm.Category, alarm.Severity, alarm.Code,
		alarm.Summary, alarm.Username, alarm.HostID} {
		if strings.ContainsAny(value, "\r\n") {
			return errors.New("security alarm message contained a line break")
		}
	}
	body := fmt.Sprintf(
		"A security alarm was recorded by the chast server.\r\n\r\n"+
			"Category: %s\r\nSeverity: %s\r\nEvent: %s\r\nSummary: %s\r\n"+
			"Account: %s\r\nHost: %s\r\nTime (UTC): %s\r\n\r\n"+
			"Sign in to the management console to review the event.",
		alarm.Category, alarm.Severity, alarm.Code, alarm.Summary,
		alarm.Username, alarm.HostID, alarm.CreatedAt.UTC().Format(time.RFC3339),
	)
	return s.deliver(recipient.Address, s.headers(recipient.Address, "chast security alert: "+alarm.Code)+body)
}

func (s *Sender) headers(to, subject string) string {
	id := make([]byte, 12)
	_, _ = rand.Read(id)
	return "From: " + s.from + "\r\n" +
		"To: " + to + "\r\n" +
		"Subject: " + subject + "\r\n" +
		"Date: " + time.Now().UTC().Format(time.RFC1123Z) + "\r\n" +
		"Message-ID: <" + hex.EncodeToString(id) + "@" + s.heloName + ">\r\n" +
		"MIME-Version: 1.0\r\n" +
		"Content-Type: text/plain; charset=UTF-8\r\n" +
		"Content-Transfer-Encoding: 8bit\r\n" +
		"\r\n"
}

func (s *Sender) deliver(to, msg string) error {
	dialer := net.Dialer{Timeout: s.timeout}
	conn, err := dialer.Dial("tcp", s.cfg.Addr)
	if err != nil {
		return fmt.Errorf("SMTP dial %s: %w", s.cfg.Addr, err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(s.timeout))
	tlsConfig := &tls.Config{ServerName: s.host, MinVersion: tls.VersionTLS12}
	if s.cfg.TLS == TLSImplicit {
		secure := tls.Client(conn, tlsConfig)
		if err := secure.Handshake(); err != nil {
			return fmt.Errorf("SMTP TLS handshake: %w", err)
		}
		conn = secure
	}
	client, err := smtp.NewClient(conn, s.host)
	if err != nil {
		return fmt.Errorf("SMTP greeting: %w", err)
	}
	defer client.Close()
	if s.cfg.TLS == TLSStart {
		if err := client.StartTLS(tlsConfig); err != nil {
			return fmt.Errorf("SMTP STARTTLS: %w", err)
		}
	}
	if s.cfg.User != "" {
		auth := smtp.PlainAuth("", s.cfg.User, s.cfg.Pass, s.host)
		if err := client.Auth(auth); err != nil {
			return fmt.Errorf("SMTP authentication: %w", err)
		}
	}
	if err := client.Mail(s.from); err != nil {
		return fmt.Errorf("SMTP MAIL FROM: %w", err)
	}
	if err := client.Rcpt(to); err != nil {
		return fmt.Errorf("SMTP RCPT TO: %w", err)
	}
	writer, err := client.Data()
	if err != nil {
		return fmt.Errorf("SMTP DATA: %w", err)
	}
	if _, err := writer.Write([]byte(msg)); err != nil {
		return fmt.Errorf("SMTP body: %w", err)
	}
	if err := writer.Close(); err != nil {
		return fmt.Errorf("SMTP body: %w", err)
	}
	if err := client.Quit(); err != nil {
		return fmt.Errorf("SMTP QUIT: %w", err)
	}
	return nil
}
