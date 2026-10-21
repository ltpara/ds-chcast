#pragma once

#include <windows.h>

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>

#include <memory>
#include <string>

#include "viewer_engine.h"

namespace flutter {
class FlutterEngine;
}

namespace chast_viewer {

class GpuFrameSink;

// Owns the viewer session and exposes it to Dart over a MethodChannel. One
// instance lives for the lifetime of the runner window. Dart registers the
// display texture with flutter_gpu_texture_renderer and passes the plugin's
// output pointer in with the start call; the sink borrows it until the session
// thread has stopped.
class ViewerBridge {
public:
    ViewerBridge(flutter::FlutterEngine* engine, HWND window);
    ~ViewerBridge();

    ViewerBridge(const ViewerBridge&) = delete;
    ViewerBridge& operator=(const ViewerBridge&) = delete;

    // Stops the session, detaches the plugin output and detaches the channel.
    // Must run on the platform thread before the Flutter controller is
    // destroyed.
    void Shutdown();

private:
    void HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetFullscreen(bool enable);

    flutter::FlutterEngine* engine_ = nullptr;
    HWND window_ = nullptr;
    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
    std::unique_ptr<GpuFrameSink> texture_sink_;
    std::unique_ptr<ViewerEngine> viewer_;
    bool fullscreen_ = false;
    LONG_PTR saved_style_ = 0;
    WINDOWPLACEMENT saved_placement_{};
    bool shutting_down_ = false;
};

}  // namespace chast_viewer
