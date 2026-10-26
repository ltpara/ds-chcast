package relay

import (
	"errors"
	"fmt"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	// The defaults match hbbr: a per-session limit, a process-wide total and a
	// slower tier for blacklisted addresses.
	DefaultSingleBandwidthBps int64 = 128 * 1024 * 1024
	DefaultTotalBandwidthBps  int64 = 1024 * 1024 * 1024
	DefaultBlacklistSpeedBps  int64 = 32 * 1024 * 1024
	DefaultDowngradeThreshold       = 0.66
	DefaultDowngradeAfter           = 30 * time.Minute

	listCheckInterval = time.Second
)

var ErrBlocked = errors.New("viewer address is on the relay blocklist")

// PolicyConfig is expressed in bits per second. A zero rate disables that
// limit. A zero downgrade threshold disables downgrading.
type PolicyConfig struct {
	SingleBandwidthBps int64
	TotalBandwidthBps  int64
	BlacklistSpeedBps  int64
	DowngradeThreshold float64
	DowngradeAfter     time.Duration
	BlacklistFile      string
	BlocklistFile      string
}

func DefaultPolicyConfig() PolicyConfig {
	return PolicyConfig{
		SingleBandwidthBps: DefaultSingleBandwidthBps,
		TotalBandwidthBps:  DefaultTotalBandwidthBps,
		BlacklistSpeedBps:  DefaultBlacklistSpeedBps,
		DowngradeThreshold: DefaultDowngradeThreshold,
		DowngradeAfter:     DefaultDowngradeAfter,
	}
}

// ConfigFromMbit builds a policy config from hbbr-style Mbit/s values. A zero
// rate disables that limit.
func ConfigFromMbit(single, total, blacklist float64, threshold float64, after time.Duration, blacklistFile, blocklistFile string) PolicyConfig {
	return PolicyConfig{
		SingleBandwidthBps: mbitToBps(single),
		TotalBandwidthBps:  mbitToBps(total),
		BlacklistSpeedBps:  mbitToBps(blacklist),
		DowngradeThreshold: threshold,
		DowngradeAfter:     after,
		BlacklistFile:      blacklistFile,
		BlocklistFile:      blocklistFile,
	}
}

func mbitToBps(value float64) int64 {
	if value <= 0 {
		return 0
	}
	return int64(value * 1024 * 1024)
}

type PolicyStats struct {
	SingleBandwidthBps   int64   `json:"single_bandwidth_bps"`
	TotalBandwidthBps    int64   `json:"total_bandwidth_bps"`
	BlacklistSpeedBps    int64   `json:"blacklist_speed_bps"`
	DowngradeThreshold   float64 `json:"downgrade_threshold"`
	DowngradeAfterSec    int64   `json:"downgrade_after_sec"`
	Blacklist            int     `json:"blacklist"`
	Blocklist            int     `json:"blocklist"`
	Sessions             int     `json:"sessions"`
	LimitedBytes         uint64  `json:"limited_bytes"`
	RateDroppedDatagrams uint64  `json:"rate_dropped_datagrams"`
	RateDroppedBytes     uint64  `json:"rate_dropped_bytes"`
	BlockedSessions      uint64  `json:"blocked_sessions"`
	DowngradedSessions   uint64  `json:"downgraded_sessions"`
}

type SessionUsage struct {
	Key         string `json:"key"`
	IP          string `json:"ip"`
	Seconds     int64  `json:"seconds"`
	TotalBytes  uint64 `json:"total_bytes"`
	HighestBps  uint64 `json:"highest_bps"`
	AverageBps  uint64 `json:"average_bps"`
	SpeedBps    uint64 `json:"speed_bps"`
	Downgraded  bool   `json:"downgraded"`
	Blacklisted bool   `json:"blacklisted"`
}

type tokenBucket struct {
	tokens float64
	last   time.Time
}

type policyCounters struct {
	limitedBytes         uint64
	rateDroppedDatagrams uint64
	rateDroppedBytes     uint64
	blockedSessions      uint64
	downgradedSessions   uint64
}

