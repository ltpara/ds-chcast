package relay

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type testClock struct {
	t time.Time
}

func (c *testClock) now() time.Time          { return c.t }
func (c *testClock) advance(d time.Duration) { c.t = c.t.Add(d) }
func newTestPolicy(t *testing.T, cfg PolicyConfig) *Policy {
	t.Helper()
	p, err := newPolicy(cfg)
	if err != nil {
		t.Fatalf("policy: %v", err)
	}
	return p
}

func TestPolicyDropLimitsSession(t *testing.T) {
	clock := &testClock{t: time.Unix(0, 0)}
	p := newTestPolicy(t, PolicyConfig{SingleBandwidthBps: 8000})
	p.now = clock.now
	s, err := p.Open("udp:1", "203.0.113.5")
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	const payload = 60000
	if allowed, err := s.Drop(payload); err != nil || !allowed {
		t.Fatalf("first datagram: allowed=%t err=%v", allowed, err)
	}
	if allowed, err := s.Drop(payload); err != nil || allowed {
		t.Fatalf("second datagram: allowed=%t err=%v", allowed, err)
	}
	stats := p.Stats()
	if stats.RateDroppedDatagrams != 1 || stats.RateDroppedBytes != payload || stats.LimitedBytes != payload {
		t.Fatalf("counters: %+v", stats)
	}
	// 60s at 8 kbit/s refills the 65535-byte bucket.
	clock.advance(60 * time.Second)
	if allowed, err := s.Drop(payload); err != nil || !allowed {
		t.Fatalf("refilled datagram: allowed=%t err=%v", allowed, err)
	}
	usage := p.Usage()
	if len(usage) != 1 || usage[0].Key != "udp:1" || usage[0].IP != "203.0.113.5" {
		t.Fatalf("usage: %+v", usage)
	}
	if usage[0].TotalBytes != payload*2 {
		t.Fatalf("usage bytes = %d", usage[0].TotalBytes)
	}
}

func TestPolicyTotalBandwidthIsShared(t *testing.T) {
	clock := &testClock{t: time.Unix(0, 0)}
	p := newTestPolicy(t, PolicyConfig{SingleBandwidthBps: 0, TotalBandwidthBps: 8000})
	p.now = clock.now
	first, err := p.Open("udp:1", "203.0.113.5")
	if err != nil {
		t.Fatalf("open first: %v", err)
	}
	second, err := p.Open("udp:2", "198.51.100.7")
	if err != nil {
		t.Fatalf("open second: %v", err)
	}
	if allowed, _ := first.Drop(60000); !allowed {
		t.Fatal("first session was limited by its own empty bucket")
	}
	if allowed, _ := second.Drop(60000); allowed {
		t.Fatal("the total bucket was not shared between sessions")
	}
	if stats := p.Stats(); stats.RateDroppedDatagrams != 1 {
		t.Fatalf("rate drops = %d", stats.RateDroppedDatagrams)
	}
}

func TestPolicyTakeWaitsForRoom(t *testing.T) {
	p := newTestPolicy(t, PolicyConfig{SingleBandwidthBps: 8 * 1024 * 1024})
	s, err := p.Open("udp:1", "203.0.113.5")
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	p.mu.Lock()
	s.bucket.tokens = 0
	s.bucket.last = time.Now()
	p.mu.Unlock()

	start := time.Now()
	if err := s.Take(5000); err != nil {
		t.Fatalf("take: %v", err)
	}
	if elapsed := time.Since(start); elapsed < 3*time.Millisecond {
		t.Fatalf("take returned after %s without waiting", elapsed)
	}
	if stats := p.Stats(); stats.LimitedBytes != 5000 {
		t.Fatalf("limited bytes = %d", stats.LimitedBytes)
	}
}

