package sms

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestNewRejectsBadConfig(t *testing.T) {
	for _, bad := range []string{"", "ftp://example.com/sms", "not a url", "http://"} {
		if _, err := New(Config{URL: bad}); err == nil {
			t.Fatalf("URL %q accepted", bad)
		}
	}
	if _, err := New(Config{URL: "http://example.com", Token: "a\r\nb"}); err == nil {
		t.Fatal("line break in token accepted")
	}
	if _, err := New(Config{URL: "http://example.com", From: "a\r\nb"}); err == nil {
		t.Fatal("line break in sender accepted")
	}
}

func TestSendPasswordReset(t *testing.T) {
	var got message
	var auth, contentType string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			t.Errorf("method = %s", r.Method)
		}
		auth = r.Header.Get("Authorization")
		contentType = r.Header.Get("Content-Type")
		if err := json.NewDecoder(r.Body).Decode(&got); err != nil {
			t.Errorf("decode body: %v", err)
		}
		w.WriteHeader(http.StatusAccepted)
	}))
	defer server.Close()
	sender, err := New(Config{
		URL: server.URL, Token: "gateway-token", From: "chast", Timeout: 2 * time.Second,
	})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendPasswordReset("15550100001", "alice", "12345678", 10*time.Minute); err != nil {
		t.Fatalf("send: %v", err)
	}
	if auth != "Bearer gateway-token" {
		t.Fatalf("authorization = %q", auth)
	}
	if contentType != "application/json" {
		t.Fatalf("content type = %q", contentType)
	}
	if got.To != "15550100001" || got.From != "chast" {
		t.Fatalf("message = %+v", got)
	}
	if !strings.Contains(got.Text, "12345678") || !strings.Contains(got.Text, "alice") ||
		!strings.Contains(got.Text, "10m0s") {
		t.Fatalf("text = %q", got.Text)
	}
}

func TestSendLoginCode(t *testing.T) {
	var got message
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if err := json.NewDecoder(r.Body).Decode(&got); err != nil {
			t.Errorf("decode body: %v", err)
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()
	sender, err := New(Config{URL: server.URL, From: "chast"})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendLoginCode("15550100001", "alice", "87654321", 5*time.Minute); err != nil {
		t.Fatalf("send login code: %v", err)
	}
	if got.To != "15550100001" || got.From != "chast" {
		t.Fatalf("message = %+v", got)
	}
	if !strings.Contains(got.Text, "login code 87654321") || !strings.Contains(got.Text, "alice") ||
		!strings.Contains(got.Text, "5m0s") {
		t.Fatalf("text = %q", got.Text)
	}
}

func TestSendRejectsControlCharacters(t *testing.T) {
	sender, err := New(Config{URL: "http://127.0.0.1:1"})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendPasswordReset("15550100001", "alice", "12\r\n34", time.Minute); err == nil {
		t.Fatal("line break in code accepted")
	}
	if err := sender.SendPasswordReset("1555\r\n0001", "alice", "12345678", time.Minute); err == nil {
		t.Fatal("line break in recipient accepted")
	}
	if err := sender.SendPasswordReset("", "alice", "12345678", time.Minute); err == nil {
		t.Fatal("empty recipient accepted")
	}
	if err := sender.SendLoginCode("15550100001", "alice", "12\r\n34", time.Minute); err == nil {
		t.Fatal("line break in login code accepted")
	}
	if err := sender.SendLoginCode("1555\r\n0001", "alice", "12345678", time.Minute); err == nil {
		t.Fatal("line break in login recipient accepted")
	}
}

func TestSendReportsGatewayFailure(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "quota exceeded", http.StatusTooManyRequests)
	}))
	defer server.Close()
	sender, err := New(Config{URL: server.URL})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	err = sender.SendPasswordReset("15550100001", "alice", "12345678", time.Minute)
	if err == nil || !strings.Contains(err.Error(), "429") || !strings.Contains(err.Error(), "quota exceeded") {
		t.Fatalf("gateway failure = %v", err)
	}
}

func TestSendTimesOut(t *testing.T) {
	block := make(chan struct{})
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-block
	}))
	defer server.Close()
	defer close(block)
	sender, err := New(Config{URL: server.URL, Timeout: 50 * time.Millisecond})
	if err != nil {
		t.Fatalf("new sender: %v", err)
	}
	if err := sender.SendPasswordReset("15550100001", "alice", "12345678", time.Minute); err == nil {
		t.Fatal("blocked gateway did not time out")
	}
}
