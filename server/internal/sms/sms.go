// Package sms sends account recovery codes through a generic HTTP webhook.
// The deployment points it at its own SMS gateway, which receives one JSON
// POST per message: {"to", "text", "from"}. An optional bearer token
// authenticates the request, and a bounded timeout keeps a stuck gateway from
// holding a recovery request open.
package sms

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type Config struct {
	// URL is the gateway endpoint. It is required and must be http or https.
	URL string
	// Token enables "Authorization: Bearer <token>" when set.
	Token string
	// From is the optional sender ID passed to the gateway.
	From string
	// Timeout bounds the whole HTTP request. Zero defaults to 10s.
	Timeout time.Duration
	Logger  *slog.Logger
}

type Sender struct {
	endpoint string
	token    string
	from     string
	client   *http.Client
	logger   *slog.Logger
}

type message struct {
	To   string `json:"to"`
	Text string `json:"text"`
	From string `json:"from,omitempty"`
}

func New(cfg Config) (*Sender, error) {
	endpoint := strings.TrimSpace(cfg.URL)
	parsed, err := url.Parse(endpoint)
	if err != nil || parsed.Host == "" || (parsed.Scheme != "http" && parsed.Scheme != "https") {
		return nil, fmt.Errorf("SMS URL %q must be an http or https URL", cfg.URL)
	}
	token := strings.TrimSpace(cfg.Token)
	if strings.ContainsAny(token, "\r\n") {
		return nil, errors.New("SMS token must not contain line breaks")
	}
	from := strings.TrimSpace(cfg.From)
	if strings.ContainsAny(from, "\r\n") {
		return nil, errors.New("SMS sender must not contain line breaks")
	}
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	logger := cfg.Logger
	if logger == nil {
		logger = slog.Default()
	}
	return &Sender{
		endpoint: endpoint,
		token:    token,
		from:     from,
		client:   &http.Client{Timeout: timeout},
		logger:   logger,
	}, nil
}

// SendPasswordReset delivers a single-use recovery code. The code and account
// name are validated for control characters; the message stays ASCII so every
// gateway can encode it.
func (s *Sender) SendPasswordReset(to, username, code string, ttl time.Duration) error {
	to = strings.TrimSpace(to)
	if to == "" || strings.ContainsAny(to, "\r\n") {
		return fmt.Errorf("recipient %q is not a phone number", to)
	}
	if strings.ContainsAny(username, "\r\n") || strings.ContainsAny(code, "\r\n") {
		return errors.New("recovery message contained a line break")
	}
	text := fmt.Sprintf(
		"chast: password reset code %s for account %s, valid %s. "+
			"If you did not request it, ignore this message; the password is unchanged.",
		code, username, ttl.Round(time.Minute),
	)
	return s.deliver(message{To: to, Text: text, From: s.from})
}

// SendLoginCode delivers the second-factor code for a password login. It uses
// the same validation and gateway as SendPasswordReset so a deployment only
// configures one sender.
func (s *Sender) SendLoginCode(to, username, code string, ttl time.Duration) error {
	to = strings.TrimSpace(to)
	if to == "" || strings.ContainsAny(to, "\r\n") {
		return fmt.Errorf("recipient %q is not a phone number", to)
	}
	if strings.ContainsAny(username, "\r\n") || strings.ContainsAny(code, "\r\n") {
		return errors.New("login message contained a line break")
	}
	text := fmt.Sprintf(
		"chast: login code %s for account %s, valid %s. "+
			"If you did not try to sign in, change your password.",
		code, username, ttl.Round(time.Minute),
	)
	return s.deliver(message{To: to, Text: text, From: s.from})
}

func (s *Sender) deliver(msg message) error {
	payload, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("encode SMS message: %w", err)
	}
	req, err := http.NewRequest(http.MethodPost, s.endpoint, bytes.NewReader(payload))
	if err != nil {
		return fmt.Errorf("build SMS request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	if s.token != "" {
		req.Header.Set("Authorization", "Bearer "+s.token)
	}
	resp, err := s.client.Do(req)
	if err != nil {
		return fmt.Errorf("SMS gateway %s: %w", s.endpoint, err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return fmt.Errorf("SMS gateway %s returned %s: %s",
			s.endpoint, resp.Status, strings.TrimSpace(string(body)))
	}
	return nil
}