func TestPolicyBlocklistLifecycle(t *testing.T) {
	dir := t.TempDir()
	blockFile := filepath.Join(dir, "blocklist.txt")
	if err := os.WriteFile(blockFile, []byte("# relay blocklist\n203.0.113.9 comment\n"), 0o644); err != nil {
		t.Fatalf("write blocklist: %v", err)
	}
	p := newTestPolicy(t, PolicyConfig{BlocklistFile: blockFile})
	if !p.Blocked("203.0.113.9") {
		t.Fatal("the blocklist file was not loaded")
	}
	if _, err := p.Open("udp:1", "203.0.113.9"); !errors.Is(err, ErrBlocked) {
		t.Fatalf("blocked open = %v", err)
	}
	if stats := p.Stats(); stats.BlockedSessions != 1 {
		t.Fatalf("blocked sessions = %d", stats.BlockedSessions)
	}

	if reply, ok := p.Command("Br 203.0.113.9"); !ok || !strings.HasPrefix(reply, "removed 1") {
		t.Fatalf("Br reply = %q", reply)
	}
	session, err := p.Open("udp:1", "203.0.113.9")
	if err != nil {
		t.Fatalf("open after unblock: %v", err)
	}
	if _, ok := p.Command("Ba 203.0.113.9"); !ok {
		t.Fatal("Ba was not handled")
	}
	data, err := os.ReadFile(blockFile)
	if err != nil || !strings.Contains(string(data), "203.0.113.9") {
		t.Fatalf("blocked address was not persisted: %q %v", data, err)
	}
	p.mu.Lock()
	session.lastListCheck = time.Now().Add(-2 * listCheckInterval)
	p.mu.Unlock()
	if err := session.Take(1024); !errors.Is(err, ErrBlocked) {
		t.Fatalf("blocked session take = %v", err)
	}
}

func TestPolicyDowngradesSustainedSession(t *testing.T) {
	clock := &testClock{t: time.Unix(0, 0)}
	p := newTestPolicy(t, PolicyConfig{
		SingleBandwidthBps: 8000,
		BlacklistSpeedBps:  16000,
		DowngradeThreshold: 0.5,
		DowngradeAfter:     10 * time.Second,
	})
	p.now = clock.now
	s, err := p.Open("udp:1", "203.0.113.5")
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	if allowed, _ := s.Drop(60000); !allowed {
		t.Fatal("initial burst was dropped")
	}
	clock.advance(11 * time.Second)
	if allowed, _ := s.Drop(100); !allowed {
		t.Fatal("post-burst datagram was dropped")
	}
	if stats := p.Stats(); stats.DowngradedSessions != 1 {
		t.Fatalf("downgraded sessions = %d", stats.DowngradedSessions)
	}
	p.mu.Lock()
	rate := s.rateLocked()
	p.mu.Unlock()
	if rate != p.blacklistBps {
		t.Fatalf("downgraded rate = %d, want %d", rate, p.blacklistBps)
	}
	usage := p.Usage()
	if len(usage) != 1 || !usage[0].Downgraded {
		t.Fatalf("usage: %+v", usage)
	}
}

func TestPolicyBlacklistedSessionUsesSlowTier(t *testing.T) {
	dir := t.TempDir()
	blackFile := filepath.Join(dir, "blacklist.txt")
	if err := os.WriteFile(blackFile, []byte("203.0.113.10\n"), 0o644); err != nil {
		t.Fatalf("write blacklist: %v", err)
	}
	p := newTestPolicy(t, PolicyConfig{
		SingleBandwidthBps: 8 * 1024 * 1024,
		BlacklistSpeedBps:  16000,
		BlacklistFile:      blackFile,
	})
	s, err := p.Open("udp:1", "203.0.113.10")
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	p.mu.Lock()
	rate := s.rateLocked()
	p.mu.Unlock()
	if rate != 16000 {
		t.Fatalf("blacklisted rate = %d", rate)
	}
	usage := p.Usage()
	if len(usage) != 1 || !usage[0].Blacklisted {
		t.Fatalf("usage: %+v", usage)
	}
}

