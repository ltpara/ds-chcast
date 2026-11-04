package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

// The account unit tests reopen the SQLite file in-process. This probe runs the
// real binary instead: it proves that a refresh token, its rotation and the
// revocation left by a replay all survive a process exit, which is the part a
// store-level test cannot cover.

type sessionReply struct {
	Token              string `json:"token"`
	ExpiresInMS        int64  `json:"expires_in_ms"`
	RefreshToken       string `json:"refresh_token"`
	RefreshExpiresInMS int64  `json:"refresh_expires_in_ms"`
	User               struct {
		Username string `json:"username"`
	} `json:"user"`
}

type serverProcess struct {
	cmd   *exec.Cmd
	base  string
	admin string
	token string
	log   bytes.Buffer
	done  bool
}

func (s *serverProcess) stop() {
	if s.done {
		return
	}
	s.done = true
	_ = s.cmd.Process.Kill()
	_ = s.cmd.Wait()
}

func (s *serverProcess) logText() string { return s.log.String() }

func TestRefreshSessionSurvivesProcessRestart(t *testing.T) {
	if testing.Short() {
		t.Skip("builds and restarts the server binary")
	}
	bin := filepath.Join(t.TempDir(), "chast-server-restart")
	if runtime.GOOS == "windows" {
		bin += ".exe"
	}
	build := exec.Command("go", "build", "-o", bin, ".")
	var buildLog bytes.Buffer
	build.Stdout, build.Stderr = &buildLog, &buildLog
	if err := build.Run(); err != nil {
		t.Fatalf("build chast-server: %v\n%s", err, buildLog.String())
	}

	dir := t.TempDir()
	args := []string{
		"--state", filepath.Join(dir, "state.db"),
		"--identity", filepath.Join(dir, "identity.key"),
		"--advertise", "127.0.0.1",
		"--punch-listen", "127.0.0.1:0",
		"--relay-listen", "127.0.0.1",
		"--relay-ports", "47000-47002",
		"--carrier-listen", "",
		"--env-file", "",
		"--insecure-no-server-key",
		"--test-punch", "no",
		"--api-rate", "0",
	}

	first := startServer(t, bin, args)
	if out := adminCommand(t, first, "user add probe probe-pass-1"); !strings.Contains(out, "probe created") {
		t.Fatalf("user add replied %q\n%s", out, first.logText())
	}
	loginPair := loginAs(t, first, "probe", "probe-pass-1")
	first.stop()

	second := startServer(t, bin, args)
	if status, body := getAccount(t, second, loginPair.Token); status != http.StatusOK {
		t.Fatalf("access token after restart = %d %s\n%s", status, body, second.logText())
	}
	rotated := refreshSession(t, second, loginPair.RefreshToken)
	second.stop()

	third := startServer(t, bin, args)
	latest := refreshSession(t, third, rotated.RefreshToken)
	third.stop()

	fourth := startServer(t, bin, args)
	// Spend the replacement first: a retired token whose replacement was
	// never used is the lost-response retry, not a replay.
	newest := refreshSession(t, fourth, latest.RefreshToken)
	if status, body := refreshSessionReplay(t, fourth, rotated.RefreshToken); status != http.StatusUnauthorized {
		t.Fatalf("replayed refresh after restart = %d %s\n%s", status, body, fourth.logText())
	}
	if status, body := getAccount(t, fourth, newest.Token); status != http.StatusUnauthorized {
		t.Fatalf("session survived replay revocation across restart = %d %s\n%s", status, body, fourth.logText())
	}
}

