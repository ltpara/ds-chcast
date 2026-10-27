package agent

import (
	"reflect"
	"testing"
)

func TestCarrierCandidateOrder(t *testing.T) {
	cases := []struct {
		name      string
		primary   string
		alternate string
		preferAlt bool
		want      []string
	}{
		{
			name:    "IPv4 only",
			primary: "relay.example:8448",
			want:    []string{"relay.example:8448"},
		},
		{
			name:      "IPv6 only",
			alternate: "[2001:db8::5]:8448",
			preferAlt: true,
			want:      []string{"[2001:db8::5]:8448"},
		},
		{
			name:      "IPv4 preferred",
			primary:   "relay.example:8448",
			alternate: "[2001:db8::5]:8448",
			want:      []string{"relay.example:8448", "[2001:db8::5]:8448"},
		},
		{
			name:      "IPv6 preferred",
			primary:   "relay.example:8448",
			alternate: "[2001:db8::5]:8448",
			preferAlt: true,
			want:      []string{"[2001:db8::5]:8448", "relay.example:8448"},
		},
		{
			name:      "same endpoint twice",
			primary:   "relay.example:8448",
			alternate: "relay.example:8448",
			want:      []string{"relay.example:8448"},
		},
		{
			name:      "malformed endpoints dropped",
			primary:   "relay.example",
			alternate: ":8448",
			want:      nil,
		},
		{
			name:      "whitespace trimmed",
			primary:   " relay.example:8448 ",
			alternate: " [2001:db8::5]:8448 ",
			want:      []string{"relay.example:8448", "[2001:db8::5]:8448"},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := carrierCandidateOrder(tc.primary, tc.alternate, tc.preferAlt)
			if len(got) != len(tc.want) || (len(got) > 0 && !reflect.DeepEqual(got, tc.want)) {
				t.Fatalf("carrierCandidateOrder(%q, %q, %v) = %v, want %v",
					tc.primary, tc.alternate, tc.preferAlt, got, tc.want)
			}
		})
	}
}
