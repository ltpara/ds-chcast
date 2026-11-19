#pragma once

#include <windows.h>

namespace chast::host::detail {

template <typename QueryStatus, typename RequestStop, typename Now, typename Sleep>
DWORD stop_service_until_stopped(DWORD timeout_ms, QueryStatus query_status,
                                 RequestStop request_stop, Now now, Sleep sleep) {
    const ULONGLONG deadline = now() + timeout_ms;
    for (;;) {
        DWORD state = SERVICE_STOPPED;
        const DWORD query_error = query_status(&state);
        if (query_error != ERROR_SUCCESS) return query_error;
        if (state == SERVICE_STOPPED) return ERROR_SUCCESS;

        ULONGLONG current = now();
        if (current >= deadline) return ERROR_TIMEOUT;
        if (state != SERVICE_STOP_PENDING) {
            const DWORD stop_error = request_stop();
            if (stop_error != ERROR_SUCCESS && stop_error != ERROR_SERVICE_NOT_ACTIVE &&
                stop_error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
                return stop_error;
            }
        }

        current = now();
        if (current >= deadline) return ERROR_TIMEOUT;
        const ULONGLONG remaining = deadline - current;
        sleep(static_cast<DWORD>(remaining < 200 ? remaining : 200));
    }
}

}  // namespace chast::host::detail
