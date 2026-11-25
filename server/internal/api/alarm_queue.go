package api

import "github.com/ltpara/ds-chcast/server/internal/account"

// AlarmQueue accepts a notification job for a newly stored security alarm.
// Implementations should enqueue quickly and return after accepting the job;
// sending mail or other notifications belongs in a worker, outside the HTTP
// request path. The alarm is committed before this method is called.
type AlarmQueue interface {
	EnqueueAlarm(account.AlarmEvent) error
}

// AlarmQueueFunc adapts a function to AlarmQueue.
type AlarmQueueFunc func(account.AlarmEvent) error

func (fn AlarmQueueFunc) EnqueueAlarm(event account.AlarmEvent) error {
	return fn(event)
}
