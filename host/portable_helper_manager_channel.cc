#include "portable_helper_manager_channel.h"

#include "portable_helper_approved_session.h"
#include "portable_helper_worker_session.h"
#include "chast/security.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <vector>

namespace chast::host {
namespace {

constexpr uint8_t kManagerFailure = 0;
constexpr uint8_t kManagerSuccess = 1;

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

class ByteVectorWiper final {
 public:
  explicit ByteVectorWiper(std::vector<uint8_t>& bytes) : bytes_(bytes) {}
  ~ByteVectorWiper() {
    if (!bytes_.empty()) SecureZeroMemory(bytes_.data(), bytes_.size());
  }
  ByteVectorWiper(const ByteVectorWiper&) = delete;
  ByteVectorWiper& operator=(const ByteVectorWiper&) = delete;

 private:
  std::vector<uint8_t>& bytes_;
};

bool valid_request(uint64_t request_id, uint32_t timeout_ms,
                   std::string* error) {
  if (request_id == 0 || timeout_ms == 0)
    return fail(error, "portable helper manager request is uncorrelated or untimed");
  return true;
}

void append_error(std::string* error, const char* prefix,
                  const std::string& detail) {
  if (error == nullptr || detail.empty()) return;
  if (!error->empty()) *error += "; ";
  *error += prefix;
  *error += detail;
}

bool same_secret(const PortableHelperSecret& left,
                 const PortableHelperSecret& right) {
  uint8_t difference = 0;
  for (size_t index = 0; index < left.size(); ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

std::wstring widen_ascii(const std::string& value) {
  return std::wstring(value.begin(), value.end());
}

bool current_executable_path(std::wstring* path, std::string* error) {
  if (path == nullptr) return fail(error, "missing manager executable path output");
  path->clear();
  std::vector<wchar_t> buffer(512);
  for (;;) {
    const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    if (copied == 0)
      return fail(error, "cannot locate the current helper executable");
    if (copied < buffer.size() - 1) {
      path->assign(buffer.data(), copied);
      return true;
    }
    if (buffer.size() >= 32768)
      return fail(error, "current helper executable path is too long");
    buffer.resize((std::min)(buffer.size() * 2, size_t{32768}));
  }
}

HANDLE launch_elevated_manager(const std::wstring& parameters,
                               DWORD& launch_error) {
  std::wstring executable;
  std::string path_error;
  if (!current_executable_path(&executable, &path_error)) {
    launch_error = ERROR_FILE_NOT_FOUND;
    return nullptr;
  }
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC |
               SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"runas";
  info.lpFile = executable.c_str();
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    launch_error = GetLastError();
    return nullptr;
  }
  if (info.hProcess == nullptr) {
    launch_error = ERROR_PROCESS_ABORTED;
    return nullptr;
  }
  launch_error = ERROR_SUCCESS;
  return info.hProcess;
}

bool random_manager_request_id(uint64_t* request_id, std::string* error) {
  if (request_id == nullptr)
    return fail(error, "missing elevated-manager request ID output");
  do {
    *request_id = 0;
    if (!chast::secure_random(reinterpret_cast<uint8_t*>(request_id),
                              sizeof(*request_id)))
      return fail(error, "cannot create an elevated-manager request ID");
  } while (*request_id == 0);
  return true;
}

bool stop_session_noexcept(const PortableHelperManagerStopSession& stop,
                           std::string* detail) {
  if (!stop) {
    if (detail != nullptr) *detail = "manager has no helper cleanup callback";
    return false;
  }
  try {
    return stop(detail);
  } catch (const std::exception& failure) {
    if (detail != nullptr) *detail = failure.what();
  } catch (...) {
    if (detail != nullptr) *detail = "helper manager cleanup raised an exception";
  }
  return false;
}

}  // namespace

bool send_portable_helper_manager_start(
    PortableHelperPipe& pipe, const PortableHelperGrant& grant,
    uint64_t request_id, uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_request(request_id, timeout_ms, error)) return false;
  PortableHelperMessage request;
  request.type = PortableHelperMessageType::ManagerStart;
  request.instance_id = grant.instance_id;
  request.request_id = request_id;
  if (!encode_portable_helper_grant_payload(grant, &request.payload, error))
    return false;
  return pipe.Send(std::move(request), timeout_ms, error);
}

bool receive_portable_helper_manager_start(
    PortableHelperPipe& pipe, uint32_t expected_windows_session_id,
    uint32_t timeout_ms, uint64_t* request_id, PortableHelperGrant* grant,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (request_id != nullptr) *request_id = 0;
  if (grant != nullptr) *grant = PortableHelperGrant{};
  if (request_id == nullptr || grant == nullptr || timeout_ms == 0 ||
      expected_windows_session_id == 0)
    return fail(error, "portable helper manager start output or scope is invalid");
  PortableHelperMessage request;
  if (!pipe.Receive(&request, timeout_ms, error)) return false;
  ByteVectorWiper payload_wiper(request.payload);
  if (request.type != PortableHelperMessageType::ManagerStart ||
      request.request_id == 0)
    return fail(error, "expected a correlated portable helper manager start");
  PortableHelperGrant received;
  if (!decode_portable_helper_grant_payload(
          request.payload, request.instance_id, expected_windows_session_id,
          &received, error))
    return false;
  *request_id = request.request_id;
  *grant = received;
  return true;
}

bool send_portable_helper_manager_ready(
    PortableHelperPipe& pipe, uint64_t request_id, bool ready,
    const PortableHelperSecret* worker_ready_proof, uint32_t timeout_ms,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_request(request_id, timeout_ms, error)) return false;
  if (ready && worker_ready_proof == nullptr)
    return fail(error, "portable helper manager is missing its WorkerReady proof");
  if (!ready && worker_ready_proof != nullptr)
    return fail(error, "failed manager startup must not return a WorkerReady proof");
  PortableHelperMessage response;
  response.type = PortableHelperMessageType::ManagerReady;
  response.request_id = request_id;
  response.payload.push_back(ready ? kManagerSuccess : kManagerFailure);
  if (ready)
    response.payload.insert(response.payload.end(), worker_ready_proof->begin(),
                            worker_ready_proof->end());
  return pipe.Send(std::move(response), timeout_ms, error);
}

