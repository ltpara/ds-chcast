package agent

import (
	"net"
	"path/filepath"
	"testing"
)

// TestApplyHostEvent covers the statements a host receives over its event
// stream: a policy push is applied once, an older version is ignored, a policy
// removal relaxes without ending sessions and a revoke names the retired key.
func TestApplyHostEvent(t *testing.T) {
	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("udp: %v", err)
	}
	defer conn.Close()
	var revoked []RevokeEvent
	h := &HostAgent{
		conn: conn,
		cfg: HostConfig{
			PolicyState: filepath.Join(t.TempDir(), "policy.json"),
			OnRevoke: func(event RevokeEvent) {
				revoked = append(revoked, event)
			},
		},
		log: quietLogger(),
	}
	managed := true
	h.applyHostEvent(hostEvent{
		Kind: "policy", Managed: &managed, Mode: "open",
		PermissionCeiling: 7, PolicyVersion: 2, UpdatedAtMS: 456,
	})
	if len(revoked) != 1 || revoked[0].Kind != "policy" || revoked[0].Policy == nil {
		t.Fatalf("policy push = %+v", revoked)
	}
	if policy := revoked[0].Policy; policy.Mode != "open" || policy.PolicyVersion != 2 ||
		policy.PermissionCeiling != 7 {
		t.Fatalf("pushed policy = %+v", policy)
	}
	if h.policy == nil || h.policy.PolicyVersion != 2 {
		t.Fatalf("agent policy = %+v", h.policy)
	}

	h.applyHostEvent(hostEvent{
		Kind: "policy", Managed: &managed, Mode: "restricted",
		PolicyVersion: 1, Reason: "out of order",
	})
	if len(revoked) != 1 {
		t.Fatalf("stale policy was applied: %+v", revoked)
	}

	h.applyHostEvent(hostEvent{
		Kind: "policy", Managed: boolPointer(false), Reason: "policy removed",
	})
	if len(revoked) != 2 || revoked[1].Policy != nil ||
		revoked[1].Reason != "policy removed" {
		t.Fatalf("policy removal = %+v", revoked)
	}
	if h.policy != nil {
		t.Fatalf("agent kept a removed policy: %+v", h.policy)
	}

	h.applyHostEvent(hostEvent{Kind: "revoke", Kid: "kid-2026-11", Reason: "rotation"})
	if len(revoked) != 3 || revoked[2].Kind != "revoke" || revoked[2].Kid != "kid-2026-11" {
		t.Fatalf("revoke = %+v", revoked)
	}
	h.applyHostEvent(hostEvent{Kind: "revoke"})
	if len(revoked) != 3 {
		t.Fatalf("empty revoke was reported: %+v", revoked)
	}
}

func boolPointer(value bool) *bool {
	return &value
}
