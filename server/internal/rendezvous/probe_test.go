package rendezvous

import (
	"context"
	"io"
	"log/slog"
	"strings"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/registry"
)

func discardLog() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestProbeTarget(t *testing.T) {
	cases := []struct {
		value, advertise, bound string
		wantTarget              string
		wantSkip                bool
		wantErr                 bool
	}{
		{"", "203.0.113.5", "0.0.0.0:8444", "", true, false},
		{"", "", "0.0.0.0:8444", "127.0.0.1:8444", false, false},
		{"no", "", "0.0.0.0:8444", "", true, false},
		{"off", "", "0.0.0.0:8444", "", true, false},
		{"auto", "", "0.0.0.0:8444", "127.0.0.1:8444", false, false},
		{"auto", "", "[::]:8444", "[::1]:8444", false, false},
		{"auto", "127.0.0.1", "0.0.0.0:8444", "127.0.0.1:8444", false, false},
		{"auto", "::1", "[::]:8444", "[::1]:8444", false, false},
		{"auto", "localhost", "127.0.0.1:8444", "127.0.0.1:8444", false, false},
		{"auto", "203.0.113.5", "0.0.0.0:8444", "", true, false},
		{"127.0.0.1:9999", "", "0.0.0.0:8444", "127.0.0.1:9999", false, false},
		{"[::1]:9999", "", "0.0.0.0:8444", "[::1]:9999", false, false},
		{"127.0.0.1", "", "0.0.0.0:8444", "", false, true},
		{"127.0.0.1:0", "", "0.0.0.0:8444", "", false, true},
	}
	for _, tc := range cases {
		target, skip, err := ProbeTarget(tc.value, tc.advertise, tc.bound)
		if tc.wantErr {
			if err == nil {
				t.Fatalf("ProbeTarget(%q, %q) was accepted", tc.value, tc.advertise)
			}
			continue
		}
		if err != nil {
			t.Fatalf("ProbeTarget(%q, %q): %v", tc.value, tc.advertise, err)
		}
		if target != tc.wantTarget {
			t.Fatalf("ProbeTarget(%q, %q) target = %q, want %q", tc.value, tc.advertise, target, tc.wantTarget)
		}
		if (skip != "") != tc.wantSkip {
			t.Fatalf("ProbeTarget(%q, %q) skip = %q", tc.value, tc.advertise, skip)
		}
	}
}

func TestProbeCompletesKeepalive(t *testing.T) {
	_, reg, addr, _ := startMeet(t)
	hosts := reg.Count()
	identities := reg.IdentityCount()
	rtt, err := Probe(context.Background(), addr.String(), reg, discardLog())
	if err != nil {
		t.Fatalf("probe: %v", err)
	}
	if rtt < 0 || rtt > time.Second {
		t.Fatalf("rtt = %s", rtt)
	}
	if reg.Count() != hosts || reg.IdentityCount() != identities {
		t.Fatalf("temporary host was not removed: hosts=%d identities=%d", reg.Count(), reg.IdentityCount())
	}
}

func TestProbeFailsWhenNoServerAnswers(t *testing.T) {
	conn := listenUDP(t)
	target := conn.LocalAddr().String()
	conn.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	started := time.Now()
	_, err := Probe(ctx, target, registry.New(time.Minute), discardLog())
	if err == nil {
		t.Fatal("probe against a closed port succeeded")
	}
	if !strings.Contains(err.Error(), "no keepalive acknowledgement") {
		t.Fatalf("error = %v", err)
	}
	if elapsed := time.Since(started); elapsed > 4*time.Second {
		t.Fatalf("probe took %s", elapsed)
	}
}