// Policy enforces hbbr-style relay limits. Every forwarding session owns one
// bucket; all sessions also share the process-wide total bucket. A blocked or
// downgraded session uses the slower blacklist rate.
type Policy struct {
	mu sync.Mutex

	singleBps      int64
	totalBps       int64
	blacklistBps   int64
	threshold      float64
	downgradeAfter time.Duration

	blacklist     map[string]struct{}
	blocklist     map[string]struct{}
	blacklistFile string
	blocklistFile string

	global   tokenBucket
	sessions map[string]*Session
	counters policyCounters

	now func() time.Time
}

type Session struct {
	policy *Policy
	key    string
	ip     string

	start  time.Time
	bucket tokenBucket

	totalBytes uint64
	totalBits  uint64

	windowStart time.Time
	windowBits  uint64
	speedBps    uint64
	highestBps  uint64

	downgraded    bool
	blacklisted   bool
	blocked       bool
	lastListCheck time.Time
}

func newPolicy(cfg PolicyConfig) (*Policy, error) {
	p := &Policy{
		sessions:  make(map[string]*Session),
		blacklist: make(map[string]struct{}),
		blocklist: make(map[string]struct{}),
		now:       time.Now,
	}
	if err := p.SetConfig(cfg); err != nil {
		return nil, err
	}
	return p, nil
}

func (p *Policy) SetConfig(cfg PolicyConfig) error {
	if cfg.SingleBandwidthBps < 0 || cfg.TotalBandwidthBps < 0 || cfg.BlacklistSpeedBps < 0 {
		return errors.New("relay bandwidth limits must not be negative")
	}
	if cfg.DowngradeThreshold < 0 || cfg.DowngradeThreshold > 1 {
		return errors.New("relay downgrade threshold must be between 0 and 1")
	}
	if cfg.DowngradeAfter < 0 {
		return errors.New("relay downgrade start must not be negative")
	}
	if cfg.DowngradeAfter == 0 {
		cfg.DowngradeAfter = DefaultDowngradeAfter
	}
	blacklist, err := loadIPList(cfg.BlacklistFile)
	if err != nil {
		return fmt.Errorf("relay blacklist: %w", err)
	}
	blocklist, err := loadIPList(cfg.BlocklistFile)
	if err != nil {
		return fmt.Errorf("relay blocklist: %w", err)
	}

	p.mu.Lock()
	defer p.mu.Unlock()
	p.singleBps = cfg.SingleBandwidthBps
	p.totalBps = cfg.TotalBandwidthBps
	p.blacklistBps = cfg.BlacklistSpeedBps
	p.threshold = cfg.DowngradeThreshold
	p.downgradeAfter = cfg.DowngradeAfter
	p.blacklistFile = strings.TrimSpace(cfg.BlacklistFile)
	p.blocklistFile = strings.TrimSpace(cfg.BlocklistFile)
	p.blacklist = blacklist
	p.blocklist = blocklist
	return nil
}

func (p *Policy) Stats() PolicyStats {
	p.mu.Lock()
	defer p.mu.Unlock()
	return PolicyStats{
		SingleBandwidthBps:   p.singleBps,
		TotalBandwidthBps:    p.totalBps,
		BlacklistSpeedBps:    p.blacklistBps,
		DowngradeThreshold:   p.threshold,
		DowngradeAfterSec:    int64(p.downgradeAfter / time.Second),
		Blacklist:            len(p.blacklist),
		Blocklist:            len(p.blocklist),
		Sessions:             len(p.sessions),
		LimitedBytes:         p.counters.limitedBytes,
		RateDroppedDatagrams: p.counters.rateDroppedDatagrams,
		RateDroppedBytes:     p.counters.rateDroppedBytes,
		BlockedSessions:      p.counters.blockedSessions,
		DowngradedSessions:   p.counters.downgradedSessions,
	}
}

func (p *Policy) Usage() []SessionUsage {
	p.mu.Lock()
	defer p.mu.Unlock()
	now := p.now()
	out := make([]SessionUsage, 0, len(p.sessions))
	for _, s := range p.sessions {
		out = append(out, s.usageLocked(now))
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].TotalBytes != out[j].TotalBytes {
			return out[i].TotalBytes > out[j].TotalBytes
		}
		if out[i].IP != out[j].IP {
			return out[i].IP < out[j].IP
		}
		return out[i].Key < out[j].Key
	})
	return out
}

