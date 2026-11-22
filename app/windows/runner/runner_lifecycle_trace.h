#ifndef RUNNER_LIFECYCLE_TRACE_H_
#define RUNNER_LIFECYCLE_TRACE_H_

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

inline void TraceRunnerLifecycle(const char* event, HWND window, UINT message,
                                 intptr_t detail = 0) {
  char* testing = nullptr;
  size_t testing_size = 0;
  char* trace_enabled = nullptr;
  size_t trace_enabled_size = 0;
  char* path = nullptr;
  size_t path_size = 0;
  const bool configured =
      _dupenv_s(&testing, &testing_size, "CHAST_NATIVE_TESTING") == 0 &&
      testing != nullptr && std::string(testing) == "1" &&
      _dupenv_s(&trace_enabled, &trace_enabled_size,
                "CHAST_NATIVE_TEST_LIFECYCLE_TRACE") == 0 &&
      trace_enabled != nullptr && std::string(trace_enabled) == "1" &&
      _dupenv_s(&path, &path_size, "CHAST_NATIVE_TEST_TABS_TRACE") == 0 &&
      path != nullptr && *path != '\0';
  if (configured) {
    std::ofstream output(path, std::ios::app);
    if (output) {
      output << "runner\t" << event << '\t' << GetCurrentProcessId() << '\t'
             << GetCurrentThreadId() << '\t'
             << reinterpret_cast<uintptr_t>(window) << '\t' << message << '\t'
             << detail << '\t'
             << reinterpret_cast<uintptr_t>(
                    window != nullptr && IsWindow(window) ? GetParent(window)
                                                          : nullptr)
             << '\t'
             << (window != nullptr && IsWindow(window) ? 1 : 0) << '\t'
             << (window != nullptr && IsWindowVisible(window) ? 1 : 0) << '\n';
    }
  }
  std::free(testing);
  std::free(trace_enabled);
  std::free(path);
}

#endif  // RUNNER_LIFECYCLE_TRACE_H_
