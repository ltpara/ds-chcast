#include "viewer_bridge.h"

#include <flutter/flutter_engine.h>
#include <flutter/standard_method_codec.h>

#include <cstdio>
#include <utility>

#include "gpu_frame_sink.h"

namespace chast_viewer {

namespace {

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
}

ViewerBridge::~ViewerBridge() {
    Shutdown();
}

void ViewerBridge::Shutdown() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
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
        if (args == nullptr || !map_string(*args, "host", &host) ||
            !map_int(*args, "port", &port) || !map_int(*args, "gpuOutput", &output) ||
            host.empty() || port <= 0 || port > 65535 || output <= 0) {
            result->Error("bad_args",
                          "start wants a host, a port in 1..65535 and a GPU texture output");
            return;
        }
        texture_sink_->SetOutput(reinterpret_cast<void*>(static_cast<intptr_t>(output)));
        if (!viewer_->Start(host, static_cast<uint16_t>(port))) {
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
        map[key("elapsedMs")] =
            flutter::EncodableValue(static_cast<int64_t>(status.elapsed_ms));
        map[key("videoFinished")] = flutter::EncodableValue(status.video_finished);
        map[key("fullscreen")] = flutter::EncodableValue(fullscreen_);
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

    result->NotImplemented();
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