func (p *Policy) Blacklisted(ip string) bool {
	ip = canonicalIP(ip)
	p.mu.Lock()
	defer p.mu.Unlock()
	_, ok := p.blacklist[ip]
	return ok
}

func (p *Policy) Blocked(ip string) bool {
	ip = canonicalIP(ip)
	p.mu.Lock()
	defer p.mu.Unlock()
	_, ok := p.blocklist[ip]
	return ok
}

// Open starts a session for one viewer. The key is unique per relay slot.
func (p *Policy) Open(key, ip string) (*Session, error) {
	ip = canonicalIP(ip)
	p.mu.Lock()
	defer p.mu.Unlock()
	if old := p.sessions[key]; old != nil {
		delete(p.sessions, key)
	}
	if _, blocked := p.blocklist[ip]; blocked {
		p.counters.blockedSessions++
		return nil, ErrBlocked
	}
	now := p.now()
	s := &Session{
		policy:        p,
		key:           key,
		ip:            ip,
		start:         now,
		windowStart:   now,
		lastListCheck: now,
	}
	_, s.blacklisted = p.blacklist[ip]
	p.sessions[key] = s
	return s, nil
}

func (s *Session) Close() {
	p := s.policy
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.sessions[s.key] == s {
		delete(p.sessions, s.key)
	}
}

// Take waits until the session and total buckets have room for payload bytes.
// It returns ErrBlocked when the viewer was blocklisted.
func (s *Session) Take(payload int) error {
	if payload <= 0 {
		return nil
	}
	p := s.policy
	bits := payload * 8
	limited := false
	for i := 0; i < 100; i++ {
		now := p.now()
		wait, err := p.reserve(s, bits, now, false)
		if err != nil {
			return err
		}
		if wait <= 0 {
			return nil
		}
		if !limited {
			p.mu.Lock()
			p.counters.limitedBytes += uint64(payload)
			p.mu.Unlock()
			limited = true
		}
		time.Sleep(wait)
	}
	return nil
}

// Drop forwards a datagram if both buckets have room and drops it otherwise.
// UDP cannot apply backpressure, so the caller discards the payload.
func (s *Session) Drop(payload int) (bool, error) {
	if payload <= 0 {
		return true, nil
	}
	now := s.policy.now()
	wait, err := s.policy.reserve(s, payload*8, now, true)
	if err != nil {
		return false, err
	}
	return wait <= 0, nil
}

func (p *Policy) reserve(s *Session, bits int, now time.Time, dry bool) (time.Duration, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if err := s.refreshListsLocked(now); err != nil {
		return 0, err
	}
	rate := s.rateLocked()
	sessionCap := bucketCapacity(rate, bits)
	totalCap := bucketCapacity(p.totalBps, bits)
	s.bucket.refill(now, rate, sessionCap)
	p.global.refill(now, p.totalBps, totalCap)
	wait := s.bucket.deficit(float64(bits), float64(rate), sessionCap)
	if total := p.global.deficit(float64(bits), float64(p.totalBps), totalCap); total > wait {
		wait = total
	}
	if wait > 0 {
		if dry {
			payloadBytes := uint64((bits + 7) / 8)
			p.counters.rateDroppedDatagrams++
			p.counters.rateDroppedBytes += payloadBytes
			p.counters.limitedBytes += payloadBytes
		}
		return wait, nil
	}
	s.bucket.consume(float64(bits))
	p.global.consume(float64(bits))
	s.accountLocked(now, bits)
	return 0, nil
}

func (s *Session) refreshListsLocked(now time.Time) error {
	p := s.policy
	if now.Sub(s.lastListCheck) < listCheckInterval {
		return nil
	}
	s.lastListCheck = now
	if _, blocked := p.blocklist[s.ip]; blocked {
		if !s.blocked {
			s.blocked = true
			p.counters.blockedSessions++
		}
		return ErrBlocked
	}
	_, s.blacklisted = p.blacklist[s.ip]
	return nil
}

func (s *Session) rateLocked() int64 {
	if s.blacklisted || s.downgraded {
		return s.policy.blacklistBps
	}
	return s.policy.singleBps
}

