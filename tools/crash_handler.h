// Crash diagnostics for the chast tools.
//
// The capture tools drive DXGI and D3D11 directly, where a bad call shows up
// as an access violation inside a driver rather than as an HRESULT. This
// installs a filter that prints the faulting address and a symbolized stack so
// the failing stage is identifiable without attaching a debugger.

#pragma once

namespace chast_tools {

// Installs the unhandled-exception filter. Call once at startup.
void install_crash_handler();

}  // namespace chast_tools
