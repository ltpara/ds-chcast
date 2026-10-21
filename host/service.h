#pragma once

// Windows service control and the headless worker it launches in the active
// console session. The service itself only supervises: DXGI capture and input
// injection must run in the interactive session.

#include <string>

#include "chast/host_settings.h"

namespace chast::host {

inline constexpr wchar_t kServiceName[] = L"chast-host";
// Named by the service before each launch; the worker waits on it so a stop
// request reaches the session process even though it has no window. The
// Global namespace is required because the service lives in session 0 and the
// worker in the console session.
inline constexpr wchar_t kWorkerStopEvent[] = L"Global\\chast-service-worker-stop";

bool service_installed();
// True while the service is starting, running or stopping: the tray must not
// bind the same ports.
bool service_running();

// Reads %ProgramData%\chast\host.ini, generating and persisting the access
// token, device id and device key when they are missing. Returns false when
// the file cannot be written; `settings` is filled either way. `warning`
// receives non-fatal read problems and may be null.
bool load_machine_settings(HostSettings& settings, std::string* path,
                           std::string* warning, std::string* error);

// Both require an elevated process. install() creates machine settings with a
// fresh token/device identity when they are missing and starts the service;
// uninstall() stops and deletes it but keeps the settings and logs.
int install_service(std::string* error);
int uninstall_service(std::string* error);

// Blocking SCM entry point, used by "chast-service.exe --run".
int run_service();
// Blocking worker loop, used by the service as "chast-service.exe --worker".
int run_worker(std::string* error);

}  // namespace chast::host
