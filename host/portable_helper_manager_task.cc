#include "portable_helper_manager_task.h"

#include "chast/security.h"

#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <exception>
#include <limits>
#include <thread>
#include <utility>

namespace chast::host {
namespace {

bool next_manager_request_id(std::atomic<uint64_t>& next, uint64_t* request_id,
                             std::string* error) {
  if (request_id == nullptr) {
    if (error != nullptr) *error = "missing elevated-manager request ID output";
    return false;
  }
  uint64_t current = next.load(std::memory_order_relaxed);
  while (current != 0) {
    const uint64_t following = current ==
            (std::numeric_limits<uint64_t>::max)()
        ? 0 : current + 1;
    if (next.compare_exchange_weak(current, following,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      *request_id = current;
      return true;
    }
  }
  if (error != nullptr) *error = "elevated-manager request ID space is exhausted";
  return false;
}

}  // namespace

struct PortableHelperElevatedManagerTask::State {
  enum class Phase { Starting, Ready, Failed, Cancelled };

  mutable std::mutex mutex;
  std::mutex session_mutex;
  Phase phase = Phase::Starting;
  bool cancellation_requested = false;
  std::string reason;
  std::shared_ptr<PortableHelperElevatedManagerSession> session;
  std::atomic<uint64_t> next_request_id{1};
};

PortableHelperElevatedManagerTask::PortableHelperElevatedManagerTask(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}

std::shared_ptr<PortableHelperElevatedManagerTask>
PortableHelperElevatedManagerTask::Start(
    PortableHelperLocalApprovalReceipt&& approval, uint32_t timeout_ms,
    PortableHelperElevatedManagerSession::Revalidate revalidate,
    std::string* error,
    PortableHelperElevatedManagerSession::Launcher launcher) {
  if (error != nullptr) error->clear();
  if (timeout_ms == 0 || !revalidate || !approval) {
    if (error != nullptr)
      *error = "elevated-manager task requires an approved receipt, revalidator and timeout";
    return {};
  }

  auto state = std::make_shared<State>();
  try {
    uint64_t seed = 0;
    if (!chast::secure_random(reinterpret_cast<uint8_t*>(&seed), sizeof(seed)) ||
        seed == 0) {
      if (error != nullptr)
        *error = "cannot initialize elevated-manager task request IDs";
      return {};
    }
    state->next_request_id.store(seed, std::memory_order_relaxed);
    auto task = std::shared_ptr<PortableHelperElevatedManagerTask>(
        new PortableHelperElevatedManagerTask(state));
    std::thread worker(
        [state, approval = std::move(approval), timeout_ms,
         revalidate = std::move(revalidate), launcher = std::move(launcher)]() mutable {
          std::unique_ptr<PortableHelperElevatedManagerSession> started;
          std::string start_error;
          bool succeeded = false;
          try {
            auto guarded_revalidate = [state, revalidate](std::string* live_error) {
              {
                std::lock_guard lock(state->mutex);
                if (state->cancellation_requested) {
                  if (live_error != nullptr)
                    *live_error = "the approved viewer request was cancelled";
                  return false;
                }
              }
              return revalidate(live_error);
            };
            succeeded = PortableHelperElevatedManagerSession::Start(
                std::move(approval), timeout_ms, std::move(guarded_revalidate),
                &started, &start_error, std::move(launcher));
          } catch (const std::exception& failure) {
            start_error = failure.what();
          } catch (...) {
            start_error = "elevated-manager startup raised an exception";
          }

          std::shared_ptr<PortableHelperElevatedManagerSession> retained;
          if (succeeded && started) {
            try {
              retained = std::shared_ptr<PortableHelperElevatedManagerSession>(
                  std::move(started));
            } catch (...) {
              start_error = "cannot retain the elevated-manager session";
              succeeded = false;
            }
          }

          bool cancel_after_start = false;
          {
            std::lock_guard lock(state->mutex);
            cancel_after_start = state->cancellation_requested;
            if (cancel_after_start) {
              state->phase = State::Phase::Cancelled;
              if (state->reason.empty())
                state->reason = "the approved viewer request was cancelled";
            } else if (succeeded && retained && retained->ready()) {
              state->session = retained;
              state->phase = State::Phase::Ready;
              state->reason.clear();
            } else {
              state->phase = State::Phase::Failed;
              state->reason = start_error.empty()
                  ? "elevated helper manager did not become ready"
                  : std::move(start_error);
            }
          }

          if (cancel_after_start && retained) {
            std::lock_guard session_lock(state->session_mutex);
            uint64_t request_id = 0;
            std::string stop_error;
            if (!next_manager_request_id(state->next_request_id, &request_id,
                                         &stop_error) ||
                !retained->Stop(request_id, 10000, &stop_error)) {
              std::lock_guard lock(state->mutex);
              if (!stop_error.empty()) {
                state->reason += "; helper cleanup pending: ";
                state->reason += stop_error;
              }
              state->session = std::move(retained);
            }
          }
        });
    worker.detach();
    return task;
  } catch (const std::exception& failure) {
    if (error != nullptr) *error = failure.what();
  } catch (...) {
    if (error != nullptr) *error = "cannot start the elevated-manager worker thread";
  }
  return {};
}

PortableHelperElevatedManagerTask::~PortableHelperElevatedManagerTask() {
  cancel();
}

chast::ElevationState PortableHelperElevatedManagerTask::poll() {
  std::shared_ptr<PortableHelperElevatedManagerSession> session;
  chast::ElevationState result;
  {
    std::lock_guard lock(state_->mutex);
    session = state_->session;
    result.reason = state_->reason;
    switch (state_->phase) {
      case State::Phase::Starting:
        result.code = chast::ElevationCode::Pending;
        break;
      case State::Phase::Ready:
        result.code = chast::ElevationCode::HelperReady;
        break;
      case State::Phase::Failed:
        result.code = chast::ElevationCode::Failed;
        break;
      case State::Phase::Cancelled:
        result.code = chast::ElevationCode::Cancelled;
        break;
    }
  }
  if (result.code == chast::ElevationCode::HelperReady) {
    bool session_ready = false;
    if (session) {
      std::unique_lock session_lock(state_->session_mutex, std::try_to_lock);
      // CaptureFrame may be waiting on a bounded worker RPC. Polling the UI or
      // host loop must not wait behind that call; the RPC validates manager
      // identity itself and a later poll will observe an exited manager.
      if (!session_lock.owns_lock()) return result;
      session_ready = session->ready();
    }
    if (!session_ready) {
      std::lock_guard lock(state_->mutex);
      if (state_->phase == State::Phase::Ready && state_->session == session) {
        state_->phase = State::Phase::Failed;
        state_->reason = "elevated helper manager exited or lost authorization";
        result.code = chast::ElevationCode::Failed;
        result.reason = state_->reason;
      }
    }
  }
  return result;
}

void PortableHelperElevatedManagerTask::commit() {}

void PortableHelperElevatedManagerTask::cancel() {
  {
    std::lock_guard lock(state_->mutex);
    state_->cancellation_requested = true;
    if (state_->phase == State::Phase::Starting ||
        state_->phase == State::Phase::Ready)
      state_->phase = State::Phase::Cancelled;
    if (state_->reason.empty())
      state_->reason = "the approved viewer request was cancelled";
  }
  stop_session(state_);
}

bool PortableHelperElevatedManagerTask::ready() const {
  std::shared_ptr<PortableHelperElevatedManagerSession> session;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != State::Phase::Ready || state_->cancellation_requested)
      return false;
    session = state_->session;
  }
  if (!session) return false;
  std::unique_lock session_lock(state_->session_mutex, std::try_to_lock);
  if (!session_lock.owns_lock()) {
    std::lock_guard lock(state_->mutex);
    return state_->phase == State::Phase::Ready &&
        !state_->cancellation_requested && state_->session == session;
  }
  {
    std::lock_guard lock(state_->mutex);
    return state_->phase == State::Phase::Ready &&
        !state_->cancellation_requested && state_->session == session &&
        session->ready();
  }
}

