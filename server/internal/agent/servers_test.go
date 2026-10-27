package agent

import (
	"context"
	"reflect"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/configpush"
	"github.com/ltpara/ds-chcast/server/internal/testutil"
)

func TestNewServerPool(t *testing.T) {
	pool, err := newServerPool(" https://one.example/ ", []string{
		"https://two.example",
		"https://one.example",
	})
	if err != nil {
		t.Fatalf("pool: %v", err)
	}
	want := []string{"https://one.example", "https://two.example"}
	if got := pool.List(); !reflect.DeepEqual(got, want) {
		t.Fatalf("list = %v, want %v", got, want)
	}
	if pool.Current() != "https://one.example" || pool.Len() != 2 {
		t.Fatalf("current = %q, len = %d", pool.Current(), pool.Len())
	}

	if _, err := newServerPool("one.example", nil); err == nil {
		t.Fatal("relative primary accepted")
	}
	if _, err := newServerPool("https://one.example", []string{"ftp://two.example"}); err == nil {
		t.Fatal("invalid extra accepted")
	}
}

func TestServerPoolNextRoundRobins(t *testing.T) {
	single, err := newServerPool("https://one.example", nil)
	if err != nil {
		t.Fatal(err)
	}
	if next, ok := single.Next(); ok || next != "https://one.example" {
		t.Fatalf("single next = %q/%v", next, ok)
	}

	pool, err := newServerPool("https://one.example", []string{"https://two.example"})
	if err != nil {
		t.Fatal(err)
	}
	if next, ok := pool.Next(); !ok || next != "https://two.example" {
		t.Fatalf("next = %q/%v", next, ok)
	}
	if next, ok := pool.Next(); !ok || next != "https://one.example" {
		t.Fatalf("wrap next = %q/%v", next, ok)
	}
}

func TestServerPoolApply(t *testing.T) {
	pool, err := newServerPool("https://one.example", nil)
	if err != nil {
		t.Fatal(err)
	}
	if pool.Apply(0, nil) {
		t.Fatal("empty list accepted")
	}
	if pool.Apply(0, []string{"relative"}) {
		t.Fatal("invalid list accepted")
	}
	if !pool.Apply(2, []string{"https://two.example"}) {
		t.Fatal("valid list rejected")
	}
	// The endpoint we are talking to is kept even when the pushed list omits it.
	want := []string{"https://two.example", "https://one.example"}
	if got := pool.List(); !reflect.DeepEqual(got, want) {
		t.Fatalf("list = %v, want %v", got, want)
	}
	if pool.Serial() != 2 {
		t.Fatalf("serial = %d", pool.Serial())
	}
	if pool.Apply(1, []string{"https://three.example"}) {
		t.Fatal("older serial applied")
	}
	if !pool.Apply(2, []string{"https://three.example", "https://four.example", "https://three.example"}) {
		t.Fatal("equal serial with a new list was not applied")
	}
	want = []string{"https://three.example", "https://four.example", "https://one.example"}
	if got := pool.List(); !reflect.DeepEqual(got, want) {
		t.Fatalf("list after equal serial = %v, want %v", got, want)
	}
	if pool.Apply(2, []string{"https://three.example", "https://four.example", "https://one.example"}) {
		t.Fatal("identical replay reported changed")
	}
	// Rotation stays inside the new list.
	if next, ok := pool.Next(); !ok || next != "https://three.example" {
		t.Fatalf("rotate = %q/%v", next, ok)
	}
	// The client stays on its current endpoint while it keeps answering.
	if !pool.Apply(3, []string{"https://three.example"}) || pool.Current() != "https://three.example" {
		t.Fatalf("apply moved current to %q", pool.Current())
	}
}

func TestHostFailsOverAndAdoptsPushedList(t *testing.T) {
	push, err := configpush.New(1, []string{"https://stale.example"})
	if err != nil {
		t.Fatalf("config: %v", err)
	}
	s := testutil.StartWith(t, testutil.Options{RendezvousConfig: push})
	serial, err := push.Set([]string{s.URL})
	if err != nil {
		t.Fatalf("publish list: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	echo := startEcho(t)
	host, err := StartHost(ctx, HostConfig{
		ServerURL:  "http://127.0.0.1:1",
		Servers:    []string{s.URL},
		ServerKey:  testutil.ServerKey,
		ID:         "devbox",
		Key:        "host-secret",
		LocalAddr:  echo.LocalAddr().String(),
		ListenAddr: "127.0.0.1:0",
		Keepalive:  time.Minute,
		Logger:     quietLogger(),
	})
	if err != nil {
		t.Fatalf("start host: %v", err)
	}
	t.Cleanup(host.Close)
	readyCtx, readyCancel := context.WithTimeout(ctx, 5*time.Second)
	defer readyCancel()
	if err := host.WaitReady(readyCtx); err != nil {
		t.Fatalf("host never became ready: %v", err)
	}
	if got := host.api.endpoint(); got != s.URL {
		t.Fatalf("agent still talks to %q, want %q", got, s.URL)
	}
	if got := host.servers.List(); !reflect.DeepEqual(got, []string{s.URL}) {
		t.Fatalf("pool = %v, want the pushed list", got)
	}
	if host.servers.Serial() != serial {
		t.Fatalf("serial = %d, want %d", host.servers.Serial(), serial)
	}
}

func TestViewerFailsOverAndAdoptsPushedList(t *testing.T) {
	push, err := configpush.New(1, []string{"https://stale.example"})
	if err != nil {
		t.Fatalf("config: %v", err)
	}
	s := testutil.StartWith(t, testutil.Options{RendezvousConfig: push})
	serial, err := push.Set([]string{s.URL})
	if err != nil {
		t.Fatalf("publish list: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	echo := startEcho(t)
	startHostAgent(t, ctx, s.URL, echo.LocalAddr().String())

	viewer, err := StartViewer(ctx, ViewerConfig{
		ServerURL:     "http://127.0.0.1:1",
		Servers:       []string{s.URL},
		ServerKey:     testutil.ServerKey,
		HostID:        "devbox",
		ListenAddr:    "127.0.0.1:0",
		Mode:          ModeAuto,
		DirectTimeout: time.Second,
		Logger:        quietLogger(),
	})
	if err != nil {
		t.Fatalf("start viewer: %v", err)
	}
	t.Cleanup(viewer.Close)
	if got := viewer.api.endpoint(); got != s.URL {
		t.Fatalf("viewer still talks to %q, want %q", got, s.URL)
	}
	if got := viewer.servers.List(); !reflect.DeepEqual(got, []string{s.URL}) {
		t.Fatalf("pool = %v, want the pushed list", got)
	}
	if viewer.servers.Serial() != serial {
		t.Fatalf("serial = %d, want %d", viewer.servers.Serial(), serial)
	}
	assertRoundTrip(t, viewer, ModeAuto)
}