func (s *Session) accountLocked(now time.Time, bits int) {
	s.totalBits += uint64(bits)
	s.totalBytes += uint64((bits + 7) / 8)
	s.windowBits += uint64(bits)
	if elapsed := now.Sub(s.windowStart); elapsed >= time.Second {
		s.speedBps = uint64(float64(s.windowBits) / elapsed.Seconds())
		if s.speedBps > s.highestBps {
			s.highestBps = s.speedBps
		}
		s.windowStart = now
		s.windowBits = 0
	}
	p := s.policy
	if s.downgraded || p.threshold <= 0 || p.singleBps <= 0 {
		return
	}
	elapsed := now.Sub(s.start)
	if elapsed < p.downgradeAfter || elapsed <= 0 {
		return
	}
	avg := float64(s.totalBits) / elapsed.Seconds()
	if avg > p.threshold*float64(p.singleBps) {
		s.downgraded = true
		p.counters.downgradedSessions++
	}
}

func (s *Session) usageLocked(now time.Time) SessionUsage {
	elapsed := now.Sub(s.start)
	usage := SessionUsage{
		Key:         s.key,
		IP:          s.ip,
		Seconds:     int64(elapsed / time.Second),
		TotalBytes:  s.totalBytes,
		HighestBps:  s.highestBps,
		SpeedBps:    s.speedBps,
		Downgraded:  s.downgraded,
		Blacklisted: s.blacklisted,
	}
	if elapsed > 0 {
		usage.AverageBps = uint64(float64(s.totalBits) / elapsed.Seconds())
	}
	return usage
}

func (b *tokenBucket) refill(now time.Time, rate int64, capacity float64) {
	if b.last.IsZero() {
		b.tokens = capacity
		b.last = now
		return
	}
	if rate <= 0 {
		b.tokens = capacity
		b.last = now
		return
	}
	elapsed := now.Sub(b.last).Seconds()
	if elapsed <= 0 {
		return
	}
	b.tokens += elapsed * float64(rate)
	if b.tokens > capacity {
		b.tokens = capacity
	}
	b.last = now
}

func (b *tokenBucket) deficit(bits, rate float64, capacity float64) time.Duration {
	if rate <= 0 {
		return 0
	}
	if bits > capacity {
		capacity = bits
	}
	if b.tokens >= bits {
		return 0
	}
	return time.Duration((bits - b.tokens) / rate * float64(time.Second))
}

func (b *tokenBucket) consume(bits float64) {
	b.tokens -= bits
	if b.tokens < 0 {
		b.tokens = 0
	}
}

func bucketCapacity(rate int64, bits int) float64 {
	capacity := float64(maxDatagram * 8)
	if rate > 0 && float64(rate) > capacity {
		capacity = float64(rate)
	}
	if float64(bits) > capacity {
		capacity = float64(bits)
	}
	return capacity
}

func canonicalIP(value string) string {
	value = strings.TrimSpace(value)
	if ip := net.ParseIP(value); ip != nil {
		return ip.String()
	}
	return value
}

// DefaultListFile returns explicit when it is non-empty. Otherwise it picks the
// first candidate that names a regular file in the working directory, matching
// hbbr's automatic load of ./blacklist.txt and ./blocklist.txt. An empty result
// means the list stays in memory only.
func DefaultListFile(explicit string, candidates ...string) string {
	if strings.TrimSpace(explicit) != "" {
		return explicit
	}
	for _, name := range candidates {
		info, err := os.Stat(name)
		if err == nil && info.Mode().IsRegular() {
			return name
		}
	}
	return ""
}

func loadIPList(path string) (map[string]struct{}, error) {
	out := make(map[string]struct{})
	path = strings.TrimSpace(path)
	if path == "" {
		return out, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return out, nil
		}
		return nil, err
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		out[canonicalIP(fields[0])] = struct{}{}
	}
	return out, nil
}

func writeIPList(path string, list map[string]struct{}) error {
	path = strings.TrimSpace(path)
	if path == "" {
		return nil
	}
	ips := make([]string, 0, len(list))
	for ip := range list {
		ips = append(ips, ip)
	}
	sort.Strings(ips)
	var b strings.Builder
	for _, ip := range ips {
		b.WriteString(ip)
		b.WriteByte('\n')
	}
	return os.WriteFile(path, []byte(b.String()), 0o644)
}

