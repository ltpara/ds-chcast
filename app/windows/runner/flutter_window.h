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
  using OpenDisplayWindow = std::function<bool(
      std::shared_ptr<chast_viewer::SharedViewerSession>, uint32_t,
      const std::string&, const std::string&, uint32_t, RECT, RECT, uint32_t)>;
  using OpenSessionWindow =
      std::function<bool(const std::string&, const std::string&)>;
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject& project,
                         OpenSessionWindow open_window = {},
                         OpenDisplayWindow open_display = {},
                         std::shared_ptr<chast_viewer::SharedViewerSession> session = {},
                         std::optional<uint32_t> stream = {},
                         bool tab_child = false,
                         bool managed_visibility = false);
  virtual ~FlutterWindow();

  // Runs once when the user (or Dart) closes this window, before the HWND is
  // destroyed, so the runner can still read its final bounds.
  void SetCloseHook(std::function<void(HWND)> hook);
  void SetDisplayTopologyHook(std::function<bool(HWND)> hook);
  void SetTabVisible(bool visible);
  void SetTabInputActive(bool active);
  const void* session_identity() const;

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
  OpenSessionWindow open_window_;
  OpenDisplayWindow open_display_;
  std::shared_ptr<chast_viewer::SharedViewerSession> session_;
  std::optional<uint32_t> stream_;
  bool tab_child_ = false;
  bool managed_visibility_ = false;
  std::function<void(HWND)> close_hook_;
  std::function<bool(HWND)> display_topology_hook_;
  UINT display_topology_retries_ = 0;
  UINT_PTR display_topology_timer_ = 0;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  // QUIC viewer session and its GPU display bridge.
  std::unique_ptr<chast_viewer::ViewerBridge> viewer_bridge_;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