bool PortableHelperElevatedManagerTask::CaptureFrame(
    PortableHelperCapturedFrame* frame, std::string* error) {
  std::shared_ptr<PortableHelperElevatedManagerSession> session;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != State::Phase::Ready || !state_->session) {
      if (error != nullptr) *error = "per-connection helper is not ready";
      return false;
    }
    session = state_->session;
  }
  std::lock_guard session_lock(state_->session_mutex);
  {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != State::Phase::Ready || state_->session != session ||
        state_->cancellation_requested || !session->ready()) {
      if (error != nullptr) *error = "per-connection helper is not ready";
      return false;
    }
  }
  uint64_t request_id = 0;
  if (!next_manager_request_id(state_->next_request_id, &request_id, error))
    return false;
  const bool captured = session->CaptureFrame(request_id, 5000, frame, error);
  if (!captured && !session->ready()) {
    std::lock_guard lock(state_->mutex);
    if (state_->phase == State::Phase::Ready && state_->session == session) {
      state_->phase = State::Phase::Failed;
      state_->reason = error == nullptr || error->empty()
          ? "elevated helper manager stopped during capture"
          : *error;
    }
  }
  return captured;
}

bool PortableHelperElevatedManagerTask::SendInputEvent(
    const chast::InputEvent& event, std::string* error) {
  std::shared_ptr<PortableHelperElevatedManagerSession> session;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != State::Phase::Ready || !state_->session) {
      if (error != nullptr) *error = "per-connection helper is not ready";
      return false;
    }
    session = state_->session;
  }
  std::lock_guard session_lock(state_->session_mutex);
  {
    std::lock_guard lock(state_->mutex);
    if (state_->phase != State::Phase::Ready || state_->session != session ||
        state_->cancellation_requested || !session->ready()) {
      if (error != nullptr) *error = "per-connection helper is not ready";
      return false;
    }
  }
  uint64_t request_id = 0;
  if (!next_manager_request_id(state_->next_request_id, &request_id, error))
    return false;
  const bool sent = session->SendInputEvent(request_id, event, 5000, error);
  if (!sent && !session->ready()) {
    std::lock_guard lock(state_->mutex);
    if (state_->phase == State::Phase::Ready && state_->session == session) {
      state_->phase = State::Phase::Failed;
      state_->reason = error == nullptr || error->empty()
          ? "elevated helper manager stopped during input forwarding"
          : *error;
    }
  }
  return sent;
}

void PortableHelperElevatedManagerTask::stop_session(
    const std::shared_ptr<State>& state) noexcept {
  std::shared_ptr<PortableHelperElevatedManagerSession> session;
  {
    std::lock_guard lock(state->mutex);
    session = state->session;
  }
  if (!session) return;

  std::string stop_error;
  uint64_t request_id = 0;
  bool stopped = false;
  {
    std::lock_guard session_lock(state->session_mutex);
    stopped = next_manager_request_id(state->next_request_id,
                                      &request_id, &stop_error) &&
        session->Stop(request_id, 10000, &stop_error);
  }
  if (stopped) {
    std::lock_guard lock(state->mutex);
    if (state->session == session) state->session.reset();
    return;
  }

  std::lock_guard lock(state->mutex);
  if (!stop_error.empty()) {
    state->reason += "; helper cleanup pending: ";
    state->reason += stop_error;
  }
}

}  // namespace chast::host
