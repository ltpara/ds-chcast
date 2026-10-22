#include "viewer_bridge.h"

#include <commctrl.h>
#include <imm.h>

#include <flutter/flutter_engine.h>
#include <flutter/standard_method_codec.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "chast/identity.h"
#include "gpu_frame_sink.h"

namespace chast_viewer {

namespace {

constexpr UINT_PTR kViewSubclassId = 1;

const char* state_name(ViewerState state) {
    switch (state) {
        case ViewerState::kIdle:
            return "idle";
        case ViewerState::kConnecting:
            return "connecting";
        case ViewerState::kHandshaking:
            return "handshaking";
        case ViewerState::kStreaming:
            return "streaming";
        case ViewerState::kEnded:
            return "ended";
        case ViewerState::kFailed:
            return "failed";
    }
    return "idle";
}

flutter::EncodableValue key(const char* name) {
    return flutter::EncodableValue(std::string(name));
}

bool map_string(const flutter::EncodableMap& map, const char* name, std::string* out) {
    const auto it = map.find(key(name));
    if (it == map.end()) {
        return false;
    }
    const auto* value = std::get_if<std::string>(&it->second);
    if (value == nullptr) {
        return false;
    }
    *out = *value;
    return true;
}

bool map_int(const flutter::EncodableMap& map, const char* name, int64_t* out) {
    const auto it = map.find(key(name));
    if (it == map.end()) {
        return false;
    }
    if (const auto* value32 = std::get_if<int32_t>(&it->second)) {
        *out = *value32;
        return true;
    }
    if (const auto* value64 = std::get_if<int64_t>(&it->second)) {
        *out = *value64;
        return true;
    }
    return false;
}

bool map_bool(const flutter::EncodableMap& map, const char* name, bool* out) {
    const auto it = map.find(key(name));
    if (it == map.end()) {
        return false;
    }
    const auto* value = std::get_if<bool>(&it->second);
    if (value == nullptr) {
        return false;
    }
    *out = *value;
    return true;
}

}  // namespace

ViewerBridge::ViewerBridge(flutter::FlutterEngine* engine, HWND window)
    : engine_(engine), window_(window) {
    texture_sink_ = std::make_unique<GpuFrameSink>();
    viewer_ = std::make_unique<ViewerEngine>(texture_sink_.get());
    channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        engine_->messenger(), "chast/viewer", &flutter::StandardMethodCodec::GetInstance());
    channel_->SetMethodCallHandler(
        [this](const flutter::MethodCall<flutter::EncodableValue>& call,
               std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
            HandleMethodCall(call, std::move(result));
        });
    if (!texture_sink_->ready()) {
        std::fprintf(stderr, "chast_viewer: GPU display is not available: %s\n",
                     texture_sink_->error().c_str());
    }
    view_ = FindWindowExW(window_, nullptr, L"FLUTTERVIEW", nullptr);
    if (view_ != nullptr) {
        SetWindowSubclass(view_, &ViewerBridge::ViewSubclassProc, kViewSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
    }
}

ViewerBridge::~ViewerBridge() {
    Shutdown();
}

void ViewerBridge::Shutdown() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    SetKeyCapture(false);
    ClearCursor();
    if (view_ != nullptr) {
        if (IsWindow(view_)) {
            RemoveWindowSubclass(view_, &ViewerBridge::ViewSubclassProc, kViewSubclassId);
        }
        view_ = nullptr;
    }
    if (channel_) {
        channel_->SetMethodCallHandler(nullptr);
    }
    if (viewer_) {
        viewer_->Stop();
    }
    if (texture_sink_) {
        texture_sink_->SetOutput(nullptr);
    }
    viewer_.reset();
    texture_sink_.reset();
    channel_.reset();
}

