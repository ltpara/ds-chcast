#define NOMINMAX
#include "portable_helper_service_worker.h"

#include "chast/security.h"
#include "portable_helper_win32_supervisor.h"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

namespace chast::host {
namespace {

constexpr uint32_t kControlIoTimeoutMs = 15000;
constexpr uint32_t kWorkerDrainTimeoutMs = 2000;
constexpr uint32_t kWorkerExitTimeoutMs = 5000;
constexpr size_t kStopPayloadBytes = kPortableHelperInstanceIdBytes + 8;

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

uint32_t get32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) |
      (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
      (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
      (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint64_t get64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8)
    value |= static_cast<uint64_t>(bytes[offset + shift / 8]) << shift;
  return value;
}

void put64(std::vector<uint8_t>* bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes->push_back(static_cast<uint8_t>(value >> shift));
}

bool has_input_operations(uint32_t operations) {
  const uint32_t input_mask =
      static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput) |
      static_cast<uint32_t>(PortableHelperOperation::SendPointerInput);
  return (operations & input_mask) != 0;
}

bool send_acknowledgement(PortableHelperPipe& channel, uint64_t request_id,
                          bool accepted, uint32_t timeout_ms,
                          std::string* error) {
  PortableHelperMessage response;
  response.type = PortableHelperMessageType::Acknowledgement;
  response.request_id = request_id;
  response.payload = {static_cast<uint8_t>(accepted ? 1 : 0)};
  return channel.Send(std::move(response), timeout_ms, error);
}

bool accept_acknowledgement(PortableHelperPipe& channel, uint64_t request_id,
                            uint32_t timeout_ms, std::string* error) {
  PortableHelperMessage response;
  if (!channel.Receive(&response, timeout_ms, error)) return false;
  if (response.type != PortableHelperMessageType::Acknowledgement ||
      response.request_id != request_id ||
      response.payload != std::vector<uint8_t>{1})
    return fail(error, "session worker returned an invalid operation acknowledgement");
  return true;
}

}  // namespace

struct PortableHelperServiceSessionWorker::Impl final {
  explicit Impl(PortableHelperServiceSpec service_spec,
                Launcher worker_launcher,
                EmergencyInputRelease input_release)
      : spec(std::move(service_spec)), launcher(std::move(worker_launcher)),
        emergency_input_release(std::move(input_release)) {}

  ~Impl() {
    std::string ignored;
    (void)Shutdown(&ignored);
    if (worker_process == nullptr || worker_process == INVALID_HANDLE_VALUE)
      return;
    if (WaitForSingleObject(worker_process, 0) != WAIT_OBJECT_0) {
      (void)TerminateProcess(worker_process, ERROR_CANCELLED);
      (void)WaitForSingleObject(worker_process, INFINITE);
    }
    CloseHandle(worker_process);
    worker_process = nullptr;
  }

  bool Handle(PortableHelperPipe& control_channel,
              const PortableHelperMessage& request,
              bool* stop_service, std::string* error) {
    if (error != nullptr) error->clear();
    if (stop_service == nullptr)
      return fail(error, "missing helper service-stop output");
    *stop_service = false;
    if (request.request_id == 0)
      return fail(error, "helper control request ID must be nonzero");

    switch (request.type) {
      case PortableHelperMessageType::Status:
        if (!request.payload.empty())
          return fail(error, "helper status request must have an empty payload");
        return send_acknowledgement(control_channel, request.request_id, true,
                                    kControlIoTimeoutMs, error);
      case PortableHelperMessageType::SessionWorkerStart:
        return Start(control_channel, request, error);
      case PortableHelperMessageType::SessionWorkerStop:
        return StopWorker(control_channel, request, error);
      case PortableHelperMessageType::CaptureFrameRequest:
      case PortableHelperMessageType::InputEventRequest:
        return ForwardDesktopRequest(control_channel, request, error);
      case PortableHelperMessageType::Stop: {
        if (!request.payload.empty())
          return fail(error, "helper service stop request must have an empty payload");
        std::string stop_error;
        const bool stopped = Shutdown(&stop_error);
        if (!send_acknowledgement(control_channel, request.request_id, stopped,
                                  kControlIoTimeoutMs, error))
          return false;
        if (!stopped) {
          if (error != nullptr)
            *error = stop_error.empty()
                ? "session worker cleanup failed before helper shutdown"
                : stop_error;
          // The negative acknowledgement is sufficient to keep the helper
          // service running so the authorized parent can retry cleanup.
          return true;
        }
        *stop_service = true;
        return true;
      }
      default:
        return fail(error, "unsupported portable helper control request");
    }
  }