bool receive_portable_helper_manager_ready(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t expected_request_id,
    uint32_t timeout_ms, PortableHelperSecret* worker_ready_proof,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (worker_ready_proof != nullptr) worker_ready_proof->fill(0);
  if (!valid_request(expected_request_id, timeout_ms, error)) return false;
  PortableHelperMessage response;
  if (!pipe.Receive(&response, timeout_ms, error)) return false;
  ByteVectorWiper payload_wiper(response.payload);
  if (response.type != PortableHelperMessageType::ManagerReady ||
      response.request_id != expected_request_id ||
      response.instance_id != grant.instance_id || response.payload.empty())
    return fail(error, "portable helper manager readiness response is stale or malformed");
  if (response.payload[0] == kManagerFailure && response.payload.size() == 1)
    return fail(error, "elevated manager could not start the approved helper session");
  if (response.payload[0] != kManagerSuccess ||
      response.payload.size() != 1 + PortableHelperSecret{}.size())
    return fail(error, "portable helper manager returned an invalid WorkerReady proof");
  PortableHelperSecret proof{};
  std::copy_n(response.payload.begin() + 1, proof.size(), proof.begin());
  if (!authorization.ConfirmBrokerManagedWorkerReady(grant, proof, error)) {
    SecureZeroMemory(proof.data(), proof.size());
    return false;
  }
  if (worker_ready_proof != nullptr) *worker_ready_proof = proof;
  SecureZeroMemory(proof.data(), proof.size());
  return true;
}

bool send_portable_helper_manager_stop(PortableHelperPipe& pipe,
                                       uint64_t request_id,
                                       uint32_t timeout_ms,
                                       std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_request(request_id, timeout_ms, error)) return false;
  PortableHelperMessage request;
  request.type = PortableHelperMessageType::ManagerStop;
  request.request_id = request_id;
  return pipe.Send(std::move(request), timeout_ms, error);
}

bool receive_portable_helper_manager_stop(PortableHelperPipe& pipe,
                                          uint64_t expected_request_id,
                                          uint32_t timeout_ms,
                                          std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_request(expected_request_id, timeout_ms, error)) return false;
  PortableHelperMessage request;
  if (!pipe.Receive(&request, timeout_ms, error)) return false;
  if (request.type != PortableHelperMessageType::ManagerStop ||
      request.request_id != expected_request_id || !request.payload.empty())
    return fail(error, "portable helper manager stop request is stale or malformed");
  return true;
}

bool send_portable_helper_manager_stopped(PortableHelperPipe& pipe,
                                           uint64_t request_id, bool stopped,
                                           uint32_t timeout_ms,
                                           std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_request(request_id, timeout_ms, error)) return false;
  PortableHelperMessage response;
  response.type = PortableHelperMessageType::ManagerStopped;
  response.request_id = request_id;
  response.payload.push_back(stopped ? kManagerSuccess : kManagerFailure);
  return pipe.Send(std::move(response), timeout_ms, error);
}

