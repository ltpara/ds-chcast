package alarmnotify

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type memoryStore struct {
	delivery *account.AlarmEmailDelivery
	readyAt  time.Time
	claimed  bool
	sentAt   time.Time
	lastErr  error
}

func (s *memoryStore) ClaimAlarmEmailDelivery(now time.Time, _ time.Duration) (*account.AlarmEmailDelivery, error) {
	if s.delivery == nil || s.claimed || now.Before(s.readyAt) {
		return nil, nil
	}
	s.claimed = true
	s.delivery.Attempts++
	return s.delivery, nil
}

func (s *memoryStore) MarkAlarmEmailDeliverySent(_ int64, sentAt time.Time) error {
	s.sentAt = sentAt
	s.claimed = false
	s.delivery = nil
	return nil
}

func (s *memoryStore) RetryAlarmEmailDelivery(_ int64, availableAt time.Time, sendErr error) error {
	s.readyAt = availableAt
	s.lastErr = sendErr
	s.claimed = false
	return nil
}

type sequenceSender struct {
	results []error
	calls   int
	to      string
	code    string
}

func (s *sequenceSender) SendSecurityAlarm(to string, alarm account.AlarmEvent) error {
	s.calls++
	s.to = to
	s.code = alarm.Code
	result := s.results[0]
	s.results = s.results[1:]
	return result
}

func TestWorkerPersistsFailureAndRetries(t *testing.T) {
	now := time.Date(2026, 11, 24, 12, 0, 0, 0, time.UTC)
	store := &memoryStore{
		delivery: &account.AlarmEmailDelivery{
			ID: 4, Recipient: "alice@example.com", Attempts: 0,
			Alarm: account.AlarmEvent{ID: 9, Code: "host_access_denied"},
		},
		readyAt: now,
	}
	sender := &sequenceSender{results: []error{errors.New("SMTP unavailable"), nil}}
	worker := New(store, sender, nil)
	ctx := context.Background()

	worked, err := worker.ProcessOne(ctx, now)
	if err != nil || !worked {
		t.Fatalf("first send = worked %v, err %v", worked, err)
	}
	if store.lastErr == nil || !store.readyAt.Equal(now.Add(15*time.Second)) || store.claimed {
		t.Fatalf("failed send was not scheduled for retry: %+v", store)
	}
	if worked, err := worker.ProcessOne(ctx, now.Add(14*time.Second)); err != nil || worked {
		t.Fatalf("early retry = worked %v, err %v", worked, err)
	}
	worked, err = worker.ProcessOne(ctx, store.readyAt)
	if err != nil || !worked || store.sentAt.IsZero() || store.delivery != nil {
		t.Fatalf("retry send = worked %v, err %v; store %+v", worked, err, store)
	}
	if sender.calls != 2 || sender.to != "alice@example.com" || sender.code != "host_access_denied" {
		t.Fatalf("sender calls=%d recipient=%q code=%q", sender.calls, sender.to, sender.code)
	}
}

func TestRetryDelayGrowsAndCaps(t *testing.T) {
	if got := retryDelay(1); got != 15*time.Second {
		t.Fatalf("first retry delay = %s", got)
	}
	if got := retryDelay(4); got != 2*time.Minute {
		t.Fatalf("fourth retry delay = %s", got)
	}
	if got := retryDelay(100); got != time.Hour {
		t.Fatalf("capped retry delay = %s", got)
	}
}
