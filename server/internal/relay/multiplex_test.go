package relay

import (
	"bufio"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strconv"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/carrier"
	"github.com/ltpara/ds-chcast/server/internal/identity"
	"github.com/ltpara/ds-chcast/server/internal/wire"
)

func TestMuxReplacementKeepsIndependentPolicySessions(t *testing.T) {
	policy, err := newPolicy(DefaultPolicyConfig())
	if err != nil {
		t.Fatal(err)
	}
	noSend := func([]byte) error { return nil }
	a := newMuxCarrier(policy, "devbox", "shared:same-host", "", 0, noSend, nil)
	b := newMuxCarrier(policy, "devbox", "shared:same-host", "", 0, noSend, nil)
	aSession, err := policy.Open(a.key+"/1", "127.0.0.1")
	if err != nil {
		t.Fatal(err)
	}
	bSession, err := policy.Open(b.key+"/1", "127.0.0.1")
	if err != nil {
		t.Fatal(err)
	}
	if policy.Stats().Sessions != 2 {
		t.Fatal("replacement hid the original viewer's policy session")
	}
	aSession.Close()
	if policy.Stats().Sessions != 1 {
		t.Fatal("old-link cleanup removed the replacement viewer")
	}
	bSession.Close()
}

func openMuxHost(t *testing.T, address, id, secret string) *sharedLink {
	t.Helper()
	conn, err := net.DialTimeout("tcp", address, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	conn.SetDeadline(time.Now().Add(3 * time.Second))
	hello, _ := json.Marshal(carrier.HostHello{HostID: id, Multiplex: true})
	io.WriteString(conn, carrier.MagicHost+string(hello)+"\n")
	br := bufio.NewReader(conn)
	line, err := carrier.ReadLine(br, carrier.MaxHandshake)
	text, ok := cutChallenge(line)
	var challenge carrier.HostChallenge
	if err != nil || !ok || json.Unmarshal([]byte(text), &challenge) != nil || !challenge.Multiplex {
		t.Fatalf("mux challenge: %q, %v", line, err)
	}
	priv := identity.PrivateKeyFromSecret(secret)
	auth, _ := json.Marshal(carrier.HostAuth{HostID: id, Nonce: challenge.Nonce, Multiplex: true,
		Signature: identity.Sign(priv, identity.CarrierHostMessage(id, "MUX1", challenge.Nonce))})
	io.WriteString(conn, carrier.MagicAuth+string(auth)+"\n")
	line, err = carrier.ReadLine(br, carrier.MaxHandshake)
	if err != nil || line+"\n" != carrier.MagicOK {
		t.Fatalf("mux ack: %q, %v", line, err)
	}
	return &sharedLink{conn: conn, br: br, frames: carrier.NewFrameReader(br), send: carrier.NewFrameWriter(conn)}
}

func TestMultiplexCarrierIsolatesTCPAndWebSocketViewers(t *testing.T) {
	for _, shared := range []bool{false, true} {
		name := "per-port"
		if shared {
			name = "shared"
		}
		t.Run(name, func(t *testing.T) {
			m, reg, address, token, secret := startSharedRelay(t, "devbox", "mux-secret")
			if !shared {
				m.requireViewerToken = true
				host, _ := reg.Lookup("devbox")
				address = net.JoinHostPort("127.0.0.1", strconv.Itoa(host.RelayPort))
			}
			host := openMuxHost(t, address, "devbox", secret)
			open := func(token string) (net.Conn, *carrier.FrameReader, *carrier.FrameWriter) {
				if shared {
					return openSharedRawViewer(t, address, "devbox", token)
				}
				conn, err := net.Dial("tcp", address)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { conn.Close() })
				hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: token})
				io.WriteString(conn, carrier.MagicRaw+string(hello)+"\n")
				return conn, carrier.NewFrameReader(conn), carrier.NewFrameWriter(conn)
			}
			a, ar, aw := open(token)
			ws, err := net.Dial("tcp", address)
			if err != nil {
				t.Fatal(err)
			}
			defer ws.Close()
			ws.SetDeadline(time.Now().Add(3 * time.Second))
			io.WriteString(ws, "GET /chast HTTP/1.1\r\nHost: carrier\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: chast-carrier.v1\r\n\r\n")
			hello, _ := json.Marshal(carrier.SharedViewerHello{HostID: "devbox", Token: token})
			ws.Write(clientBinaryFrame(hello))
			br := bufio.NewReader(ws)
			response, err := http.ReadResponse(br, nil)
			if err != nil || response.StatusCode != 101 {
				t.Fatalf("websocket upgrade: %v", err)
			}
			aw.WriteFrame([]byte("tcp-first"))
			ws.Write(clientBinaryFrame([]byte("ws-second")))
			routes := map[string]uint64{}
			for len(routes) < 2 {
				id, payload, ok := carrier.DecodeMux(host.read(t))
				if !ok {
					t.Fatal("invalid multiplex data")
				}
				routes[string(payload)] = id
			}
			if routes["tcp-first"] == 0 || routes["ws-second"] == 0 || routes["tcp-first"] == routes["ws-second"] {
				t.Fatalf("routes are not independent: %v", routes)
			}
			// Forged and unknown session IDs must not be delivered to either viewer.
			host.send.WriteFrame(carrier.EncodeMux(999999, []byte("unknown")))
			host.send.WriteFrame([]byte("malformed-envelope"))
			host.send.WriteFrame(carrier.EncodeMux(routes["ws-second"], []byte("ws-reply")))
			host.send.WriteFrame(carrier.EncodeMux(routes["tcp-first"], []byte("tcp-reply")))
			a.SetReadDeadline(time.Now().Add(3 * time.Second))
			got, err := ar.ReadFrame()
			if err != nil || string(got) != "tcp-reply" {
				t.Fatalf("TCP got %q, %v", got, err)
			}
			if got := readWSBinary(t, br); string(got) != "ws-reply" {
				t.Fatalf("WS got %q", got)
			}
			bad, _, _ := open("wrong-token")
			expectClosed(t, bad)
			a.Close()
			ws.Write(clientBinaryFrame([]byte("still-connected")))
			for {
				id, payload, ok := carrier.DecodeMux(host.read(t))
				if !ok {
					t.Fatal("invalid subsequent multiplex data")
				}
				if len(payload) == 0 {
					continue
				}
				if id != routes["ws-second"] || string(payload) != "still-connected" {
					t.Fatalf("wrong route after close: %d %q", id, payload)
				}
				host.send.WriteFrame(carrier.EncodeMux(id, []byte("survived")))
				break
			}
			if got := readWSBinary(t, br); string(got) != "survived" {
				t.Fatalf("WS after peer close got %q", got)
			}
			if shared {
				waitForSharedStats(t, m, func(s SharedCarrierStats) bool { return s.Viewers == 1 })
			}
		})
	}
}

