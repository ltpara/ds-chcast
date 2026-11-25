#define NOMINMAX
#include "portable_helper_win32_supervisor.h"

#include "chast/security.h"

#include <utility>

namespace chast::host {
namespace {

constexpr uint32_t kGracefulStopTimeoutMs = 1000;

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

}  // namespace

bool make_win32_portable_helper_revocation_callbacks(
    HANDLE worker_process,
    std::unique_ptr<PortableHelperPipe>& authenticated_channel,
    std::function<bool(std::string*)> release_input,
    PortableHelperWorkerRevocationCallbacks* callbacks,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (worker_process == nullptr || worker_process == INVALID_HANDLE_VALUE ||
      GetProcessId(worker_process) == 0)
    return fail(error, "missing or invalid authenticated worker process handle");
  if (!authenticated_channel)
    return fail(error, "missing authenticated worker channel");
  if (!release_input)
    return fail(error, "missing desktop input release callback");
  if (callbacks == nullptr)
    return fail(error, "missing worker revocation callback output");

  PortableHelperWorkerRevocationCallbacks configured;
  configured.request_cancellation = [&authenticated_channel](
                                        std::string* detail) {
    if (!authenticated_channel)
      return fail(detail, "authenticated worker channel is already closed");
    PortableHelperMessage stop;
    stop.type = PortableHelperMessageType::Stop;
    do {
      if (!secure_random(reinterpret_cast<uint8_t*>(&stop.request_id),
                         sizeof(stop.request_id)))
        return fail(detail, "cannot generate a worker stop request ID");
    } while (stop.request_id == 0);
    return authenticated_channel->Send(std::move(stop),
                                        kGracefulStopTimeoutMs, detail);
  };
  configured.terminate_worker = [worker_process](std::string* detail) {
    const DWORD state = WaitForSingleObject(worker_process, 0);
    if (state == WAIT_OBJECT_0) return true;
    if (state == WAIT_FAILED)
      return fail(detail, "cannot query authenticated worker process state");
    if (state != WAIT_TIMEOUT)
      return fail(detail, "unexpected worker process wait result");
    if (TerminateProcess(worker_process, ERROR_CANCELLED)) return true;
    const DWORD terminate_error = GetLastError();
    if (WaitForSingleObject(worker_process, 0) == WAIT_OBJECT_0) return true;
    if (detail != nullptr)
      *detail = "cannot terminate authenticated worker (error " +
          std::to_string(terminate_error) + ")";
    return false;
  };
  configured.wait_for_worker_exit = [worker_process](
                                         uint32_t timeout_ms,
                                         std::string* detail) {
    const DWORD result = WaitForSingleObject(worker_process, timeout_ms);
    if (result == WAIT_OBJECT_0) return true;
    if (result == WAIT_TIMEOUT)
      return fail(detail, "authenticated worker did not exit before the deadline");
    if (detail != nullptr)
      *detail = "cannot wait for authenticated worker (error " +
          std::to_string(GetLastError()) + ")";
    return false;
  };
  configured.release_input = std::move(release_input);
  configured.close_channel = [&authenticated_channel](std::string*) {
    authenticated_channel.reset();
    return true;
  };
  *callbacks = std::move(configured);
  return true;
}

}  // namespace chast::host
