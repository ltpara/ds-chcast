package rendezvous

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"log/slog"
	"net"
	"strconv"
	"strings"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

const (
	// ProbeTimeout is the startup self-check budget, matching hbbs's TEST_HBBS.
	ProbeTimeout = 12 * time.Second
	// probeReplyWait bounds one challenge or acknowledgement read so a silent
	// port leaves room for another keepalive within the budget.
	probeReplyWait = time.Second
)

// ProbeTarget resolves --test-punch into a UDP endpoint. An empty target means
// the check is skipped; auto only runs when advertise is unset or loopback, and
// otherwise reports why it was skipped. An explicit host:port is always used.
func ProbeTarget(value, advertise, bound string) (target, skip string, err error) {
	value = strings.TrimSpace(value)
	switch {
	case value == "":
		return "", "disabled", nil
	case strings.EqualFold(value, "auto"):
		host, port, splitErr := net.SplitHostPort(bound)
		if splitErr != nil {
			return "", "", fmt.Errorf("test punch sees listener %q: %w", bound, splitErr)
		}
		advertise = strings.TrimSpace(advertise)
		if advertise == "" {
			return loopbackTarget(host, port), "", nil
		}
		if !isLoopbackHost(advertise) {
			return "", fmt.Sprintf("--advertise %q is not a loopback address", advertise), nil
		}
		if ip := net.ParseIP(strings.Trim(advertise, "[]")); ip != nil {
			return net.JoinHostPort(ip.String(), port), "", nil
		}
		// "localhost" can resolve to the other address family than the one the
		// socket actually bound, so stay on the listener's loopback.
		return loopbackTarget(host, port), "", nil
	default:
		host, port, splitErr := net.SplitHostPort(value)
		if splitErr != nil || strings.TrimSpace(host) == "" {
			return "", "", fmt.Errorf("--test-punch %q must be auto or host:port", value)
		}
		number, convErr := strconv.Atoi(port)
		if convErr != nil || number < 1 || number > 65535 {
			return "", "", fmt.Errorf("--test-punch %q has an invalid port", value)
		}
		return value, "", nil
	}
}

func isLoopbackHost(host string) bool {
	host = strings.Trim(strings.TrimSpace(host), "[]")
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

func loopbackTarget(host, port string) string {
	ip := net.ParseIP(strings.Trim(host, "[]"))
	switch {
	case ip == nil:
		host = "127.0.0.1"
	case ip.IsUnspecified():
		if ip.To4() == nil {
			host = "::1"
		} else {
			host = "127.0.0.1"
		}
	default:
		host = ip.String()
	}
	return net.JoinHostPort(host, port)
}

// Probe registers a temporary host and completes the signed keepalive exchange
// against target once per second until it succeeds or ctx expires. It returns
// the round-trip time of the first accepted exchange.
func Probe(ctx context.Context, target string, reg *registry.Registry, log *slog.Logger) (time.Duration, error) {
	if reg == nil {
		return 0, errors.New("punch self-check needs a registry")
	}
	if log == nil {
		log = slog.Default()
	}
	addr, err := net.ResolveUDPAddr("udp", target)
	if err != nil {
		return 0, fmt.Errorf("resolve %q: %w", target, err)
	}
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return 0, err
	}
	raw := make([]byte, 8)
	if _, err := rand.Read(raw); err != nil {
		return 0, err
	}
	id := "chast-selfcheck-" + hex.EncodeToString(raw)
	if _, err := reg.Register(registry.RegisterRequest{
		ID:        id,
		PublicKey: identity.PublicKeyText(pub),
		TTL:       30 * time.Second,
	}); err != nil {
		return 0, fmt.Errorf("register temporary host %s: %w", id, err)
	}
	defer func() {
		removed, err := reg.RemoveIdentity(id)
		if err != nil {
			log.Warn("punch self-check could not remove its temporary host", "id", id, "error", err)
		} else if removed {
			log.Debug("punch self-check removed its temporary host", "id", id)
		}
	}()

	conn, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		return 0, fmt.Errorf("dial %s: %w", target, err)
	}
	defer conn.Close()

	ctx, cancel := context.WithTimeout(ctx, ProbeTimeout)
	defer cancel()
	var lastErr error
	for attempt := 1; ; attempt++ {
		rtt, err := probeOnce(conn, id, priv)
		if err == nil {
			return rtt, nil
		}
		lastErr = err
		log.Debug("punch self-check attempt failed", "target", target, "attempt", attempt, "error", err)
		select {
		case <-ctx.Done():
			return 0, fmt.Errorf("no keepalive acknowledgement from %s within %s: %w (last error: %v)",
				target, ProbeTimeout, ctx.Err(), lastErr)
		case <-time.After(time.Second):
		}
	}
}

func probeOnce(conn *net.UDPConn, id string, priv ed25519.PrivateKey) (time.Duration, error) {
	start := time.Now()
	if _, err := conn.Write(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: id})); err != nil {
		return 0, err
	}
	challenge, err := readProbeReply(conn, time.Now().Add(probeReplyWait))
	if err != nil {
		return 0, err
	}
	if challenge.Type != wire.TypeKeepaliveChallenge {
		return 0, fmt.Errorf("expected keepalive_challenge, got %s: %s", challenge.Type, challenge.Error)
	}
	if challenge.Nonce == "" || challenge.Candidate == "" {
		return 0, errors.New("keepalive challenge carried no nonce or candidate")
	}
	signature := identity.Sign(priv, identity.KeepaliveMessage(id, challenge.Candidate, challenge.Nonce))
	if _, err := conn.Write(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    id,
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: signature,
	})); err != nil {
		return 0, err
	}
	ack, err := readProbeReply(conn, time.Now().Add(probeReplyWait))
	if err != nil {
		return 0, err
	}
	if ack.Type != wire.TypeKeepaliveAck {
		return 0, fmt.Errorf("expected keepalive_ack, got %s", ack.Type)
	}
	if !ack.OK {
		return 0, fmt.Errorf("keepalive refused: %s", ack.Error)
	}
	return time.Since(start), nil
}

func readProbeReply(conn *net.UDPConn, deadline time.Time) (wire.Message, error) {
	if err := conn.SetReadDeadline(deadline); err != nil {
		return wire.Message{}, err
	}
	buf := make([]byte, wire.MaxControlSize+1)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			return wire.Message{}, err
		}
		msg, err := wire.Decode(buf[:n])
		if err != nil {
			continue
		}
		return msg, nil
	}
}
