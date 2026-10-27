package rendezvous

import (
	"crypto/ed25519"
	"io"
	"log/slog"
	"net"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/punchlog"
	"github.com/ltpara/ds-chcast/server/internal/registry"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func listenUDP(t *testing.T) *net.UDPConn {
	t.Helper()
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

func recv(t *testing.T, conn *net.UDPConn) wire.Message {
	t.Helper()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, _, err := conn.ReadFromUDP(buf)
	if err != nil {
		t.Fatalf("receive: %v", err)
	}
	msg, err := wire.Decode(buf[:n])
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return msg
}

func startMeet(t *testing.T, opts ...Option) (*Server, *registry.Registry, *net.UDPAddr, ed25519.PrivateKey) {
	t.Helper()
	priv := identity.PrivateKeyFromSecret("host-secret")
	pub := identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
	reg := registry.New(5 * time.Second)
	if _, err := reg.Register(registry.RegisterRequest{
		ID:        "devbox",
		PublicKey: pub,
		TTL:       time.Minute,
	}); err != nil {
		t.Fatalf("register: %v", err)
	}
	reg.SetRelay("devbox", 41000, "")
	meet, err := New("127.0.0.1:0", reg, func(port int) string { return "192.0.2.1:41000" },
		slog.New(slog.NewTextHandler(io.Discard, nil)), opts...)
	if err != nil {
		t.Fatalf("rendezvous: %v", err)
	}
	serverIdentity, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	meet.SetIdentity(serverIdentity)
	go meet.Serve()
	t.Cleanup(meet.Close)
	addr, err := net.ResolveUDPAddr("udp", meet.Addr())
	if err != nil {
		t.Fatal(err)
	}
	return meet, reg, addr, priv
}

// keepalive performs the challenge-response and returns the final ack.
func keepalive(t *testing.T, conn *net.UDPConn, meetAddr *net.UDPAddr, priv ed25519.PrivateKey) wire.Message {
	t.Helper()
	conn.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, conn)
	if challenge.Type != wire.TypeKeepaliveChallenge || challenge.Nonce == "" {
		t.Fatalf("challenge: %+v", challenge)
	}
	message := identity.KeepaliveMessage("devbox", challenge.Candidate, challenge.Nonce)
	conn.WriteToUDP(wire.Encode(wire.Message{
		Type:       wire.TypeKeepaliveAuth,
		HostID:     "devbox",
		Nonce:      challenge.Nonce,
		Candidate:  challenge.Candidate,
		Signature:  identity.Sign(priv, message),
		LocalAddrs: []string{"192.168.1.9:14434"},
		NATType:    "port-restricted",
	}), meetAddr)
	return recv(t, conn)
}

func TestKeepaliveAndPunchExchange(t *testing.T) {
	meet, reg, meetAddr, priv := startMeet(t)
	host := listenUDP(t)
	viewer := listenUDP(t)

	ack := keepalive(t, host, meetAddr, priv)
	if !ack.OK || ack.Type != wire.TypeKeepaliveAck {
		t.Fatalf("keepalive: %+v", ack)
	}
	if ack.Candidate != host.LocalAddr().String() {
		t.Fatalf("observed candidate %q, want %q", ack.Candidate, host.LocalAddr().String())
	}
	if ack.Relay != "192.0.2.1:41000" || ack.NATType != "port-restricted" {
		t.Fatalf("relay/nat: %+v", ack)
	}
	hostNow, _ := reg.Lookup("devbox")
	if len(hostNow.LocalAddrs) != 1 {
		t.Fatalf("local addrs: %+v", hostNow.LocalAddrs)
	}

	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.Type != wire.TypePunchPeer {
		t.Fatalf("punch reply: %+v", reply)
	}
	if reply.Candidate != host.LocalAddr().String() || reply.Nonce != nonce {
		t.Fatalf("punch reply: %+v", reply)
	}
	if reply.PublicKey == "" || reply.Binding == "" {
		t.Fatalf("punch reply is missing the host identity: %+v", reply)
	}
	if _, err := identity.VerifyHostBinding(reply.Binding, meet.identity.PublicKeyText()); err != nil {
		t.Fatalf("punch binding: %v", err)
	}
	notify := recv(t, host)
	if notify.Type != wire.TypePunchPeer || notify.Candidate != viewer.LocalAddr().String() {
		t.Fatalf("host notification: %+v", notify)
	}
	if notify.Nonce != nonce || notify.ExpiresMS <= 0 {
		t.Fatalf("host notification: %+v", notify)
	}
}

func TestUpnpPortIsRelayedToBothPeers(t *testing.T) {
	_, reg, meetAddr, priv := startMeet(t)
	host := listenUDP(t)
	viewer := listenUDP(t)

	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, host)
	message := identity.KeepaliveMessage("devbox", challenge.Candidate, challenge.Nonce)
	host.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, message),
		UpnpPort:  50000,
	}), meetAddr)
	if ack := recv(t, host); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}
	if hostNow, _ := reg.Lookup("devbox"); hostNow.UpnpPort != 50000 {
		t.Fatalf("registered upnp port = %d, want 50000", hostNow.UpnpPort)
	}

	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce, UpnpPort: 60000,
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.UpnpPort != 50000 {
		t.Fatalf("punch reply upnp port: %+v", reply)
	}
	notify := recv(t, host)
	if notify.Type != wire.TypePunchPeer || notify.UpnpPort != 60000 {
		t.Fatalf("host notification upnp port: %+v", notify)
	}

	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce, UpnpPort: -1,
	}), meetAddr)
	if refused := recv(t, viewer); refused.OK || refused.Error != "upnp_port must be 0..65535" {
		t.Fatalf("invalid upnp port: %+v", refused)
	}
}

