#include "flutter_window.h"
#include "flutter_view_initialization.h"

#include <optional>

namespace {

constexpr UINT kMaxDisplayTopologyRetries = 40;

}  // namespace

#include "flutter/generated_plugin_registrant.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project,
                             OpenSessionWindow open_window,
                             OpenDisplayWindow open_display,
                             std::shared_ptr<chast_viewer::SharedViewerSession> session,
                             std::optional<uint32_t> stream,
                             bool tab_child,
                             bool managed_visibility)
    : project_(project), open_window_(std::move(open_window)),
      open_display_(std::move(open_display)), session_(std::move(session)),
      stream_(stream), tab_child_(tab_child),
      managed_visibility_(managed_visibility) {}

FlutterWindow::~FlutterWindow() {
  // Tearing the engine down from WM_DESTROY crashes flutter_windows.dll when
  // other windows of this process are still alive, so the runner deletes the
  // window after DispatchMessage returns and the engine dies here instead.
  if (viewer_bridge_) {
    viewer_bridge_->Shutdown();
    viewer_bridge_.reset();
  }
  flutter_controller_ = nullptr;
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  {
    ScopedFlutterViewInitialization initialization;
    if (!initialization.ready()) return false;
    flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
        frame.right - frame.left, frame.bottom - frame.top, project_);
  }
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  viewer_bridge_ = std::make_unique<chast_viewer::ViewerBridge>(
      flutter_controller_->engine(), GetHandle(), session_, stream_);
  viewer_bridge_->open_session_window = open_window_;
  viewer_bridge_->open_display_window = open_display_;

  flutter_controller_->engine()->SetNextFrameCallback([this]() {
    if (!tab_child_ && !managed_visibility_) this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  Win32Window::OnDestroy();
}

void FlutterWindow::SetCloseHook(std::function<void(HWND)> hook) {
  close_hook_ = std::move(hook);
}

void FlutterWindow::SetDisplayTopologyHook(std::function<bool(HWND)> hook) {
  display_topology_hook_ = std::move(hook);
}

void FlutterWindow::SetTabVisible(bool visible) {
  if (!tab_child_) return;
  if (viewer_bridge_) viewer_bridge_->SetTabActive(visible);
  SetVisible(visible);
  if (visible) FocusChildContent();
}

void FlutterWindow::SetTabInputActive(bool active) {
  if (viewer_bridge_) viewer_bridge_->SetTabActive(active);
}

const void* FlutterWindow::session_identity() const {
  return viewer_bridge_ ? viewer_bridge_->session_identity() : nullptr;
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  if (message == WM_DISPLAYCHANGE && display_topology_hook_) {
    display_topology_retries_ = 0;
    if (display_topology_timer_ != 0) {
      KillTimer(hwnd, display_topology_timer_);
      display_topology_timer_ = 0;
    }
    bool recovered = false;
    try {
      recovered = display_topology_hook_(hwnd);
    } catch (...) {
      // Keep exceptions from crossing the noexcept window procedure.
    }
    if (!recovered) {
      display_topology_timer_ = SetTimer(hwnd, 0, 250, nullptr);
    }
  } else if (message == WM_TIMER &&
             display_topology_timer_ != 0 &&
             wparam == display_topology_timer_ &&
             display_topology_hook_) {
    bool recovered = false;
    try {
      recovered = display_topology_hook_(hwnd);
    } catch (...) {
      // A later timer tick can retry a transient monitor query failure.
    }
    if (recovered || ++display_topology_retries_ >= kMaxDisplayTopologyRetries) {
      KillTimer(hwnd, display_topology_timer_);
      display_topology_timer_ = 0;
    }
  }
  if (message == WM_CLOSE && close_hook_) {
    close_hook_(hwnd);
  }
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