void ViewerBridge::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    const std::string& method = call.method_name();

    if (method == "start") {
        if (texture_sink_ == nullptr || !texture_sink_->ready()) {
            result->Error("no_gpu", texture_sink_ != nullptr
                                        ? texture_sink_->error()
                                        : "the GPU frame sink is not available");
            return;
        }
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        std::string host;
        int64_t port = 0;
        int64_t output = 0;
        int64_t display = chast::kDisplayAuto;
        std::string fingerprint;
        std::string access_token;
        bool allow_unverified = false;
        if (args == nullptr || !map_string(*args, "host", &host) ||
            !map_int(*args, "port", &port) || !map_int(*args, "gpuOutput", &output) ||
            host.empty() || port <= 0 || port > 65535 || output <= 0) {
            result->Error("bad_args",
                          "start wants a host, a port in 1..65535 and a GPU texture output");
            return;
        }
        if (map_int(*args, "display", &display) && display != chast::kDisplayAuto &&
            (display < 0 || display >= chast::kMaxDisplayCount)) {
            result->Error("bad_args", "display must be an index in the announced list or auto");
            return;
        }
        map_bool(*args, "allowUnverified", &allow_unverified);
        if (map_string(*args, "fingerprint", &fingerprint) && !fingerprint.empty()) {
            fingerprint = chast::normalize_fingerprint(fingerprint);
            if (fingerprint.empty()) {
                result->Error("bad_args", "fingerprint must be 64 hex digits of a SHA-256");
                return;
            }
        }
        map_string(*args, "accessToken", &access_token);
        if (access_token.size() > chast::kMaxAccessTokenLength) {
            result->Error("bad_args", "accessToken must be at most 128 bytes");
            return;
        }
        texture_sink_->SetOutput(reinterpret_cast<void*>(static_cast<intptr_t>(output)));
        if (!viewer_->Start(host, static_cast<uint16_t>(port),
                            static_cast<uint16_t>(display), std::move(fingerprint),
                            allow_unverified, std::move(access_token))) {
            texture_sink_->SetOutput(nullptr);
            result->Error("busy", "a session is already running");
            return;
        }
        result->Success(flutter::EncodableValue(true));
        return;
    }

    if (method == "stop") {
        viewer_->Stop();
        // The plugin texture may be unregistered as soon as this returns.
        texture_sink_->SetOutput(nullptr);
        result->Success();
        return;
    }

    if (method == "status") {
        const ViewerStatus status = viewer_->Snapshot();
        flutter::EncodableMap map;
        map[key("state")] = flutter::EncodableValue(std::string(state_name(status.state)));
        map[key("error")] = flutter::EncodableValue(status.error);
        map[key("host")] = flutter::EncodableValue(status.host);
        map[key("port")] = flutter::EncodableValue(static_cast<int32_t>(status.port));
        map[key("codec")] = flutter::EncodableValue(
            std::string(status.codec == chast::VideoCodec::H264 ? "h264" : "hevc"));
        map[key("width")] = flutter::EncodableValue(static_cast<int32_t>(status.width));
        map[key("height")] = flutter::EncodableValue(static_cast<int32_t>(status.height));
        map[key("framerate")] =
            flutter::EncodableValue(static_cast<int32_t>(status.framerate));
        map[key("serverBitrate")] =
            flutter::EncodableValue(static_cast<int64_t>(status.server_bitrate));
        map[key("decoder")] = flutter::EncodableValue(status.decoder);
        map[key("serverEncoder")] = flutter::EncodableValue(status.server_encoder);
        map[key("serverFingerprint")] = flutter::EncodableValue(status.server_fingerprint);
        map[key("denial")] = flutter::EncodableValue(static_cast<int32_t>(status.denial));
        map[key("received")] =
            flutter::EncodableValue(static_cast<int64_t>(status.received));
        map[key("decoded")] = flutter::EncodableValue(static_cast<int64_t>(status.decoded));
        map[key("keyframes")] =
            flutter::EncodableValue(static_cast<int64_t>(status.keyframes));
        map[key("published")] =
            flutter::EncodableValue(static_cast<int64_t>(status.published));
        map[key("crcErrors")] =
            flutter::EncodableValue(static_cast<int64_t>(status.crc_errors));
        map[key("decodeErrors")] =
            flutter::EncodableValue(static_cast<int64_t>(status.decode_errors));
        map[key("protocolErrors")] =
            flutter::EncodableValue(static_cast<int64_t>(status.protocol_errors));
        map[key("gaps")] = flutter::EncodableValue(static_cast<int64_t>(status.gaps));
        map[key("bytes")] = flutter::EncodableValue(static_cast<int64_t>(status.bytes));
        map[key("fps")] = flutter::EncodableValue(status.fps);
        map[key("kbps")] = flutter::EncodableValue(status.kbps);
        map[key("rttMs")] = flutter::EncodableValue(status.rtt_ms);
        map[key("latencyMs")] = flutter::EncodableValue(status.latency_ms);
        map[key("elapsedMs")] =
            flutter::EncodableValue(static_cast<int64_t>(status.elapsed_ms));
        map[key("videoFinished")] = flutter::EncodableValue(status.video_finished);
        map[key("fullscreen")] = flutter::EncodableValue(fullscreen_);
        map[key("inputEnabled")] = flutter::EncodableValue(status.input_enabled);
        map[key("originX")] = flutter::EncodableValue(static_cast<int32_t>(status.origin_x));
        map[key("originY")] = flutter::EncodableValue(static_cast<int32_t>(status.origin_y));
        map[key("inputSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.input_sent));
        map[key("inputFailed")] =
            flutter::EncodableValue(static_cast<int64_t>(status.input_failed));
        map[key("clipboardEnabled")] = flutter::EncodableValue(status.clipboard_enabled);
        map[key("clipboardSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.clipboard_sent));
        map[key("clipboardReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.clipboard_received));
        map[key("clipboardFailed")] =
            flutter::EncodableValue(static_cast<int64_t>(status.clipboard_failed));
        map[key("cursorUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.cursor_updates));
        map[key("cursorPending")] = flutter::EncodableValue(status.cursor_pending);
        map[key("audioAnnounced")] = flutter::EncodableValue(status.audio_announced);
        map[key("audioEnabled")] = flutter::EncodableValue(status.audio_enabled);
        map[key("audioMuted")] = flutter::EncodableValue(status.audio_muted);
        map[key("audioSampleRate")] =
            flutter::EncodableValue(static_cast<int32_t>(status.audio_sample_rate));
        map[key("audioChannels")] =
            flutter::EncodableValue(static_cast<int32_t>(status.audio_channels));
        map[key("audioBitrate")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_bitrate));
        map[key("audioReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_received));
        map[key("audioDecoded")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_decoded));
        map[key("audioErrors")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_errors));
        map[key("audioUnderruns")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_underruns));
        map[key("audioRecoveries")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_recoveries));
        map[key("audioRecovering")] = flutter::EncodableValue(status.audio_recovering);
        map[key("audioBufferMs")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_buffer_ms));
        map[key("audioError")] = flutter::EncodableValue(status.audio_error);
        flutter::EncodableList displays;
        for (const chast::DisplayDescription& display : status.displays) {
            flutter::EncodableMap entry;
            entry[key("name")] = flutter::EncodableValue(display.name);
            entry[key("left")] = flutter::EncodableValue(display.left);
            entry[key("top")] = flutter::EncodableValue(display.top);
            entry[key("right")] = flutter::EncodableValue(display.right);
            entry[key("bottom")] = flutter::EncodableValue(display.bottom);
            entry[key("rotation")] =
                flutter::EncodableValue(static_cast<int32_t>(display.rotation_degrees));
            entry[key("primary")] = flutter::EncodableValue(display.primary);
            displays.push_back(flutter::EncodableValue(std::move(entry)));
        }
        map[key("displays")] = flutter::EncodableValue(std::move(displays));
        result->Success(flutter::EncodableValue(std::move(map)));
        return;
    }

    if (method == "setFullscreen") {
        const auto* enable = std::get_if<bool>(call.arguments());
        if (enable == nullptr) {
            result->Error("bad_args", "setFullscreen wants a bool");
            return;
        }
        SetFullscreen(*enable);
        result->Success(flutter::EncodableValue(fullscreen_));
        return;
    }

    if (method == "setKeyCapture") {
        const auto* enable = std::get_if<bool>(call.arguments());
        if (enable == nullptr) {
            result->Error("bad_args", "setKeyCapture wants a bool");
            return;
        }
        SetKeyCapture(*enable);
        result->Success(
            flutter::EncodableValue(key_capture_.load(std::memory_order_relaxed)));
        return;
    }

    if (method == "releaseInput") {
        const bool queued = viewer_ != nullptr && viewer_->ReleaseInput();
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "sendInput") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "sendInput wants an argument map");
            return;
        }
        SendInput(*args, std::move(result));
        return;
    }

    if (method == "sendClipboard") {
        const auto* text = std::get_if<std::string>(call.arguments());
        if (text == nullptr || !chast::valid_clipboard_text(*text)) {
            result->Error("bad_args", "sendClipboard wants 1..262144 bytes of valid UTF-8");
            return;
        }
        const bool queued = viewer_ != nullptr && viewer_->SendClipboard(*text);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "setAudioMuted") {
        const auto* mute = std::get_if<bool>(call.arguments());
        if (mute == nullptr) {
            result->Error("bad_args", "setAudioMuted wants a bool");
            return;
        }
        const bool applied = viewer_ != nullptr && viewer_->SetAudioMuted(*mute);
        result->Success(flutter::EncodableValue(applied));
        return;
    }

    if (method == "takeClipboard") {
        std::string text;
        if (viewer_ != nullptr && viewer_->TakeClipboard(text)) {
            result->Success(flutter::EncodableValue(std::move(text)));
        } else {
            result->Success();
        }
        return;
    }

    if (method == "refreshRemoteCursor") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "refreshRemoteCursor wants an argument map");
            return;
        }
        ApplyCursor(*args);
        result->Success(flutter::EncodableValue(cursor_override_));
        return;
    }

    if (method == "clearRemoteCursor") {
        ClearCursor();
        result->Success();
        return;
    }

    result->NotImplemented();
}

