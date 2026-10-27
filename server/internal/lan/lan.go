// Package lan implements opt-in LAN peer discovery for chast hosts. It is
// independent of the rendezvous protocol: a viewer broadcasts a short ping on
// the local link and hosts answer with display metadata only. Knowing a peer's
// address does not grant access; a viewer still has to complete the normal
// QUIC authentication.
package lan

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

const (
	// DefaultPort is a chast-specific port. It deliberately differs from
	// RustDesk's discovery port because the payload formats are incompatible.
	DefaultPort = 21129
	// Magic separates discovery datagrams from unrelated UDP traffic.
	Magic = "CHASTL1\n"
	// TypePing asks every reachable host to identify itself.
	TypePing = "ping"
	// TypePong is a host's answer to a ping.
	TypePong = "pong"
	// MaxDatagram bounds requests and replies. Both sides use the same limit,
	// so a malformed length cannot turn into a large allocation.
	MaxDatagram = 2048
	// DefaultTimeout is how long Discover waits for answers.
	DefaultTimeout = 1500 * time.Millisecond
)

// Message is one discovery datagram. A ping only carries Type and optionally
// the sender's own id; a pong adds the host's display metadata. No key
// material, password or relay assignment is exposed.
type Message struct {
	Type     string `json:"type"`
	ID       string `json:"id,omitempty"`
	Hostname string `json:"hostname,omitempty"`
	Username string `json:"username,omitempty"`
	Platform string `json:"platform,omitempty"`
	Version  string `json:"version,omitempty"`
	MAC      string `json:"mac,omitempty"`
}

// Info is the local host's discovery profile.
type Info struct {
	ID       string
	Hostname string
	Username string
	Platform string
	Version  string
	MAC      string
}

// Peer is a host that answered a discovery ping. Addresses holds every source
// address that answered for this id; Address is the preferred one (a LAN
// address before loopback).
type Peer struct {
	ID        string   `json:"id"`
	Address   string   `json:"address"`
	Addresses []string `json:"addresses,omitempty"`
	Hostname  string   `json:"hostname,omitempty"`
	Username  string   `json:"username,omitempty"`
	Platform  string   `json:"platform,omitempty"`
	Version   string   `json:"version,omitempty"`
	MAC       string   `json:"mac,omitempty"`
}

// LocalInfo fills the parts of Info that describe this machine. The caller
// supplies the registered id and the build version.
func LocalInfo(id, version string) Info {
	hostname, _ := os.Hostname()
	return Info{
		ID:       id,
		Hostname: clean(hostname, 128),
		Username: clean(username(), 64),
		Platform: runtime.GOOS + "/" + runtime.GOARCH,
		Version:  clean(version, 64),
		MAC:      LocalMAC(),
	}
}

// LocalMAC returns the hardware address of the first up, non-loopback
// interface with an IPv4 address. It is a Wake-on-LAN hint, not an identity.
func LocalMAC() string {
	interfaces, err := net.Interfaces()
	if err != nil {
		return ""
	}
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 ||
			len(iface.HardwareAddr) == 0 {
			continue
		}
		if hasIPv4(iface) {
			return iface.HardwareAddr.String()
		}
	}
	return ""
}

func hasIPv4(iface net.Interface) bool {
	addrs, err := iface.Addrs()
	if err != nil {
		return false
	}
	for _, addr := range addrs {
		if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
			return true
		}
	}
	return false
}

func username() string {
	for _, key := range []string{"USERNAME", "USER", "LOGNAME"} {
		if value := os.Getenv(key); value != "" {
			return value
		}
	}
	return ""
}

// Responder answers discovery pings from a single UDP socket.
type Responder struct {
	conn    *net.UDPConn
	info    Info
	log     *slog.Logger
	limiter *sourceLimiter
	done    chan struct{}
	once    sync.Once
}

