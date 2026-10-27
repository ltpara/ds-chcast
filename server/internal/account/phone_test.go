package account

import (
	"errors"
	"strings"
	"testing"
	"time"
)

func TestPhoneNormalization(t *testing.T) {
	cases := map[string]string{
		"":                     "",
		"  +86 138-0013-8000 ": "8613800138000",
		"(555) 010.1234":       "5550101234",
		"+15550101234":         "15550101234",
	}
	for input, want := range cases {
		got, err := normalizePhone(input)
		if err != nil || got != want {
			t.Fatalf("normalizePhone(%q) = %q, %v; want %q", input, got, err, want)
		}
	}
	for _, bad := range []string{"12345", "1234567890123456", "555-CALL-NOW", "++1555", "+"} {
		if _, err := normalizePhone(bad); !errors.Is(err, ErrInvalidPhone) {
			t.Fatalf("normalizePhone(%q) error = %v", bad, err)
		}
	}
}

func TestPhoneStoredAndUnique(t *testing.T) {
	store := openTemp(t, "state.db")
	user, err := store.CreateUserWithRecovery("alice", "secret123", "", "+1 (555) 010-0001")
	if err != nil {
		t.Fatalf("create user with phone: %v", err)
	}
	if user.Phone != "15550100001" {
		t.Fatalf("created phone = %q", user.Phone)
	}
	found, err := store.FindUser("alice")
	if err != nil || found.Phone != "15550100001" {
		t.Fatalf("find user phone = %q, %v", found.Phone, err)
	}
	if _, err := store.CreateUserWithRecovery("bob", "secret123", "", "15550100001"); !errors.Is(err, ErrPhoneTaken) {
		t.Fatalf("duplicate phone error = %v", err)
	}
	if _, err := store.FindUser("bob"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("duplicate phone left a user behind: %v", err)
	}
	if _, err := store.SetPhone("nobody", "15550100002"); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("set phone for unknown user: %v", err)
	}
	bob, err := store.CreateUser("bob", "secret123")
	if err != nil {
		t.Fatalf("create bob: %v", err)
	}
	bob, err = store.SetPhone("bob", "15550100002")
	if err != nil || bob.Phone != "15550100002" {
		t.Fatalf("set phone: %+v, %v", bob, err)
	}
	if _, err := store.SetPhone("bob", "15550100001"); !errors.Is(err, ErrPhoneTaken) {
		t.Fatalf("set taken phone: %v", err)
	}
	users, err := store.ListUsers()
	if err != nil || len(users) != 2 || users[1].Phone != "15550100002" {
		t.Fatalf("list users = %+v, %v", users, err)
	}
	if bob, err = store.SetPhone("bob", ""); err != nil || bob.Phone != "" {
		t.Fatalf("clear phone: %+v, %v", bob, err)
	}
}

func TestPhoneResetLifecycle(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUserWithRecovery("alice", "secret123", "", "15550100001"); err != nil {
		t.Fatalf("create alice: %v", err)
	}
	if _, err := store.CreateUser("bob", "secret123"); err != nil {
		t.Fatalf("create bob: %v", err)
	}
	if _, _, _, err := store.CreatePhoneReset("nobody", time.Minute); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("reset unknown user: %v", err)
	}
	if _, _, _, err := store.CreatePhoneReset("bob", time.Minute); !errors.Is(err, ErrNoRecoveryPhone) {
		t.Fatalf("reset without phone: %v", err)
	}
	_, token, _, err := store.Login("alice", "secret123")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	first, user, expires, err := store.CreatePhoneReset("alice", 10*time.Minute)
	if err != nil {
		t.Fatalf("create reset: %v", err)
	}
	if len(first) != phoneCodeDigits || user.Phone != "15550100001" || !expires.After(time.Now()) {
		t.Fatalf("reset = %q, %+v, %v", first, user, expires)
	}
	var stored string
	if err := store.db.QueryRow(`SELECT code_hash FROM phone_resets`).Scan(&stored); err != nil {
		t.Fatalf("read reset row: %v", err)
	}
	if stored != phoneCodeHash(first) || stored == first {
		t.Fatalf("stored reset hash = %q", stored)
	}
	second, _, _, err := store.CreatePhoneReset("alice", 10*time.Minute)
	if err != nil {
		t.Fatalf("replace reset: %v", err)
	}
	if _, err := store.ConsumePhoneReset("alice", first, "newsecret123"); !errors.Is(err, ErrInvalidResetCode) {
		t.Fatalf("superseded code: %v", err)
	}
	if _, err := store.ConsumePhoneReset("alice", second, "short"); !errors.Is(err, ErrWeakPassword) {
		t.Fatalf("weak password: %v", err)
	}
	if _, err := store.ConsumePhoneReset("alice", "123", "newsecret123"); !errors.Is(err, ErrInvalidResetCode) {
		t.Fatalf("malformed code: %v", err)
	}
	consumed, err := store.ConsumePhoneReset("alice", second, "newsecret123")
	if err != nil || consumed.Username != "alice" {
		t.Fatalf("consume reset: %+v, %v", consumed, err)
	}
	if _, err := store.ConsumePhoneReset("alice", second, "another123"); !errors.Is(err, ErrInvalidResetCode) {
		t.Fatalf("reused code: %v", err)
	}
	if _, _, _, err := store.Login("alice", "secret123"); !errors.Is(err, ErrInvalidCredentials) {
		t.Fatalf("old password still works: %v", err)
	}
	if _, _, _, err := store.Login("alice", "newsecret123"); err != nil {
		t.Fatalf("new password rejected: %v", err)
	}
	if _, err := store.Authenticate(token); !errors.Is(err, ErrInvalidToken) {
		t.Fatalf("session survived reset: %v", err)
	}
}

