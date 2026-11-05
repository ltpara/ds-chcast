#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  std::vector<std::unique_ptr<FlutterWindow>> windows;
  std::function<bool(const std::string&)> open_window;
  open_window = [&](const std::string& request) {
    if (windows.size() >= 16 || request.size() > 1024 * 1024) return false;
    flutter::DartProject session_project(L"data");
    session_project.set_dart_entrypoint_arguments({"--session-window", request});
    auto next = std::make_unique<FlutterWindow>(session_project, open_window);
    const unsigned int offset = static_cast<unsigned int>(windows.size() % 8) * 28;
    if (!next->Create(L"Chast", {100 + offset, 80 + offset}, {1120, 720})) return false;
    next->SetQuitOnClose(false);
    windows.push_back(std::move(next));
    return true;
  };
  auto window = std::make_unique<FlutterWindow>(project, open_window);
  Win32Window::Point origin(80, 60);
  Win32Window::Size size(1120, 720);
  if (!window->Create(L"Chast", origin, size)) {
    return EXIT_FAILURE;
  }
  window->SetQuitOnClose(false);
  windows.push_back(std::move(window));

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
    windows.erase(std::remove_if(windows.begin(), windows.end(),
        [](const auto& item) { return item->GetHandle() == nullptr; }), windows.end());
    if (windows.empty()) break;
  }

  windows.clear();
  ::CoUninitialize();
  return EXIT_SUCCESS;
}
