#define NOMINMAX
#include "portable_helper_service.h"

#include <windows.h>
#include <winsvc.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "chast/machine_state_security.h"
#include "portable_helper_lifecycle.h"
#include "portable_helper_recovery.h"
#include "portable_helper_service_channel.h"
#include "portable_helper_service_worker.h"
#include "portable_helper_worker_session.h"

namespace chast::host {
namespace {

std::mutex g_service_mutex;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;
std::wstring g_expected_service_name;
std::atomic<DWORD> g_service_exit_code{ERROR_SUCCESS};

bool set_service_status_locked(DWORD state,
                               DWORD win32_exit_code = ERROR_SUCCESS) {
  if (g_status_handle == nullptr) return false;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = win32_exit_code;
  g_status.dwServiceSpecificExitCode = 0;
  g_status.dwControlsAccepted = state == SERVICE_RUNNING
      ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
      : 0;
  g_status.dwCheckPoint = state == SERVICE_START_PENDING ||
                                  state == SERVICE_STOP_PENDING
                              ? 1
                              : 0;
  g_status.dwWaitHint = state == SERVICE_START_PENDING ||
                                state == SERVICE_STOP_PENDING
                            ? 20000
                            : 0;
  return SetServiceStatus(g_status_handle, &g_status) != FALSE;
}

DWORD WINAPI service_control(DWORD control, DWORD, void*, void*) {
  std::lock_guard lock(g_service_mutex);
  if (control == SERVICE_CONTROL_INTERROGATE) {
    return set_service_status_locked(g_status.dwCurrentState,
                                     g_status.dwWin32ExitCode)
        ? NO_ERROR
        : GetLastError();
  }
  if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN)
    return ERROR_CALL_NOT_IMPLEMENTED;
  if (g_status.dwCurrentState == SERVICE_STOPPED) return NO_ERROR;
  const bool status_updated = set_service_status_locked(SERVICE_STOP_PENDING);
  const DWORD status_error = status_updated ? NO_ERROR : GetLastError();
  if (g_stop_event != nullptr) SetEvent(g_stop_event);
  return status_error;
}

bool identities_match(const PortableHelperProcessIdentity& actual,
                      const PortableHelperProcessIdentity& expected) {
  return actual.process_id == expected.process_id &&
      actual.session_id == expected.session_id &&
      actual.creation_time == expected.creation_time &&
      _wcsicmp(actual.image_path.c_str(), expected.image_path.c_str()) == 0 &&
      actual.user_sid == expected.user_sid &&
      actual.logon_sid == expected.logon_sid;
}

DWORD run_service_main(DWORD count, LPWSTR* arguments) {
  HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_event == nullptr) {
    const DWORD error = GetLastError();
    g_service_exit_code.store(error, std::memory_order_release);
    return error;
  }
  {
    std::lock_guard lock(g_service_mutex);
    g_stop_event = stop_event;
    g_status_handle = RegisterServiceCtrlHandlerExW(
        g_expected_service_name.c_str(), service_control, nullptr);
    if (g_status_handle == nullptr) {
      const DWORD error = GetLastError();
      g_stop_event = nullptr;
      CloseHandle(stop_event);
      g_service_exit_code.store(error, std::memory_order_release);
      return error;
    }
    g_status = {};
    if (!set_service_status_locked(SERVICE_START_PENDING)) {
      const DWORD error = GetLastError();
      g_stop_event = nullptr;
      g_status_handle = nullptr;
      CloseHandle(stop_event);
      g_service_exit_code.store(error, std::memory_order_release);
      return error;
    }
  }

