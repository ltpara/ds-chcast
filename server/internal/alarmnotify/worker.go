package alarmnotify

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/ltpara/ds-chcast/server/internal/account"
)

type Store interface {
	ClaimAlarmEmailDelivery(time.Time, time.Duration) (*account.AlarmEmailDelivery, error)
	MarkAlarmEmailDeliverySent(int64, time.Time) error
	RetryAlarmEmailDelivery(int64, time.Time, error) error
}

type Sender interface {
	SendSecurityAlarm(string, account.AlarmEvent) error
}

type Worker struct {
	store  Store
	sender Sender
	logger *slog.Logger
}

func New(store Store, sender Sender, logger *slog.Logger) *Worker {
	if logger == nil {
		logger = slog.Default()
	}
	return &Worker{store: store, sender: sender, logger: logger}
}

func (w *Worker) ProcessOne(ctx context.Context, now time.Time) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	delivery, err := w.store.ClaimAlarmEmailDelivery(now, 30*time.Second)
	if err != nil || delivery == nil {
		return false, err
	}
	if err := w.sender.SendSecurityAlarm(delivery.Recipient, delivery.Alarm); err != nil {
		retryAt := now.Add(retryDelay(delivery.Attempts))
		if markErr := w.store.RetryAlarmEmailDelivery(delivery.ID, retryAt, err); markErr != nil {
			return true, fmt.Errorf("record alarm email retry: %w (send error: %v)", markErr, err)
		}
		w.logger.Error("security alarm email delivery failed",
			"delivery_id", delivery.ID, "alarm_id", delivery.Alarm.ID,
			"attempt", delivery.Attempts, "retry_at", retryAt, "error", err)
		return true, nil
	}
	if err := w.store.MarkAlarmEmailDeliverySent(delivery.ID, now); err != nil {
		return true, fmt.Errorf("record sent alarm email: %w", err)
	}
	w.logger.Info("security alarm email delivered",
		"delivery_id", delivery.ID, "alarm_id", delivery.Alarm.ID,
		"attempt", delivery.Attempts)
	return true, nil
}

func (w *Worker) Run(ctx context.Context) {
	for {
		worked, err := w.ProcessOne(ctx, time.Now().UTC())
		if ctx.Err() != nil {
			return
		}
		if err != nil {
			w.logger.Error("security alarm email worker", "error", err)
		}
		if worked && err == nil {
			continue
		}
		timer := time.NewTimer(time.Second)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}

func retryDelay(attempt int) time.Duration {
	if attempt < 1 {
		attempt = 1
	}
	shift := attempt - 1
	if shift > 9 {
		shift = 9
	}
	delay := 15 * time.Second * time.Duration(1<<shift)
	if delay > time.Hour {
		return time.Hour
	}
	return delay
}
