#include "viewer_bridge.h"

#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>

#include <flutter/flutter_engine.h>
#include <flutter/standard_method_codec.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/security.h"
#include "gpu_frame_sink.h"

namespace chast_viewer {

namespace {

constexpr UINT_PTR kViewSubclassId = 1;
// The Dart side uses a server URL or host:port as the password-store key.
constexpr size_t kMaxTrustKeyLength = 512;

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

std::string utf8_from_wide(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                        static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// OFN_EXPLORER|OFN_ALLOWMULTISELECT returns "dir\0name1\0name2\0\0"; a single
// selection arrives as one full path in the same buffer.
std::vector<std::string> selected_paths(const wchar_t* buffer) {
    std::vector<std::string> paths;
    const std::wstring first(buffer);
    if (first.empty()) {
        return paths;
    }
    const wchar_t* cursor = buffer + first.size() + 1;
    if (*cursor == L'\0') {
        paths.push_back(utf8_from_wide(first));
        return paths;
    }
    const std::filesystem::path directory(first);
    while (*cursor != L'\0') {
        const std::wstring item(cursor);
        if (!item.empty()) {
            paths.push_back(utf8_from_wide((directory / item).wstring()));
        }
        cursor += item.size() + 1;
    }
    return paths;
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

// %APPDATA%\chast, where the viewer's device key and saved passwords live.
// Empty when the shell cannot name the roaming profile.
std::filesystem::path app_data_chast_dir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        return {};
    }
    const std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path / L"chast";
}

// The password store is keyed by the same trust key Dart uses for the pin, so
// a file cannot be moved to another target by editing plain text. FNV-1a is
// enough here: the key is not a secret, only a lookup name.
std::string password_file_name(const std::string& trust_key) {
    uint64_t hash = 1469598103934665603ull;
    for (const char raw : trust_key) {
        hash ^= static_cast<unsigned char>(raw);
        hash *= 1099511628211ull;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin",
                  static_cast<unsigned long long>(hash));
    return name;
}

std::string password_entropy(const std::string& trust_key) {
    return "chast-viewer-password:" + trust_key;
}

bool read_text_file(const std::filesystem::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_text_file(const std::filesystem::path& path, const std::string& text,
                     std::string* error) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        *error = "create " + path.parent_path().string() + ": " + code.message();
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        *error = "open " + path.string() + " for writing";
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        *error = "write " + path.string();
        return false;
    }
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
        DragAcceptFiles(view_, TRUE);
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
            DragAcceptFiles(view_, FALSE);
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

void ViewerBridge::EnsureIdentity() {
    if (identity_loaded_) {
        return;
    }
    identity_loaded_ = true;
    const std::filesystem::path dir = app_data_chast_dir();
    if (dir.empty()) {
        std::fprintf(stderr, "chast_viewer: cannot locate the roaming profile for the device key\n");
        return;
    }
    std::error_code code;
    std::filesystem::create_directories(dir, code);
    if (code) {
        std::fprintf(stderr, "chast_viewer: create %s: %s\n", dir.string().c_str(),
                     code.message().c_str());
        return;
    }
    std::string error;
    identity_ = chast::DeviceIdentity::load_or_create(
        (dir / L"viewer-identity.bin").string(), &error);
    if (!identity_) {
        std::fprintf(stderr, "chast_viewer: device identity: %s\n", error.c_str());
    }
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
        std::string password;
        std::string totp;
        std::string transport_text = "auto";
        std::string carrier_host;
        int64_t carrier_port = 0;
        std::string carrier_host_id;
        std::string carrier_token;
        ViewerTransport transport = ViewerTransport::kAuto;
        bool allow_unverified = false;
        bool trust_device = false;
        if (args == nullptr || !map_string(*args, "host", &host) ||
            !map_int(*args, "port", &port) || !map_int(*args, "gpuOutput", &output) ||
            host.empty() || port <= 0 || port > 65535 || output <= 0) {
            result->Error("bad_args",
                          "start wants a host, a port in 1..65535 and a GPU texture output");
            return;
        }
        if (map_int(*args, "display", &display) && display != chast::kDisplayAuto &&
            display != chast::kDisplayAll &&
            (display < 0 || display >= chast::kMaxDisplayCount)) {
            result->Error("bad_args",
                          "display must be an announced index, auto or the merged view");
            return;
        }
        map_bool(*args, "allowUnverified", &allow_unverified);
        map_bool(*args, "trustDevice", &trust_device);
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
        map_string(*args, "password", &password);
        if (password.size() > chast::kMaxPasswordLength) {
            result->Error("bad_args", "password must be at most 128 bytes");
            return;
        }
        map_string(*args, "totp", &totp);
        if (totp.size() > chast::kMaxTotpLength) {
            result->Error("bad_args", "totp must be at most 16 characters");
            return;
        }
        if (map_string(*args, "transport", &transport_text)) {
            if (transport_text.empty() || transport_text == "auto") {
                transport = ViewerTransport::kAuto;
            } else if (transport_text == "quic" || transport_text == "udp") {
                transport = ViewerTransport::kQuic;
            } else if (transport_text == "tcp") {
                transport = ViewerTransport::kTcp;
            } else if (transport_text == "ws" || transport_text == "websocket") {
                transport = ViewerTransport::kWebSocket;
            } else {
                result->Error("bad_args", "transport must be auto, quic, tcp or ws");
                return;
            }
        }
        map_string(*args, "carrierHost", &carrier_host);
        if (map_int(*args, "carrierPort", &carrier_port) &&
            (carrier_port < 0 || carrier_port > 65535)) {
            result->Error("bad_args", "carrierPort must be 0 or a TCP port");
            return;
        }
        map_string(*args, "carrierHostId", &carrier_host_id);
        map_string(*args, "carrierToken", &carrier_token);
        if (carrier_host_id.size() > 64 || carrier_token.size() > 128) {
            result->Error("bad_args", "carrier host id or token is too long");
            return;
        }
        EnsureIdentity();
        texture_sink_->SetOutput(reinterpret_cast<void*>(static_cast<intptr_t>(output)));
        if (!viewer_->Start(host, static_cast<uint16_t>(port),
                            static_cast<uint16_t>(display), std::move(fingerprint),
                            allow_unverified, std::move(access_token),
                            std::move(password), std::move(totp), identity_,
                            trust_device, transport, std::move(carrier_host),
                            static_cast<uint16_t>(carrier_port),
                            std::move(carrier_host_id), std::move(carrier_token))) {
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
        map[key("transport")] = flutter::EncodableValue(status.transport);
        map[key("transportError")] = flutter::EncodableValue(status.transport_error);
        map[key("codec")] = flutter::EncodableValue(
            std::string(status.codec == chast::VideoCodec::H264 ? "h264" : "hevc"));
        map[key("width")] = flutter::EncodableValue(static_cast<int32_t>(status.width));
        map[key("height")] = flutter::EncodableValue(static_cast<int32_t>(status.height));
        map[key("sourceWidth")] =
            flutter::EncodableValue(static_cast<int32_t>(status.source_width));
        map[key("sourceHeight")] =
            flutter::EncodableValue(static_cast<int32_t>(status.source_height));
        map[key("framerate")] =
            flutter::EncodableValue(static_cast<int32_t>(status.framerate));
        map[key("serverBitrate")] =
            flutter::EncodableValue(static_cast<int64_t>(status.server_bitrate));
        map[key("decoder")] = flutter::EncodableValue(status.decoder);
        map[key("serverEncoder")] = flutter::EncodableValue(status.server_encoder);
        map[key("serverFingerprint")] = flutter::EncodableValue(status.server_fingerprint);
        map[key("auditRef")] = flutter::EncodableValue(status.audit_ref);
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
        map[key("permissions")] =
            flutter::EncodableValue(static_cast<int32_t>(status.permissions));
        map[key("permissionUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.permission_updates));
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
        map[key("clipboardSentFormats")] =
            flutter::EncodableValue(static_cast<int32_t>(status.clipboard_sent_formats));
        map[key("clipboardReceivedFormats")] =
            flutter::EncodableValue(static_cast<int32_t>(status.clipboard_received_formats));
        map[key("filePhase")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_phase));
        map[key("fileName")] = flutter::EncodableValue(status.file_name);
        map[key("fileRelativePath")] =
            flutter::EncodableValue(status.file_relative_path);
        map[key("fileQueue")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_queue));
        map[key("fileSize")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_size));
        map[key("fileSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_sent));
        map[key("fileError")] = flutter::EncodableValue(status.file_error);
        map[key("lastFileName")] = flutter::EncodableValue(status.last_file_name);
        map[key("lastFileError")] = flutter::EncodableValue(status.last_file_error);
        map[key("filesSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.files_sent));
        map[key("fileFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_failures));
        map[key("downloadPhase")] =
            flutter::EncodableValue(static_cast<int32_t>(status.download_phase));
        map[key("downloadName")] = flutter::EncodableValue(status.download_name);
        map[key("downloadSize")] =
            flutter::EncodableValue(static_cast<int64_t>(status.download_size));
        map[key("downloadReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.download_received));
        map[key("downloadError")] = flutter::EncodableValue(status.download_error);
        map[key("filesReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.files_received));
        map[key("fileReceiveFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_receive_failures));
        map[key("cursorUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.cursor_updates));
        map[key("cursorPending")] = flutter::EncodableValue(status.cursor_pending);
        map[key("audioAnnounced")] = flutter::EncodableValue(status.audio_announced);
        map[key("audioEnabled")] = flutter::EncodableValue(status.audio_enabled);
        map[key("audioMuted")] = flutter::EncodableValue(status.audio_muted);
        map[key("recording")] = flutter::EncodableValue(status.recording);
        map[key("recordPath")] = flutter::EncodableValue(status.record_path);
        map[key("recordError")] = flutter::EncodableValue(status.record_error);
        map[key("recordSamples")] =
            flutter::EncodableValue(static_cast<int64_t>(status.record_samples));
        map[key("recordKeyframes")] =
            flutter::EncodableValue(static_cast<int64_t>(status.record_keyframes));
        map[key("recordSkipped")] =
            flutter::EncodableValue(static_cast<int64_t>(status.record_skipped));
        map[key("recordSegments")] =
            flutter::EncodableValue(static_cast<int32_t>(status.record_segments));
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
        map[key("audioDevice")] = flutter::EncodableValue(status.audio_device);
        map[key("audioDeviceName")] = flutter::EncodableValue(status.audio_device_name);
        map[key("audioAec")] = flutter::EncodableValue(status.audio_aec);
        map[key("audioNoiseSuppression")] =
            flutter::EncodableValue(status.audio_noise_suppression);
        map[key("microphoneAecFrames")] =
            flutter::EncodableValue(static_cast<int64_t>(status.microphone_aec_frames));
        map[key("microphoneNsFrames")] =
            flutter::EncodableValue(static_cast<int64_t>(status.microphone_ns_frames));
        map[key("microphoneAuthorized")] =
            flutter::EncodableValue(status.microphone_authorized);
        map[key("microphoneEnabled")] = flutter::EncodableValue(status.microphone_enabled);
        map[key("microphoneDevice")] = flutter::EncodableValue(status.microphone_device);
        map[key("microphoneName")] = flutter::EncodableValue(status.microphone_name);
        map[key("microphonePackets")] =
            flutter::EncodableValue(static_cast<int64_t>(status.microphone_packets));
        map[key("microphoneDropped")] =
            flutter::EncodableValue(static_cast<int64_t>(status.microphone_dropped));
        map[key("microphoneErrors")] =
            flutter::EncodableValue(static_cast<int64_t>(status.microphone_errors));
        map[key("microphoneRecoveries")] =
            flutter::EncodableValue(static_cast<int64_t>(status.microphone_recoveries));
        map[key("microphoneRecovering")] =
            flutter::EncodableValue(status.microphone_recovering);
        map[key("microphoneError")] = flutter::EncodableValue(status.microphone_error);
        map[key("privacySupported")] = flutter::EncodableValue(status.privacy_supported);
        map[key("privacyEnabled")] = flutter::EncodableValue(status.privacy_enabled);
        map[key("privacyCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.privacy_code));
        map[key("privacyError")] = flutter::EncodableValue(status.privacy_error);
        map[key("privacyUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.privacy_updates));
        map[key("desktop")] =
            flutter::EncodableValue(static_cast<int32_t>(status.desktop));
        map[key("desktopUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.desktop_updates));
        map[key("sasCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.sas_code));
        map[key("sasError")] = flutter::EncodableValue(status.sas_error);
        map[key("sasUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.sas_updates));
        map[key("chatSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.chat_sent));
        map[key("chatReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.chat_received));
        map[key("chatFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.chat_failures));
        map[key("chatError")] = flutter::EncodableValue(status.chat_error);
        map[key("terminalSupported")] =
            flutter::EncodableValue(status.terminal_supported);
        map[key("terminalPhase")] =
            flutter::EncodableValue(static_cast<int32_t>(status.terminal_phase));
        map[key("terminalCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.terminal_code));
        map[key("terminalReason")] = flutter::EncodableValue(status.terminal_reason);
        map[key("terminalError")] = flutter::EncodableValue(status.terminal_error);
        map[key("terminalColumns")] =
            flutter::EncodableValue(static_cast<int32_t>(status.terminal_columns));
        map[key("terminalRows")] =
            flutter::EncodableValue(static_cast<int32_t>(status.terminal_rows));
        map[key("terminalBytes")] =
            flutter::EncodableValue(static_cast<int64_t>(status.terminal_bytes));
        map[key("terminalUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.terminal_updates));
        map[key("displaySwitches")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_switches));
        map[key("displaySwitchFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_switch_failures));
        map[key("displaySwitchError")] =
            flutter::EncodableValue(status.display_switch_error);
        map[key("displayModeLists")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_mode_lists));
        map[key("displayModeChanges")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_mode_changes));
        map[key("displayModeFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_mode_failures));
        map[key("displayModeError")] = flutter::EncodableValue(status.display_mode_error);
        map[key("displayModesDisplay")] =
            flutter::EncodableValue(static_cast<int32_t>(status.display_modes_display));
        map[key("displayModesCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.display_modes_code));
        map[key("displayModesError")] = flutter::EncodableValue(status.display_modes_error);
        map[key("streamQuality")] =
            flutter::EncodableValue(static_cast<int32_t>(status.stream_quality));
        map[key("streamOptionCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.stream_option_code));
        map[key("streamOptionError")] =
            flutter::EncodableValue(status.stream_option_error);
        map[key("streamOptionResults")] =
            flutter::EncodableValue(static_cast<int64_t>(status.stream_option_results));
        map[key("portForwardEnabled")] =
            flutter::EncodableValue(status.port_forward_enabled);
        map[key("portForwardActive")] =
            flutter::EncodableValue(status.port_forward_active);
        map[key("portForwardLocalPort")] =
            flutter::EncodableValue(static_cast<int32_t>(status.port_forward_local_port));
        map[key("portForwardRemoteHost")] =
            flutter::EncodableValue(status.port_forward_remote_host);
        map[key("portForwardRemotePort")] =
            flutter::EncodableValue(static_cast<int32_t>(status.port_forward_remote_port));
        map[key("portForwardConnections")] =
            flutter::EncodableValue(static_cast<int64_t>(status.port_forward_connections));
        map[key("portForwardFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.port_forward_failures));
        map[key("portForwardBytesUp")] =
            flutter::EncodableValue(static_cast<int64_t>(status.port_forward_bytes_up));
        map[key("portForwardBytesDown")] =
            flutter::EncodableValue(static_cast<int64_t>(status.port_forward_bytes_down));
        map[key("portForwardError")] =
            flutter::EncodableValue(status.port_forward_error);
        flutter::EncodableList modes;
        for (const chast::DisplayModeInfo& mode : status.display_modes) {
            flutter::EncodableMap entry;
            entry[key("width")] = flutter::EncodableValue(static_cast<int32_t>(mode.width));
            entry[key("height")] = flutter::EncodableValue(static_cast<int32_t>(mode.height));
            entry[key("refresh")] =
                flutter::EncodableValue(static_cast<int32_t>(mode.refresh_hz));
            entry[key("current")] = flutter::EncodableValue(mode.current);
            modes.push_back(flutter::EncodableValue(std::move(entry)));
        }
        map[key("displayModes")] = flutter::EncodableValue(std::move(modes));
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
        map[key("mergedLayout")] = flutter::EncodableValue(status.merged_layout);
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

    if (method == "switchDisplay") {
        const auto* display = std::get_if<int32_t>(call.arguments());
        if (display == nullptr ||
            (*display != chast::kDisplayAuto && *display != chast::kDisplayAll &&
             (*display < 0 || *display >= chast::kMaxDisplayCount))) {
            result->Error("bad_args",
                          "switchDisplay wants an announced index, auto or the merged view");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->SwitchDisplay(static_cast<uint16_t>(*display));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "requestDisplayModes") {
        const auto* display = std::get_if<int32_t>(call.arguments());
        if (display == nullptr || *display < 0 || *display >= chast::kMaxDisplayCount) {
            result->Error("bad_args", "requestDisplayModes wants an announced display index");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->RequestDisplayModes(static_cast<uint16_t>(*display));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "setDisplayMode") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "setDisplayMode wants an argument map");
            return;
        }
        int64_t display = 0;
        int64_t width = 0;
        int64_t height = 0;
        int64_t refresh = 0;
        if (!map_int(*args, "display", &display) || !map_int(*args, "width", &width) ||
            !map_int(*args, "height", &height) || !map_int(*args, "refresh", &refresh) ||
            display < 0 || display >= chast::kMaxDisplayCount || width <= 0 || width > 16384 ||
            height <= 0 || height > 16384 || refresh <= 0 || refresh > 1000) {
            result->Error("bad_args",
                          "setDisplayMode wants display, width, height and refresh in range");
            return;
        }
        const bool queued = viewer_ != nullptr &&
                            viewer_->SetDisplayMode(static_cast<uint16_t>(display),
                                                    static_cast<uint16_t>(width),
                                                    static_cast<uint16_t>(height),
                                                    static_cast<uint16_t>(refresh));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "setStreamOptions") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "setStreamOptions wants an argument map");
            return;
        }
        int64_t codec = 0;
        int64_t max_fps = 0;
        int64_t quality = 0;
        if (!map_int(*args, "codec", &codec) || !map_int(*args, "maxFps", &max_fps) ||
            !map_int(*args, "quality", &quality) || codec < 0 || codec > 2 ||
            max_fps < 0 || max_fps > chast::kMaxStreamFps || quality < 0 || quality > 2) {
            result->Error("bad_args",
                          "setStreamOptions wants codec and quality in 0..2 and maxFps in "
                          "0..240");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->SetStreamOptions(static_cast<chast::StreamCodecPreference>(codec),
                                      static_cast<uint16_t>(max_fps),
                                      static_cast<chast::StreamQuality>(quality));
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

    if (method == "setClipboardSync") {
        const auto* enabled = std::get_if<bool>(call.arguments());
        if (enabled == nullptr) {
            result->Error("bad_args", "setClipboardSync wants a bool");
            return;
        }
        if (viewer_ != nullptr) {
            viewer_->SetClipboardSync(*enabled);
        }
        result->Success();
        return;
    }

    if (method == "setRecording") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "setRecording wants a map");
            return;
        }
        bool enabled = false;
        if (const auto it = args->find(flutter::EncodableValue("enabled"));
            it != args->end()) {
            if (const auto* value = std::get_if<bool>(&it->second)) {
                enabled = *value;
            }
        }
        std::string directory;
        if (const auto it = args->find(flutter::EncodableValue("directory"));
            it != args->end()) {
            if (const auto* value = std::get_if<std::string>(&it->second)) {
                directory = *value;
            }
        }
        if (viewer_ == nullptr) {
            result->Error("unavailable", "no viewer session is running");
            return;
        }
        // Starting needs a streaming session; stopping must always be possible
        // so a teardown can turn the recorder off.
        if (!viewer_->SetRecording(enabled, directory) && enabled) {
            result->Error("unavailable", "recording needs a streaming session");
            return;
        }
        result->Success(flutter::EncodableValue(enabled));
        return;
    }

    if (method == "sendFile" || method == "sendFileAt") {
        if (viewer_ == nullptr) {
            result->Error("unavailable", "no viewer session is running");
            return;
        }
        const auto start_upload = [this, &result](const std::vector<std::string>& paths) {
            if (paths.empty() || !viewer_->SendFiles(paths)) {
                result->Error("unavailable",
                              "the files cannot be queued now (session ended or not permitted)");
                return;
            }
            result->Success(flutter::EncodableValue(static_cast<int32_t>(paths.size())));
        };
        if (method == "sendFileAt") {
            // The paths come from a drop on the window, so they are already
            // absolute; SendFiles expands folders and skips anything else.
            std::vector<std::string> paths;
            if (const auto* one = std::get_if<std::string>(call.arguments())) {
                paths.push_back(*one);
            } else if (const auto* list =
                           std::get_if<flutter::EncodableList>(call.arguments())) {
                for (const flutter::EncodableValue& item : *list) {
                    if (const auto* item_path = std::get_if<std::string>(&item)) {
                        paths.push_back(*item_path);
                    }
                }
            }
            if (paths.empty()) {
                result->Error("bad_args", "sendFileAt wants a path or a list of paths");
                return;
            }
            start_upload(paths);
            return;
        }
        // The multi-select format needs a 64 KiB buffer; longer selections
        // come back as FNERR_BUFFERTOOSMALL instead of a truncated list.
        std::vector<wchar_t> selected(65536, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"All files (*.*)\0*.*\0\0";
        dialog.lpstrFile = selected.data();
        dialog.nMaxFile = static_cast<DWORD>(selected.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                       OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
        if (!GetOpenFileNameW(&dialog)) {
            // Cancelling is not an error; the picker simply closed.
            result->Success();
            return;
        }
        start_upload(selected_paths(selected.data()));
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

    if (method == "setMicrophone") {
        const auto* enabled = std::get_if<bool>(call.arguments());
        if (enabled == nullptr) {
            result->Error("bad_args", "setMicrophone wants a bool");
            return;
        }
        const bool applied = viewer_ != nullptr && viewer_->SetMicrophone(*enabled);
        result->Success(flutter::EncodableValue(applied));
        return;
    }

    if (method == "selectMicrophone") {
        const auto* id = std::get_if<std::string>(call.arguments());
        if (id == nullptr) {
            result->Error("bad_args", "selectMicrophone wants a device id");
            return;
        }
        const bool applied = viewer_ != nullptr && viewer_->SelectMicrophone(*id);
        result->Success(flutter::EncodableValue(applied));
        return;
    }

    if (method == "selectAudioOutput") {
        const auto* id = std::get_if<std::string>(call.arguments());
        if (id == nullptr) {
            result->Error("bad_args", "selectAudioOutput wants a device id");
            return;
        }
        const bool applied = viewer_ != nullptr && viewer_->SelectAudioOutput(*id);
        result->Success(flutter::EncodableValue(applied));
        return;
    }

    if (method == "setAudioProcessing") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        bool aec = false;
        bool noise_suppression = false;
        if (args == nullptr || !map_bool(*args, "aec", &aec) ||
            !map_bool(*args, "noiseSuppression", &noise_suppression)) {
            result->Error("bad_args", "setAudioProcessing wants aec and noiseSuppression");
            return;
        }
        const bool applied =
            viewer_ != nullptr && viewer_->SetAudioProcessing(aec, noise_suppression);
        result->Success(flutter::EncodableValue(applied));
        return;
    }

    if (method == "setPrivacyMode") {
        const auto* enabled = std::get_if<bool>(call.arguments());
        if (enabled == nullptr) {
            result->Error("bad_args", "setPrivacyMode wants a bool");
            return;
        }
        const bool applied = viewer_ != nullptr && viewer_->SetPrivacyMode(*enabled);
        result->Success(flutter::EncodableValue(applied));
        return;
    }

    if (method == "sendSas") {
        const bool queued = viewer_ != nullptr && viewer_->SendSas();
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "sendChat") {
        const auto* text = std::get_if<std::string>(call.arguments());
        if (text == nullptr) {
            result->Error("bad_args", "sendChat wants the message text");
            return;
        }
        const bool queued = viewer_ != nullptr && viewer_->SendChat(*text);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "takeChat") {
        std::string text;
        if (viewer_ == nullptr || !viewer_->TakeChat(text)) {
            // Null is the normal "nothing waiting" answer, not an error.
            result->Success();
            return;
        }
        result->Success(flutter::EncodableValue(std::move(text)));
        return;
    }

    if (method == "openTerminal") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t columns = 0;
        int64_t rows = 0;
        if (args == nullptr || !map_int(*args, "columns", &columns) ||
            !map_int(*args, "rows", &rows) || columns <= 0 || columns > 500 ||
            rows <= 0 || rows > 200) {
            result->Error("bad_args", "openTerminal wants columns 1..500 and rows 1..200");
            return;
        }
        const bool opened =
            viewer_ != nullptr && viewer_->OpenTerminal(
                                      static_cast<uint16_t>(columns),
                                      static_cast<uint16_t>(rows));
        result->Success(flutter::EncodableValue(opened));
        return;
    }

    if (method == "sendTerminalInput") {
        const auto* text = std::get_if<std::string>(call.arguments());
        if (text == nullptr) {
            result->Error("bad_args", "sendTerminalInput wants the input text");
            return;
        }
        const bool queued = viewer_ != nullptr && viewer_->SendTerminalInput(*text);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "resizeTerminal") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t columns = 0;
        int64_t rows = 0;
        if (args == nullptr || !map_int(*args, "columns", &columns) ||
            !map_int(*args, "rows", &rows) || columns <= 0 || columns > 500 ||
            rows <= 0 || rows > 200) {
            result->Error("bad_args", "resizeTerminal wants columns 1..500 and rows 1..200");
            return;
        }
        const bool queued =
            viewer_ != nullptr && viewer_->ResizeTerminal(
                                      static_cast<uint16_t>(columns),
                                      static_cast<uint16_t>(rows));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "closeTerminal") {
        if (viewer_ != nullptr) {
            viewer_->CloseTerminal();
        }
        result->Success();
        return;
    }

    if (method == "takeTerminalOutput") {
        // Bytes, not text: a UTF-8 sequence may be split across chunks, so
        // decoding belongs to the panel that reassembles the console picture.
        std::vector<uint8_t> data;
        bool ended = false;
        if (viewer_ != nullptr) {
            viewer_->TakeTerminalOutput(data, ended);
        }
        flutter::EncodableMap reply;
        reply[flutter::EncodableValue("data")] =
            flutter::EncodableValue(std::move(data));
        reply[flutter::EncodableValue("ended")] = flutter::EncodableValue(ended);
        result->Success(flutter::EncodableValue(std::move(reply)));
        return;
    }

    if (method == "startPortForward") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "startPortForward wants an argument map");
            return;
        }
        int64_t local_port = 0;
        int64_t remote_port = 0;
        std::string remote_host;
        if (!map_int(*args, "localPort", &local_port) ||
            !map_int(*args, "remotePort", &remote_port) ||
            !map_string(*args, "remoteHost", &remote_host) || local_port < 0 ||
            local_port > 65535 || remote_port <= 0 || remote_port > 65535) {
            result->Error("bad_args",
                          "startPortForward wants localPort 0..65535, "
                          "remotePort 1..65535 and remoteHost");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->StartPortForward(static_cast<uint16_t>(local_port), remote_host,
                                      static_cast<uint16_t>(remote_port));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "stopPortForward") {
        if (viewer_ != nullptr) {
            viewer_->StopPortForward();
        }
        result->Success();
        return;
    }

    if (method == "listMicrophones") {
        std::string list_error;
        const std::vector<chast::MicrophoneDevice> devices =
            chast::list_microphones(&list_error);
        if (devices.empty() && !list_error.empty()) {
            result->Error("audio", list_error);
            return;
        }
        flutter::EncodableList list;
        for (const chast::MicrophoneDevice& device : devices) {
            flutter::EncodableMap entry;
            entry[key("id")] = flutter::EncodableValue(device.id);
            entry[key("name")] = flutter::EncodableValue(device.name);
            entry[key("default")] = flutter::EncodableValue(device.is_default);
            list.push_back(flutter::EncodableValue(std::move(entry)));
        }
        result->Success(flutter::EncodableValue(std::move(list)));
        return;
    }

    if (method == "listAudioOutputs") {
        std::string list_error;
        const std::vector<chast::AudioOutputDevice> devices =
            chast::list_audio_outputs(&list_error);
        if (devices.empty() && !list_error.empty()) {
            result->Error("audio", list_error);
            return;
        }
        flutter::EncodableList list;
        for (const chast::AudioOutputDevice& device : devices) {
            flutter::EncodableMap entry;
            entry[key("id")] = flutter::EncodableValue(device.id);
            entry[key("name")] = flutter::EncodableValue(device.name);
            entry[key("default")] = flutter::EncodableValue(device.is_default);
            list.push_back(flutter::EncodableValue(std::move(entry)));
        }
        result->Success(flutter::EncodableValue(std::move(list)));
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

    if (method == "loadPassword") {
        const auto* trust_key = std::get_if<std::string>(call.arguments());
        if (trust_key == nullptr || trust_key->empty() ||
            trust_key->size() > kMaxTrustKeyLength) {
            result->Error("bad_args", "loadPassword wants a non-empty trust key");
            return;
        }
        const std::filesystem::path dir = app_data_chast_dir();
        if (dir.empty()) {
            result->Success();
            return;
        }
        std::string stored;
        if (!read_text_file(dir / L"passwords" / password_file_name(*trust_key), &stored)) {
            result->Success();
            return;
        }
        std::string password;
        std::string error;
        if (!chast::unprotect_secret(stored, chast::SecretScope::CurrentUser,
                                     password_entropy(*trust_key), password, &error)) {
            std::fprintf(stderr, "chast_viewer: saved password: %s\n", error.c_str());
            result->Success();
            return;
        }
        result->Success(flutter::EncodableValue(std::move(password)));
        return;
    }

    if (method == "savePassword") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        std::string trust_key;
        std::string password;
        if (args == nullptr || !map_string(*args, "key", &trust_key) ||
            !map_string(*args, "password", &password) || trust_key.empty() ||
            trust_key.size() > kMaxTrustKeyLength ||
            password.size() > chast::kMaxPasswordLength) {
            result->Error("bad_args",
                          "savePassword wants a trust key and at most 128 bytes of password");
            return;
        }
        const std::filesystem::path dir = app_data_chast_dir();
        if (dir.empty()) {
            result->Error("no_profile", "the roaming profile is not available");
            return;
        }
        const std::filesystem::path file =
            dir / L"passwords" / password_file_name(trust_key);
        if (password.empty()) {
            std::error_code code;
            std::filesystem::remove(file, code);
            result->Success();
            return;
        }
        std::string protected_text;
        std::string error;
        if (!chast::protect_secret(password, chast::SecretScope::CurrentUser,
                                   password_entropy(trust_key), protected_text, &error)) {
            result->Error("dpapi", error);
            return;
        }
        if (!write_text_file(file, protected_text, &error)) {
            result->Error("io", error);
            return;
        }
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
            case WM_DROPFILES: {
                HDROP drop = reinterpret_cast<HDROP>(wparam);
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                flutter::EncodableList paths;
                for (UINT index = 0; index < count; ++index) {
                    const UINT length =
                        DragQueryFileW(drop, index, nullptr, 0);
                    if (length == 0) {
                        continue;
                    }
                    std::vector<wchar_t> path(static_cast<size_t>(length) + 1, L'\0');
                    if (DragQueryFileW(drop, index, path.data(),
                                       static_cast<UINT>(path.size())) == 0) {
                        continue;
                    }
                    const std::string utf8 = utf8_from_wide(path.data());
                    if (!utf8.empty()) {
                        paths.push_back(flutter::EncodableValue(utf8));
                    }
                }
                if (self->channel_ && !paths.empty()) {
                    self->channel_->InvokeMethod(
                        "droppedFiles",
                        std::make_unique<flutter::EncodableValue>(paths));
                }
                DragFinish(drop);
                return 0;
            }
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
