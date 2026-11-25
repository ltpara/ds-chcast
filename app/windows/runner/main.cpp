#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "display_layout.h"
#include "flutter_window.h"
#include "runner_lifecycle_trace.h"
#include "session_tab_host.h"
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
  std::unique_ptr<SessionTabHostWindow> session_tabs;
  auto tab_visibility = std::make_shared<SessionTabVisibility>();

  // Session windows receive their connection request through an in-process
  // Dart entrypoint argument, so credentials never reach the process command
  // line or a temporary file.
  SessionTabHostWindow::OpenSessionWindow open_window;
  FlutterWindow::OpenDisplayWindow open_display;
  open_display = [&windows, &session_tabs, tab_visibility](std::shared_ptr<chast_viewer::SharedViewerSession> session, uint32_t stream,
                            const std::string& peer_id,
                            const std::string& recording_upload_context,
                            uint32_t display_index,
                            RECT remote_display, RECT remote_desktop,
                            uint32_t remote_display_count) {
    if (windows.size() + (session_tabs ? session_tabs->session_count() : 0) >=
        kMaxSessionWindows + 1) return false;
    const void* session_identity = session.get();
    const bool tab_managed = session_tabs && session_tabs->OwnsSession(session_identity);
    flutter::DartProject display_project(L"data");
    std::vector<std::string> display_arguments{
        "--display-window", std::to_string(stream), "--peer-id", peer_id};
    if (!recording_upload_context.empty()) {
      display_arguments.push_back("--recording-upload");
      display_arguments.push_back(recording_upload_context);
    }
    display_project.set_dart_entrypoint_arguments(std::move(display_arguments));
    auto child = std::make_unique<FlutterWindow>(display_project,
        SessionTabHostWindow::OpenSessionWindow{}, FlutterWindow::OpenDisplayWindow{},
        std::move(session), stream, false, tab_managed);
    const unsigned int offset = static_cast<unsigned int>(windows.size() % 8) * 28;
    const std::filesystem::path layout = chast::DisplayLayoutPath();
    const std::optional<RECT> saved =
        chast::LoadDisplayBounds(layout, peer_id, display_index);
    const std::optional<RECT> automatic = saved
        ? std::nullopt
        : chast::AutoDisplayBounds(remote_display, remote_desktop,
                                   remote_display_count);
    const std::optional<RECT> placement = saved ? saved : automatic;
    child->SetDisplayTopologyHook(
        [remote_display, remote_desktop, remote_display_count](HWND window) {
          RECT current{};
          if (!GetWindowRect(window, &current)) return false;
          if (chast::DisplayBoundsHaveUsableVisibility(current)) return true;
          const auto recovered = chast::AutoDisplayBounds(
              remote_display, remote_desktop, remote_display_count);
          if (!recovered) return false;
          return SetWindowPos(window, nullptr, recovered->left, recovered->top,
                              recovered->right - recovered->left,
                              recovered->bottom - recovered->top,
                              SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        });
    const bool created = placement
        ? child->CreateAtPhysicalBounds(L"Chast - Display", *placement)
        : child->Create(L"Chast - Display",
                        Win32Window::Point(100 + offset, 80 + offset),
                        Win32Window::Size(1120, 720));
    if (!created) return false;
    const std::weak_ptr<SessionTabVisibility> weak_visibility = tab_visibility;
    child->SetCloseHook([layout, peer_id, display_index, session_identity,
                         weak_visibility](HWND window) {
      RECT bounds{};
      if (GetWindowRect(window, &bounds)) {
        chast::SaveDisplayBounds(layout, peer_id, display_index, bounds);
      }
      if (const auto visibility = weak_visibility.lock())
        visibility->Remove(session_identity, window);
    });
    child->SetQuitOnClose(false);
    if (tab_managed)
      tab_visibility->Register(session_identity, child->GetHandle());
    windows.push_back(std::move(child));
    return true;
  };
  open_window = [&windows, &session_tabs, &tab_visibility, &open_window,
                 &open_display](const std::string& request,
                                const std::string& label,
                                const std::string& device_hint) {
    if (windows.size() + (session_tabs ? session_tabs->session_count() : 0) >=
            kMaxSessionWindows + 1 ||
        request.size() > kMaxSessionRequestBytes) {
      return false;
    }
    if (session_tabs && session_tabs->GetHandle() == nullptr) {
      session_tabs->PruneClosed();
      session_tabs.reset();
    }
    if (!session_tabs) {
      auto host = std::make_unique<SessionTabHostWindow>(
          [&windows] {
            if (!windows.empty() && windows.front()->GetHandle()) {
              windows.front()->Show();
              SetForegroundWindow(windows.front()->GetHandle());
            }
          }, tab_visibility);
      if (!host->Create(L"Chast", Win32Window::Point(100, 80),
                        Win32Window::Size(1200, 780)))
        return false;
      host->SetQuitOnClose(false);
      session_tabs = std::move(host);
    }
    const bool opened = session_tabs->CreateSession(
        request, label, device_hint, open_window, open_display);
    if (!opened && session_tabs->session_count() == 0)
      PostMessageW(session_tabs->GetHandle(), WM_CLOSE, 0, 0);
    return opened;
  };

  auto window = std::make_unique<FlutterWindow>(project, open_window, open_display);
  Win32Window::Point origin(80, 60);
  Win32Window::Size size(1120, 720);
  if (!window->Create(L"Chast", origin, size)) {
    return EXIT_FAILURE;
  }
  window->SetQuitOnClose(false);
  windows.push_back(std::move(window));

  ::MSG msg{};
  BOOL message_result = 0;
  while ((message_result = ::GetMessage(&msg, nullptr, 0, 0)) > 0) {
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
    if (session_tabs) {
      const size_t tabs_before_prune = session_tabs->session_count();
      session_tabs->PruneClosed();
      const size_t tabs_after_prune = session_tabs->session_count();
      if (tabs_before_prune > 0 && tabs_after_prune == 0) {
        TraceRunnerLifecycle("runner-empty-tabs", session_tabs->GetHandle(), 0,
                             static_cast<intptr_t>(tabs_after_prune));
      }
      if (session_tabs->GetHandle() == nullptr) {
        TraceRunnerLifecycle("runner-tab-host-destroyed", nullptr,
                             WM_NCDESTROY,
                             static_cast<intptr_t>(tabs_after_prune));
        session_tabs.reset();
      }
    }
    const bool any_live = std::any_of(
        windows.begin(), windows.end(), [](const std::unique_ptr<FlutterWindow>& window) {
          return window->GetHandle() != nullptr;
        }) || (session_tabs && session_tabs->GetHandle() != nullptr);
    if (!any_live) {
      TraceRunnerLifecycle("runner-window-set-empty", nullptr, 0,
                           static_cast<intptr_t>(windows.size()));
      break;
    }
  }
  if (message_result == 0) {
    TraceRunnerLifecycle("main-loop-quit", msg.hwnd, msg.message,
                         static_cast<intptr_t>(msg.wParam));
  } else if (message_result == -1) {
    TraceRunnerLifecycle("main-loop-error", nullptr, 0,
                         static_cast<intptr_t>(GetLastError()));
  }
  const int exit_code = message_result == -1 ? EXIT_FAILURE : EXIT_SUCCESS;

  // Destroy child engines before the VM-owning home engine.
  session_tabs.reset();
  for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
    it->reset();
  }
  windows.clear();

  ::CoUninitialize();
  return exit_code;
}
