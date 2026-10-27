package account

import (
	"errors"
	"testing"
	"time"
)

func enableLoginTwoFactor(t *testing.T, store *Store, username, phone string) User {
	t.Helper()
	user, err := store.FindUser(username)
	if err != nil {
		t.Fatalf("find %s: %v", username, err)
	}
	if phone != "" {
		user, err = store.SetPhone(username, phone)
		if err != nil {
			t.Fatalf("set phone: %v", err)
		}
	}
	secret, err := store.StartTwoFactor(user.ID)
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	code, err := TOTPCode(secret, store.now().UTC())
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, code); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	return user
}

func countLoginSMSCodes(t *testing.T, store *Store, userID string) int {
	t.Helper()
	var count int
	if err := store.db.QueryRow(
		`SELECT COUNT(*) FROM login_sms_codes WHERE user_id = ?`, userID,
	).Scan(&count); err != nil {
		t.Fatalf("count login SMS codes: %v", err)
	}
	return count
}

func TestLoginSMSCodeRequirements(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, _, _, err := store.CreateLoginSMSCode("nobody", "wrong horse", time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("unknown user = %v", err)
	}
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, _, _, err := store.CreateLoginSMSCode("alice", "wrong horse", time.Minute); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("wrong password = %v", err)
	}
	if _, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", time.Minute); !errors.Is(err, ErrTwoFactorNotEnabled) {
		t.Fatalf("without two-factor = %v", err)
	}
	user := enableLoginTwoFactor(t, store, "alice", "")
	if _, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", time.Minute); !errors.Is(err, ErrNoRecoveryPhone) {
		t.Fatalf("without phone = %v", err)
	}
	if _, err := store.SetPhone("alice", "15550100001"); err != nil {
		t.Fatalf("set phone: %v", err)
	}
	code, _, expires, err := store.CreateLoginSMSCode("alice", "correct horse", 5*time.Minute)
	if err != nil || len(code) != phoneCodeDigits {
		t.Fatalf("create login code = %q, %v", code, err)
	}
	if remaining := expires.Sub(store.now().UTC()); remaining <= 4*time.Minute || remaining > 5*time.Minute {
		t.Fatalf("expiry is %v away", remaining)
	}
	if got := countLoginSMSCodes(t, store, user.ID); got != 1 {
		t.Fatalf("stored login codes = %d", got)
	}

	if _, err := store.CreateUser("bob", "battery staple"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	bob := enableLoginTwoFactor(t, store, "bob", "15550100002")
	if _, err := store.SetBanned("bob", true); err != nil {
		t.Fatalf("ban bob: %v", err)
	}
	if _, _, _, err := store.CreateLoginSMSCode("bob", "battery staple", time.Minute); !errors.Is(err, ErrAccountBanned) {
		t.Fatalf("banned account = %v", err)
	}
	if got := countLoginSMSCodes(t, store, bob.ID); got != 0 {
		t.Fatalf("banned account stores %d codes", got)
	}
}

func TestLoginSMSCodeLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUser("alice", "correct horse"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	user := enableLoginTwoFactor(t, store, "alice", "15550100001")

	code, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("create login code: %v", err)
	}
	if _, _, _, err := store.Login("alice", "correct horse"); !errors.Is(err, ErrTwoFactorRequired) {
		t.Fatalf("login without any code = %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshSMS("alice", "correct horse", "", "", "", "", "00000000"); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("login with wrong SMS code = %v", err)
	}
	spaced := code[:4] + " " + code[4:]
	logged, token, _, refresh, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "device-a", "Viewer", "", "", spaced,
	)
	if err != nil || logged.ID != user.ID || token == "" || refresh == "" {
		t.Fatalf("login with SMS code = %+v %q %q %v", logged, token, refresh, err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "", "", "", "", code,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replayed SMS code = %v", err)
	}
	if got := countLoginSMSCodes(t, store, user.ID); got != 0 {
		t.Fatalf("spent login code still stored: %d", got)
	}

	// A new request replaces the previous code, and five wrong attempts retire
	// the active one.
	first, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("first code: %v", err)
	}
	second, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("second code: %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "", "", "", "", first,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("replaced code = %v", err)
	}
	for i := 0; i < loginCodeTries; i++ {
		if _, _, _, _, _, err := store.LoginWithRefreshSMS(
			"alice", "correct horse", "", "", "", "", "00000000",
		); !errors.Is(err, ErrInvalidTwoFactorCode) {
			t.Fatalf("wrong attempt %d = %v", i+1, err)
		}
	}
	if _, _, _, _, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "", "", "", "", second,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code after five wrong attempts = %v", err)
	}
	if got := countLoginSMSCodes(t, store, user.ID); got != 0 {
		t.Fatalf("retired code still stored: %d", got)
	}

	// Expired codes fail and are dropped.
	expiring, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", time.Minute)
	if err != nil {
		t.Fatalf("expiring code: %v", err)
	}
	now = now.Add(2 * time.Minute)
	if _, _, _, _, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "", "", "", "", expiring,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("expired code = %v", err)
	}
	if got := countLoginSMSCodes(t, store, user.ID); got != 0 {
		t.Fatalf("expired code still stored: %d", got)
	}

	// A new number retires the code sent to the old one.
	old, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("old-number code: %v", err)
	}
	if _, err := store.SetPhone("alice", "15550100009"); err != nil {
		t.Fatalf("change phone: %v", err)
	}
	if got := countLoginSMSCodes(t, store, user.ID); got != 0 {
		t.Fatalf("code survived phone change: %d", got)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "", "", "", "", old,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code after phone change = %v", err)
	}

	// Resetting the authenticator and re-enrolling retires old codes.
	reissued, _, _, err := store.CreateLoginSMSCode("alice", "correct horse", 10*time.Minute)
	if err != nil {
		t.Fatalf("pre-reset code: %v", err)
	}
	if err := store.ResetTwoFactor("alice"); err != nil {
		t.Fatalf("reset two-factor: %v", err)
	}
	if got := countLoginSMSCodes(t, store, user.ID); got != 0 {
		t.Fatalf("code survived two-factor reset: %d", got)
	}
	secret, err := store.StartTwoFactor(user.ID)
	if err != nil {
		t.Fatalf("start two-factor: %v", err)
	}
	totp, err := TOTPCode(secret, now)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.EnableTwoFactor(user.ID, totp); err != nil {
		t.Fatalf("enable two-factor: %v", err)
	}
	if _, _, _, _, _, err := store.LoginWithRefreshSMS(
		"alice", "correct horse", "", "", "", "", reissued,
	); !errors.Is(err, ErrInvalidTwoFactorCode) {
		t.Fatalf("code after re-enrollment = %v", err)
	}
}