func TestKeepaliveRejectsInvalidUpnpPort(t *testing.T) {
	_, _, meetAddr, _ := startMeet(t)
	host := listenUDP(t)
	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, host)
	host.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		UpnpPort:  70000,
	}), meetAddr)
	if ack := recv(t, host); ack.OK || ack.Error != "upnp_port must be 0..65535" {
		t.Fatalf("invalid upnp port: %+v", ack)
	}
}

func TestKeepaliveRejectsWrongKeyAndUnknownPunch(t *testing.T) {
	_, _, meetAddr, priv := startMeet(t)
	stranger := listenUDP(t)
	stranger.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, stranger)
	if challenge.Type != wire.TypeKeepaliveChallenge {
		t.Fatalf("challenge: %+v", challenge)
	}
	wrong := identity.PrivateKeyFromSecret("another-secret")
	message := identity.KeepaliveMessage("devbox", challenge.Candidate, challenge.Nonce)
	stranger.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(wrong, message),
	}), meetAddr)
	ack := recv(t, stranger)
	if ack.OK || ack.Error == "" {
		t.Fatalf("wrong key accepted: %+v", ack)
	}
	// The nonce was consumed by the failed proof, so even a correctly signed
	// retry of the same challenge must be refused.
	stranger.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: challenge.Candidate,
		Signature: identity.Sign(priv, message),
	}), meetAddr)
	if replay := recv(t, stranger); replay.OK {
		t.Fatalf("replayed proof accepted: %+v", replay)
	}

	missing := listenUDP(t)
	missing.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "ghost", Nonce: "0123456789abcdef",
	}), meetAddr)
	reply := recv(t, missing)
	if reply.OK || reply.Error == "" {
		t.Fatalf("unknown host accepted: %+v", reply)
	}
}

func TestKeepaliveRejectsCandidateMismatch(t *testing.T) {
	_, _, meetAddr, priv := startMeet(t)
	host := listenUDP(t)
	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, host)
	// Signing a candidate other than the one the server observed must not bind
	// the host's mapping to that address.
	message := identity.KeepaliveMessage("devbox", "198.51.100.7:9999", challenge.Nonce)
	host.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypeKeepaliveAuth,
		HostID:    "devbox",
		Nonce:     challenge.Nonce,
		Candidate: "198.51.100.7:9999",
		Signature: identity.Sign(priv, message),
	}), meetAddr)
	ack := recv(t, host)
	if ack.OK || ack.Error == "" {
		t.Fatalf("mismatched candidate accepted: %+v", ack)
	}
}

