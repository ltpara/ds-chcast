#include "flutter_window.h"

#include <optional>

#include "flutter/generated_plugin_registrant.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project,
                             std::function<bool(const std::string&)> open_window,
                             OpenDisplayWindow open_display,
                             std::shared_ptr<chast_viewer::SharedViewerSession> session,
                             std::optional<uint32_t> stream)
    : project_(project), open_window_(std::move(open_window)),
      open_display_(std::move(open_display)), session_(std::move(session)), stream_(stream) {}

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
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
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

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
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

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
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
