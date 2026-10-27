package account

import (
	"errors"
	"testing"
	"time"
)

func countLoginEmailCodes(t *testing.T, store *Store, userID string) int {
	t.Helper()
	var count int
	if err := store.db.QueryRow(
		`SELECT COUNT(*) FROM login_email_codes WHERE user_id = ?`, userID,
	).Scan(&count); err != nil {
		t.Fatalf("count login email codes: %v", err)
	}
	return count
}

func TestLoginEmailCodeRequirements(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, _, _, err := store.CreateLoginEmailCode("nobody", "wrong horse", time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("unknown user = %v", err)
	}
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, _, _, err := store.CreateLoginEmailCode("alice", "wrong horse", time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password = %v", err)
	}
	if _, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", time.Minute); !errors.Is(err, ErrTwoFactorNotEnabled) {
		t.Fatalf("without two-factor = %v", err)
	}
	user := enableLoginTwoFactor(t, store, "alice", "")
	if _, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", time.Minute); !errors.Is(err, ErrNoRecoveryEmail) {
		t.Fatalf("without email = %v", err)
	}
	if _, err := store.SetEmail("alice", "alice@example.com"); err != nil {
		t.Fatalf("set email: %v", err)
	}
	code, _, expires, err := store.CreateLoginEmailCode("alice", "correct horse", 5*time.Minute)
	if err != nil || len(code) != phoneCodeDigits {
		t.Fatalf("create login code = %q, %v", code, err)
	}
	if remaining := expires.Sub(store.now().UTC()); remaining <= 4*time.Minute || remaining > 5*time.Minute {
		t.Fatalf("expiry is %v away", remaining)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 1 {
		t.Fatalf("stored login codes = %d", got)
	}

	if _, err := store.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	bob := enableLoginTwoFactor(t, store, "bob", "")
	if _, err := store.SetEmail("bob", "bob@example.com"); err != nil {
		t.Fatalf("set bob email: %v", err)
	}
	if _, err := store.SetBanned("bob", true); err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	if _, _, _, err := store.CreateLoginEmailCode("bob", "battery staple", time.Minute); !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("banned account = %v", err)
	}
	if got := countLoginEmailCodes(t, store, bob.ID); got != 0 {
		t.Fatalf("banned account stores %d codes", got)
	}

	// Changing the recovery address retires the code sent to the old mailbox.
	if _, err := store.SetEmail("alice", "alice2@example.com"); err != nil {
		t.Fatalf("change email: %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 0 {
		t.Fatalf("code survived email change: %d", got)
	}
}

func TestLoginEmailCodeLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	user, err := store.SetEmail("alice", "alice@example.com")
	if err != nil {
		t.Fatalf("set email: %v", err)
	}
	secret, err := store.StartTwoFactor(user.ID)
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	enableCode, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, enableCode); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}

	code, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("create login code: %v", err)
	}
	if _, _, _, err := store.Login("alice", "correct horse"); !errors.Is(err, ErrTwoFactorRequired) {
		t.Fatalf("login without any code = %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", "", "00000000",
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("login with wrong email code = %v", err)
	}
	spaced := code[:4] + " " + code[4:]
	logged, token, _, refresh, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "device-a", "Viewer", "", "", "", spaced,
	)
	if err != nil || logged.ID != user.ID || token == "" || refresh == "" {
		t.Fatalf("login with email code = %+v %q %q %v", logged, token, refresh, err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", "", code,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replayed email code = %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 0 {
		t.Fatalf("spent login code still stored: %d", got)
	}

	// TOTP wins over the email code and leaves it usable.
	emailCode, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("email code before TOTP: %v", err)
	}
	now = now.Add(30 * time.Second)
	totp, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", totp, "", "", emailCode,
	); err != nil {
		t.Fatalf("login with TOTP and email = %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 1 {
		t.Fatalf("TOTP login spent the email code: %d", got)
	}

	// SMS wins over the email code when both are supplied.
	if _, err := store.SetPhone("alice", "15550100001"); err != nil {
		t.Fatalf("set phone: %v", err)
	}
	smsCode, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("SMS code: %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", smsCode, emailCode,
	); err != nil {
		t.Fatalf("login with SMS and email = %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 1 {
		t.Fatalf("SMS login spent the email code: %d", got)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", "", emailCode,
	); err != nil {
		t.Fatalf("email code after SMS login = %v", err)
	}

	// A new request replaces the previous code, and five wrong attempts retire
	// the active one.
	first, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("first code: %v", err)
	}
	second, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("second code: %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", "", first,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replaced code = %v", err)
	}
	for i := 0; i < loginCodeTries; i++ {
		if _, _, _, _, _, err := store.LoginWithRefreshFactors(
			"alice", "correct horse", "", "", "", "", "", "00000000",
		); !errors.Is(err, ErrInvalidTwoFactorCode) {
			t.Fatalf("wrong attempt %d = %v", i+1, err)
		}
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", "", second,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code after five wrong attempts = %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 0 {
		t.Fatalf("retired code still stored: %d", got)
	}

	// Expired codes fail and are dropped.
	expiring, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", time.Minute)
	if err != nil {
		t.Fatalf("expiring code: %v", err)
	}
	now = now.Add(2 * time.Minute)
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "correct horse", "", "", "", "", "", expiring,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("expired code = %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 0 {
		t.Fatalf("expired code still stored: %d", got)
	}

	// A password change and a two-factor reset both retire pending codes.
	preChange, _, _, err := store.CreateLoginEmailCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("pre-change code: %v", err)
	}
	if err := store.ChangePassword(user.ID, "", "correct horse", "battery staple"); err != nil {
		t.Fatalf("change password: %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 0 {
		t.Fatalf("code survived password change: %d", got)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "battery staple", "", "", "", "", "", preChange,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code after password change = %v", err)
	}

	reissued, _, _, err := store.CreateLoginEmailCode("alice", "battery staple", 10*time.Minute)
	if err != nil {
		t.Fatalf("pre-reset code: %v", err)
	}
	if err := store.ResetTwoFactor("alice"); err != nil {
		t.Fatalf("reset two-factor: %v", err)
	}
	if got := countLoginEmailCodes(t, store, user.ID); got != 0 {
		t.Fatalf("code survived two-factor reset: %d", got)
	}
	secret, err = store.StartTwoFactor(user.ID)
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	totp, err = TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, totp); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshFactors(
		"alice", "battery staple", "", "", "", "", "", reissued,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code after re-enrollment = %v", err)
	}
}