  DWORD exit_code = ERROR_INVALID_DATA;
  HANDLE parent = nullptr;
  PortableHelperServiceSpec spec;
  std::unique_ptr<PortableHelperPipe> control_channel;
  std::string detail;
  try {
    if (count < 1 || arguments == nullptr || arguments[0] == nullptr ||
        _wcsicmp(arguments[0], g_expected_service_name.c_str()) != 0) {
      exit_code = ERROR_INVALID_DATA;
    } else {
      std::vector<std::wstring> start_arguments;
      start_arguments.reserve(count - 1);
      for (DWORD index = 1; index < count; ++index) {
        if (arguments[index] == nullptr) {
          start_arguments.clear();
          break;
        }
        start_arguments.emplace_back(arguments[index]);
      }

      PortableHelperProcessIdentity current;
      if (!current_portable_helper_process_identity(&current, &detail)) {
        exit_code = ERROR_ACCESS_DENIED;
      } else if (current.session_id != 0 || current.user_sid != L"S-1-5-18") {
        exit_code = ERROR_ACCESS_DENIED;
      } else {
        const bool valid_spec = parse_portable_helper_service_start_arguments(
            g_expected_service_name, start_arguments, current.image_path,
            &spec, &detail);
        if (!valid_spec ||
            _wcsicmp(current.image_path.c_str(), spec.executable_path.c_str()) != 0 ||
            !chast::machine_state_file_trusted(spec.executable_path, &detail)) {
          exit_code = ERROR_ACCESS_DENIED;
        } else {
          PortableHelperServiceSpec recorded_spec;
          if (!read_portable_helper_recovery_record(spec, &recorded_spec, &detail) ||
              !identities_match(recorded_spec.parent, spec.parent) ||
              recorded_spec.instance_id != spec.instance_id ||
              _wcsicmp(recorded_spec.executable_path.c_str(),
                       spec.executable_path.c_str()) != 0) {
            exit_code = ERROR_ACCESS_DENIED;
          } else {
            PortableHelperProcessIdentity actual_parent;
            if (!query_portable_helper_process_identity(spec.parent.process_id,
                                                       &actual_parent, &detail) ||
                !identities_match(actual_parent, spec.parent)) {
              exit_code = ERROR_ACCESS_DENIED;
            } else {
              parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, spec.parent.process_id);
              if (parent == nullptr) {
                exit_code = GetLastError();
              } else {
                FILETIME created{}, exited{}, kernel{}, user{};
                const bool same_process =
                    GetProcessId(parent) == spec.parent.process_id &&
                    GetProcessTimes(parent, &created, &exited, &kernel, &user) != FALSE &&
                    ((static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                     created.dwLowDateTime) == spec.parent.creation_time;
                const DWORD parent_state = same_process
                    ? WaitForSingleObject(parent, 0)
                    : WAIT_FAILED;
                if (!same_process) {
                  exit_code = ERROR_ACCESS_DENIED;
                } else if (parent_state == WAIT_OBJECT_0) {
                  exit_code = ERROR_PROCESS_ABORTED;
                } else if (parent_state == WAIT_FAILED) {
                  exit_code = GetLastError();
                } else {
                  control_channel = PortableHelperPipe::CreateServer(
                      spec.instance_id, spec.parent.logon_sid, &detail);
                  if (!control_channel) {
                    exit_code = ERROR_ACCESS_DENIED;
                  } else {
                    std::lock_guard lock(g_service_mutex);
                    if (g_stop_event == nullptr ||
                        WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
                      exit_code = ERROR_CANCELLED;
                    } else if (!set_service_status_locked(SERVICE_RUNNING)) {
                      exit_code = GetLastError();
                    } else {
                      exit_code = ERROR_SUCCESS;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  } catch (...) {
    exit_code = ERROR_NOT_ENOUGH_MEMORY;
  }

  if (exit_code == ERROR_SUCCESS) {
    PortableHelperServiceSessionWorker session_worker(
        spec, {}, [spec](const PortableHelperGrant& grant,
                         std::string* cleanup_error) {
          PortableHelperInputJournalScope scope;
          scope.instance_id = grant.instance_id;
          scope.connection_id = grant.connection_id;
          scope.windows_session_id = grant.windows_session_id;
          scope.generation = grant.generation;
          return execute_portable_helper_authenticated_input_cleanup(
              spec, scope, cleanup_error);
        });
    const bool channel_ok = run_portable_helper_service_control_channel(
        std::move(control_channel), spec.parent, parent, stop_event,
        [stop_event] {
          std::lock_guard lock(g_service_mutex);
          if (g_status.dwCurrentState != SERVICE_STOP_PENDING)
            (void)set_service_status_locked(SERVICE_STOP_PENDING);
          if (g_stop_event != nullptr) SetEvent(stop_event);
        },
        &detail,
        [&session_worker](PortableHelperPipe& pipe,
                          const PortableHelperMessage& request,
                          bool* stop_service, std::string* handler_error) {
          return session_worker.HandleControlRequest(
              pipe, request, stop_service, handler_error);
        });
    std::string worker_cleanup_error;
    const bool workers_cleaned =
        session_worker.Shutdown(&worker_cleanup_error);
    exit_code = channel_ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
    if (!workers_cleaned) {
      if (detail.empty()) detail = worker_cleanup_error;
      exit_code = ERROR_PROCESS_ABORTED;
    }
  } else if (control_channel) {
    control_channel.reset();
  }

  if (parent != nullptr) CloseHandle(parent);
  {
    std::lock_guard lock(g_service_mutex);
    if (g_status.dwCurrentState == SERVICE_RUNNING)
      (void)set_service_status_locked(SERVICE_STOP_PENDING);
    if (!set_service_status_locked(SERVICE_STOPPED, exit_code) &&
        exit_code == ERROR_SUCCESS)
      exit_code = GetLastError();
    g_stop_event = nullptr;
  }
  CloseHandle(stop_event);
  g_service_exit_code.store(exit_code, std::memory_order_release);
  return exit_code;
}

void WINAPI service_main(DWORD count, LPWSTR* arguments) {
  (void)run_service_main(count, arguments);
}

bool valid_dispatch_name(const std::wstring& name) {
  constexpr wchar_t prefix[] = L"ChastPortableHelper-";
  if (name.size() != (sizeof(prefix) / sizeof(prefix[0]) - 1) + 32 ||
      _wcsnicmp(name.c_str(), prefix, sizeof(prefix) / sizeof(prefix[0]) - 1) != 0)
    return false;
  for (size_t index = sizeof(prefix) / sizeof(prefix[0]) - 1;
       index < name.size(); ++index) {
    const wchar_t value = name[index];
    if (!((value >= L'0' && value <= L'9') ||
          (value >= L'a' && value <= L'f') ||
          (value >= L'A' && value <= L'F')))
      return false;
  }
  return true;
}

}  // namespace

int run_portable_helper_service(const std::wstring& service_name) {
  if (!valid_dispatch_name(service_name)) return ERROR_INVALID_NAME;
  g_service_exit_code.store(ERROR_SUCCESS, std::memory_order_release);
  g_expected_service_name = service_name;
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(g_expected_service_name.c_str()), service_main},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    const DWORD error = GetLastError();
    return error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT ? 2
                                                              : static_cast<int>(error);
  }
  return static_cast<int>(g_service_exit_code.load(std::memory_order_acquire));
}

}  // namespace chast::host
