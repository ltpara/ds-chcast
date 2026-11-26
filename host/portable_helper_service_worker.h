#pragma once

#define NOMINMAX
#include <windows.h>

#include "portable_helper_service_channel.h"
#include "portable_helper_supervisor.h"
#include "portable_helper_worker_session.h"

#include <functional>
#include <memory>
#include <string>

namespace chast::host {

// Owns the one session worker authorized for a temporary helper service
// instance. The parent control pipe is already authenticated to the exact
// interactive host process. A service instance is single-use: the host must
// create it only after local consent/UAC for the connection, and this
// controller accepts at most one Start request before the instance is cleaned
// up. Desktop operations remain scoped by the worker grant and are rechecked
// here before forwarding.
class PortableHelperServiceSessionWorker final {
 public:
  using Launcher = PortableHelperSessionWorkerLauncher;
  using EmergencyInputRelease = std::function<bool(
      const PortableHelperGrant&, std::string*)>;

  PortableHelperServiceSessionWorker(
      PortableHelperServiceSpec spec, Launcher launcher = {},
      EmergencyInputRelease emergency_input_release = {});
  ~PortableHelperServiceSessionWorker();
  PortableHelperServiceSessionWorker(
      const PortableHelperServiceSessionWorker&) = delete;
  PortableHelperServiceSessionWorker& operator=(
      const PortableHelperServiceSessionWorker&) = delete;

  bool HandleControlRequest(
      PortableHelperPipe& control_channel,
      const PortableHelperMessage& request, bool* stop_service,
      std::string* error);
  bool Shutdown(std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace chast::host