func TestPunchRequiresServerKey(t *testing.T) {
	const serverKey = "viewer-secret"
	_, _, meetAddr, priv := startMeet(t, WithServerKey(serverKey))
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}
	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
	}), meetAddr)
	if reply := recv(t, viewer); reply.OK || reply.Error != "punch authentication failed" {
		t.Fatalf("unauthenticated punch: %+v", reply)
	}
	stamp := time.Now().Unix()
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypePunchRequest,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: stamp - 120,
		Auth:      identity.PunchAuth(serverKey, "devbox", nonce, stamp-120),
	}), meetAddr)
	if reply := recv(t, viewer); reply.OK {
		t.Fatalf("stale punch was accepted: %+v", reply)
	}
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type:      wire.TypePunchRequest,
		HostID:    "devbox",
		Nonce:     nonce,
		Timestamp: stamp,
		Auth:      identity.PunchAuth(serverKey, "devbox", nonce, stamp),
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.Candidate == "" {
		t.Fatalf("authenticated punch: %+v", reply)
	}
	if notify := recv(t, host); notify.Type != wire.TypePunchPeer {
		t.Fatalf("host notification: %+v", notify)
	}
}

func TestPunchRequiresFreshKeepalive(t *testing.T) {
	_, _, meetAddr, _ := startMeet(t)
	viewer := listenUDP(t)
	// This host is registered but has never completed a keepalive, so its
	// observed address is unknown and a punch cannot succeed.
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: "0123456789abcdef",
	}), meetAddr)
	reply := recv(t, viewer)
	if reply.OK || reply.Error != "host is offline" {
		t.Fatalf("offline host punch: %+v", reply)
	}
}

func TestPunchRequestsAreAudited(t *testing.T) {
	meet, _, meetAddr, priv := startMeet(t)
	log := punchlog.New(10, time.Minute)
	meet.SetPunchLog(log)
	host := listenUDP(t)
	viewer := listenUDP(t)
	if ack := keepalive(t, host, meetAddr, priv); !ack.OK {
		t.Fatalf("keepalive: %+v", ack)
	}

	punch := func() {
		viewer.WriteToUDP(wire.Encode(wire.Message{
			Type: wire.TypePunchRequest, HostID: "devbox", Nonce: "0123456789abcdef",
		}), meetAddr)
		if reply := recv(t, viewer); !reply.OK {
			t.Fatalf("punch reply: %+v", reply)
		}
		recv(t, host)
	}
	punch()
	entries := log.Snapshot(0, 10)
	if len(entries) != 1 || entries[0].ToID != "devbox" ||
		entries[0].FromIP != "127.0.0.1" || entries[0].ToIP != "127.0.0.1" {
		t.Fatalf("punch log: %+v", entries)
	}
	punch()
	if stats := log.Stats(); stats.Total != 2 || stats.Deduped != 1 || stats.Stored != 1 {
		t.Fatalf("dedupe stats: %+v", stats)
	}

	unknown := listenUDP(t)
	unknown.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "ghost", Nonce: "0123456789abcdef",
	}), meetAddr)
	if reply := recv(t, unknown); reply.OK {
		t.Fatalf("unknown host accepted: %+v", reply)
	}
	if stats := log.Stats(); stats.Total != 2 {
		t.Fatalf("failed punch was logged: %+v", stats)
	}
}