bool receive_portable_helper_manager_stopped(PortableHelperPipe& pipe,
                                              uint64_t expected_request_id,
                                              uint32_t timeout_ms,
                                              std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_request(expected_request_id, timeout_ms, error)) return false;
  PortableHelperMessage response;
  if (!pipe.Receive(&response, timeout_ms, error)) return false;
  if (response.type != PortableHelperMessageType::ManagerStopped ||
      response.request_id != expected_request_id ||
      response.payload.size() != 1)
    return fail(error, "portable helper manager stop response is stale or malformed");
  if (response.payload[0] != kManagerSuccess)
    return fail(error, "elevated manager could not confirm helper cleanup");
  return true;
}

bool run_portable_helper_manager_control_channel(
    PortableHelperPipe& pipe, uint32_t expected_windows_session_id,
    uint32_t startup_timeout_ms,
    const PortableHelperManagerStartSession& start_session,
    const PortableHelperManagerStopSession& stop_session,
    std::string* error,
    const PortableHelperManagerCaptureFrame& capture_frame,
    const PortableHelperManagerInputEvent& input_event) {
  if (error != nullptr) error->clear();
  if (expected_windows_session_id == 0 || startup_timeout_ms == 0 ||
      !start_session || !stop_session)
    return fail(error, "portable helper manager lifecycle callbacks or scope are invalid");

  uint64_t start_request_id = 0;
  PortableHelperGrant grant;
  if (!receive_portable_helper_manager_start(
          pipe, expected_windows_session_id, startup_timeout_ms,
          &start_request_id, &grant, error))
    return false;

  PortableHelperSecret proof{};
  std::string start_error;
  bool started = false;
  try {
    started = start_session(grant, &proof, &start_error);
  } catch (const std::exception& failure) {
    start_error = failure.what();
  } catch (...) {
    start_error = "elevated manager session startup raised an exception";
  }
  PortableHelperSecret expected_proof{};
  std::string proof_error;
  const bool valid_proof = started &&
      make_portable_helper_worker_ready_proof(grant, &expected_proof,
                                               &proof_error) &&
      same_secret(proof, expected_proof);
  SecureZeroMemory(expected_proof.data(), expected_proof.size());
  if (!started || !valid_proof) {
    if (start_error.empty())
      start_error = proof_error.empty()
          ? "manager startup did not return the grant-bound WorkerReady proof"
          : proof_error;
    std::string cleanup_error;
    const bool cleaned = stop_session_noexcept(stop_session, &cleanup_error);
    std::string reply_error;
    const bool replied = send_portable_helper_manager_ready(
        pipe, start_request_id, false, nullptr, startup_timeout_ms,
        &reply_error);
    if (error != nullptr) *error = start_error;
    if (!cleaned) append_error(error, "cleanup pending: ", cleanup_error);
    if (!replied) append_error(error, "failed to report startup failure: ",
                               reply_error);
    SecureZeroMemory(proof.data(), proof.size());
    return false;
  }

  if (!send_portable_helper_manager_ready(
          pipe, start_request_id, true, &proof, startup_timeout_ms, error)) {
    SecureZeroMemory(proof.data(), proof.size());
    std::string cleanup_error;
    if (!stop_session_noexcept(stop_session, &cleanup_error))
      append_error(error, "cleanup pending: ", cleanup_error);
    return false;
  }
  SecureZeroMemory(proof.data(), proof.size());

  // Pipe closure is the manager's implicit cancellation signal if the host
  // exits before sending Stop. All desktop requests remain serialized on this
  // authenticated channel and are constrained by the original Start grant.
  for (;;) {
    PortableHelperMessage request;
    if (!pipe.Receive(&request, INFINITE, error)) {
      std::string cleanup_error;
      if (!stop_session_noexcept(stop_session, &cleanup_error))
        append_error(error, "cleanup pending after host disconnect: ",
                     cleanup_error);
      return false;
    }
    ByteVectorWiper payload_wiper(request.payload);
    if (request.request_id == 0) {
      if (error != nullptr)
        *error = "portable helper manager received an uncorrelated request";
      std::string cleanup_error;
      if (!stop_session_noexcept(stop_session, &cleanup_error))
        append_error(error, "cleanup pending: ", cleanup_error);
      return false;
    }

    if (request.type == PortableHelperMessageType::ManagerStop) {
      if (!request.payload.empty()) {
        if (error != nullptr)
          *error = "portable helper manager received a malformed stop request";
        std::string cleanup_error;
        if (!stop_session_noexcept(stop_session, &cleanup_error))
          append_error(error, "cleanup pending: ", cleanup_error);
        return false;
      }
      std::string stop_error;
      const bool stopped = stop_session_noexcept(stop_session, &stop_error);
      std::string reply_error;
      const bool replied = send_portable_helper_manager_stopped(
          pipe, request.request_id, stopped, startup_timeout_ms, &reply_error);
      if (!stopped && error != nullptr)
        *error = stop_error.empty()
            ? "elevated manager could not stop the helper session"
            : stop_error;
      if (!replied)
        append_error(error, "failed to report helper cleanup: ", reply_error);
      return stopped && replied;
    }

    if (request.type == PortableHelperMessageType::ManagerCaptureFrameRequest) {
      if (!request.payload.empty() || !capture_frame) {
        if (error != nullptr)
          *error = "portable helper manager capture request is malformed or unavailable";
        std::string cleanup_error;
        if (!stop_session_noexcept(stop_session, &cleanup_error))
          append_error(error, "cleanup pending: ", cleanup_error);
        return false;
      }
      PortableHelperCapturedFrame frame;
      std::string capture_error;
      bool captured = false;
      try {
        captured = capture_frame(request.request_id, startup_timeout_ms,
                                &frame, &capture_error);
      } catch (const std::exception& failure) {
        capture_error = failure.what();
      } catch (...) {
        capture_error = "manager capture callback raised an exception";
      }
      std::string reply_error;
      const bool replied = captured && send_portable_helper_captured_frame(
          pipe, grant, request.request_id, frame, startup_timeout_ms,
          &reply_error);
      if (!frame.png.empty())
        SecureZeroMemory(frame.png.data(), frame.png.size());
      if (!replied) {
        if (error != nullptr)
          *error = capture_error.empty()
              ? "portable helper manager could not relay a captured frame"
              : capture_error;
        append_error(error, "capture relay failed: ", reply_error);
        std::string cleanup_error;
        if (!stop_session_noexcept(stop_session, &cleanup_error))
          append_error(error, "cleanup pending: ", cleanup_error);
        return false;
      }
      continue;
    }

    if (request.type == PortableHelperMessageType::ManagerInputEventRequest) {
      chast::InputEvent event;
      std::string input_error;
      bool accepted = input_event &&
          chast::parse_input_event(request.payload, event, &input_error);
      if (accepted) {
        try {
          accepted = input_event(request.request_id, event,
                                 startup_timeout_ms, &input_error);
        } catch (const std::exception& failure) {
          input_error = failure.what();
          accepted = false;
        } catch (...) {
          input_error = "manager input callback raised an exception";
          accepted = false;
        }
      }
      if (accepted) {
        PortableHelperMessage response;
        response.type = PortableHelperMessageType::Acknowledgement;
        response.request_id = request.request_id;
        response.payload = {1};
        if (pipe.Send(std::move(response), startup_timeout_ms, error))
          continue;
        input_error = error != nullptr ? *error : "manager input acknowledgement failed";
      }
      if (error != nullptr)
        *error = input_error.empty()
            ? "portable helper manager rejected the input request"
            : input_error;
      std::string cleanup_error;
      if (!stop_session_noexcept(stop_session, &cleanup_error))
        append_error(error, "cleanup pending: ", cleanup_error);
      return false;
    }

    if (error != nullptr)
      *error = "portable helper manager received an unexpected request type";
    std::string cleanup_error;
    if (!stop_session_noexcept(stop_session, &cleanup_error))
      append_error(error, "cleanup pending: ", cleanup_error);
    return false;
  }
}

