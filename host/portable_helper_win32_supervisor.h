#pragma once

#define NOMINMAX
#include <windows.h>

#include "portable_helper_supervisor.h"

#include <memory>

namespace chast::host {

// Builds callbacks bound to a caller-owned worker process handle and an
// authenticated local pipe. The caller must keep both objects alive through
// revocation and must have compared the process identity against its trusted
// SCM/process-launch record before constructing these callbacks. The input
// release callback must synthesize any required key/button-up events. It is
// invoked only after the worker process has exited and the supervisor has
// drained in-flight operations, so it cannot race further injection. A
// correlated Stop acknowledgement distinguishes graceful worker cleanup; if
// it is missing, the supervisor still attempts external input cleanup after
// confirmed process exit and reports the unclean stop as a failure.
bool make_win32_portable_helper_revocation_callbacks(
    HANDLE worker_process,
    std::unique_ptr<PortableHelperPipe>& authenticated_channel,
    std::function<bool(std::string*)> release_input,
    PortableHelperWorkerRevocationCallbacks* callbacks,
    std::string* error);

}  // namespace chast::host
