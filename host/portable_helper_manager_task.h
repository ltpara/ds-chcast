#pragma once

#include "chast/elevation.h"
#include "portable_helper_manager_channel.h"

#include <memory>
#include <mutex>
#include <string>

namespace chast::host {

// Owns the asynchronous elevated-manager startup for one viewer approval.
// Cancellation is non-blocking while runas/UAC is pending; the background
// transaction revalidates before grant delivery and stops a manager that wins
// a race with cancellation.
class PortableHelperElevatedManagerTask final : public chast::IElevationTask {
 public:
  static std::shared_ptr<PortableHelperElevatedManagerTask> Start(
      PortableHelperLocalApprovalReceipt&& approval, uint32_t timeout_ms,
      PortableHelperElevatedManagerSession::Revalidate revalidate,
      std::string* error, PortableHelperElevatedManagerSession::Launcher launcher = {});
  ~PortableHelperElevatedManagerTask() override;

  PortableHelperElevatedManagerTask(const PortableHelperElevatedManagerTask&) =
      delete;
  PortableHelperElevatedManagerTask& operator=(
      const PortableHelperElevatedManagerTask&) = delete;

  chast::ElevationState poll() override;
  void commit() override;
  void cancel() override;

  bool ready() const;
  bool CaptureFrame(PortableHelperCapturedFrame* frame, std::string* error);
  bool SendInputEvent(const chast::InputEvent& event, std::string* error);

 private:
  struct State;
  explicit PortableHelperElevatedManagerTask(std::shared_ptr<State> state);
  static void stop_session(const std::shared_ptr<State>& state) noexcept;

  std::shared_ptr<State> state_;
};

}  // namespace chast::host