PortableHelperManagerRpc::PortableHelperManagerRpc(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant)
    : pipe_(pipe), authorization_(authorization), grant_(grant) {}

PortableHelperManagerRpc::~PortableHelperManagerRpc() {
  std::lock_guard lock(rpc_mutex_);
  if (!closed_)
    (void)fail_closed("manager RPC proxy was destroyed before confirmed Stop",
                      nullptr);
}

bool PortableHelperManagerRpc::fail_closed(const std::string& reason,
                                           std::string* error) {
  closed_ = true;
  pipe_.Close();
  std::string revoke_error;
  if (authorization_.state() == PortableHelperAuthorizationState::Ready ||
      authorization_.state() == PortableHelperAuthorizationState::AwaitingWorkerReady)
    (void)authorization_.Revoke([] { return true; }, [] { return true; },
                                &revoke_error);
  if (error != nullptr) {
    *error = reason;
    append_error(error, "local forwarding gate closed: ", revoke_error);
  }
  return false;
}

bool PortableHelperManagerRpc::CaptureFrame(
    uint64_t request_id, uint32_t timeout_ms,
    PortableHelperCapturedFrame* frame, std::string* error) {
  std::lock_guard lock(rpc_mutex_);
  if (error != nullptr) error->clear();
  if (closed_ || frame == nullptr || timeout_ms == 0 || request_id == 0)
    return fail(error, "manager capture request is invalid or the channel is closed");
  *frame = {};
  PortableHelperMessage scoped_request;
  std::string rpc_error;
  if (!make_portable_helper_capture_request(authorization_, grant_, request_id,
                                             &scoped_request, &rpc_error))
    return fail(error, rpc_error);
  PortableHelperMessage request;
  request.type = PortableHelperMessageType::ManagerCaptureFrameRequest;
  request.request_id = request_id;
  if (!pipe_.Send(std::move(request), timeout_ms, &rpc_error) ||
      !receive_portable_helper_captured_frame(
          pipe_, grant_, request_id, frame, timeout_ms, &rpc_error)) {
    if (!frame->png.empty()) {
      SecureZeroMemory(frame->png.data(), frame->png.size());
      frame->png.clear();
    }
    return fail_closed(rpc_error.empty()
        ? "portable helper manager capture RPC failed" : rpc_error, error);
  }
  return true;
}

