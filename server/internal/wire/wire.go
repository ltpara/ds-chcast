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

const (
	TypeKeepalive          = "keepalive"
	TypeKeepaliveChallenge = "keepalive_challenge"
	TypeKeepaliveAuth      = "keepalive_auth"
	TypeKeepaliveAck       = "keepalive_ack"
	TypePunchRequest       = "punch_request"
	TypePunchPeer          = "punch_peer"
	TypePunch              = "punch"
	TypeRelayHello         = "relay_hello"
	TypeRelayChallenge     = "relay_challenge"
	TypeRelayAuth          = "relay_auth"
	TypeRelayAck           = "relay_hello_ack"
	TypeBusy               = "busy"
)

// MaxControlSize bounds a control datagram. The agents use the same limit when
// reading, so a malformed length cannot turn into a large allocation.
const MaxControlSize = 2048

type Message struct {
	Type        string   `json:"type"`
	HostID      string   `json:"host_id,omitempty"`
	Nonce       string   `json:"nonce,omitempty"`
	Candidate   string   `json:"candidate,omitempty"`
	LocalAddrs  []string `json:"local_addrs,omitempty"`
	NATType     string   `json:"nat_type,omitempty"`
	Fingerprint string   `json:"fingerprint,omitempty"`
	PublicKey   string   `json:"public_key,omitempty"`
	Binding     string   `json:"binding,omitempty"`
	Signature   string   `json:"signature,omitempty"`
	Relay       string   `json:"relay,omitempty"`
	OK          bool     `json:"ok"`
	Error       string   `json:"error,omitempty"`
	ExpiresMS   int64    `json:"expires_ms,omitempty"`
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
