package agent

import (
	"context"
	"errors"
	"io"
	"net"
	"net/http"
)

type viewerStartError struct {
	err       error
	retryable bool
}

func (e *viewerStartError) Error() string { return e.err.Error() }
func (e *viewerStartError) Unwrap() error { return e.err }

// IsRetryableStartError lets an embedding client continue an existing bounded
// reconnect series without interpreting human-readable or localized messages.
// Identity, configuration and authentication failures remain terminal.
func IsRetryableStartError(err error) bool {
	var start *viewerStartError
	return errors.As(err, &start) && start.retryable
}

func retryableLookupError(err error) bool {
	if errors.Is(err, context.Canceled) {
		return false
	}
	var status *apiStatusError
	if errors.As(err, &status) {
		// A previously connected host can disappear until it registers again.
		return status.status == http.StatusNotFound || status.status == http.StatusRequestTimeout ||
			status.status == http.StatusTooManyRequests || status.status >= 500
	}
	if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
		return true
	}
	var op *net.OpError
	var dns *net.DNSError
	return errors.As(err, &op) || errors.As(err, &dns)
}