bool PortableHelperManagerRpc::SendInputEvent(
    uint64_t request_id, const chast::InputEvent& event,
    uint32_t timeout_ms, std::string* error) {
  std::lock_guard lock(rpc_mutex_);
  if (error != nullptr) error->clear();
  if (closed_ || timeout_ms == 0 || request_id == 0)
    return fail(error, "manager input request is invalid or the channel is closed");
  PortableHelperMessage scoped_request;
  std::string rpc_error;
  if (!make_portable_helper_input_request(authorization_, grant_, request_id,
                                          event, &scoped_request, &rpc_error))
    return fail(error, rpc_error);
  PortableHelperMessage request;
  request.type = PortableHelperMessageType::ManagerInputEventRequest;
  request.request_id = request_id;
  request.payload = chast::make_input_event_payload(event);
  if (!pipe_.Send(std::move(request), timeout_ms, &rpc_error))
    return fail_closed(rpc_error.empty()
        ? "portable helper manager input RPC failed" : rpc_error, error);
  PortableHelperMessage response;
  if (!pipe_.Receive(&response, timeout_ms, &rpc_error) ||
      response.type != PortableHelperMessageType::Acknowledgement ||
      response.request_id != request_id || response.payload !=
          std::vector<uint8_t>{1})
    return fail_closed(rpc_error.empty()
        ? "portable helper manager returned an invalid input acknowledgement"
        : rpc_error, error);
  return true;
}

bool PortableHelperManagerRpc::Stop(uint64_t request_id, uint32_t timeout_ms,
                                    std::string* error) {
  std::lock_guard lock(rpc_mutex_);
  if (error != nullptr) error->clear();
  if (closed_ || timeout_ms == 0 || request_id == 0)
    return fail(error, "manager stop request is invalid or the channel is closed");
  std::string rpc_error;
  if (!send_portable_helper_manager_stop(pipe_, request_id, timeout_ms,
                                         &rpc_error) ||
      !receive_portable_helper_manager_stopped(pipe_, request_id, timeout_ms,
                                               &rpc_error))
    return fail_closed(rpc_error.empty()
        ? "portable helper manager cleanup was not confirmed" : rpc_error,
        error);
  if (!authorization_.Revoke([] { return true; }, [] { return true; },
                             &rpc_error))
    return fail_closed(rpc_error.empty()
        ? "parent authorization could not be revoked after manager cleanup"
        : rpc_error, error);
  closed_ = true;
  return true;
}

bool PortableHelperElevatedManagerSession::Start(
    PortableHelperLocalApprovalReceipt&& approval, uint32_t timeout_ms,
    Revalidate revalidate,
    std::unique_ptr<PortableHelperElevatedManagerSession>* session,
    std::string* error, Launcher launcher) {
  if (error != nullptr) error->clear();
  if (session == nullptr)
    return fail(error, "missing elevated-manager session output");
  session->reset();
  if (timeout_ms == 0 || !revalidate)
    return fail(error, "elevated-manager timeout or connection revalidation is missing");
  auto created = std::unique_ptr<PortableHelperElevatedManagerSession>(
      new PortableHelperElevatedManagerSession());
  if (!created->start(std::move(approval), timeout_ms, std::move(revalidate),
                      launcher, error))
    return false;
  *session = std::move(created);
  return true;
}

