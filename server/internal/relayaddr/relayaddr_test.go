package relayaddr

import (
	"reflect"
	"sync"
	"testing"
)

func mustList(t *testing.T, defaults ...string) *List {
	t.Helper()
	list, err := New(defaults)
	if err != nil {
		t.Fatalf("New(%v): %v", defaults, err)
	}
	return list
}

func TestNewNormalizesAndDeduplicates(t *testing.T) {
	list := mustList(t, " 127.0.0.1 ", "localhost", "[::1]", "EXAMPLE.com", "example.com", "")
	want := []string{"127.0.0.1", "localhost", "::1", "example.com"}
	if got := list.Addrs(); !reflect.DeepEqual(got, want) {
		t.Fatalf("addrs = %v, want %v", got, want)
	}
}

func TestRejectsInvalidAddresses(t *testing.T) {
	for _, raw := range []string{
		"example.com:21117",
		"[::1]:21117",
		"http://example.com",
		"bad host",
		"example.com/path",
		"@example.com",
		"[example.com]",
		"-bad.example",
		"bad-.example",
	} {
		if _, err := New([]string{raw}); err == nil {
			t.Fatalf("New accepted %q", raw)
		}
	}
	empty := mustList(t)
	if empty.Len() != 0 || empty.Next() != "" {
		t.Fatalf("empty list should stay empty, got %v", empty.Addrs())
	}
	if got := empty.Join(41000); len(got) != 0 {
		t.Fatalf("Join on empty list = %v", got)
	}
}

func TestRotation(t *testing.T) {
	list := mustList(t, "a.example", "b.example", "c.example")
	var got []string
	for i := 0; i < 4; i++ {
		got = append(got, list.Next())
	}
	want := []string{"a.example", "b.example", "c.example", "a.example"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("rotation = %v, want %v", got, want)
	}
	addr, all := list.Assign()
	if addr != "b.example" {
		t.Fatalf("Assign = %q, want b.example", addr)
	}
	if !reflect.DeepEqual(all, []string{"a.example", "b.example", "c.example"}) {
		t.Fatalf("Assign list = %v", all)
	}
}

func TestSingleAddressDoesNotRotate(t *testing.T) {
	list := mustList(t, "only.example")
	for i := 0; i < 3; i++ {
		if got := list.Next(); got != "only.example" {
			t.Fatalf("Next = %q", got)
		}
	}
	if err := list.SetCSV("a.example,b.example"); err != nil {
		t.Fatal(err)
	}
	if got := list.Next(); got != "a.example" {
		t.Fatalf("single-address list advanced the rotation counter: %q", got)
	}
}

func TestAssignFromKeepsListedAddress(t *testing.T) {
	list := mustList(t, "a.example", "b.example", "c.example")
	addr, all := list.AssignFrom("b.example")
	if addr != "b.example" || len(all) != 3 {
		t.Fatalf("AssignFrom kept=%q list=%v", addr, all)
	}
	if next := list.Next(); next != "a.example" {
		t.Fatalf("keeping a listed address consumed rotation: next=%q", next)
	}
	addr, _ = list.AssignFrom("gone.example")
	if addr != "b.example" {
		t.Fatalf("AssignFrom missing previous = %q, want b.example", addr)
	}
	if next := list.Next(); next != "c.example" {
		t.Fatalf("rotation after reassignment = %q, want c.example", next)
	}
	addr, _ = list.AssignFrom("")
	if addr != "a.example" {
		t.Fatalf("AssignFrom empty previous = %q", addr)
	}
	if addr, _ := mustList(t).AssignFrom("a.example"); addr != "" {
		t.Fatalf("AssignFrom on empty list = %q", addr)
	}
}

func TestJoinPort(t *testing.T) {
	list := mustList(t, "127.0.0.1", "::1", "relay.example")
	want := []string{"127.0.0.1:41000", "[::1]:41000", "relay.example:41000"}
	if got := list.Join(41000); !reflect.DeepEqual(got, want) {
		t.Fatalf("Join = %v, want %v", got, want)
	}
}

func TestSetResetAndClear(t *testing.T) {
	list := mustList(t, "a.example", "b.example")
	if err := list.SetCSV(" c.example, d.example ,c.example"); err != nil {
		t.Fatal(err)
	}
	if got, want := list.Addrs(), []string{"c.example", "d.example"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("set = %v, want %v", got, want)
	}
	if err := list.SetCSV("bad:123"); err == nil {
		t.Fatal("SetCSV accepted a port")
	}
	if got, want := list.Addrs(), []string{"c.example", "d.example"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("invalid set changed the list: %v", got)
	}
	if got, want := list.Reset(), []string{"a.example", "b.example"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("reset = %v, want %v", got, want)
	}
	if err := list.SetCSV(""); err != nil {
		t.Fatal(err)
	}
	if list.Len() != 0 {
		t.Fatalf("empty set kept %v", list.Addrs())
	}
	if got, want := list.Reset(), []string{"a.example", "b.example"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("reset after clear = %v, want %v", got, want)
	}
}

func TestConcurrentAccess(t *testing.T) {
	list := mustList(t, "a.example", "b.example", "c.example")
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for n := 0; n < 100; n++ {
				list.Next()
				list.Addrs()
				list.Join(41000)
				list.Len()
			}
		}()
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		for n := 0; n < 100; n++ {
			if err := list.SetCSV("a.example,b.example"); err != nil {
				t.Errorf("SetCSV: %v", err)
				return
			}
			list.Reset()
		}
	}()
	wg.Wait()
}
