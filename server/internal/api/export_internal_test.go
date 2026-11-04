package api

import "testing"

func TestCSVSafe(t *testing.T) {
	for _, cell := range []string{"=cmd", "+cmd", "-cmd", "@cmd", "\tcmd", "\rcmd"} {
		if got := csvSafe(cell); got != "'"+cell {
			t.Fatalf("csvSafe(%q) = %q", cell, got)
		}
	}
	for _, cell := range []string{"", "-", "plain", "1", "true", "  spaced"} {
		if got := csvSafe(cell); got != cell {
			t.Fatalf("csvSafe(%q) = %q", cell, got)
		}
	}
}
