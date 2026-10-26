package ratelimit

import (
	"testing"
	"time"
)

func TestBurstThenRefill(t *testing.T) {
	now := time.Unix(1000, 0)
	l := New(2, 3)
	l.SetClock(func() time.Time { return now })

	for i := 0; i < 3; i++ {
		if !l.Allow("client") {
			t.Fatalf("burst request %d was rejected", i+1)
		}
	}
	if l.Allow("client") {
		t.Fatal("request beyond the burst was allowed")
	}
	now = now.Add(500 * time.Millisecond)
	if !l.Allow("client") {
		t.Fatal("one token should have refilled after 0.5 s")
	}
	if l.Allow("client") {
		t.Fatal("a second token appeared before the refill interval")
	}
	now = now.Add(time.Second)
	if !l.Allow("client") || !l.Allow("client") {
		t.Fatal("two tokens should have refilled after one second")
	}
	if l.Allow("client") {
		t.Fatal("bucket exceeded its burst after refill")
	}
}

func TestKeysAreIndependent(t *testing.T) {
	l := New(1, 1)
	if !l.Allow("a") {
		t.Fatal("first key rejected")
	}
	if l.Allow("a") {
		t.Fatal("first key was not limited")
	}
	if !l.Allow("b") {
		t.Fatal("second key inherited the first key's bucket")
	}
}

func TestDisabledLimiterAllowsEverything(t *testing.T) {
	var nilLimiter *Limiter
	if !nilLimiter.Allow("a") {
		t.Fatal("nil limiter rejected a request")
	}
	l := New(0, 1)
	if l.Enabled() {
		t.Fatal("rate 0 should disable the limiter")
	}
	for i := 0; i < 100; i++ {
		if !l.Allow("a") {
			t.Fatal("disabled limiter rejected a request")
		}
	}
	if l.Len() != 0 {
		t.Fatalf("disabled limiter tracked %d keys", l.Len())
	}
}

func TestIdleBucketsAreDropped(t *testing.T) {
	now := time.Unix(2000, 0)
	l := New(1, 1)
	l.SetClock(func() time.Time { return now })
	if !l.Allow("old") {
		t.Fatal("initial request rejected")
	}
	now = now.Add(idleTTL + time.Minute)
	if !l.Allow("new") {
		t.Fatal("request after idle window rejected")
	}
	if l.Len() != 1 {
		t.Fatalf("tracked %d keys, want only the fresh one", l.Len())
	}
}
