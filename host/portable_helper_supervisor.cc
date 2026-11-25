#include "portable_helper_supervisor.h"

namespace chast::host {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

void remember_error(std::string* first, const char* operation,
                    const std::string& detail) {
  if (first == nullptr || !first->empty()) return;
  *first = operation;
  if (!detail.empty()) *first += ": " + detail;
}

}  // namespace

bool revoke_portable_helper_worker(
    PortableHelperAuthorization& authorization,
    const PortableHelperWorkerRevocationCallbacks& callbacks,
    uint32_t drain_timeout_ms, uint32_t worker_exit_timeout_ms,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (!callbacks.request_cancellation || !callbacks.terminate_worker ||
      !callbacks.wait_for_worker_exit || !callbacks.release_input ||
      !callbacks.close_channel)
    return fail(error, "portable helper revocation callbacks are incomplete");

  const auto initial_state = authorization.state();
  if (initial_state == PortableHelperAuthorizationState::Idle ||
      initial_state == PortableHelperAuthorizationState::Revoked) {
    std::string close_error;
    try {
      if (callbacks.close_channel(&close_error)) return true;
    } catch (...) {
      close_error = "callback raised an exception";
    }
    return fail(error, close_error.empty() ? "worker channel close failed"
                                           : close_error);
  }

  std::string first_error;
  std::string callback_error;
  const auto close_channel = [&] {
    std::string close_error;
    try {
      if (callbacks.close_channel(&close_error)) return;
    } catch (...) {
      close_error = "callback raised an exception";
    }
    remember_error(&first_error, "worker channel close failed", close_error);
  };

  bool cancel_ok = initial_state == PortableHelperAuthorizationState::Revoking;
  bool begin_ok = cancel_ok;
  if (!begin_ok) {
    try {
      begin_ok = authorization.BeginRevocation(
          [&] {
            callback_error.clear();
            cancel_ok = callbacks.request_cancellation(&callback_error);
            return cancel_ok;
          },
          &callback_error);
    } catch (...) {
      callback_error = "callback raised an exception";
    }
  }
  if (authorization.state() != PortableHelperAuthorizationState::Revoking) {
    return fail(error, callback_error.empty()
                            ? "portable helper authorization is not live"
                            : callback_error);
  }
  if (!begin_ok || !cancel_ok)
    remember_error(&first_error, "worker cancellation request failed",
                   callback_error);

  const auto force_terminate_and_drain = [&] {
    bool terminated = false;
    callback_error.clear();
    try {
      terminated = callbacks.terminate_worker(&callback_error);
    } catch (...) {
      callback_error = "termination callback raised an exception";
    }
    if (!terminated) {
      remember_error(&first_error, "worker termination failed",
                     callback_error);
      close_channel();
      return false;
    }

    callback_error.clear();
    bool exited = false;
    try {
      exited = callbacks.wait_for_worker_exit(worker_exit_timeout_ms,
                                               &callback_error);
    } catch (...) {
      callback_error = "worker-exit callback raised an exception";
    }
    if (!exited) {
      remember_error(&first_error, "terminated worker did not exit",
                     callback_error);
      close_channel();
      return false;
    }

    bool drained_after_exit = false;
    callback_error.clear();
    try {
      drained_after_exit = authorization.WaitForOperations(drain_timeout_ms,
                                                            &callback_error);
    } catch (...) {
      callback_error = "post-termination operation drain raised an exception";
    }
    if (!drained_after_exit) {
      remember_error(&first_error,
                     "operation leases remain after worker exit",
                     callback_error);
      close_channel();
      return false;
    }
    return true;
  };

  std::string drain_error;
  bool drained = false;
  try {
    drained = authorization.WaitForOperations(drain_timeout_ms, &drain_error);
  } catch (...) {
    drain_error = "operation drain raised an exception";
  }
  if (!drained) {
    if (!force_terminate_and_drain()) return fail(error, first_error);
  } else {
    // A drained queue is not proof that the worker stopped. Keep the process
    // lifetime bounded too, so a revoked but still-live worker cannot retain a
    // privileged desktop channel indefinitely.
    bool worker_exited = false;
    callback_error.clear();
    try {
      worker_exited = callbacks.wait_for_worker_exit(worker_exit_timeout_ms,
                                                      &callback_error);
    } catch (...) {
      callback_error = "worker-exit callback raised an exception";
    }
    if (!worker_exited && !force_terminate_and_drain())
      return fail(error, first_error);
  }

  callback_error.clear();
  bool finish_ok = false;
  try {
    finish_ok = authorization.FinishRevocation(
        [&] {
          callback_error.clear();
          return callbacks.release_input(&callback_error);
        },
        &callback_error);
  } catch (...) {
    callback_error = "input release callback raised an exception";
  }
  if (!finish_ok)
    remember_error(&first_error, "input release/revocation finalization failed",
                   callback_error);

  close_channel();

  if (!first_error.empty()) return fail(error, first_error);
  return true;
}

}  // namespace chast::host
