#define NOMINMAX
#include "portable_helper_worker_session.h"

#include <algorithm>
#include <utility>

namespace chast::host {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

bool nonzero(const PortableHelperInstanceId& value) {
  return std::any_of(value.begin(), value.end(),
                     [](uint8_t byte) { return byte != 0; });
}

std::string win32_error(const char* action, DWORD code) {
  return std::string(action) + " (Win32 error " + std::to_string(code) + ")";
}

}  // namespace

bool start_portable_helper_session_worker_channel(
    const PortableHelperServiceSpec& spec,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant,
    const PortableHelperSessionWorkerLauncher& launch_worker,
    uint32_t timeout_ms,
    std::unique_ptr<PortableHelperPipe>* channel,
    HANDLE* worker_process,
    PortableHelperProcessIdentity* worker_identity,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (channel == nullptr || worker_process == nullptr ||
      worker_identity == nullptr || *channel != nullptr ||
      *worker_process != nullptr || !launch_worker)
    return fail(error, "session worker startup outputs or launcher are invalid");
  *worker_identity = {};
  if (!nonzero(spec.instance_id) || spec.instance_id != grant.instance_id ||
      !nonzero(grant.connection_id) || grant.generation == 0 ||
      grant.windows_session_id == 0 ||
      grant.windows_session_id != spec.parent.session_id ||
      grant.operations == 0 ||
      (grant.operations & ~kPortableHelperKnownOperations) != 0 ||
      spec.executable_path.empty() || spec.parent.logon_sid.empty() ||
      spec.parent.process_id == 0 || spec.parent.creation_time == 0)
    return fail(error, "session worker grant does not match its authorized parent");
  if (authorization.state() !=
          PortableHelperAuthorizationState::AwaitingWorkerReady ||
      authorization.generation() != grant.generation)
    return fail(error, "authorization is not awaiting this worker generation");

  auto server = PortableHelperPipe::CreateSessionWorkerServer(
      spec.instance_id, grant.connection_id, spec.parent.logon_sid, error);
  if (!server) return false;

  HANDLE process = nullptr;
  PortableHelperProcessIdentity identity;
  std::string launch_error;
  try {
    if (!launch_worker(spec, grant, &process, &launch_error)) {
      if (launch_error.empty())
        launch_error = "session worker launcher refused to start the process";
    }
  } catch (...) {
    launch_error = "session worker launcher raised an exception";
  }

  const auto cleanup_failed_start = [&](const std::string& primary_error) {
    server.reset();
    std::string cleanup_error;
    if (authorization.state() ==
            PortableHelperAuthorizationState::AwaitingWorkerReady &&
        !authorization.Revoke([] { return true; }, [] { return true; },
                              &cleanup_error)) {
      // Even if local cleanup reports an error, Revoke clears the capability
      // before invoking either callback. Keep the original startup failure as
      // the primary error and retain the cleanup detail for diagnosis.
    }
    const std::string failure = cleanup_error.empty()
        ? primary_error
        : primary_error + "; pending grant revocation: " + cleanup_error;
    if (process == nullptr || process == INVALID_HANDLE_VALUE)
      return fail(error, failure);

    const DWORD initial_wait = WaitForSingleObject(process, 0);
    if (initial_wait == WAIT_TIMEOUT) {
      if (!TerminateProcess(process, ERROR_CANCELLED)) {
        const DWORD terminate_error = GetLastError();
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
          *worker_process = process;
          process = nullptr;
          if (identity.process_id != 0) *worker_identity = std::move(identity);
          return fail(error, failure + "; " +
              win32_error("cannot terminate failed session worker", terminate_error));
        }
      }
      const DWORD terminated = WaitForSingleObject(process, 5000);
      if (terminated != WAIT_OBJECT_0) {
        const DWORD wait_error = terminated == WAIT_FAILED ? GetLastError() :
                                                              ERROR_TIMEOUT;
        *worker_process = process;
        process = nullptr;
        if (identity.process_id != 0) *worker_identity = std::move(identity);
        return fail(error, failure + "; " +
            win32_error("failed session worker has not exited", wait_error));
      }
    } else if (initial_wait != WAIT_OBJECT_0) {
      const DWORD wait_error = GetLastError();
      *worker_process = process;
      process = nullptr;
      if (identity.process_id != 0) *worker_identity = std::move(identity);
      return fail(error, failure + "; " +
          win32_error("cannot query failed session worker", wait_error));
    }

    CloseHandle(process);
    process = nullptr;
    return fail(error, failure);
  };

  if (process == nullptr || process == INVALID_HANDLE_VALUE)
    return cleanup_failed_start(launch_error.empty()
        ? "session worker launcher returned no process handle"
        : launch_error);

  const DWORD process_id = GetProcessId(process);
  std::string identity_error;
  if (process_id == 0 ||
      !query_portable_helper_process_identity(process_id, &identity,
                                               &identity_error))
    return cleanup_failed_start(identity_error.empty()
        ? "cannot query launched session worker identity"
        : identity_error);
  if (process_id == spec.parent.process_id ||
      identity.session_id != grant.windows_session_id ||
      identity.creation_time == 0 || identity.image_path.empty() ||
      _wcsicmp(identity.image_path.c_str(), spec.executable_path.c_str()) != 0 ||
      identity.user_sid.empty() || identity.logon_sid.empty())
    return cleanup_failed_start(
        "launched session worker identity is outside the authorized scope");
  const DWORD process_state = WaitForSingleObject(process, 0);
  if (process_state != WAIT_TIMEOUT)
    return cleanup_failed_start(process_state == WAIT_OBJECT_0
        ? "session worker exited before pipe authentication"
        : win32_error("cannot query launched session worker", GetLastError()));

  if (!server->Accept(identity, timeout_ms, error))
    return cleanup_failed_start(error != nullptr && !error->empty()
        ? "session worker pipe authentication failed: " + *error
        : "session worker pipe authentication failed");
  if (!send_portable_helper_grant(*server, grant, timeout_ms, error))
    return cleanup_failed_start(error != nullptr && !error->empty()
        ? "cannot send capability to session worker: " + *error
        : "cannot send capability to session worker");
  if (!receive_portable_helper_worker_ready(
          *server, authorization, grant, identity, timeout_ms, error))
    return cleanup_failed_start(error != nullptr && !error->empty()
        ? "session worker did not authenticate WorkerReady: " + *error
        : "session worker did not authenticate WorkerReady");

  *worker_identity = std::move(identity);
  *worker_process = process;
  process = nullptr;
  *channel = std::move(server);
  return true;
}

}  // namespace chast::host
