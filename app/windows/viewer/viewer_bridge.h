#pragma once

#include <windows.h>
#include <imm.h>

#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "remote_cursor.h"
#include "keyboard_capture.h"
#include "local_text_input.h"
#include "viewer_engine.h"
#include "shared_viewer_session.h"

namespace flutter {
class FlutterEngine;
}

namespace chast_viewer {

class GpuFrameSink;

// Exposes a shared viewer session to Dart over a MethodChannel. The parent
// window owns its lifetime; display windows borrow it. Dart registers the
// display texture with flutter_gpu_texture_renderer and passes the plugin's
// output pointer in with the start call; the sink borrows it until the session
// thread has stopped.
class ViewerBridge {
public:
    ViewerBridge(flutter::FlutterEngine* engine, HWND window,
                 std::shared_ptr<SharedViewerSession> session = {},
                 std::optional<uint32_t> display_stream = {});
    ~ViewerBridge();

    ViewerBridge(const ViewerBridge&) = delete;
    ViewerBridge& operator=(const ViewerBridge&) = delete;

    // Stops the session, detaches the plugin output and detaches the channel.
    // Must run on the platform thread before the Flutter controller is
    // destroyed.
    void Shutdown();

    // Set by the runner: starts another engine window for an encoded
    // SessionWindowRequest. False means the caller should fall back to an
    // in-process page.
    std::function<bool(const std::string&)> open_session_window;
    // peer_id and the host display index identify the window's saved layout.
    std::function<bool(std::shared_ptr<SharedViewerSession>, uint32_t, const std::string&, uint32_t)> open_display_window;

private:
    bool QueueInput(const chast::InputEvent& event);
    bool ReleaseInput();
    bool SetKeyboardMode(const std::string& mode);
    bool PostLocalKey(const chast::InputEvent& event, bool release = false);
    bool QueueLocalKey(const KBDLLHOOKSTRUCT& key, bool down,
                       const chast::InputEvent& event, bool consumed);
    bool ObserveLocalKey(UINT message, WPARAM wparam, LPARAM lparam);
    void DrainLocalKey(uintptr_t ticket);
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
    // Pins the local pointer to the view and turns mouse motion into relative
    // moves for the host, so a drag never reaches a screen edge. [scale] is
    // physical window pixels per captured pixel on the host.
    void SetRelativeMouse(bool enable, double scale);
    // Confines the pointer to the view's client rect while relative mode is on.
    void ClipRelativePointer();
    // Picks the host's newest pointer up and installs it as the local system
    // cursor while the page shows remote control on this window. Called from
    // the poll timer, so it runs on the platform thread.
    void ApplyCursor(const flutter::EncodableMap& args);
    // Empties the session's cursor queue into the bridge's cache: the newest
    // state for the control path, and the newest sprite for the view-only
    // overlay. Both callers run on the platform thread.
    void DrainCursorUpdates();
    void ClearCursor();
    // Loads the device key once from %APPDATA%\chast, so hosts that trust this
    // viewer can skip the interactive challenges on later connections.
    void EnsureIdentity();

    // Swaps the physical key back in for the VK_PROCESSKEY an IME reports when
    // the context could not be detached. Installed on the Flutter view window,
    // and only active while the page has remote control.
    static LRESULT CALLBACK ViewSubclassProc(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data);

    // Runs a Windows Hello ceremony on a worker thread and answers the method
    // result on the platform thread through the message posted by
    // WindowSubclassProc.
    void StartSecurityKey(const flutter::EncodableMap& args, bool make_credential,
                          std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    // Installed on the runner window to deliver security-key results posted by
    // the worker thread.
    static LRESULT CALLBACK WindowSubclassProc(HWND window, UINT message, WPARAM wparam,
                                               LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data);

    flutter::FlutterEngine* engine_ = nullptr;
    HWND window_ = nullptr;
    HWND view_ = nullptr;
    std::atomic<bool> key_capture_{false};
    std::unique_ptr<KeyboardCapture> keyboard_capture_;
    std::unique_ptr<LocalTextInput> local_text_input_;
    std::atomic<bool> local_keyboard_{false};
    std::atomic<bool> legacy_keyboard_{false};
    struct LocalKey {
        chast::InputEvent event;
        bool release = false;
        uint64_t generation = 0;
        uintptr_t ticket = 0;
        uint16_t scan = 0;
        uint16_t usage = 0;
        uint32_t virtual_key = 0;
        bool down = false;
        bool native = false;
        bool received = false;
        bool completed = true;
    };
    std::mutex local_keys_mutex_;
    std::deque<LocalKey> local_keys_;
    uintptr_t next_local_ticket_ = 0;
    std::array<bool, 256> local_ime_keys_{};
    std::atomic<bool> relative_mouse_{false};
    // Physical pixels per captured pixel. Relative moves use the native
    // keyboard snapshot; the last Dart bitmap is the hook-failure fallback.
    double relative_scale_ = 1.0;
    uint16_t last_modifiers_ = 0;
    HIMC saved_himc_ = nullptr;
    // The host pointer, installed as the local cursor while this window is
    // the one under it. Windows asks WM_SETCURSOR for HTCLIENT, so answering
    // that message is all it takes; Flutter's own cursor is left alone.
    RemoteCursor remote_cursor_;
    chast::CursorUpdate last_cursor_;
    bool have_cursor_ = false;
    // The newest sprite, kept whole for the view-only overlay. Its version
    // rises with each new sprite so the page copies it once per shape.
    std::vector<uint8_t> cursor_sprite_;
    uint64_t cursor_sprite_shape_ = 0;
    uint64_t cursor_sprite_version_ = 0;
    uint64_t cursor_sprite_sent_ = 0;
    bool cursor_override_ = false;
    uint64_t cursor_shape_ = 0;
    double cursor_scale_ = 1.0;
    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
    std::shared_ptr<SharedViewerSession> session_;
    std::optional<uint32_t> display_stream_;
    std::atomic<uint64_t> session_generation_{0};
    std::map<uint32_t, uint64_t> cursor_revisions_;
    GpuFrameSink* texture_sink_ = nullptr;
    // The shared camera gets its own Flutter texture and sink, so both
    // pictures can render at once.
    GpuFrameSink* camera_sink_ = nullptr;
    ViewerEngine* viewer_ = nullptr;
    std::shared_ptr<chast::DeviceIdentity> identity_;
    bool identity_loaded_ = false;
    // One Windows Hello prompt at a time: webauthn.dll would otherwise stack
    // verification windows from concurrent Dart calls. Shared with the worker
    // so a prompt that outlives the bridge still clears the flag.
    std::shared_ptr<std::atomic<bool>> security_key_busy_ =
        std::make_shared<std::atomic<bool>>(false);
    bool fullscreen_ = false;
    LONG_PTR saved_style_ = 0;
    WINDOWPLACEMENT saved_placement_{};
    bool shutting_down_ = false;
    // With a page-installed close handler the window's X asks Dart to shut the
    // session down first; the second WM_CLOSE arrives after that and closes
    // for real.
    bool window_close_handler_ = false;
    bool window_close_requested_ = false;
};

}  // namespace chast_viewer