bool PortableHelperElevatedManagerSession::start(
    PortableHelperLocalApprovalReceipt&& approval, uint32_t timeout_ms,
    Revalidate revalidate, const Launcher& launcher, std::string* error) {
  revalidate_ = std::move(revalidate);
  if (!authorization_.BeginGrant(std::move(approval), &grant_, error))
    return false;
  if (!connection_is_live(error)) {
    revoke_parent_gate();
    return false;
  }

  PortableHelperProcessIdentity parent_identity;
  if (!current_portable_helper_process_identity(&parent_identity, error) ||
      parent_identity.session_id == 0 || parent_identity.image_path.empty() ||
      parent_identity.user_sid.empty() || parent_identity.logon_sid.empty()) {
    if (error != nullptr && error->empty())
      *error = "interactive host identity is incomplete for manager launch";
    revoke_parent_gate();
    return false;
  }

  pipe_ = PortableHelperPipe::CreateManagerServer(
      grant_.instance_id, parent_identity.logon_sid, error);
  if (!pipe_) {
    revoke_parent_gate();
    return false;
  }

  const std::wstring parameters = L"--portable-helper-manager " +
      widen_ascii(chast::hex_encode(grant_.instance_id.data(),
                                    grant_.instance_id.size())) + L" " +
      std::to_wstring(parent_identity.process_id) + L" " +
      std::to_wstring(parent_identity.creation_time);
  DWORD launch_error = ERROR_SUCCESS;
  try {
    manager_process_ = launcher ? launcher(parameters, launch_error)
                                : launch_elevated_manager(parameters,
                                                          launch_error);
  } catch (const std::exception& failure) {
    if (error != nullptr) *error = failure.what();
    pipe_->Close();
    revoke_parent_gate();
    pipe_.reset();
    return false;
  } catch (...) {
    if (error != nullptr)
      *error = "elevated-manager launcher raised an exception";
    pipe_->Close();
    revoke_parent_gate();
    pipe_.reset();
    return false;
  }
  if (manager_process_ == nullptr || manager_process_ == INVALID_HANDLE_VALUE) {
    if (error != nullptr) {
      *error = launch_error == ERROR_CANCELLED
          ? "Windows UAC elevation was cancelled"
          : "cannot launch the elevated helper manager (error " +
                std::to_string(launch_error) + ")";
    }
    pipe_->Close();
    revoke_parent_gate();
    pipe_.reset();
    return false;
  }

  const auto abort_start = [&](const std::string& reason,
                               bool grant_was_sent) {
    if (pipe_) pipe_->Close();
    revoke_parent_gate();
    if (manager_process_ != nullptr) {
      const DWORD bounded_wait = timeout_ms < 5000u
          ? static_cast<DWORD>(timeout_ms) : DWORD{5000};
      const DWORD waited = WaitForSingleObject(manager_process_, bounded_wait);
      if (waited == WAIT_TIMEOUT && !grant_was_sent) {
        (void)TerminateProcess(manager_process_, ERROR_CANCELLED);
        (void)WaitForSingleObject(manager_process_, 5000);
      }
    }
    rpc_.reset();
    pipe_.reset();
    if (manager_process_ != nullptr) {
      CloseHandle(manager_process_);
      manager_process_ = nullptr;
    }
    if (error != nullptr) *error = reason;
    return false;
  };

  const DWORD manager_pid = GetProcessId(manager_process_);
  if (manager_pid == 0 || WaitForSingleObject(manager_process_, 0) != WAIT_TIMEOUT)
    return abort_start("elevated manager exited before connecting", false);

  PortableHelperProcessIdentity manager_identity;
  std::string identity_error;
  if (!query_portable_helper_process_identity(manager_pid, &manager_identity,
                                               &identity_error))
    return abort_start(identity_error.empty()
        ? "cannot authenticate the elevated manager process identity"
        : identity_error, false);
  if (manager_identity.process_id != manager_pid ||
      manager_identity.session_id != parent_identity.session_id ||
      manager_identity.user_sid != parent_identity.user_sid ||
      manager_identity.logon_sid != parent_identity.logon_sid ||
      _wcsicmp(manager_identity.image_path.c_str(),
               parent_identity.image_path.c_str()) != 0)
    return abort_start("elevated manager does not match the host logon and image",
                       false);
  bool elevated = false;
  if (!query_portable_helper_process_elevation(manager_pid, &elevated,
                                                &identity_error) || !elevated)
    return abort_start(identity_error.empty()
        ? "portable helper manager requires a UAC-elevated process"
        : identity_error, false);
  if (!pipe_->Accept(manager_identity, timeout_ms, error))
    return abort_start(error != nullptr && !error->empty()
        ? *error : "manager pipe did not authenticate its launched child",
        false);
  if (WaitForSingleObject(manager_process_, 0) != WAIT_TIMEOUT)
    return abort_start("elevated manager exited during pipe authentication",
                       false);
  std::string revalidation_error;
  if (!connection_is_live(&revalidation_error))
    return abort_start(revalidation_error.empty()
        ? "the approved viewer disconnected before grant delivery"
        : revalidation_error, false);

  uint64_t start_request_id = 0;
  if (!random_manager_request_id(&start_request_id, error))
    return abort_start(error != nullptr ? *error
        : "cannot create an elevated-manager start request ID", false);
  if (!send_portable_helper_manager_start(*pipe_, grant_, start_request_id,
                                         timeout_ms, error))
    return abort_start(error != nullptr && !error->empty()
        ? *error : "cannot send the approved grant to the manager", false);

  PortableHelperSecret proof{};
  const bool ready = receive_portable_helper_manager_ready(
      *pipe_, authorization_, grant_, start_request_id, timeout_ms, &proof,
      error);
  SecureZeroMemory(proof.data(), proof.size());
  if (!ready)
    return abort_start(error != nullptr && !error->empty()
        ? *error : "elevated manager did not prove WorkerReady", true);

  try {
    rpc_ = std::make_unique<PortableHelperManagerRpc>(
        *pipe_, authorization_, grant_);
  } catch (...) {
    return abort_start("cannot create the parent manager RPC proxy", true);
  }
  ready_ = true;
  if (!connection_is_live(&revalidation_error)) {
    const std::string reason = revalidation_error.empty()
        ? "the approved viewer disconnected before WorkerReady completed"
        : revalidation_error;
    uint64_t stop_request_id = 0;
    std::string cleanup_error;
    if (!random_manager_request_id(&stop_request_id, &cleanup_error) ||
        !Stop(stop_request_id, timeout_ms, &cleanup_error))
      append_error(&revalidation_error, "manager cleanup after stale approval failed: ",
                   cleanup_error);
    if (error != nullptr)
      *error = revalidation_error.empty() ? reason : revalidation_error;
    return false;
  }
  return true;
}

