#pragma once

#include <string>

namespace chast::host {

// Runs the dedicated SCM entry point embedded in the host image. The helper
// path exits through this function before tray, settings, network, or UI setup.
int run_portable_helper_service(const std::wstring& service_name);

}  // namespace chast::host
