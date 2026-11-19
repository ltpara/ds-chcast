#include <windows.h>

#include <cstdio>

#include "service_stop.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

struct FakeService {
    DWORD state = SERVICE_STOPPED;
    DWORD query_error = ERROR_SUCCESS;
    DWORD stop_error = ERROR_SUCCESS;
    ULONGLONG now_ms = 0;
    ULONGLONG stopped_at_ms = 0;
    int query_calls = 0;
    int stop_calls = 0;
};

DWORD run(FakeService& fake, DWORD timeout_ms) {
    return chast::host::detail::stop_service_until_stopped(
        timeout_ms,
        [&fake](DWORD* state) -> DWORD {
            ++fake.query_calls;
            if (fake.query_error != ERROR_SUCCESS) return fake.query_error;
            *state = fake.state;
            return ERROR_SUCCESS;
        },
        [&fake]() -> DWORD {
            ++fake.stop_calls;
            if (fake.stop_error != ERROR_SUCCESS) return fake.stop_error;
            fake.state = SERVICE_STOP_PENDING;
            return ERROR_SUCCESS;
        },
        [&fake] { return fake.now_ms; },
        [&fake](DWORD delay) {
            fake.now_ms += delay;
            if (fake.stopped_at_ms != 0 && fake.now_ms >= fake.stopped_at_ms) {
                fake.state = SERVICE_STOPPED;
            }
        });
}

void test_already_stopped() {
    FakeService fake;
    check(run(fake, 1000) == ERROR_SUCCESS && fake.stop_calls == 0,
          "an already-stopped service needs no stop request");
}

void test_stop_pending_finishes() {
    FakeService fake;
    fake.state = SERVICE_STOP_PENDING;
    fake.stopped_at_ms = 200;
    check(run(fake, 1000) == ERROR_SUCCESS && fake.stop_calls == 0,
          "a stop-pending service is polled without duplicate stop requests");
}

void test_running_service_stops() {
    FakeService fake;
    fake.state = SERVICE_RUNNING;
    fake.stopped_at_ms = 200;
    check(run(fake, 1000) == ERROR_SUCCESS && fake.stop_calls == 1,
          "a running service is stopped before deletion can proceed");
}

void test_query_failure_aborts() {
    FakeService fake;
    fake.state = SERVICE_RUNNING;
    fake.query_error = ERROR_ACCESS_DENIED;
    check(run(fake, 1000) == ERROR_ACCESS_DENIED && fake.stop_calls == 0,
          "a service status query error is returned without requesting stop");
}

void test_stop_failure_aborts() {
    FakeService fake;
    fake.state = SERVICE_RUNNING;
    fake.stop_error = ERROR_ACCESS_DENIED;
    check(run(fake, 1000) == ERROR_ACCESS_DENIED && fake.now_ms == 0,
          "a non-retryable stop error aborts immediately");
}

void test_transient_stop_error_retries_until_timeout() {
    FakeService fake;
    fake.state = SERVICE_START_PENDING;
    fake.stop_error = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
    fake.stopped_at_ms = 200;
    check(run(fake, 1000) == ERROR_SUCCESS && fake.stop_calls == 1,
          "a transient control error is retried through status polling");

    FakeService timeout;
    timeout.state = SERVICE_RUNNING;
    timeout.stop_error = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
    check(run(timeout, 450) == ERROR_TIMEOUT && timeout.now_ms == 450,
          "a service that does not stop is bounded by the timeout");
}

}  // namespace

int main() {
    test_already_stopped();
    test_stop_pending_finishes();
    test_running_service_stops();
    test_query_failure_aborts();
    test_stop_failure_aborts();
    test_transient_stop_error_retries_until_timeout();
    return failures == 0 ? 0 : 1;
}
