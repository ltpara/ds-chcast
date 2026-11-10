package relay

import (
	"crypto/ed25519"
	"encoding/base64"
	"strconv"
	"sync"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/acl"
	"github.com/ltpara/ds-chcast/server/internal/identity"
)

const (
	// transportProofWindow bounds the age of a presented proof. A credential
	// itself lives for a minute; the proof must be fresher than that so a
	// captured hello cannot authorize a new source long after it was sent.
	transportProofWindow = 30 * time.Second
	// transportReplayCapacity bounds the seen-proof cache. The guard never
	// evicts a live entry to make room: when full it refuses new admissions,
	// which fails closed instead of forgetting a replay.
	transportReplayCapacity = 8192
)

// transportGuard verifies short-lived transport credentials presented to a
// relay or carrier. It is built from deployment-pinned issuer keys, never from
// data supplied by the caller.
type transportGuard struct {
	verifier *acl.TransportVerifier

	mu   sync.Mutex
	seen map[string]transportReplay
}

type transportReplay struct {
	peer    string
	expires time.Time
}

func newTransportGuard(verifier *acl.TransportVerifier) *transportGuard {
	return &transportGuard{
		verifier: verifier,
		seen:     make(map[string]transportReplay),
	}
}

// verify checks the signature, audience, host binding, proof of possession and
// replay state of one presented credential. The peer is the source the
// datagram or hello arrived from; an identical proof may repeat from the same
// peer (a retransmit) but never from another one.
func (g *transportGuard) verify(signedB64, proof, hostID, nonce string, timestamp int64,
	audience, peer string, now time.Time) bool {
	if g == nil || g.verifier == nil || signedB64 == "" || proof == "" {
		return false
	}
	if len(nonce) < 8 || len(nonce) > 64 || timestamp <= 0 {
		return false
	}
	drift := now.Unix() - timestamp
	if drift < 0 {
		drift = -drift
	}
	if drift > int64(transportProofWindow/time.Second) {
		return false
	}
	signed, err := base64.RawURLEncoding.DecodeString(signedB64)
	if err != nil {
		return false
	}
	ticket, err := g.verifier.Verify(signed, now)
	if err != nil || ticket.HostID != hostID || ticket.Audience != audience {
		return false
	}
	message := identity.TransportProofMessage(audience, hostID, nonce, timestamp)
	if !identity.Verify(ed25519.PublicKey(ticket.ClientPublicKey[:]), message, proof) {
		return false
	}
	return g.admit(ticket.ID, nonce, timestamp, ticket.ExpiresAt, peer, now)
}

func (g *transportGuard) admit(ticketID, nonce string, timestamp int64, expiresAt int64,
	peer string, now time.Time) bool {
	key := ticketID + "\x00" + nonce + "\x00" + strconv.FormatInt(timestamp, 10)
	g.mu.Lock()
	defer g.mu.Unlock()
	for seenKey, entry := range g.seen {
		if !now.Before(entry.expires) {
			delete(g.seen, seenKey)
		}
	}
	if entry, ok := g.seen[key]; ok {
		return entry.peer == peer
	}
	if len(g.seen) >= transportReplayCapacity {
		return false
	}
	expiry := time.Unix(expiresAt, 0).Add(transportProofWindow)
	if !expiry.After(now) {
		expiry = now.Add(transportProofWindow)
	}
	g.seen[key] = transportReplay{peer: peer, expires: expiry}
	return true
}
