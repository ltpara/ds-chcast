package strategy

import (
	"encoding/json"
	"fmt"
	"strings"
	"unicode/utf8"
)

const (
	Applied        = "applied"
	PendingRestart = "pending_restart"
	PendingSession = "pending_session"
	Unsupported    = "unsupported"
	Failed         = "failed"
)

type Result struct {
	Status string `json:"status"`
	Detail string `json:"detail,omitempty"`
}

// Receipt is an application result, never a delivery acknowledgement. Sequence
// is persisted by the consumer and increases within a snapshot revision, so
// delayed/replayed results cannot replace a newer outcome for the same config.
type Receipt struct {
	Revision int64             `json:"revision"`
	Digest   string            `json:"digest"`
	Sequence int64             `json:"sequence"`
	Status   string            `json:"status"`
	Results  map[string]Result `json:"results"`
}

func statusRank(status string) int {
	switch status {
	case Applied:
		return 1
	case PendingSession:
		return 2
	case PendingRestart:
		return 3
	case Unsupported:
		return 4
	case Failed:
		return 5
	default:
		return 0
	}
}

// Validate requires an outcome for every requested key. Otherwise a client
// that only implemented part of a document could claim full application.
func (r Receipt) Validate(settings Settings) error {
	if r.Revision <= 0 || r.Revision > MaxRevision || r.Sequence <= 0 || r.Sequence > MaxRevision || len(r.Digest) != 64 {
		return fmt.Errorf("%w: invalid receipt identity or sequence", ErrInvalid)
	}
	if r.Results == nil || len(r.Results) != len(settings) || statusRank(r.Status) == 0 {
		return fmt.Errorf("%w: receipt must cover every setting", ErrInvalid)
	}
	rank := 0
	for key, result := range r.Results {
		if _, exists := settings[key]; !exists {
			return fmt.Errorf("%w: unexpected result key", ErrInvalid)
		}
		current := statusRank(result.Status)
		if current == 0 || len(result.Detail) > 512 || !utf8.ValidString(result.Detail) || strings.ContainsAny(result.Detail, "\x00\r\n") {
			return fmt.Errorf("%w: invalid setting result", ErrInvalid)
		}
		if current > rank {
			rank = current
		}
	}
	if len(settings) > 0 && statusRank(r.Status) != rank {
		return fmt.Errorf("%w: overall status disagrees with setting outcomes", ErrInvalid)
	}
	// Removing a previous overlay may still require a restart or fail; the
	// empty document therefore also carries an explicit application outcome.
	return nil
}

// CanonicalReceipt is also the signed payload. Go's JSON encoder sorts map
// keys; the wire envelope's formatting is not part of the signature.
func CanonicalReceipt(receipt Receipt) ([]byte, error) { return json.Marshal(receipt) }