func TestPhoneResetWrongCodeRetiresAfterFiveTries(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUserWithRecovery("alice", "secret123", "", "15550100001"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	code, _, _, err := store.CreatePhoneReset("alice", 10*time.Minute)
	if err != nil {
		t.Fatalf("create reset: %v", err)
	}
	wrong := "00000000"
	if code == wrong {
		wrong = "11111111"
	}
	for i := 0; i < phoneResetTries; i++ {
		if _, err := store.ConsumePhoneReset("alice", wrong, "newsecret123"); !errors.Is(err, ErrInvalidResetCode) {
			t.Fatalf("wrong code attempt %d: %v", i+1, err)
		}
	}
	var rows int
	if err := store.db.QueryRow(`SELECT COUNT(*) FROM phone_resets`).Scan(&rows); err != nil {
		t.Fatalf("count resets: %v", err)
	}
	if rows != 0 {
		t.Fatalf("code survived %d wrong attempts", phoneResetTries)
	}
	if _, err := store.ConsumePhoneReset("alice", code, "newsecret123"); !errors.Is(err, ErrInvalidResetCode) {
		t.Fatalf("retired code accepted: %v", err)
	}
}

func TestPhoneResetExpiry(t *testing.T) {
	store := openTemp(t, "state.db")
	base := time.Now().UTC()
	store.SetClock(func() time.Time { return base })
	if _, err := store.CreateUserWithRecovery("alice", "secret123", "", "15550100001"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	code, _, _, err := store.CreatePhoneReset("alice", 10*time.Minute)
	if err != nil {
		t.Fatalf("create reset: %v", err)
	}
	store.SetClock(func() time.Time { return base.Add(11 * time.Minute) })
	if _, err := store.ConsumePhoneReset("alice", code, "newsecret123"); !errors.Is(err, ErrInvalidResetCode) {
		t.Fatalf("expired code: %v", err)
	}
	if _, _, _, err := store.Login("alice", "secret123"); err != nil {
		t.Fatalf("expired reset changed the password: %v", err)
	}
}

func TestSetPhoneClearsPendingCode(t *testing.T) {
	store := openTemp(t, "state.db")
	if _, err := store.CreateUserWithRecovery("alice", "secret123", "", "15550100001"); err != nil {
		t.Fatalf("create user: %v", err)
	}
	code, _, _, err := store.CreatePhoneReset("alice", 10*time.Minute)
	if err != nil {
		t.Fatalf("create reset: %v", err)
	}
	if _, err := store.SetPhone("alice", "15550100002"); err != nil {
		t.Fatalf("change phone: %v", err)
	}
	if _, err := store.ConsumePhoneReset("alice", code, "newsecret123"); !errors.Is(err, ErrInvalidResetCode) {
		t.Fatalf("code issued for old number still works: %v", err)
	}
	if strings.TrimSpace(code) == "" {
		t.Fatalf("empty generated code")
	}
}