func startServer(t *testing.T, bin string, args []string) *serverProcess {
	t.Helper()
	httpPort := freeTCPPort(t)
	adminPort := freeTCPPort(t)
	const adminToken = "restart-test-token"
	full := append([]string{
		"--http-listen", fmt.Sprintf("127.0.0.1:%d", httpPort),
		"--admin-listen", fmt.Sprintf("127.0.0.1:%d", adminPort),
		"--admin-token", adminToken,
	}, args...)
	cmd := exec.Command(bin, full...)
	s := &serverProcess{
		cmd:   cmd,
		base:  fmt.Sprintf("http://127.0.0.1:%d", httpPort),
		admin: fmt.Sprintf("127.0.0.1:%d", adminPort),
		token: adminToken,
	}
	cmd.Stdout, cmd.Stderr = &s.log, &s.log
	if err := cmd.Start(); err != nil {
		t.Fatalf("start chast-server: %v", err)
	}
	t.Cleanup(func() {
		s.stop()
		if t.Failed() {
			t.Logf("chast-server log:\n%s", s.logText())
		}
	})
	waitReady(t, s)
	return s
}

func waitReady(t *testing.T, s *serverProcess) {
	t.Helper()
	client := &http.Client{Timeout: 2 * time.Second}
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", s.admin, 200*time.Millisecond)
		if err == nil {
			conn.Close()
			resp, err := client.Get(s.base + "/v1/stats")
			if err == nil {
				resp.Body.Close()
				return
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("chast-server did not become ready:\n%s", s.logText())
}

func freeTCPPort(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("pick port: %v", err)
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port
}

func adminCommand(t *testing.T, s *serverProcess, line string) string {
	t.Helper()
	conn, err := net.DialTimeout("tcp", s.admin, 2*time.Second)
	if err != nil {
		t.Fatalf("admin dial: %v\n%s", err, s.logText())
	}
	defer conn.Close()
	if _, err := io.WriteString(conn, "auth "+s.token+"\n"+line+"\n"); err != nil {
		t.Fatalf("admin write: %v\n%s", err, s.logText())
	}
	if err := conn.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		t.Fatalf("admin deadline: %v", err)
	}
	reply, err := io.ReadAll(conn)
	if err != nil {
		t.Fatalf("admin reply: %v\n%s", err, s.logText())
	}
	return string(reply)
}

func loginAs(t *testing.T, s *serverProcess, username, password string) sessionReply {
	t.Helper()
	body, err := json.Marshal(map[string]string{"username": username, "password": password})
	if err != nil {
		t.Fatal(err)
	}
	status, data := postJSON(t, s.base+"/v1/account/login", string(body))
	if status != http.StatusOK {
		t.Fatalf("login: %d %s\n%s", status, data, s.logText())
	}
	var reply sessionReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("login reply: %v (%s)", err, data)
	}
	if reply.Token == "" || reply.RefreshToken == "" || reply.User.Username != username {
		t.Fatalf("unexpected login reply: %s", data)
	}
	return reply
}

func refreshSession(t *testing.T, s *serverProcess, token string) sessionReply {
	t.Helper()
	status, data := refreshSessionReplay(t, s, token)
	if status != http.StatusOK {
		t.Fatalf("refresh: %d %s\n%s", status, data, s.logText())
	}
	var reply sessionReply
	if err := json.Unmarshal(data, &reply); err != nil {
		t.Fatalf("refresh reply: %v (%s)", err, data)
	}
	if reply.Token == "" || reply.RefreshToken == "" {
		t.Fatalf("refresh reply lacks tokens: %s", data)
	}
	return reply
}

func refreshSessionReplay(t *testing.T, s *serverProcess, token string) (int, []byte) {
	t.Helper()
	body, err := json.Marshal(map[string]string{"refresh_token": token})
	if err != nil {
		t.Fatal(err)
	}
	return postJSON(t, s.base+"/v1/account/refresh", string(body))
}

func getAccount(t *testing.T, s *serverProcess, token string) (int, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, s.base+"/v1/account", nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("GET /v1/account: %v\n%s", err, s.logText())
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read /v1/account: %v", err)
	}
	return resp.StatusCode, data
}

func postJSON(t *testing.T, url, body string) (int, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, url, strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	client := &http.Client{Timeout: 5 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("POST %s: %v", url, err)
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read %s: %v", url, err)
	}
	return resp.StatusCode, data
}