void ViewerBridge::SendInput(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    int64_t kind = 0;
    int64_t action = 0;
    int64_t buttons = 0;
    int64_t x = 0;
    int64_t y = 0;
    int64_t usage = 0;
    int64_t modifiers = 0;
    if (!map_int(args, "kind", &kind) || !map_int(args, "action", &action) ||
        !map_int(args, "buttons", &buttons) || !map_int(args, "x", &x) ||
        !map_int(args, "y", &y) || !map_int(args, "modifiers", &modifiers) ||
        modifiers < 0 || modifiers > chast::kInputStateMask) {
        result->Error("bad_args",
                      "sendInput wants kind, action, buttons, x, y and an input-state bitmap");
        return;
    }

    chast::InputEvent event;
    event.modifiers = static_cast<uint16_t>(modifiers);
    if (kind == 1) {
        event.kind = chast::InputKind::Mouse;
        if (action < 0 || action > 5 || buttons < 0 || buttons > 0x1F ||
            x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
            result->Error("bad_args", "mouse event out of range");
            return;
        }
        event.mouse.action = static_cast<chast::MouseAction>(action);
        event.mouse.buttons = static_cast<uint16_t>(buttons);
        event.mouse.x = static_cast<int32_t>(x);
        event.mouse.y = static_cast<int32_t>(y);
        switch (event.mouse.action) {
            case chast::MouseAction::ButtonDown:
            case chast::MouseAction::ButtonUp: {
                const uint16_t bit = event.mouse.buttons;
                if (bit == 0 || (bit & static_cast<uint16_t>(bit - 1)) != 0) {
                    result->Error("bad_args", "a button event carries exactly one button");
                    return;
                }
                break;
            }
            case chast::MouseAction::Wheel:
            case chast::MouseAction::WheelHorizontal:
                if (event.mouse.buttons != 0) {
                    result->Error("bad_args", "a wheel event carries no buttons");
                    return;
                }
                break;
            default:
                break;
        }
    } else if (kind == 2) {
        event.kind = chast::InputKind::Key;
        if (!map_int(args, "hidUsage", &usage) || usage <= 0 || usage > 0xFFFF ||
            action < 0 || action > 1) {
            result->Error("bad_args", "key event wants action 0/1 and hidUsage");
            return;
        }
        event.key.action = static_cast<chast::KeyAction>(action);
        event.key.hid_usage = static_cast<uint16_t>(usage);
    } else {
        result->Error("bad_args", "input kind must be 1 (mouse) or 2 (key)");
        return;
    }

    const bool queued = viewer_ != nullptr && viewer_->SendInput(event);
    result->Success(flutter::EncodableValue(queued));
}