  bool Start(PortableHelperPipe& control_channel,
             const PortableHelperMessage& request, std::string* error) {
    if (start_attempted_) {
      if (!send_acknowledgement(control_channel, request.request_id, false,
                                kControlIoTimeoutMs, error))
        return false;
      if (error != nullptr)
        *error = "this helper service instance has already consumed its worker start";
      return true;
    }

    std::string start_error;
    const bool parsed = parse_portable_helper_session_worker_start_request(
        request, spec.instance_id, spec.parent.session_id, &grant,
        &start_error);
    const bool granted = parsed &&
        authorization.AcceptParentAuthorizedGrant(
            grant, spec.parent.session_id, &start_error);
    if (granted && has_input_operations(grant.operations) &&
        !emergency_input_release) {
      start_error =
          "keyboard/pointer worker grants require an external input release callback";
      std::string revoke_error;
      (void)authorization.Revoke([] { return true; }, [] { return true; },
                                &revoke_error);
      const bool replied = send_acknowledgement(
          control_channel, request.request_id, false, kControlIoTimeoutMs,
          error);
      if (!replied) return false;
      if (error != nullptr) *error = start_error;
      return true;
    }
    if (granted) start_attempted_ = true;
    bool started = false;
    PortableHelperSecret worker_ready_proof{};
    if (granted) {
      if (launcher) {
        started = start_portable_helper_session_worker_channel(
            spec, authorization, grant, launcher, kControlIoTimeoutMs,
            &worker_channel_, &worker_process, &worker_identity, &start_error,
            &worker_ready_proof);
      } else {
        started = start_portable_helper_session_worker_channel(
            spec, authorization, grant, kControlIoTimeoutMs, &worker_channel_,
            &worker_process, &worker_identity, &start_error,
            &worker_ready_proof);
      }
    }
    if (!started) {
      if (authorization.state() ==
          PortableHelperAuthorizationState::AwaitingWorkerReady) {
        std::string revoke_error;
        (void)authorization.Revoke([] { return true; }, [] { return true; },
                                  &revoke_error);
      }
      const bool replied = send_acknowledgement(
          control_channel, request.request_id, false, kControlIoTimeoutMs,
          error);
      if (!replied) return false;
      if (error != nullptr)
        *error = start_error.empty()
            ? "session worker failed to start or authenticate"
            : start_error;
      SecureZeroMemory(worker_ready_proof.data(), worker_ready_proof.size());
      return true;
    }

    worker_active_ = true;
    if (!send_acknowledgement(control_channel, request.request_id, true,
                              kControlIoTimeoutMs, error)) {
      std::string cleanup_error;
      (void)StopActive(&cleanup_error);
      SecureZeroMemory(worker_ready_proof.data(), worker_ready_proof.size());
      return false;
    }
    PortableHelperMessage ready;
    ready.type = PortableHelperMessageType::WorkerReady;
    ready.request_id = grant.generation;
    ready.payload.assign(worker_ready_proof.begin(), worker_ready_proof.end());
    SecureZeroMemory(worker_ready_proof.data(), worker_ready_proof.size());
    if (!control_channel.Send(std::move(ready), kControlIoTimeoutMs, error)) {
      std::string cleanup_error;
      (void)StopActive(&cleanup_error);
      return false;
    }
    return true;
  }

  bool StopWorker(PortableHelperPipe& control_channel,
                  const PortableHelperMessage& request,
                  std::string* error) {
    if (request.payload.size() != kStopPayloadBytes)
      return fail(error, "session worker stop payload has an invalid size");
    PortableHelperConnectionId connection{};
    std::copy_n(request.payload.begin(), connection.size(), connection.begin());
    const uint64_t generation = get64(request.payload, connection.size());
    if (!worker_active_ || connection != grant.connection_id ||
        generation == 0 || generation != grant.generation)
      return fail(error, "session worker stop scope does not match the active grant");
    std::string stop_error;
    const bool stopped = StopActive(&stop_error);
    if (!send_acknowledgement(control_channel, request.request_id, stopped,
                              kControlIoTimeoutMs, error))
      return false;
    if (!stopped && error != nullptr)
      *error = stop_error.empty() ? "session worker cleanup failed" : stop_error;
    return true;
  }

  bool ForwardDesktopRequest(PortableHelperPipe& control_channel,
                             const PortableHelperMessage& request,
                             std::string* error) {
    if (!worker_active_ || !worker_channel_)
      return fail(error, "no ready session worker is available");

    PortableHelperAuthorization::OperationLease lease;
    PortableHelperOperation operation{};
    chast::InputEvent input_event;
    if (!acquire_portable_helper_desktop_request(
            authorization, request, &lease, &operation, &input_event, error))
      return false;
    (void)operation;
    (void)input_event;

    if (!worker_channel_->Send(request, kControlIoTimeoutMs, error))
      return false;
    if (request.type == PortableHelperMessageType::InputEventRequest) {
      PortableHelperMessage response;
      if (!worker_channel_->Receive(&response, kControlIoTimeoutMs, error))
        return false;
      if (response.type != PortableHelperMessageType::Acknowledgement ||
          response.request_id != request.request_id ||
          response.payload != std::vector<uint8_t>{1})
        return fail(error, "session worker rejected the authorized input request");
      return control_channel.Send(std::move(response), kControlIoTimeoutMs,
                                  error);
    }

    PortableHelperCapturedFrame frame;
    if (!receive_portable_helper_captured_frame(
            *worker_channel_, grant, request.request_id, &frame,
            kControlIoTimeoutMs, error))
      return false;
    const bool sent = send_portable_helper_captured_frame(
        control_channel, grant, request.request_id, frame,
        kControlIoTimeoutMs, error);
    if (!frame.png.empty())
      SecureZeroMemory(frame.png.data(), frame.png.size());
    return sent;
  }

