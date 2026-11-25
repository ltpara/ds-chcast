#pragma once

#include "portable_helper_authorization.h"

#include <functional>
#include <string>

namespace chast::host {

// Process/transport operations supplied by the component that owns the
// authenticated worker process and pipe. The supervisor deliberately keeps
// Windows SCM and desktop side effects injectable so the shutdown ordering can
// be tested without installing a service or injecting real input.
struct PortableHelperWorkerRevocationCallbacks {
  std::function<bool(std::string*)> request_cancellation;
  std::function<bool(std::string*)> terminate_worker;
  std::function<bool(uint32_t timeout_ms, std::string*)> wait_for_worker_exit;
  std::function<bool(std::string*)> release_input;
  // Must be idempotent because a failed revocation can be retried.
  std::function<bool(std::string*)> close_channel;
};

// Revokes a live authorization in fail-closed order:
//   1. close the capability gate and request cooperative cancellation;
//   2. bound both operation drain and graceful worker exit;
//   3. if either wait times out, terminate and observe worker exit, then
//      re-check the drain;
//   4. release held input only after all operation leases are gone;
//   5. close the authenticated channel on every path after revocation begins.
//
// If the worker cannot be terminated/exited or leases remain after it exits,
// input release is intentionally skipped and authorization remains revoked.
// The caller can retry cleanup after resolving the stuck operation.
bool revoke_portable_helper_worker(
    PortableHelperAuthorization& authorization,
    const PortableHelperWorkerRevocationCallbacks& callbacks,
    uint32_t drain_timeout_ms, uint32_t worker_exit_timeout_ms,
    std::string* error);

}  // namespace chast::host