// Start listens for discovery pings and answers them until ctx is done or the
// responder is closed. Failures to bind are returned to the caller, which may
// treat them as a warning.
func Start(ctx context.Context, listen string, info Info, logger *slog.Logger) (*Responder, error) {
	if !registry.ValidID(info.ID) {
		return nil, fmt.Errorf("lan: invalid host id %q", info.ID)
	}
	addr, err := net.ResolveUDPAddr("udp4", listen)
	if err != nil {
		return nil, fmt.Errorf("lan: resolve %q: %w", listen, err)
	}
	conn, err := net.ListenUDP("udp4", addr)
	if err != nil {
		return nil, fmt.Errorf("lan: listen %q: %w", listen, err)
	}
	if logger == nil {
		logger = slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	}
	r := &Responder{
		conn:    conn,
		info:    sanitizeInfo(info),
		log:     logger,
		limiter: newSourceLimiter(),
		done:    make(chan struct{}),
	}
	go func() {
		if err := r.serve(ctx); err != nil && ctx.Err() == nil && !errors.Is(err, net.ErrClosed) {
			logger.Warn("LAN discovery responder stopped", "error", err)
		}
		r.Close()
	}()
	go func() {
		select {
		case <-ctx.Done():
			r.Close()
		case <-r.done:
		}
	}()
	return r, nil
}

// Addr reports the bound address, which is useful when listen used port 0.
func (r *Responder) Addr() net.Addr { return r.conn.LocalAddr() }

// Close stops the responder. It is safe to call more than once.
func (r *Responder) Close() error {
	r.once.Do(func() { close(r.done) })
	return r.conn.Close()
}

func (r *Responder) serve(ctx context.Context) error {
	buf := make([]byte, MaxDatagram+1)
	for {
		_ = r.conn.SetReadDeadline(time.Now().Add(time.Second))
		n, src, err := r.conn.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return nil
			}
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}
			// A datagram larger than the buffer makes Windows return
			// WSAEMSGSIZE instead of truncating it. That error belongs to the
			// offending packet, not to the socket, so keep serving; the short
			// pause also keeps a persistently failing socket from spinning.
			r.log.Debug("LAN discovery read failed", "error", err)
			select {
			case <-time.After(10 * time.Millisecond):
			case <-r.done:
				return nil
			}
			continue
		}
		r.handle(buf[:n], src)
	}
}

func (r *Responder) handle(p []byte, src *net.UDPAddr) {
	msg, err := decode(p)
	if err != nil || msg.Type != TypePing {
		return
	}
	// An empty id is what a desktop viewer sends, because the machine may not
	// host its own id. A non-empty id must be a real one; this keeps the
	// responder from amplifying arbitrary garbage.
	if msg.ID != "" && !registry.ValidID(msg.ID) {
		return
	}
	if msg.ID == r.info.ID {
		return
	}
	if !r.limiter.allow(src.IP) {
		return
	}
	reply := Message{
		Type:     TypePong,
		ID:       r.info.ID,
		Hostname: r.info.Hostname,
		Username: r.info.Username,
		Platform: r.info.Platform,
		Version:  r.info.Version,
		MAC:      r.info.MAC,
	}
	if _, err := r.conn.WriteToUDP(encode(reply), src); err != nil {
		r.log.Debug("LAN discovery reply failed", "remote", src.String(), "error", err)
		return
	}
	r.log.Debug("LAN discovery answered", "remote", src.String())
}

// DiscoverOptions controls one discovery round.
type DiscoverOptions struct {
	// Port is the discovery UDP port; zero means DefaultPort.
	Port int
	// Timeout bounds the whole round; zero means DefaultTimeout.
	Timeout time.Duration
	// SelfID is filtered out of the answers, so a machine that runs both a
	// host agent and a viewer does not list itself.
	SelfID string
	// Targets adds explicit host:port unicast probes. It is meant for
	// diagnostics and tests; the default probes are broadcast addresses.
	Targets []string
}