  bool Shutdown(std::string* error) {
    if (error != nullptr) error->clear();
    if (worker_process == nullptr || worker_process == INVALID_HANDLE_VALUE) {
      worker_channel_.reset();
      worker_active_ = false;
      return true;
    }
    if (!worker_active_) return ReapFailedStart(error);

    PortableHelperWorkerRevocationCallbacks callbacks;
    auto graceful_acknowledgement =
        std::make_shared<std::atomic<bool>>(false);
    std::string callback_error;
    if (!make_win32_portable_helper_revocation_callbacks(
            worker_process, worker_channel_,
            [this, graceful_acknowledgement](std::string* release_error) {
              if (emergency_input_release)
                return emergency_input_release(grant, release_error);
              if (!has_input_operations(grant.operations) ||
                  graceful_acknowledgement->load(std::memory_order_acquire))
                return true;
              return fail(release_error,
                  "worker exited without cleanup acknowledgement and no emergency input releaser is configured");
            },
            &callbacks, &callback_error))
      return fail(error, callback_error.empty()
                             ? "cannot create session worker cleanup callbacks"
                             : callback_error);

    const auto request_cancellation = callbacks.request_cancellation;
    callbacks.request_cancellation =
        [request_cancellation, graceful_acknowledgement](std::string* detail) {
          const bool acknowledged = request_cancellation(detail);
          if (acknowledged)
            graceful_acknowledgement->store(true, std::memory_order_release);
          return acknowledged;
        };
    const bool revoked = revoke_portable_helper_worker(
        authorization, callbacks, kWorkerDrainTimeoutMs,
        kWorkerExitTimeoutMs, &callback_error);

    if (WaitForSingleObject(worker_process, 0) == WAIT_OBJECT_0) {
      worker_channel_.reset();
      CloseHandle(worker_process);
      worker_process = nullptr;
      worker_active_ = false;
      worker_identity = {};
    }
    if (!revoked)
      return fail(error, callback_error.empty()
                             ? "session worker did not complete a clean revocation"
                             : callback_error);
    return true;
  }

  bool ReapFailedStart(std::string* error) {
    const DWORD state = WaitForSingleObject(worker_process, 0);
    if (state == WAIT_TIMEOUT) {
      if (!TerminateProcess(worker_process, ERROR_CANCELLED) &&
          WaitForSingleObject(worker_process, 0) != WAIT_OBJECT_0) {
        const DWORD code = GetLastError();
        if (error != nullptr)
          *error = "cannot terminate failed session worker (Win32 error " +
              std::to_string(code) + ")";
        return false;
      }
      if (WaitForSingleObject(worker_process, kWorkerExitTimeoutMs) !=
          WAIT_OBJECT_0)
        return fail(error, "failed session worker did not exit before the deadline");
    } else if (state != WAIT_OBJECT_0) {
      return fail(error, "cannot query failed session worker state");
    }
    worker_channel_.reset();
    CloseHandle(worker_process);
    worker_process = nullptr;
    worker_active_ = false;
    worker_identity = {};
    return true;
  }

  bool StopActive(std::string* error) {
    if (!worker_active_) return ReapFailedStart(error);
    return Shutdown(error);
  }

  PortableHelperServiceSpec spec;
  Launcher launcher;
  EmergencyInputRelease emergency_input_release;
  PortableHelperAuthorization authorization;
  PortableHelperGrant grant;
  std::unique_ptr<PortableHelperPipe> worker_channel_;
  HANDLE worker_process = nullptr;
  PortableHelperProcessIdentity worker_identity;
  bool start_attempted_ = false;
  bool worker_active_ = false;
};

PortableHelperServiceSessionWorker::PortableHelperServiceSessionWorker(
    PortableHelperServiceSpec spec, Launcher launcher,
    EmergencyInputRelease emergency_input_release)
    : impl_(std::make_unique<Impl>(std::move(spec), std::move(launcher),
                                   std::move(emergency_input_release))) {}

PortableHelperServiceSessionWorker::~PortableHelperServiceSessionWorker() = default;

bool PortableHelperServiceSessionWorker::HandleControlRequest(
    PortableHelperPipe& control_channel,
    const PortableHelperMessage& request, bool* stop_service,
    std::string* error) {
  return impl_->Handle(control_channel, request, stop_service, error);
}

bool PortableHelperServiceSessionWorker::Shutdown(std::string* error) {
  return impl_->Shutdown(error);
}

}  // namespace chast::host
