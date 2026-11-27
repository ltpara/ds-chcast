#include "portable_helper_approved_session.h"

#include "portable_helper_staging.h"

#include <exception>

namespace chast::host {
namespace {

void append_error(std::string* output, const char* prefix,
                  const std::string& detail) {
  if (output == nullptr || detail.empty()) return;
  if (!output->empty()) *output += "; ";
  *output += prefix;
  *output += detail;
}

bool default_stage(const PortableHelperInstanceId& instance_id,
                   PortableHelperServiceSpec* spec, std::string* error) {
  return stage_portable_helper_service_image_for_instance(instance_id, spec,
                                                            error);
}

}  // namespace

PortableHelperApprovedSession::PortableHelperApprovedSession(
    PortableHelperServiceBackend& backend)
    : backend_(backend) {}

PortableHelperApprovedSession::~PortableHelperApprovedSession() {
  std::string ignored;
  Stop(5000, &ignored);
}

bool PortableHelperApprovedSession::Start(
    PortableHelperLocalApprovalReceipt&& approval,
    PortableHelperServiceBackend& backend, Stage stage, uint32_t timeout_ms,
    std::unique_ptr<PortableHelperApprovedSession>* session,
    std::string* error, PortableHelperSecret* worker_ready_proof) {
  if (error != nullptr) error->clear();
  if (worker_ready_proof != nullptr)
    SecureZeroMemory(worker_ready_proof->data(), worker_ready_proof->size());
  if (session == nullptr) {
    if (error != nullptr) *error = "missing portable helper session output";
    return false;
  }
  session->reset();
  auto created = std::unique_ptr<PortableHelperApprovedSession>(
      new PortableHelperApprovedSession(backend));
  *session = std::move(created);
  if (timeout_ms == 0) {
    if (error != nullptr) *error = "portable helper session timeout is zero";
    return false;
  }
  if (!stage) stage = default_stage;
  return (*session)->start(std::move(approval), std::move(stage), timeout_ms,
                           worker_ready_proof, error);
}

bool PortableHelperApprovedSession::StartAuthorizedGrant(
    const PortableHelperGrant& grant, uint32_t expected_windows_session_id,
    PortableHelperServiceBackend& backend, Stage stage, uint32_t timeout_ms,
    std::unique_ptr<PortableHelperApprovedSession>* session,
    std::string* error, PortableHelperSecret* worker_ready_proof) {
  if (error != nullptr) error->clear();
  if (worker_ready_proof != nullptr)
    SecureZeroMemory(worker_ready_proof->data(), worker_ready_proof->size());
  if (session == nullptr) {
    if (error != nullptr) *error = "missing portable helper session output";
    return false;
  }
  session->reset();
  auto created = std::unique_ptr<PortableHelperApprovedSession>(
      new PortableHelperApprovedSession(backend));
  *session = std::move(created);
  if (timeout_ms == 0) {
    if (error != nullptr) *error = "portable helper session timeout is zero";
    return false;
  }
  DWORD actual_session_id = 0;
  if (expected_windows_session_id == 0 ||
      !ProcessIdToSessionId(GetCurrentProcessId(), &actual_session_id) ||
      actual_session_id != expected_windows_session_id) {
    if (error != nullptr)
      *error = "elevated helper manager is in a different Windows session";
    return false;
  }
  (*session)->grant_ = grant;
  if (!(*session)->authorization_.AcceptParentAuthorizedGrant(
          grant, expected_windows_session_id, error))
    return false;
  if (!stage) stage = default_stage;
  return (*session)->start_granted(std::move(stage), timeout_ms,
                                   worker_ready_proof, error);
}

bool PortableHelperApprovedSession::start(
    PortableHelperLocalApprovalReceipt&& approval, Stage stage,
    uint32_t timeout_ms, PortableHelperSecret* worker_ready_proof,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (!authorization_.BeginGrant(std::move(approval), &grant_, error))
    return false;

  return start_granted(std::move(stage), timeout_ms, worker_ready_proof, error);
}

bool PortableHelperApprovedSession::start_granted(
    Stage stage, uint32_t timeout_ms, PortableHelperSecret* worker_ready_proof,
    std::string* error) {
  PortableHelperServiceSpec spec;
  std::string stage_error;
  bool staged = false;
  try {
    staged = stage(grant_.instance_id, &spec, &stage_error);
  } catch (const std::exception& failure) {
    stage_error = failure.what();
  } catch (...) {
    stage_error = "portable helper staging raised an exception";
  }
  if (!staged) {
    if (error != nullptr) {
      *error = stage_error.empty() ? "portable helper image staging failed"
                                   : stage_error;
    }
    revoke_before_worker(error);
    return false;
  }

  if (spec.instance_id != grant_.instance_id) {
    if (error != nullptr)
      *error = "staging returned an instance outside the approved grant scope";
    lifecycle_ = std::make_unique<PortableHelperServiceLifecycle>(
        std::move(spec), backend_);
    revoke_before_worker(error);
    std::string cleanup_error;
    if (!lifecycle_->Cleanup(&cleanup_error))
      append_error(error, "cleanup pending: ", cleanup_error);
    return false;
  }

  lifecycle_ = std::make_unique<PortableHelperServiceLifecycle>(
      std::move(spec), backend_);
  std::string service_error;
  if (!lifecycle_->CreateAndStart(&service_error)) {
    if (error != nullptr) *error = service_error;
    revoke_before_worker(error);
    std::string cleanup_error;
    if (!lifecycle_->Cleanup(&cleanup_error))
      append_error(error, "cleanup pending: ", cleanup_error);
    return false;
  }

  PortableHelperServiceConnection* connection = lifecycle_->connection();
  if (connection == nullptr || connection->instance_id != grant_.instance_id) {
    if (error != nullptr)
      *error = "authenticated helper service does not match the approved instance";
    revoke_before_worker(error);
    std::string cleanup_error;
    if (!lifecycle_->Cleanup(&cleanup_error))
      append_error(error, "cleanup pending: ", cleanup_error);
    return false;
  }

  if (!start_portable_helper_session_worker(*connection, authorization_, grant_,
                                             timeout_ms, &service_error,
                                             worker_ready_proof)) {
    if (error != nullptr) *error = service_error;
    std::string cleanup_error;
    if (!lifecycle_->Cleanup(&cleanup_error))
      append_error(error, "cleanup pending: ", cleanup_error);
    return false;
  }
  worker_started_ = true;
  ready_ = authorization_.state() ==
           PortableHelperAuthorizationState::Ready;
  if (!ready_) {
    if (error != nullptr)
      *error = "portable helper worker did not enter the ready authorization state";
    if (worker_ready_proof != nullptr)
      SecureZeroMemory(worker_ready_proof->data(), worker_ready_proof->size());
    std::string cleanup_error;
    stop_locked(timeout_ms, &cleanup_error);
    append_error(error, "cleanup: ", cleanup_error);
    return false;
  }
  return true;
}

void PortableHelperApprovedSession::revoke_before_worker(std::string* error) {
  std::string revoke_error;
  // Until WorkerReady is accepted no desktop operation can acquire a parent
  // authorization lease, so the parent has no side effect requiring release.
  if (!authorization_.Revoke([] { return true; }, [] { return true; },
                             &revoke_error))
    append_error(error, "local authorization revocation failed: ",
                 revoke_error);
  ready_ = false;
}

bool PortableHelperApprovedSession::ready() const {
  std::lock_guard lock(mutex_);
  return ready_ && authorization_.state() ==
                       PortableHelperAuthorizationState::Ready;
}

uint32_t PortableHelperApprovedSession::operations() const {
  std::lock_guard lock(mutex_);
  return grant_.operations;
}

bool PortableHelperApprovedSession::CaptureFrame(
    uint64_t request_id, uint32_t timeout_ms,
    PortableHelperCapturedFrame* frame, std::string* error) {
  std::lock_guard lock(mutex_);
  if (error != nullptr) error->clear();
  if (!ready_ || lifecycle_ == nullptr || frame == nullptr) {
    if (error != nullptr) *error = "portable helper session is not ready for capture";
    return false;
  }
  auto* connection = lifecycle_->connection();
  if (connection == nullptr) {
    if (error != nullptr) *error = "portable helper session lost its service connection";
    std::string cleanup_error;
    stop_locked(timeout_ms, &cleanup_error);
    append_error(error, "cleanup: ", cleanup_error);
    return false;
  }
  if (request_portable_helper_capture_frame(
          *connection, authorization_, grant_, request_id, timeout_ms, frame,
          error))
    return true;
  if (authorization_.state() != PortableHelperAuthorizationState::Ready ||
      !connection->channel) {
    ready_ = false;
    std::string cleanup_error;
    stop_locked(timeout_ms, &cleanup_error);
    append_error(error, "cleanup: ", cleanup_error);
  }
  return false;
}

bool PortableHelperApprovedSession::SendInputEvent(
    uint64_t request_id, const chast::InputEvent& event,
    uint32_t timeout_ms, std::string* error) {
  std::lock_guard lock(mutex_);
  if (error != nullptr) error->clear();
  if (!ready_ || lifecycle_ == nullptr) {
    if (error != nullptr) *error = "portable helper session is not ready for input";
    return false;
  }
  auto* connection = lifecycle_->connection();
  if (connection == nullptr) {
    if (error != nullptr) *error = "portable helper session lost its service connection";
    std::string cleanup_error;
    stop_locked(timeout_ms, &cleanup_error);
    append_error(error, "cleanup: ", cleanup_error);
    return false;
  }
  if (request_portable_helper_input_event(
          *connection, authorization_, grant_, request_id, event, timeout_ms,
          error))
    return true;
  if (authorization_.state() != PortableHelperAuthorizationState::Ready ||
      !connection->channel) {
    ready_ = false;
    std::string cleanup_error;
    stop_locked(timeout_ms, &cleanup_error);
    append_error(error, "cleanup: ", cleanup_error);
  }
  return false;
}

bool PortableHelperApprovedSession::stop_locked(uint32_t timeout_ms,
                                                std::string* error) {
  if (error != nullptr) error->clear();
  bool ok = true;
  std::string first_error;
  ready_ = false;

  if (worker_started_) {
    auto* connection = lifecycle_ != nullptr ? lifecycle_->connection() : nullptr;
    std::string stop_error;
    if (connection == nullptr) {
      ok = false;
      first_error = "portable helper worker stop has no authenticated service connection";
      std::string revoke_error;
      if (!authorization_.Revoke([] { return true; }, [] { return true; },
                                 &revoke_error))
        append_error(&first_error, "local authorization revocation failed: ",
                     revoke_error);
    } else if (!stop_portable_helper_session_worker(
                   *connection, authorization_, grant_, timeout_ms,
                   &stop_error)) {
      ok = false;
      first_error = stop_error.empty()
          ? "portable helper worker stop could not be confirmed"
          : stop_error;
    }
    worker_started_ = false;
  } else if (authorization_.state() != PortableHelperAuthorizationState::Idle &&
             authorization_.state() != PortableHelperAuthorizationState::Revoked) {
    std::string revoke_error;
    if (!authorization_.Revoke([] { return true; }, [] { return true; },
                               &revoke_error)) {
      ok = false;
      first_error = revoke_error.empty()
          ? "portable helper authorization could not be revoked"
          : revoke_error;
    }
  }

  if (lifecycle_ != nullptr &&
      lifecycle_->state() != PortableHelperServiceState::Removed) {
    std::string cleanup_error;
    if (!lifecycle_->Cleanup(&cleanup_error)) {
      ok = false;
      if (first_error.empty()) first_error = cleanup_error;
      else append_error(&first_error, "helper cleanup pending: ",
                        cleanup_error);
    }
  }
  if (!ok && error != nullptr) *error = first_error;
  return ok;
}

bool PortableHelperApprovedSession::Stop(uint32_t timeout_ms,
                                         std::string* error) {
  std::lock_guard lock(mutex_);
  return stop_locked(timeout_ms, error);
}

PortableHelperServiceState PortableHelperApprovedSession::service_state() const {
  std::lock_guard lock(mutex_);
  return lifecycle_ != nullptr ? lifecycle_->state()
                               : PortableHelperServiceState::NotCreated;
}

}  // namespace chast::host
