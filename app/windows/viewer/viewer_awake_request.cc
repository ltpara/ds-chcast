#include "viewer_awake_request.h"

#include <windows.h>
#include <powrprof.h>

#include <cstdio>

#include "chast/reference_counted_lease.h"

namespace chast_viewer {
namespace {

class ProcessAwakeRequest {
public:
    ProcessAwakeRequest()
        : leases_([this] { return acquire_request(); },
                  [this] { return release_request(); }) {}

    bool acquire() { return leases_.acquire(); }
    bool release() { return leases_.release(); }

private:
    bool acquire_request() {
        wchar_t reason[] = L"chast viewer: a remote session is active";
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = reason;
        handle_ = PowerCreateRequest(&context);
        if (handle_ == INVALID_HANDLE_VALUE) {
            report("PowerCreateRequest");
            return false;
        }
        if (!PowerSetRequest(handle_, PowerRequestSystemRequired)) {
            report("PowerRequestSystemRequired");
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        return true;
    }

    bool release_request() {
        if (handle_ == INVALID_HANDLE_VALUE) return true;
        const bool cleared = PowerClearRequest(handle_, PowerRequestSystemRequired) != FALSE;
        if (!cleared) {
            report("PowerClearRequest");
        }
        const bool closed = CloseHandle(handle_) != FALSE;
        if (!closed) report("CloseHandle");
        handle_ = INVALID_HANDLE_VALUE;
        return cleared && closed;
    }

    static void report(const char* operation) {
        std::fprintf(stderr, "chast_viewer: %s failed (error %lu)\n",
                     operation, GetLastError());
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    chast::ReferenceCountedLease leases_;
};

ProcessAwakeRequest& process_request() {
    static ProcessAwakeRequest request;
    return request;
}

}  // namespace

bool AcquireViewerAwakeRequest() {
    return process_request().acquire();
}

bool ReleaseViewerAwakeRequest() {
    return process_request().release();
}

}  // namespace chast_viewer