// Command implements the hbbr console subset. The boolean is false for an
// unknown command so the caller can keep its own command set.
func (p *Policy) Command(line string) (string, bool) {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return "", false
	}
	switch fields[0] {
	case "h", "help":
		return p.Help(), true
	case "blacklist-add", "ba":
		return p.addList(true, fields[1:]), true
	case "blacklist-remove", "br":
		return p.removeList(true, fields[1:]), true
	case "blacklist", "b":
		return p.listText(true, fields[1:]), true
	case "blocklist-add", "Ba":
		return p.addList(false, fields[1:]), true
	case "blocklist-remove", "Br":
		return p.removeList(false, fields[1:]), true
	case "blocklist", "B":
		return p.listText(false, fields[1:]), true
	case "downgrade-threshold", "dt":
		return p.numberCommand(fields[1:], "downgrade-threshold", func(v float64) bool {
			if v < 0 || v > 1 {
				return false
			}
			p.mu.Lock()
			p.threshold = v
			p.mu.Unlock()
			return true
		}, func() string {
			p.mu.Lock()
			defer p.mu.Unlock()
			return fmt.Sprintf("%.2f\n", p.threshold)
		}), true
	case "downgrade-start-check", "t":
		return p.numberCommand(fields[1:], "downgrade-start-check", func(v float64) bool {
			if v < 0 {
				return false
			}
			p.mu.Lock()
			p.downgradeAfter = time.Duration(v * float64(time.Second))
			p.mu.Unlock()
			return true
		}, func() string {
			p.mu.Lock()
			defer p.mu.Unlock()
			return fmt.Sprintf("%ds\n", int64(p.downgradeAfter/time.Second))
		}), true
	case "limit-speed", "ls":
		return p.rateCommand(fields[1:], "limit-speed", func(v int64) {
			p.mu.Lock()
			p.blacklistBps = v
			p.mu.Unlock()
		}, func() int64 {
			p.mu.Lock()
			defer p.mu.Unlock()
			return p.blacklistBps
		}), true
	case "total-bandwidth", "tb":
		return p.rateCommand(fields[1:], "total-bandwidth", func(v int64) {
			p.mu.Lock()
			p.totalBps = v
			p.mu.Unlock()
		}, func() int64 {
			p.mu.Lock()
			defer p.mu.Unlock()
			return p.totalBps
		}), true
	case "single-bandwidth", "sb":
		return p.rateCommand(fields[1:], "single-bandwidth", func(v int64) {
			p.mu.Lock()
			p.singleBps = v
			p.mu.Unlock()
		}, func() int64 {
			p.mu.Lock()
			defer p.mu.Unlock()
			return p.singleBps
		}), true
	case "usage", "u":
		return p.usageText(), true
	default:
		return "", false
	}
}

func (p *Policy) Help() string {
	return `blacklist-add(ba) <ip[|ip...]>
blacklist-remove(br) <ip|all>
blacklist(b) [ip]
blocklist-add(Ba) <ip[|ip...]>
blocklist-remove(Br) <ip|all>
blocklist(B) [ip]
downgrade-threshold(dt) [0..1]
downgrade-start-check(t) [seconds]
limit-speed(ls) [Mbit/s]
total-bandwidth(tb) [Mbit/s]
single-bandwidth(sb) [Mbit/s]
usage(u)
`
}

func (p *Policy) addList(blacklist bool, args []string) string {
	ips, bad := parseIPArgs(args)
	if len(ips) == 0 {
		if bad != "" {
			return fmt.Sprintf("invalid address: %s\n", bad)
		}
		return "usage: " + listCommand(blacklist, "add") + " <ip[|ip...]>\n"
	}
	p.mu.Lock()
	list := p.blocklist
	path := p.blocklistFile
	if blacklist {
		list = p.blacklist
		path = p.blacklistFile
	}
	for _, ip := range ips {
		list[ip] = struct{}{}
	}
	err := writeIPList(path, list)
	count := len(list)
	p.mu.Unlock()
	if err != nil {
		return fmt.Sprintf("write list: %v\n", err)
	}
	return fmt.Sprintf("added %d, total=%d\n", len(ips), count)
}