func TestPolicyConsoleCommands(t *testing.T) {
	dir := t.TempDir()
	blackFile := filepath.Join(dir, "blacklist.txt")
	blockFile := filepath.Join(dir, "blocklist.txt")
	p := newTestPolicy(t, PolicyConfig{
		SingleBandwidthBps: DefaultSingleBandwidthBps,
		TotalBandwidthBps:  DefaultTotalBandwidthBps,
		BlacklistSpeedBps:  DefaultBlacklistSpeedBps,
		DowngradeThreshold: DefaultDowngradeThreshold,
		DowngradeAfter:     DefaultDowngradeAfter,
		BlacklistFile:      blackFile,
		BlocklistFile:      blockFile,
	})

	if reply, ok := p.Command("ba 203.0.113.9 198.51.100.7"); !ok || !strings.HasPrefix(reply, "added 2") {
		t.Fatalf("ba = %q", reply)
	}
	if reply, _ := p.Command("b"); !strings.Contains(reply, "203.0.113.9") || !strings.Contains(reply, "198.51.100.7") {
		t.Fatalf("b = %q", reply)
	}
	if reply, _ := p.Command("b 203.0.113.9"); reply != "true\n" {
		t.Fatalf("b lookup = %q", reply)
	}
	if reply, _ := p.Command("br all"); !strings.HasPrefix(reply, "removed 2") {
		t.Fatalf("br = %q", reply)
	}
	if p.Blacklisted("203.0.113.9") {
		t.Fatal("blacklist was not cleared")
	}

	if reply, _ := p.Command("sb 64"); reply != "64.00Mb/s\n" {
		t.Fatalf("sb = %q", reply)
	}
	if p.Stats().SingleBandwidthBps != 64*1024*1024 {
		t.Fatalf("single bandwidth = %d", p.Stats().SingleBandwidthBps)
	}
	if reply, _ := p.Command("tb 0"); reply != "unlimited\n" {
		t.Fatalf("tb = %q", reply)
	}
	if reply, _ := p.Command("ls 16.5"); reply != "16.50Mb/s\n" {
		t.Fatalf("ls = %q", reply)
	}
	if reply, _ := p.Command("dt 0.5"); reply != "0.50\n" {
		t.Fatalf("dt = %q", reply)
	}
	if reply, _ := p.Command("t 60"); reply != "60s\n" {
		t.Fatalf("t = %q", reply)
	}
	if reply, _ := p.Command("sb bad"); !strings.HasPrefix(reply, "usage:") {
		t.Fatalf("bad sb = %q", reply)
	}
	if _, ok := p.Command("unknown-command"); ok {
		t.Fatal("unknown command was claimed")
	}
	if reply, _ := p.Command("u"); reply != "no relay sessions\n" {
		t.Fatalf("u = %q", reply)
	}
	if reply, ok := p.Command("h"); !ok || !strings.Contains(reply, "single-bandwidth") {
		t.Fatalf("h = %q", reply)
	}
}

func TestPolicyConfigValidation(t *testing.T) {
	if _, err := newPolicy(PolicyConfig{SingleBandwidthBps: -1}); err == nil {
		t.Fatal("negative bandwidth was accepted")
	}
	if _, err := newPolicy(PolicyConfig{DowngradeThreshold: 1.5}); err == nil {
		t.Fatal("threshold above one was accepted")
	}
	if _, err := newPolicy(PolicyConfig{DowngradeAfter: -time.Second}); err == nil {
		t.Fatal("negative downgrade start was accepted")
	}
	if _, err := newPolicy(PolicyConfig{BlocklistFile: filepath.Join(t.TempDir(), "missing")}); err != nil {
		t.Fatalf("missing list file should be empty: %v", err)
	}
	cfg := ConfigFromMbit(64, 0, 8, 0.66, 30*time.Minute, "", "")
	if cfg.SingleBandwidthBps != 64*1024*1024 || cfg.TotalBandwidthBps != 0 || cfg.BlacklistSpeedBps != 8*1024*1024 {
		t.Fatalf("ConfigFromMbit: %+v", cfg)
	}
}
