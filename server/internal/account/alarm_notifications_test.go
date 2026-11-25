package account

import (
	"errors"
	"testing"
	"time"
)

func verifiedAccountWithEmail(t *testing.T, store *Store, username, email, role string) User {
	t.Helper()
	user, err := store.CreateUserWithRole(username, "secret123", email, "", role)
	if err != nil {
		t.Fatalf("create %s: %v", username, err)
	}
	token, _, _, err := store.CreateEmailVerification(username, time.Hour)
	if err != nil {
		t.Fatalf("create %s email verification: %v", username, err)
	}
	if err := store.ConsumeEmailVerification(token, username); err != nil {
		t.Fatalf("verify %s email: %v", username, err)
	}
	return user
}

func TestAlarmEmailOutboxPersistsAndRetriesPerGlobalAuditor(t *testing.T) {
	store := openTemp(t, "alarm-email-outbox.db")
	now := time.Date(2026, 11, 24, 12, 0, 0, 0, time.UTC)
	store.SetClock(func() time.Time { return now })
	alice := verifiedAccountWithEmail(t, store, "alice", "alice@example.com", RoleAdmin)
	verifiedAccountWithEmail(t, store, "bob", "bob@example.com", RoleUser)
	verifiedAccountWithEmail(t, store, "carol", "carol@example.com", RoleAdmin)
	if err := store.SetAlarmEmailPreference(alice.ID, true); err != nil {
		t.Fatalf("subscribe global auditor: %v", err)
	}
	if err := store.SetAlarmEmailPreference("missing", true); !errors.Is(err, ErrUnknownUser) {
		t.Fatalf("subscribe missing user: %v", err)
	}
	if err := store.SetAlarmEmailPreference(mustUserID(t, store, "bob"), true); err != nil {
		t.Fatalf("save preference for a verified account: %v", err)
	}
	if err := store.SetAlarmEmailPreference(mustUserID(t, store, "carol"), false); err != nil {
		t.Fatalf("disable administrator notifications: %v", err)
	}
	if err := store.AppendAudit(AuditEvent{
		Username: "alice", Action: AuditEmergencyAccess,
		Detail: "grant accepted", Success: true, CreatedAt: now,
	}); err != nil {
		t.Fatalf("append alarm: %v", err)
	}
	var count int
	if err := store.db.QueryRow(`SELECT COUNT(*) FROM alarm_email_deliveries`).Scan(&count); err != nil || count != 1 {
		t.Fatalf("notification recipient count = %d, %v; want only opted-in global auditor", count, err)
	}

	delivery, err := store.ClaimAlarmEmailDelivery(now, 30*time.Second)
	if err != nil || delivery == nil {
		t.Fatalf("claim alarm delivery: %+v, %v", delivery, err)
	}
	if delivery.Username != "alice" || delivery.Recipient != "alice@example.com" ||
		delivery.Attempts != 1 || delivery.Alarm.Code != "emergency_access_granted" {
		t.Fatalf("claimed alarm email delivery = %+v", delivery)
	}
	if duplicate, err := store.ClaimAlarmEmailDelivery(now, 30*time.Second); err != nil || duplicate != nil {
		t.Fatalf("leased notification was claimed twice: %+v, %v", duplicate, err)
	}

	retryAt := now.Add(15 * time.Second)
	if err := store.RetryAlarmEmailDelivery(delivery.ID, retryAt, errors.New("SMTP relay unavailable")); err != nil {
		t.Fatalf("schedule retry: %v", err)
	}
	if ready, err := store.ClaimAlarmEmailDelivery(now.Add(14*time.Second), 30*time.Second); err != nil || ready != nil {
		t.Fatalf("delivery retried before its scheduled time: %+v, %v", ready, err)
	}
	delivery, err = store.ClaimAlarmEmailDelivery(retryAt, 30*time.Second)
	if err != nil || delivery == nil || delivery.Attempts != 2 || delivery.Recipient != "alice@example.com" {
		t.Fatalf("retry lost attempt or original recipient: %+v, %v", delivery, err)
	}
	if err := store.MarkAlarmEmailDeliverySent(delivery.ID, retryAt); err != nil {
		t.Fatalf("mark delivery sent: %v", err)
	}
	statuses, err := store.AlarmEmailDeliveries(delivery.Alarm.ID)
	if err != nil || len(statuses) != 1 || statuses[0].Status != "sent" || statuses[0].Attempts != 2 ||
		statuses[0].LastError != "" || !statuses[0].SentAt.Equal(retryAt) {
		t.Fatalf("delivery status after retry = %+v, %v", statuses, err)
	}

	if err := store.AppendAudit(AuditEvent{
		Username: "alice", Action: AuditEmergencyAccess,
		Detail: "second grant accepted", Success: true, CreatedAt: now,
	}); err != nil {
		t.Fatalf("append second alarm: %v", err)
	}
	alarms, err := store.AlarmEventsForFilterBefore("", AlarmFilter{Code: "emergency_access_granted"}, 0, 10)
	if err != nil || len(alarms) != 2 {
		t.Fatalf("list second alarm: %+v, %v", alarms, err)
	}
	if _, err := store.SetEmail("alice", "new-alice@example.com"); err != nil {
		t.Fatalf("change address with pending notification: %v", err)
	}
	statuses, err = store.AlarmEmailDeliveries(alarms[0].ID)
	if err != nil || len(statuses) != 1 || statuses[0].Status != "cancelled" {
		t.Fatalf("address change did not cancel pending notification: %+v, %v", statuses, err)
	}
}

func TestAlarmEmailPreferenceRequiresVerifiedAddress(t *testing.T) {
	store := openTemp(t, "alarm-email-preference.db")
	user, err := store.CreateUserWithEmail("alice", "secret123", "alice@example.com")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.SetAlarmEmailPreference(user.ID, true); !errors.Is(err, ErrAlarmEmailRequiresVerifiedAddress) {
		t.Fatalf("unverified address was subscribed: %v", err)
	}
	if enabled, err := store.AlarmEmailPreference(user.ID); err != nil || enabled {
		t.Fatalf("unverified subscription changed preference: %v, %v", enabled, err)
	}
	if err := store.SetAlarmEmailPreference(user.ID, false); err != nil {
		t.Fatalf("disable notifications: %v", err)
	}
}

func mustUserID(t *testing.T, store *Store, username string) string {
	t.Helper()
	user, err := store.FindUser(username)
	if err != nil {
		t.Fatalf("find %s: %v", username, err)
	}
	return user.ID
}
