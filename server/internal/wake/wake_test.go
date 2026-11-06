package wake

import (
	"errors"
	"testing"
	"time"
)

const testLink = "192.168.1.255:9"

func TestRequestCooldown(t *testing.T) {
	now := time.Unix(1000, 0)
	broker := New(time.Minute, 30*time.Second, 8)
	broker.SetClock(func() time.Time { return now })
	if _, _, err := broker.Request("target", "AA:BB:CC:DD:EE:FF", []string{testLink}); err != nil {
		t.Fatal(err)
	}
	_, wait, err := broker.Request("target", "AA:BB:CC:DD:EE:FF", []string{testLink})
	if !errors.Is(err, ErrCooldown) || wait != 30*time.Second {
		t.Fatalf("second request = %v after %v", err, wait)
	}
	now = now.Add(31 * time.Second)
	if _, _, err := broker.Request("target", "AA:BB:CC:DD:EE:FF", []string{testLink}); err != nil {
		t.Fatalf("request after cooldown: %v", err)
	}
}

func TestPendingForDeliversOncePerAgent(t *testing.T) {
	broker := New(time.Minute, 30*time.Second, 8)
	job, _, err := broker.Request("target", "AA:BB:CC:DD:EE:FF", []string{testLink})
	if err != nil {
		t.Fatal(err)
	}
	first := broker.PendingFor("proxy-a", true, []string{testLink})
	if len(first) != 1 || first[0].ID != job.ID || first[0].Target != "target" {
		t.Fatalf("first delivery = %+v", first)
	}
	if len(first[0].Broadcasts) != 1 || first[0].Broadcasts[0] != testLink {
		t.Fatalf("job broadcasts = %v", first[0].Broadcasts)
	}
	if second := broker.PendingFor("proxy-a", true, []string{testLink}); len(second) != 0 {
		t.Fatalf("repeated delivery = %+v", second)
	}
	if other := broker.PendingFor("proxy-b", true, []string{testLink}); len(other) != 1 {
		t.Fatalf("other agent delivery = %+v", other)
	}
	if own := broker.PendingFor("target", true, []string{testLink}); len(own) != 0 {
		t.Fatalf("target received its own job = %+v", own)
	}
}

func TestPendingForRequiresRelayAndSharedLink(t *testing.T) {
	broker := New(time.Minute, 30*time.Second, 8)
	if _, _, err := broker.Request("target", "AA:BB:CC:DD:EE:FF", []string{testLink}); err != nil {
		t.Fatal(err)
	}
	if got := broker.PendingFor("plain", false, []string{testLink}); len(got) != 0 {
		t.Fatalf("non-relay delivery = %+v", got)
	}
	if got := broker.PendingFor("far", true, []string{"10.0.0.255:9"}); len(got) != 0 {
		t.Fatalf("foreign-link delivery = %+v", got)
	}
	if got := broker.PendingFor("proxy", true, nil); len(got) != 0 {
		t.Fatalf("agent without link received = %+v", got)
	}
	// The skipped registrations must not consume the job.
	if got := broker.PendingFor("proxy", true, []string{testLink}); len(got) != 1 {
		t.Fatalf("matching relay delivery = %+v", got)
	}
}

func TestJobsExpire(t *testing.T) {
	now := time.Unix(2000, 0)
	broker := New(time.Minute, 30*time.Second, 8)
	broker.SetClock(func() time.Time { return now })
	if _, _, err := broker.Request("target", "AA:BB:CC:DD:EE:FF", []string{testLink}); err != nil {
		t.Fatal(err)
	}
	now = now.Add(61 * time.Second)
	if got := broker.PendingFor("proxy", true, []string{testLink}); len(got) != 0 {
		t.Fatalf("expired job delivered: %+v", got)
	}
	if broker.Len() != 0 {
		t.Fatalf("queue length = %d", broker.Len())
	}
}

func TestQueueFull(t *testing.T) {
	broker := New(time.Minute, time.Second, 1)
	if _, _, err := broker.Request("one", "AA:BB:CC:DD:EE:FF", []string{testLink}); err != nil {
		t.Fatal(err)
	}
	if _, _, err := broker.Request("two", "AA:BB:CC:DD:EE:01", []string{testLink}); !errors.Is(err, ErrQueueFull) {
		t.Fatalf("second target error = %v", err)
	}
}
