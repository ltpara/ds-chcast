#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "flutter_window.h"
#include "utils.h"

namespace {

// Keep these in sync with the Dart-side request encoder.
constexpr size_t kMaxSessionWindows = 16;
constexpr size_t kMaxSessionRequestBytes = 1024 * 1024;

}  // namespace

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

  // Session windows receive their connection request through an in-process
  // Dart entrypoint argument, so credentials never reach the process command
  // line or a temporary file.
  std::function<bool(const std::string&)> open_window;
  FlutterWindow::OpenDisplayWindow open_display;
  open_display = [&windows](std::shared_ptr<chast_viewer::SharedViewerSession> session, uint32_t stream,
                            const std::string& peer_id) {
    if (windows.size() >= kMaxSessionWindows) return false;
    flutter::DartProject display_project(L"data");
    display_project.set_dart_entrypoint_arguments(
        {"--display-window", std::to_string(stream), "--peer-id", peer_id});
    auto child = std::make_unique<FlutterWindow>(display_project,
        std::function<bool(const std::string&)>{}, FlutterWindow::OpenDisplayWindow{}, std::move(session), stream);
    const unsigned int offset = static_cast<unsigned int>(windows.size() % 8) * 28;
    if (!child->Create(L"Chast - Display", Win32Window::Point(100 + offset, 80 + offset),
                       Win32Window::Size(1120, 720))) return false;
    child->SetQuitOnClose(false);
    windows.push_back(std::move(child));
    return true;
  };
  open_window = [&windows, &open_window, &open_display](const std::string& request) {
    if (windows.size() >= kMaxSessionWindows ||
        request.size() > kMaxSessionRequestBytes) {
      return false;
    }
    flutter::DartProject session_project(L"data");
    session_project.set_dart_entrypoint_arguments({"--session-window", request});
    auto window = std::make_unique<FlutterWindow>(session_project, open_window, open_display);
    const unsigned int offset =
        static_cast<unsigned int>(windows.size() % 8) * 28;
    Win32Window::Point origin(100 + offset, 80 + offset);
    Win32Window::Size size(1120, 720);
    if (!window->Create(L"Chast", origin, size)) {
      return false;
    }
    window->SetQuitOnClose(false);
    windows.push_back(std::move(window));
    return true;
  };

  auto window = std::make_unique<FlutterWindow>(project, open_window, open_display);
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
    // The first engine owns the process-wide Dart VM. Tearing it down while
    // other windows keep dispatching messages crashes the next Dart callback
    // (access violation in dart::DartEntry::InvokeFunction), so keep the home
    // window object alive until the loop ends and only drop the other windows
    // once their HWNDs are gone.
    if (windows.size() > 1) {
      windows.erase(
          std::remove_if(windows.begin() + 1, windows.end(),
                         [](const std::unique_ptr<FlutterWindow>& window) {
                           return window->GetHandle() == nullptr;
                         }),
          windows.end());
    }
    const bool any_live = std::any_of(
        windows.begin(), windows.end(), [](const std::unique_ptr<FlutterWindow>& window) {
          return window->GetHandle() != nullptr;
        });
    if (!any_live) {
      break;
    }
  }

  // Destroy child engines before the VM-owning home engine.
  for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
    it->reset();
  }
  windows.clear();

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
