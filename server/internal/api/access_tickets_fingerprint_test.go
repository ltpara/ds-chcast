package api

import (
	"strings"
	"testing"
)

func TestTicketFingerprintSpelling(t *testing.T) {
	digest := strings.Repeat("0123456789abcdef", 4)
	if got := ticketFingerprint(digest); got != "SHA256:"+digest {
		t.Fatalf("bare digest = %q", got)
	}
	if got := ticketFingerprint("SHA256:" + digest); got != "SHA256:"+digest {
		t.Fatalf("already spelled digest = %q", got)
	}
	if got := ticketFingerprint("AB12"); got != "AB12" {
		t.Fatalf("short fingerprint = %q", got)
	}
	if got := ticketFingerprint(strings.Repeat("A", 64)); got != "SHA256:"+strings.Repeat("a", 64) {
		t.Fatalf("uppercase digest = %q", got)
	}
}
