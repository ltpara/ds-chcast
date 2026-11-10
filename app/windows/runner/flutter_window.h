#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>

#include <functional>
#include <memory>

#include "viewer_bridge.h"
#include "win32_window.h"

// A window that does nothing but host a Flutter view.
class FlutterWindow : public Win32Window {
 public:
  using OpenDisplayWindow = std::function<bool(std::shared_ptr<chast_viewer::SharedViewerSession>, uint32_t, const std::string&, uint32_t)>;
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject& project,
                         std::function<bool(const std::string&)> open_window = {},
                         OpenDisplayWindow open_display = {},
                         std::shared_ptr<chast_viewer::SharedViewerSession> session = {},
                         std::optional<uint32_t> stream = {});
  virtual ~FlutterWindow();

  // Runs once when the user (or Dart) closes this window, before the HWND is
  // destroyed, so the runner can still read its final bounds.
  void SetCloseHook(std::function<void(HWND)> hook);

 protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

 private:
  // The project to run.
  flutter::DartProject project_;

  // Runner hook that starts another session window.
  std::function<bool(const std::string&)> open_window_;
  OpenDisplayWindow open_display_;
  std::shared_ptr<chast_viewer::SharedViewerSession> session_;
  std::optional<uint32_t> stream_;
  std::function<void(HWND)> close_hook_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  // QUIC viewer session and its GPU display bridge.
  std::unique_ptr<chast_viewer::ViewerBridge> viewer_bridge_;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