PortableHelperElevatedManagerSession::~PortableHelperElevatedManagerSession() {
  if (!stopped_ && ready_ && rpc_) {
    uint64_t request_id = 0;
    std::string stop_error;
    if (random_manager_request_id(&request_id, &stop_error))
      (void)Stop(request_id, 5000, &stop_error);
  }
  if (pipe_) pipe_->Close();
  revoke_parent_gate();
  rpc_.reset();
  pipe_.reset();
  if (manager_process_ != nullptr) {
    (void)WaitForSingleObject(manager_process_, 5000);
    CloseHandle(manager_process_);
    manager_process_ = nullptr;
  }
}

bool PortableHelperElevatedManagerSession::ready() const {
  return ready_ && !stopped_ &&
      authorization_.state() == PortableHelperAuthorizationState::Ready;
}

bool PortableHelperElevatedManagerSession::CaptureFrame(
    uint64_t request_id, uint32_t timeout_ms,
    PortableHelperCapturedFrame* frame, std::string* error) {
  if (!ready() || !rpc_)
    return fail(error, "elevated helper manager session is not ready");
  if (!connection_is_live(error)) {
    uint64_t stop_request_id = 0;
    std::string cleanup_error;
    if (random_manager_request_id(&stop_request_id, &cleanup_error) &&
        !Stop(stop_request_id, timeout_ms, &cleanup_error))
      append_error(error, "manager cleanup after viewer revocation failed: ",
                   cleanup_error);
    return false;
  }
  return rpc_->CaptureFrame(request_id, timeout_ms, frame, error);
}

bool PortableHelperElevatedManagerSession::SendInputEvent(
    uint64_t request_id, const chast::InputEvent& event,
    uint32_t timeout_ms, std::string* error) {
  if (!ready() || !rpc_)
    return fail(error, "elevated helper manager session is not ready");
  if (!connection_is_live(error)) {
    uint64_t stop_request_id = 0;
    std::string cleanup_error;
    if (random_manager_request_id(&stop_request_id, &cleanup_error) &&
        !Stop(stop_request_id, timeout_ms, &cleanup_error))
      append_error(error, "manager cleanup after viewer revocation failed: ",
                   cleanup_error);
    return false;
  }
  return rpc_->SendInputEvent(request_id, event, timeout_ms, error);
}

bool PortableHelperElevatedManagerSession::Stop(uint64_t request_id,
                                                 uint32_t timeout_ms,
                                                 std::string* error) {
  if (error != nullptr) error->clear();
  if (stopped_) return true;
  if (timeout_ms == 0 || request_id == 0)
    return fail(error, "elevated manager stop scope or timeout is invalid");
  if (!stop_acknowledged_) {
    if (rpc_ == nullptr || !rpc_->Stop(request_id, timeout_ms, error)) {
      ready_ = false;
      return false;
    }
    stop_acknowledged_ = true;
    ready_ = false;
    rpc_.reset();
  }
  if (manager_process_ != nullptr) {
    const DWORD waited = WaitForSingleObject(manager_process_, timeout_ms);
    if (waited != WAIT_OBJECT_0)
      return fail(error, "manager acknowledged cleanup but did not exit before the deadline");
    DWORD exit_code = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(manager_process_, &exit_code) ||
        exit_code != ERROR_SUCCESS)
      return fail(error, "manager exited unsuccessfully after its cleanup acknowledgement");
    CloseHandle(manager_process_);
    manager_process_ = nullptr;
  }
  pipe_.reset();
  stopped_ = true;
  return true;
}