func (p *Policy) removeList(blacklist bool, args []string) string {
	if len(args) == 0 {
		return "usage: " + listCommand(blacklist, "remove") + " <ip|all>\n"
	}
	p.mu.Lock()
	list := p.blocklist
	path := p.blocklistFile
	if blacklist {
		list = p.blacklist
		path = p.blacklistFile
	}
	removed := 0
	if args[0] == "all" {
		removed = len(list)
		clear(list)
	} else {
		ips, bad := parseIPArgs(args)
		if bad != "" {
			p.mu.Unlock()
			return fmt.Sprintf("invalid address: %s\n", bad)
		}
		for _, ip := range ips {
			if _, ok := list[ip]; ok {
				delete(list, ip)
				removed++
			}
		}
	}
	err := writeIPList(path, list)
	count := len(list)
	p.mu.Unlock()
	if err != nil {
		return fmt.Sprintf("write list: %v\n", err)
	}
	return fmt.Sprintf("removed %d, total=%d\n", removed, count)
}

func (p *Policy) listText(blacklist bool, args []string) string {
	p.mu.Lock()
	defer p.mu.Unlock()
	list := p.blocklist
	if blacklist {
		list = p.blacklist
	}
	if len(args) > 0 {
		_, ok := list[canonicalIP(args[0])]
		return fmt.Sprintf("%t\n", ok)
	}
	if len(list) == 0 {
		return "empty\n"
	}
	ips := make([]string, 0, len(list))
	for ip := range list {
		ips = append(ips, ip)
	}
	sort.Strings(ips)
	var b strings.Builder
	for _, ip := range ips {
		b.WriteString(ip)
		b.WriteByte('\n')
	}
	return b.String()
}

func (p *Policy) numberCommand(args []string, name string, set func(float64) bool, get func() string) string {
	if len(args) == 0 {
		return get()
	}
	v, err := strconv.ParseFloat(args[0], 64)
	if err != nil || !set(v) {
		return fmt.Sprintf("usage: %s [value]\n", name)
	}
	return get()
}

func (p *Policy) rateCommand(args []string, name string, set func(int64), get func() int64) string {
	if len(args) == 0 {
		return formatRate(get())
	}
	v, err := strconv.ParseFloat(args[0], 64)
	if err != nil || v < 0 {
		return fmt.Sprintf("usage: %s [Mbit/s]\n", name)
	}
	set(int64(v * 1024 * 1024))
	return formatRate(get())
}

func formatRate(bps int64) string {
	if bps <= 0 {
		return "unlimited\n"
	}
	return fmt.Sprintf("%.2fMb/s\n", float64(bps)/(1024*1024))
}

func (p *Policy) usageText() string {
	usage := p.Usage()
	if len(usage) == 0 {
		return "no relay sessions\n"
	}
	var b strings.Builder
	for _, u := range usage {
		fmt.Fprintf(&b, "%s key=%s %ds %.2fMB highest=%.0fkb/s avg=%.0fkb/s now=%.0fkb/s downgraded=%t blacklisted=%t\n",
			u.IP, u.Key, u.Seconds, float64(u.TotalBytes)/(1024*1024),
			float64(u.HighestBps)/1000, float64(u.AverageBps)/1000, float64(u.SpeedBps)/1000,
			u.Downgraded, u.Blacklisted)
	}
	return b.String()
}

func parseIPArgs(args []string) ([]string, string) {
	var out []string
	seen := make(map[string]struct{})
	for _, arg := range args {
		for _, raw := range strings.Split(arg, "|") {
			raw = strings.TrimSpace(raw)
			if raw == "" {
				continue
			}
			ip := net.ParseIP(raw)
			if ip == nil {
				return nil, raw
			}
			value := ip.String()
			if _, ok := seen[value]; ok {
				continue
			}
			seen[value] = struct{}{}
			out = append(out, value)
		}
	}
	return out, ""
}

func listCommand(blacklist bool, action string) string {
	if blacklist {
		if action == "add" {
			return "blacklist-add | ba"
		}
		return "blacklist-remove | br"
	}
	if action == "add" {
		return "blocklist-add | Ba"
	}
	return "blocklist-remove | Br"
}
