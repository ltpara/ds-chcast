package account

import (
	"strings"
	"testing"
	"time"
)

// The RFC 6238 SHA-1 vectors use the ASCII secret "12345678901234567890".
// TOTPCode emits six digits, so each expected value is the truncated vector.
func TestTOTPCodeMatchesRFC6238(t *testing.T) {
	const secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
	cases := []struct {
		at   int64
		want string
	}{
		{59, "287082"},
		{1111111109, "081804"},
		{1111111111, "050471"},
		{1234567890, "005924"},
		{2000000000, "279037"},
		{20000000000, "353130"},
	}
	for _, tc := range cases {
		got, err := TOTPCode(secret, time.Unix(tc.at, 0))
		if err != nil {
			t.Fatalf("code at %d: %v", tc.at, err)
		}
		if got != tc.want {
			t.Fatalf("code at %d = %q, want %q", tc.at, got, tc.want)
		}
	}
	if _, err := TOTPCode("not-base32!", time.Now()); err == nil {
		t.Fatal("invalid secret was accepted")
	}
}

func TestTOTPMatchAllowsOneStepOfSkew(t *testing.T) {
	const secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
	at := time.Unix(1234567890, 0)
	code, err := TOTPCode(secret, at)
	if err != nil {
		t.Fatal(err)
	}
	for _, offset := range []time.Duration{-30 * time.Second, 0, 30 * time.Second} {
		if _, ok := totpMatch(secret, code, at.Add(offset)); !ok {
			t.Fatalf("code did not match with %v skew", offset)
		}
	}
	if _, ok := totpMatch(secret, code, at.Add(2*30*time.Second)); ok {
		t.Fatal("code matched two steps away")
	}
	for _, bad := range []string{"", "12345", "1234567", "12a456"} {
		if _, ok := totpMatch(secret, bad, at); ok {
			t.Fatalf("malformed code %q matched", bad)
		}
	}
}

func TestTOTPURIEscapesAndDescribesEnrollment(t *testing.T) {
	uri := TOTPURI("chast", "alice@example.com", "JBSWY3DPEHPK3PXP")
	for _, want := range []string{
		"otpauth://totp/chast:alice@example.com?",
		"secret=JBSWY3DPEHPK3PXP",
		"issuer=chast",
		"algorithm=SHA1",
		"digits=6",
		"period=30",
	} {
		if !strings.Contains(uri, want) {
			t.Fatalf("URI %q is missing %q", uri, want)
		}
	}
}
