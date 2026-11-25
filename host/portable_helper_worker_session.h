#pragma once

#define NOMINMAX
#include <windows.h>

#include "portable_helper_authorization.h"
#include "portable_helper_lifecycle.h"

#include <functional>
#include <memory>
#include <string>

namespace chast::host {

// The service supplies the actual token/session process-creation policy. This
// layer owns the security-critical ordering around it: create the per-session
// pipe first, launch one process, bind its live identity, then exchange the
// capability and accept WorkerReady before returning a usable channel.
using PortableHelperSessionWorkerLauncher = std::function<bool(
    const PortableHelperServiceSpec& spec, const PortableHelperGrant& grant,
    HANDLE* process, std::string* error)>;

// Starts and authenticates one session worker over its connection-scoped pipe.
// On success, ownership of *channel and *worker_process transfers to the
// caller. If startup fails and the child cannot be terminated, its process
// handle is still returned so the caller can retry cleanup; the channel is
// always closed on failure. This function does not itself install a service or
// choose which Windows token the launcher uses.
bool start_portable_helper_session_worker_channel(
    const PortableHelperServiceSpec& spec,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant,
    const PortableHelperSessionWorkerLauncher& launch_worker,
    uint32_t timeout_ms,
    std::unique_ptr<PortableHelperPipe>* channel,
    HANDLE* worker_process,
    PortableHelperProcessIdentity* worker_identity,
    std::string* error);

}  // namespace chast::host
