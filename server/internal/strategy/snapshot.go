package strategy

import "fmt"

// Snapshot is a complete per-device/target overlay. An empty settings object
// explicitly revokes a previous overlay; a missing response never does.
type Snapshot struct {
	PeerID      string   `json:"peer_id"`
	Target      string   `json:"target"`
	Revision    int64    `json:"revision"`
	PolicyID    string   `json:"policy_id,omitempty"`
	Settings    Settings `json:"settings"`
	Digest      string   `json:"digest"`
	UpdatedAtMS int64    `json:"updated_at_ms"`
}

func (s Snapshot) Validate(peer, target string) error {
	if s.PeerID != peer || s.Target != target || !ValidTarget(target) || s.Revision <= 0 || s.Revision > MaxRevision || s.Settings == nil {
		return fmt.Errorf("%w: snapshot identity is invalid", ErrInvalid)
	}
	filtered, err := ForTarget(s.Settings, target)
	if err != nil {
		return err
	}
	if len(filtered) != len(s.Settings) {
		return fmt.Errorf("%w: snapshot contains another target's settings", ErrInvalid)
	}
	encoded, err := Encode(filtered)
	if err != nil {
		return err
	}
	if s.Digest != Digest(target, encoded) {
		return fmt.Errorf("%w: snapshot digest mismatch", ErrInvalid)
	}
	return nil
}