func TestMultiplexUDPRequiresSignedCapabilityAndKeepsAuthorizedPeers(t *testing.T) {
	m, reg, address, priv := startRelayWith(t, "devbox", "mux-udp", WithViewerTokenRequired(true))
	host := listenUDP(t)
	handshake := func(signed bool) wire.Message {
		host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayHello, HostID: "devbox", Multiplex: true}), address)
		challenge, err := wire.Decode(recv(t, host, time.Second))
		if err != nil || !challenge.Multiplex {
			t.Fatal("multiplex challenge missing")
		}
		candidate := challenge.Candidate
		if signed {
			candidate += "\nMUX1"
		}
		host.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayAuth, HostID: "devbox", Multiplex: true,
			Candidate: challenge.Candidate, Nonce: challenge.Nonce,
			Signature: identity.Sign(priv, identity.RelayMessage("devbox", address.Port, candidate, challenge.Nonce))}), address)
		ack, err := wire.Decode(recv(t, host, time.Second))
		if err != nil {
			t.Fatal(err)
		}
		return ack
	}
	if handshake(false).OK {
		t.Fatal("unsigned capability change was accepted")
	}
	if !handshake(true).OK {
		t.Fatal("signed multiplex login failed")
	}
	record, _ := reg.Lookup("devbox")
	a, b, bad := listenUDP(t), listenUDP(t), listenUDP(t)
	for _, peer := range []*net.UDPConn{a, b} {
		peer.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayViewer, HostID: "devbox", Token: record.CarrierToken}), address)
		ack, err := wire.Decode(recv(t, peer, time.Second))
		if err != nil || !ack.OK {
			t.Fatalf("viewer auth: %v %+v", err, ack)
		}
	}
	bad.WriteToUDP(wire.Encode(wire.Message{Type: wire.TypeRelayViewer, HostID: "devbox", Token: "wrong"}), address)
	ack, _ := wire.Decode(recv(t, bad, time.Second))
	if ack.OK {
		t.Fatal("invalid viewer accepted")
	}
	bad.WriteToUDP([]byte("must-be-dropped"), address)
	a.WriteToUDP([]byte("alpha"), address)
	b.WriteToUDP([]byte("beta"), address)
	routes := map[string]uint64{}
	for len(routes) < 2 {
		id, payload, ok := carrier.DecodeMux(recv(t, host, time.Second))
		if !ok {
			t.Fatal("bad envelope")
		}
		routes[string(payload)] = id
	}
	if routes["alpha"] == 0 || routes["beta"] == 0 || routes["alpha"] == routes["beta"] {
		t.Fatalf("routes: %v", routes)
	}
	host.WriteToUDP(carrier.EncodeMux(routes["beta"], []byte("B")), address)
	host.WriteToUDP(carrier.EncodeMux(routes["alpha"], []byte("A")), address)
	if string(recv(t, a, time.Second)) != "A" || string(recv(t, b, time.Second)) != "B" {
		t.Fatal("UDP crossed viewer routes")
	}
	if len(m.Stats().Sessions) != 2 {
		t.Fatalf("policy did not track both sessions: %+v", m.Stats().Sessions)
	}
}