void PortableHelperElevatedManagerSession::revoke_parent_gate() noexcept {
  const auto state = authorization_.state();
  if (state == PortableHelperAuthorizationState::Ready ||
      state == PortableHelperAuthorizationState::AwaitingWorkerReady ||
      state == PortableHelperAuthorizationState::Revoking) {
    std::string ignored;
    try {
      (void)authorization_.Revoke([] { return true; }, [] { return true; },
                                  &ignored);
    } catch (...) {
    }
  }
  ready_ = false;
}

bool PortableHelperElevatedManagerSession::connection_is_live(
    std::string* error) const {
  if (error != nullptr) error->clear();
  if (!revalidate_)
    return fail(error, "manager session has no original-connection revalidator");
  try {
    if (revalidate_(error)) return true;
  } catch (...) {
    if (error != nullptr && error->empty())
      *error = "original-connection revalidation raised an exception";
  }
  if (error != nullptr && error->empty())
    *error = "the original approved viewer is no longer eligible";
  return false;
}

int run_portable_helper_elevated_manager(
    const PortableHelperInstanceId& instance_id, uint32_t parent_process_id,
    uint64_t parent_creation_time, std::string* error) {
  if (error != nullptr) error->clear();
  if (parent_process_id == 0 || parent_creation_time == 0 ||
      std::all_of(instance_id.begin(), instance_id.end(),
                  [](uint8_t value) { return value == 0; })) {
    fail(error, "portable helper manager launch scope is invalid");
    return ERROR_INVALID_PARAMETER;
  }

  PortableHelperProcessIdentity manager_identity;
  PortableHelperProcessIdentity parent_identity;
  if (!current_portable_helper_process_identity(&manager_identity, error) ||
      !query_portable_helper_process_identity(parent_process_id,
                                              &parent_identity, error))
    return ERROR_ACCESS_DENIED;
  if (manager_identity.session_id == 0 ||
      manager_identity.session_id != parent_identity.session_id ||
      manager_identity.user_sid != parent_identity.user_sid ||
      manager_identity.logon_sid != parent_identity.logon_sid ||
      _wcsicmp(manager_identity.image_path.c_str(),
               parent_identity.image_path.c_str()) != 0) {
    if (error != nullptr)
      *error = "portable helper manager does not match the interactive parent identity";
    return ERROR_ACCESS_DENIED;
  }
  if (parent_identity.creation_time != parent_creation_time) {
    if (error != nullptr)
      *error = "portable helper manager parent creation time does not match";
    return ERROR_ACCESS_DENIED;
  }
  bool elevated = false;
  if (!query_portable_helper_process_elevation(manager_identity.process_id,
                                               &elevated, error) ||
      !elevated) {
    if (error != nullptr && error->empty())
      *error = "portable helper manager requires an elevated token";
    return ERROR_ELEVATION_REQUIRED;
  }

  auto pipe = PortableHelperPipe::ConnectManagerClient(
      instance_id, parent_identity, 30000, error);
  if (!pipe) return ERROR_PIPE_NOT_CONNECTED;

  Win32PortableHelperServiceBackend service_backend;
  std::unique_ptr<PortableHelperApprovedSession> approved_session;
  const auto start_session = [&] (
      const PortableHelperGrant& grant, PortableHelperSecret* proof,
      std::string* start_error) {
    return PortableHelperApprovedSession::StartAuthorizedGrant(
        grant, manager_identity.session_id, service_backend, {}, 30000,
        &approved_session, start_error, proof);
  };
  const auto stop_session = [&] (std::string* stop_error) {
    if (!approved_session) return true;
    if (!approved_session->Stop(10000, stop_error)) return false;
    approved_session.reset();
    return true;
  };
  const bool served = run_portable_helper_manager_control_channel(
      *pipe, manager_identity.session_id, 30000, start_session, stop_session,
      error,
      [&approved_session](uint64_t request_id, uint32_t timeout_ms,
                          PortableHelperCapturedFrame* frame,
                          std::string* capture_error) {
        return approved_session && approved_session->CaptureFrame(
            request_id, timeout_ms, frame, capture_error);
      },
      [&approved_session](uint64_t request_id, const chast::InputEvent& event,
                          uint32_t timeout_ms, std::string* input_error) {
        return approved_session && approved_session->SendInputEvent(
            request_id, event, timeout_ms, input_error);
      });
  if (!served) {
    if (error != nullptr && error->empty())
      *error = "portable helper manager control channel ended before cleanup was confirmed";
    return ERROR_SERVICE_REQUEST_TIMEOUT;
  }
  return ERROR_SUCCESS;
}

}  // namespace chast::host
