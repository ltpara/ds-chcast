package account

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha1"
	"crypto/subtle"
	"encoding/base32"
	"encoding/binary"
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"
)

const (
	totpDigits     = 6
	totpPeriod     = 30 * time.Second
	totpSecretSize = 20
	totpSkew       = 1
)

var totpBase32 = base32.StdEncoding.WithPadding(base32.NoPadding)

func newTOTPSecret() (string, error) {
	raw := make([]byte, totpSecretSize)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate two-factor secret: %w", err)
	}
	return totpBase32.EncodeToString(raw), nil
}

// TOTPCode returns the six-digit code for secret at at. It is exported for the
// API tests and enrollment probes; the server itself only stores the secret.
func TOTPCode(secret string, at time.Time) (string, error) {
	return totpCodeAt(secret, totpStepAt(at))
}

// TOTPURI builds the otpauth URI an authenticator app can scan or import by
// hand. Issuer and account are escaped so a username cannot break the label.
func TOTPURI(issuer, accountName, secret string) string {
	label := url.PathEscape(issuer) + ":" + url.PathEscape(accountName)
	query := url.Values{}
	query.Set("secret", secret)
	query.Set("issuer", issuer)
	query.Set("algorithm", "SHA1")
	query.Set("digits", fmt.Sprintf("%d", totpDigits))
	query.Set("period", fmt.Sprintf("%d", int(totpPeriod/time.Second)))
	return "otpauth://totp/" + label + "?" + query.Encode()
}

func totpStepAt(at time.Time) int64 {
	return at.UTC().Unix() / int64(totpPeriod/time.Second)
}

func totpCodeAt(secret string, step int64) (string, error) {
	key, err := totpBase32.DecodeString(strings.ToUpper(strings.TrimSpace(secret)))
	if err != nil || len(key) == 0 {
		return "", errors.New("invalid two-factor secret")
	}
	var counter [8]byte
	binary.BigEndian.PutUint64(counter[:], uint64(step))
	mac := hmac.New(sha1.New, key)
	mac.Write(counter[:])
	sum := mac.Sum(nil)
	offset := sum[len(sum)-1] & 0x0f
	value := binary.BigEndian.Uint32(sum[offset:offset+4]) & 0x7fffffff
	return fmt.Sprintf("%0*d", totpDigits, value%1_000_000), nil
}

// totpMatch returns the step whose code equals code. One step of clock skew is
// accepted in either direction; the caller decides whether that step was
// already spent.
func totpMatch(secret, code string, at time.Time) (int64, bool) {
	code = strings.TrimSpace(code)
	if len(code) != totpDigits {
		return 0, false
	}
	for _, r := range code {
		if r < '0' || r > '9' {
			return 0, false
		}
	}
	step := totpStepAt(at)
	for offset := int64(-totpSkew); offset <= totpSkew; offset++ {
		want, err := totpCodeAt(secret, step+offset)
		if err != nil {
			return 0, false
		}
		if subtle.ConstantTimeCompare([]byte(want), []byte(code)) == 1 {
			return step + offset, true
		}
	}
	return 0, false
}
