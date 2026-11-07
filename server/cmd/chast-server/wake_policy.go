package main

import (
	"encoding/json"
	"fmt"
	"os"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

// The policy is operator configuration, never a claim from an agent. Agent
// registration already binds each ID permanently to its signing public key.
func loadWakeRelayPolicy(path string) (map[string][]string, error) {
	if path == "" {
		return nil, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var policy map[string][]string
	if err := json.Unmarshal(data, &policy); err != nil {
		return nil, err
	}
	if policy == nil {
		return nil, fmt.Errorf("expected an object mapping target IDs to relay ID arrays")
	}
	for target, relays := range policy {
		if !registry.ValidID(target) {
			return nil, fmt.Errorf("invalid target ID %q", target)
		}
		for _, relay := range relays {
			if !registry.ValidID(relay) || relay == target {
				return nil, fmt.Errorf("invalid relay ID %q for target %q", relay, target)
			}
		}
	}
	return policy, nil
}

// wakeRelayPolicyCounts sums the policy so startup can report how many targets
// are authorized and how many distinct relays serve them.
func wakeRelayPolicyCounts(policy map[string][]string) (targets, relays int) {
	distinct := make(map[string]struct{})
	for _, allowed := range policy {
		if len(allowed) == 0 {
			continue
		}
		targets++
		for _, relay := range allowed {
			distinct[relay] = struct{}{}
		}
	}
	return targets, len(distinct)
}