func TestRateLimiter(t *testing.T) {
	l := newLimiter(2, 50*time.Millisecond)
	if !l.allow("198.51.100.1") || !l.allow("198.51.100.1") {
		t.Fatal("first two requests were refused")
	}
	if l.allow("198.51.100.1") {
		t.Fatal("third request in the window was allowed")
	}
	if !l.allow("198.51.100.2") {
		t.Fatal("another source was refused")
	}
	time.Sleep(60 * time.Millisecond)
	if !l.allow("198.51.100.1") {
		t.Fatal("window did not reset")
	}

	disabled := newLimiter(0, time.Second)
	for i := 0; i < 100; i++ {
		if !disabled.allow("198.51.100.3") {
			t.Fatal("zero limit still refused a request")
		}
	}
}

func TestKeepaliveOverIPv6(t *testing.T) {
	priv := identity.PrivateKeyFromSecret("host-secret")
	pub := identity.PublicKeyText(priv.Public().(ed25519.PublicKey))
	reg := registry.New(5 * time.Second)
	if _, err := reg.Register(registry.RegisterRequest{
		ID: "devbox", PublicKey: pub, TTL: time.Minute,
	}); err != nil {
		t.Fatal(err)
	}
	meet, err := New("[::1]:0", reg, func(port int) string { return "192.0.2.1:41000" },
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Skipf("IPv6 loopback unavailable: %v", err)
	}
	serverIdentity, err := identity.GenerateServer()
	if err != nil {
		t.Fatal(err)
	}
	meet.SetIdentity(serverIdentity)
	go meet.Serve()
	t.Cleanup(meet.Close)
	meetAddr, err := net.ResolveUDPAddr("udp", meet.Addr())
	if err != nil {
		t.Fatal(err)
	}
	host, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Skipf("IPv6 loopback socket unavailable: %v", err)
	}
	t.Cleanup(func() { host.Close() })

	host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeKeepalive, HostID: "devbox"}), meetAddr)
	challenge := recv(t, host)
	if challenge.Type != wire.TypeKeepaliveChallenge || challenge.Nonce == "" {
		t.Fatalf("challenge: %+v", challenge)
	}
	if challenge.Candidate != "" || challenge.CandidateV6 != host.LocalAddr().String() {
		t.Fatalf("challenge candidates: %+v", challenge)
	}
	message := identity.KeepaliveMessage("devbox", challenge.CandidateV6, challenge.Nonce)
	host.WriteToUDP(wire.Encode(wire.Message{
		Type:         wire.TypeKeepaliveAuth,
		HostID:       "devbox",
		Nonce:        challenge.Nonce,
		CandidateV6:  challenge.CandidateV6,
		Signature:    identity.Sign(priv, message),
		LocalAddrsV6: []string{"[fd00::9]:14434"},
		NATType:      "cone",
	}), meetAddr)
	ack := recv(t, host)
	if !ack.OK || ack.Candidate != "" || ack.CandidateV6 != host.LocalAddr().String() {
		t.Fatalf("ack: %+v", ack)
	}
	if len(ack.LocalAddrsV6) != 1 || ack.LocalAddrsV6[0] != "[fd00::9]:14434" {
		t.Fatalf("ack local addrs: %+v", ack.LocalAddrsV6)
	}
	stored, _ := reg.Lookup("devbox")
	if stored.Candidate != "" || stored.CandidateV6 == "" {
		t.Fatalf("stored candidates: %+v", stored)
	}

	viewer, err := net.ListenUDP("udp6", &net.UDPAddr{IP: net.IPv6loopback})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { viewer.Close() })
	const nonce = "0123456789abcdef"
	viewer.WriteToUDP(wire.Encode(wire.Message{
		Type: wire.TypePunchRequest, HostID: "devbox", Nonce: nonce,
	}), meetAddr)
	reply := recv(t, viewer)
	if !reply.OK || reply.Candidate != "" || reply.CandidateV6 != host.LocalAddr().String() {
		t.Fatalf("punch reply: %+v", reply)
	}
	notify := recv(t, host)
	if notify.Type != wire.TypePunchPeer || notify.Candidate != "" ||
		notify.CandidateV6 != viewer.LocalAddr().String() {
		t.Fatalf("host notification: %+v", notify)
	}
}