void ViewerBridge::SetKeyCapture(bool enable) {
    const bool was = key_capture_.exchange(enable, std::memory_order_relaxed);
    if (was == enable || view_ == nullptr || !IsWindow(view_)) {
        return;
    }
    if (enable) {
        saved_himc_ = ImmAssociateContext(view_, nullptr);
    } else if (saved_himc_ != nullptr) {
        ImmAssociateContext(view_, saved_himc_);
        saved_himc_ = nullptr;
    }
}

LRESULT CALLBACK ViewerBridge::ViewSubclassProc(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<ViewerBridge*>(ref_data);
    if (self != nullptr) {
        switch (message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP:
                // An active local IME replaces the virtual key with
                // VK_PROCESSKEY and the engine drops such events; while the
                // remote host owns composition, hand it the physical key.
                // ImmGetVirtualKey knows the original for real key presses,
                // but not for synthesized ones, so fall back to the scan code
                // in lParam (the remote side injects by HID usage anyway).
                if (wparam == VK_PROCESSKEY &&
                    self->key_capture_.load(std::memory_order_relaxed)) {
                    UINT original = ImmGetVirtualKey(window);
                    if (original == 0 || original == VK_PROCESSKEY) {
                        const UINT scan = static_cast<UINT>((lparam >> 16) & 0xff);
                        if (scan != 0) {
                            const UINT extended =
                                scan | ((lparam & (1 << 24)) != 0 ? 0xE000u : 0u);
                            original = MapVirtualKeyW(extended, MAPVK_VSC_TO_VK_EX);
                        }
                    }
                    if (original != 0 && original != VK_PROCESSKEY) {
                        wparam = original;
                    }
                }
                break;
            case WM_SETCURSOR:
                // The host's pointer shape over the video area. Answering here
                // keeps Flutter from installing its own cursor on top: the
                // page hides the system cursor while control is on, and this
                // is what puts the host's arrow/I-beam/resize grip back.
                if (LOWORD(lparam) == HTCLIENT && self->cursor_override_) {
                    HCURSOR cursor = self->remote_cursor_.cursor();
                    if (cursor != nullptr) {
                        ::SetCursor(cursor);
                        return TRUE;
                    }
                }
                break;
            case WM_NCDESTROY:
                RemoveWindowSubclass(window, &ViewerBridge::ViewSubclassProc, id);
                self->view_ = nullptr;
                break;
            default:
                break;
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void ViewerBridge::ApplyCursor(const flutter::EncodableMap& args) {
    if (viewer_ == nullptr) {
        return;
    }
    // Every update the session received since the last poll: register the
    // sprite of each new shape, then remember only the newest state. The
    // sprite crosses no boundary here, which is why this stays in native code.
    chast::CursorUpdate update;
    while (viewer_->TakeCursor(update)) {
        if (!update.sprite.empty() && update.shape_id != 0) {
            std::string error;
            if (!remote_cursor_.Register(update.shape_id, update.width, update.height,
                                         update.hotspot_x, update.hotspot_y, update.sprite,
                                         &error)) {
                std::fprintf(stderr, "chast_viewer: remote cursor: %s\n", error.c_str());
            }
        }
        last_cursor_ = std::move(update);
        have_cursor_ = true;
    }

    bool enabled = false;
    int64_t scale_milli = 1000;
    map_bool(args, "enabled", &enabled);
    map_int(args, "scaleMilli", &scale_milli);
    if (!enabled || !have_cursor_ || !last_cursor_.visible || last_cursor_.shape_id == 0) {
        // View-only, or the host says its pointer is hidden: Flutter's own
        // cursor is the right one again.
        if (cursor_override_) {
            cursor_override_ = false;
            remote_cursor_.Deactivate();
            if (view_ != nullptr && IsWindow(view_)) {
                ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
            }
        }
        return;
    }

    const double scale =
        scale_milli > 0 ? static_cast<double>(scale_milli) / 1000.0 : 1.0;
    if (cursor_override_ && cursor_shape_ == last_cursor_.shape_id &&
        RemoteCursor::QuantiseScale(cursor_scale_) == RemoteCursor::QuantiseScale(scale)) {
        return;
    }
    std::string error;
    if (!remote_cursor_.Activate(last_cursor_.shape_id, scale, &error)) {
        // The sprite for the shape the host is showing has not arrived yet (a
        // shape id can precede its image after a cache eviction). Keep the
        // previous cursor and try again on the next poll.
        if (error != "the cursor shape was never registered") {
            std::fprintf(stderr, "chast_viewer: remote cursor: %s\n", error.c_str());
        }
        return;
    }
    cursor_shape_ = last_cursor_.shape_id;
    cursor_scale_ = scale;
    cursor_override_ = true;
    if (view_ != nullptr && IsWindow(view_)) {
        ::SetCursor(remote_cursor_.cursor());
    }
}

void ViewerBridge::ClearCursor() {
    cursor_override_ = false;
    have_cursor_ = false;
    cursor_shape_ = 0;
    remote_cursor_.Clear();
    if (view_ != nullptr && IsWindow(view_)) {
        ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
    }
}

void ViewerBridge::SetFullscreen(bool enable) {
    if (window_ == nullptr || fullscreen_ == enable) {
        return;
    }
    if (enable) {
        saved_placement_.length = sizeof(saved_placement_);
        if (!GetWindowPlacement(window_, &saved_placement_)) {
            return;
        }
        saved_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
        const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        SetWindowLongPtrW(window_, GWL_STYLE,
                          (saved_style_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        SetWindowPos(window_, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left,
                     info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        fullscreen_ = true;
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, saved_style_);
        SetWindowPlacement(window_, &saved_placement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                         SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

}  // namespace chast_viewer