// Discover broadcasts a ping and collects answers until the timeout. It
// returns no error when nobody answers.
func Discover(ctx context.Context, opts DiscoverOptions) ([]Peer, error) {
	port := opts.Port
	if port == 0 {
		port = DefaultPort
	}
	timeout := opts.Timeout
	if timeout <= 0 {
		timeout = DefaultTimeout
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		return nil, fmt.Errorf("lan: open discovery socket: %w", err)
	}
	defer conn.Close()

	targets := append(discoveryTargets(port), opts.Targets...)
	send := func() {
		ping := encode(Message{Type: TypePing, ID: opts.SelfID})
		for _, target := range targets {
			addr, err := net.ResolveUDPAddr("udp4", target)
			if err != nil {
				continue
			}
			_, _ = conn.WriteToUDP(ping, addr)
		}
	}
	// Send twice: broadcast is best-effort and one lost datagram should not
	// hide an otherwise reachable host.
	sendDone := make(chan struct{})
	go func() {
		defer close(sendDone)
		send()
		select {
		case <-time.After(400 * time.Millisecond):
			send()
		case <-ctx.Done():
		}
	}()
	defer func() { <-sendDone }()

	deadline := time.Now().Add(timeout)
	buf := make([]byte, MaxDatagram+1)
	index := make(map[string]int)
	var peers []Peer
	for {
		if ctx.Err() != nil {
			break
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		wait := 100 * time.Millisecond
		if remaining < wait {
			wait = remaining
		}
		_ = conn.SetReadDeadline(time.Now().Add(wait))
		n, src, err := conn.ReadFromUDP(buf)
		if err != nil {
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Timeout() {
				continue
			}
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				break
			}
			// Windows reports an oversized datagram as WSAEMSGSIZE instead of
			// truncating it. The reply is not ours, so ignore that packet.
			time.Sleep(10 * time.Millisecond)
			continue
		}
		msg, err := decode(buf[:n])
		if err != nil || msg.Type != TypePong || !registry.ValidID(msg.ID) {
			continue
		}
		if opts.SelfID != "" && msg.ID == opts.SelfID {
			continue
		}
		mergePeer(&peers, index, msg, src.String())
	}
	for i := range peers {
		preferAddresses(&peers[i])
	}
	sort.Slice(peers, func(i, j int) bool { return peers[i].ID < peers[j].ID })
	return peers, nil
}

// preferAddresses sorts a peer's answers so a real LAN address comes before
// loopback, then makes the first one the displayed address.
func preferAddresses(peer *Peer) {
	sort.Slice(peer.Addresses, func(a, b int) bool {
		leftLoop := isLoopbackAddr(peer.Addresses[a])
		rightLoop := isLoopbackAddr(peer.Addresses[b])
		if leftLoop != rightLoop {
			return !leftLoop
		}
		return peer.Addresses[a] < peer.Addresses[b]
	})
	if len(peer.Addresses) > 0 {
		peer.Address = peer.Addresses[0]
	}
}

// mergePeer combines answers from one host seen on several interfaces, as
// RustDesk keeps multiple ip_mac entries per discovery peer. A duplicate
// answer from the same address is ignored.
func mergePeer(peers *[]Peer, index map[string]int, msg Message, addr string) {
	if i, ok := index[msg.ID]; ok {
		peer := &(*peers)[i]
		if !containsString(peer.Addresses, addr) {
			peer.Addresses = append(peer.Addresses, addr)
		}
		return
	}
	index[msg.ID] = len(*peers)
	*peers = append(*peers, Peer{
		ID:        msg.ID,
		Address:   addr,
		Addresses: []string{addr},
		Hostname:  clean(msg.Hostname, 128),
		Username:  clean(msg.Username, 64),
		Platform:  clean(msg.Platform, 32),
		Version:   clean(msg.Version, 64),
		MAC:       clean(msg.MAC, 32),
	})
}

func containsString(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}

