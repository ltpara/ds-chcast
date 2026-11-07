// Package wire defines the small control protocol shared by the server and the
// tunnel agents. Control datagrams are distinguishable from the opaque QUIC
// payload by their magic prefix; anything without it is forwarded untouched.
package wire

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
)

const Magic = "CHASTC1\n"

// Roles of a TCP punch announce. Both halves carry one; only the host can
// prove its half with the host key.
const (
	RoleViewer = "viewer"
	RoleHost   = "host"
)

const (
	TypeKeepalive          = "keepalive"
	TypeKeepaliveChallenge = "keepalive_challenge"
	TypeKeepaliveAuth      = "keepalive_auth"
	TypeKeepaliveAck       = "keepalive_ack"
	TypePunchRequest       = "punch_request"
	TypePunchPeer          = "punch_peer"
	TypePunch              = "punch"
	TypePunchProbe         = "punch_probe"
	TypePunchAck           = "punch_ack"
	TypeNatProbe           = "nat_probe"
	TypeNatProbeAck        = "nat_probe_ack"
	// TypeTCPPunch coordinates a TCP hole punch: agents announce themselves on
	// the server's TCP punch port and learn the endpoint the server observed
	// for their peer, mirroring RustDesk's punch-hole TCP channel.
	TypeTCPPunch = "tcp_punch"
	// TypeTCPDirect is the signed hello of a punched TCP link itself.
	TypeTCPDirect      = "tcp_direct"
	TypeRelayHello     = "relay_hello"
	TypeRelayChallenge = "relay_challenge"
	TypeRelayAuth      = "relay_auth"
	TypeRelayAck       = "relay_hello_ack"
	TypeRelayViewer    = "relay_viewer"
	TypeRelayViewerAck = "relay_viewer_ack"
	TypeBusy           = "busy"
)

// MaxControlSize bounds a control datagram. The agents use the same limit when
// reading, so a malformed length cannot turn into a large allocation.
const MaxControlSize = 2048

type Message struct {
	Multiplex bool   `json:"multiplex,omitempty"`
	Type      string `json:"type"`
	// Role distinguishes the host and viewer halves of a TCP punch. Both sides
	// sign their announce, but only the host key can prove the host half.
	Role   string `json:"role,omitempty"`
	HostID string `json:"host_id,omitempty"`
	Nonce  string `json:"nonce,omitempty"`
	// TID is the transaction id of one punch probe. An acknowledgement echoes
	// it, which is what proves a probe survived the round trip; the session
	// Nonce alone only proves where the peer learned the address.
	TID         string `json:"tid,omitempty"`
	Candidate   string `json:"candidate,omitempty"`
	CandidateV6 string `json:"candidate_v6,omitempty"`
	// SelfV6 is the agent's own IPv6 UDP endpoint, learned by asking the
	// kernel which source address a global route would use. RustDesk reports
	// the same value as local_addr_v6: without it a peer behind an IPv4-only
	// rendezvous port could never be told where to punch an IPv6 path.
	SelfV6     string   `json:"self_v6,omitempty"`
	LocalAddrs []string `json:"local_addrs,omitempty"`
	// LocalAddrsV6 mirrors LocalAddrs for IPv6. RustDesk carries the same split
	// as socket_addr/socket_addr_v6; a dual-stack agent reports both.
	LocalAddrsV6 []string `json:"local_addrs_v6,omitempty"`
	NATType      string   `json:"nat_type,omitempty"`
	// UpnpPort is the external UDP port an agent mapped on its gateway. It is
	// only a hint: the peer still has to be reached through the NAT, but a
	// stable mapping can rescue symmetric NATs where the observed source port
	// differs per destination. RustDesk carries the same field in its punch,
	// response and relay messages.
	UpnpPort    int    `json:"upnp_port,omitempty"`
	Fingerprint string `json:"fingerprint,omitempty"`
	PublicKey   string `json:"public_key,omitempty"`
	Binding     string `json:"binding,omitempty"`
	Signature   string `json:"signature,omitempty"`
	Auth        string `json:"auth,omitempty"`
	Token       string `json:"token,omitempty"`
	Timestamp   int64  `json:"timestamp,omitempty"`
	Relay       string `json:"relay,omitempty"`
	OK          bool   `json:"ok"`
	Error       string `json:"error,omitempty"`
	ExpiresMS   int64  `json:"expires_ms,omitempty"`
}

var ErrNotControl = errors.New("not a control datagram")

func Encode(m Message) []byte {
	body, err := json.Marshal(m)
	if err != nil {
		// Message only holds strings, bools and ints, so this cannot happen.
		panic(fmt.Sprintf("wire: encode: %v", err))
	}
	out := make([]byte, 0, len(Magic)+len(body))
	out = append(out, Magic...)
	return append(out, body...)
}

func Decode(p []byte) (Message, error) {
	if len(p) > MaxControlSize {
		return Message{}, ErrNotControl
	}
	if !bytes.HasPrefix(p, []byte(Magic)) {
		return Message{}, ErrNotControl
	}
	var m Message
	if err := json.Unmarshal(p[len(Magic):], &m); err != nil {
		return Message{}, fmt.Errorf("control json: %w", err)
	}
	if m.Type == "" {
		return Message{}, errors.New("control message without a type")
	}
	return m, nil
}

func IsControl(p []byte) bool {
	return bytes.HasPrefix(p, []byte(Magic))
}
