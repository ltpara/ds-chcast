#pragma once

#include <windows.h>
#include <imm.h>

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>

#include <atomic>
#include <memory>
#include <string>

#include "remote_cursor.h"
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
    void SendInput(const flutter::EncodableMap& args,
                   std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetFullscreen(bool enable);
    // Detaches the view's IME context while remote control owns the keyboard,
    // so a local IME neither composes locally nor turns keys into
    // VK_PROCESSKEY. Restored when control is dropped, which keeps the home
    // page's text fields working in Chinese.
    void SetKeyCapture(bool enable);
    // Picks the host's newest pointer up and installs it as the local system
    // cursor while the page shows remote control on this window. Called from
    // the poll timer, so it runs on the platform thread.
    void ApplyCursor(const flutter::EncodableMap& args);
    void ClearCursor();

    // Swaps the physical key back in for the VK_PROCESSKEY an IME reports when
    // the context could not be detached. Installed on the Flutter view window,
    // and only active while the page has remote control.
    static LRESULT CALLBACK ViewSubclassProc(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data);

    flutter::FlutterEngine* engine_ = nullptr;
    HWND window_ = nullptr;
    HWND view_ = nullptr;
    std::atomic<bool> key_capture_{false};
    HIMC saved_himc_ = nullptr;
    // The host pointer, installed as the local cursor while this window is
    // the one under it. Windows asks WM_SETCURSOR for HTCLIENT, so answering
    // that message is all it takes; Flutter's own cursor is left alone.
    RemoteCursor remote_cursor_;
    chast::CursorUpdate last_cursor_;
    bool have_cursor_ = false;
    bool cursor_override_ = false;
    uint64_t cursor_shape_ = 0;
    double cursor_scale_ = 1.0;
    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
    std::unique_ptr<GpuFrameSink> texture_sink_;
    std::unique_ptr<ViewerEngine> viewer_;
    bool fullscreen_ = false;
    LONG_PTR saved_style_ = 0;
    WINDOWPLACEMENT saved_placement_{};
    bool shutting_down_ = false;
};

}  // namespace chast_viewer