func isLoopbackAddr(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return false
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// discoveryTargets is the global broadcast plus every interface's directed
// broadcast and loopback. A host on the same machine is reachable through the
// loopback probe even when broadcast is filtered.
func discoveryTargets(port int) []string {
	portText := strconv.Itoa(port)
	seen := make(map[string]bool)
	var targets []string
	add := func(ip net.IP) {
		if ip == nil || ip.IsUnspecified() {
			return
		}
		addr := net.JoinHostPort(ip.String(), portText)
		if seen[addr] {
			return
		}
		seen[addr] = true
		targets = append(targets, addr)
	}
	add(net.IPv4bcast)
	add(net.IPv4(127, 0, 0, 1))
	interfaces, err := net.Interfaces()
	if err != nil {
		return targets
	}
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			ipnet, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipnet.IP.To4()
			if ip4 == nil || len(ipnet.Mask) != 4 {
				continue
			}
			broadcast := make(net.IP, 4)
			for i := 0; i < 4; i++ {
				broadcast[i] = ip4[i] | ^ipnet.Mask[i]
			}
			if broadcast.Equal(ip4) {
				continue
			}
			add(broadcast)
		}
	}
	return targets
}

func encode(m Message) []byte {
	body, err := json.Marshal(m)
	if err != nil {
		// Message only holds strings, so this cannot happen.
		panic(fmt.Sprintf("lan: encode: %v", err))
	}
	out := make([]byte, 0, len(Magic)+len(body))
	out = append(out, Magic...)
	return append(out, body...)
}

func decode(p []byte) (Message, error) {
	if len(p) == 0 || len(p) > MaxDatagram || !bytes.HasPrefix(p, []byte(Magic)) {
		return Message{}, errors.New("lan: not a discovery datagram")
	}
	var m Message
	if err := json.Unmarshal(p[len(Magic):], &m); err != nil {
		return Message{}, fmt.Errorf("lan: decode: %w", err)
	}
	if m.Type != TypePing && m.Type != TypePong {
		return Message{}, fmt.Errorf("lan: unknown message type %q", m.Type)
	}
	return m, nil
}

func sanitizeInfo(info Info) Info {
	return Info{
		ID:       info.ID,
		Hostname: clean(info.Hostname, 128),
		Username: clean(info.Username, 64),
		Platform: clean(info.Platform, 32),
		Version:  clean(info.Version, 64),
		MAC:      clean(info.MAC, 32),
	}
}

// clean bounds a display string and removes control characters. Discovery
// output is shown in terminals and UI lists, so a malicious peer must not be
// able to inject escape sequences.
func clean(s string, max int) string {
	if len(s) > max {
		s = s[:max]
	}
	if !utf8.ValidString(s) {
		s = strings.ToValidUTF8(s, "")
	}
	return strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, s)
}

// sourceLimiter keeps one ping per source from producing more than a few
// replies per second. It is an amplification guard, not authentication.
type sourceLimiter struct {
	mu    sync.Mutex
	seen  map[string]*sourceBucket
	lastP time.Time
}

type sourceBucket struct {
	tokens float64
	last   time.Time
}

func newSourceLimiter() *sourceLimiter {
	return &sourceLimiter{seen: make(map[string]*sourceBucket)}
}

func (l *sourceLimiter) allow(ip net.IP) bool {
	now := time.Now()
	key := ip.String()
	l.mu.Lock()
	defer l.mu.Unlock()
	if now.Sub(l.lastP) > time.Minute || len(l.seen) > 1024 {
		for k, bucket := range l.seen {
			if now.Sub(bucket.last) > time.Minute {
				delete(l.seen, k)
			}
		}
		l.lastP = now
	}
	bucket, ok := l.seen[key]
	if !ok {
		bucket = &sourceBucket{tokens: 5, last: now}
		l.seen[key] = bucket
	}
	elapsed := now.Sub(bucket.last).Seconds()
	bucket.last = now
	bucket.tokens += elapsed
	if bucket.tokens > 5 {
		bucket.tokens = 5
	}
	if bucket.tokens < 1 {
		return false
	}
	bucket.tokens--
	return true
}
