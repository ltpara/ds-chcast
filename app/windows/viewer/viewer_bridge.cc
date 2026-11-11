#include "viewer_bridge.h"

#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>

#include <flutter/flutter_engine.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "chast/access_ticket.h"
#include "chast/audio.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/security.h"
#include "gpu_frame_sink.h"
#include "security_key.h"

namespace chast_viewer {

namespace {

constexpr UINT_PTR kViewSubclassId = 1;
constexpr UINT_PTR kWindowSubclassId = 2;
// Posted by a security-key worker thread once Windows Hello returns. The
// LPARAM owns a SecurityKeyCompletion that only the platform thread may answer.
constexpr UINT kSecurityKeyDoneMessage = WM_APP + 0x51;
constexpr UINT kKeyboardEscapeMessage = WM_APP + 0x52;
constexpr UINT kLocalKeyMessage = WM_APP + 0x53;
// The Dart side uses a server URL or host:port as the password-store key.
constexpr size_t kMaxTrustKeyLength = 512;
// Binds account-token ciphertext to its purpose, so a blob lifted from another
// DPAPI-protected file cannot be swapped in.
constexpr char kAccountSecretEntropy[] = "chast.account.v1";
// The ticket key is separate from the account envelope on purpose: one names a
// connection, the other authorizes the account.
constexpr char kTicketSeedEntropy[] = "chast.access-ticket.v1";
constexpr size_t kMaxProtectPlaintext = 64 * 1024;
constexpr size_t kMaxProtectedText = 128 * 1024;
constexpr size_t kMaxTicketText = 2048;

// A finished Windows Hello ceremony waiting for the platform thread. Both the
// worker and the platform handler hold it through a shared_ptr, and `answered`
// keeps a race between them (or with Shutdown) from replying twice.
struct SecurityKeyCompletion {
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result;
    flutter::EncodableMap value;
    std::string error_code;
    std::string error;
    bool ok = false;
    std::atomic<bool> answered{false};
};

void complete_security_key(const std::shared_ptr<SecurityKeyCompletion>& completion) {
    if (completion == nullptr || completion->answered.exchange(true)) {
        return;
    }
    if (completion->ok) {
        completion->result->Success(flutter::EncodableValue(completion->value));
    } else {
        completion->result->Error(completion->error_code, completion->error);
    }
}

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

// The terminal calls address a slot either with a bare integer or with a
// `terminalId` map entry. No argument means the first console, which keeps
// the single-console callers working.
int64_t terminal_id_arg(const flutter::EncodableValue* value) {
    if (value == nullptr) {
        return 1;
    }
    if (const auto* id = std::get_if<int32_t>(value)) {
        return *id;
    }
    if (const auto* id = std::get_if<int64_t>(value)) {
        return *id;
    }
    if (const auto* args = std::get_if<flutter::EncodableMap>(value)) {
        int64_t id = 1;
        if (map_int(*args, "terminalId", &id)) {
            return id;
        }
    }
    return 1;
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

std::filesystem::path ticket_seed_path() {
    const std::filesystem::path dir = app_data_chast_dir();
    return dir.empty() ? std::filesystem::path() : dir / L"access-ticket.key";
}

// Loads the viewer's Ed25519 ticket seed, creating and persisting one on first
// use. A corrupt or foreign-profile store cannot authorize anything and is
// replaced; only the next ticket fetch is affected.
bool load_or_create_ticket_seed(std::vector<uint8_t>* seed, std::string* error) {
    const std::filesystem::path path = ticket_seed_path();
    if (path.empty()) {
        *error = "the roaming profile is not available";
        return false;
    }
    std::string stored;
    if (read_text_file(path, &stored) && !stored.empty()) {
        std::string plain;
        if (chast::unprotect_secret(stored, chast::SecretScope::CurrentUser,
                                    kTicketSeedEntropy, plain, error) &&
            plain.size() == chast::kTicketClientKeySize) {
            seed->assign(plain.begin(), plain.end());
            std::fill(plain.begin(), plain.end(), '\0');
            return true;
        }
        std::fprintf(stderr, "chast_viewer: ticket key unusable (%s); regenerating\n",
                     error->empty() ? "wrong length" : error->c_str());
        error->clear();
    }
    std::vector<uint8_t> generated;
    std::vector<uint8_t> public_key;
    if (!chast::generate_access_ticket_key(&generated, &public_key)) {
        *error = "could not generate an access-ticket key";
        return false;
    }
    const std::string plain(generated.begin(), generated.end());
    std::string sealed;
    if (!chast::protect_secret(plain, chast::SecretScope::CurrentUser,
                               kTicketSeedEntropy, sealed, error) ||
        !write_text_file(path, sealed, error)) {
        return false;
    }
    *seed = std::move(generated);
    return true;
}

bool decode_ticket_text(const std::string& text, std::vector<uint8_t>* ticket) {
    return !text.empty() && text.size() <= kMaxTicketText &&
           chast::decode_ticket_base64url(text, ticket) &&
           ticket->size() > chast::kTicketSignatureSize &&
           ticket->size() <= chast::kTicketMaxSigned;
}

}  // namespace

ViewerBridge::ViewerBridge(flutter::FlutterEngine* engine, HWND window,
    std::shared_ptr<SharedViewerSession> session, std::optional<uint32_t> display_stream)
    : engine_(engine), window_(window),
      session_(session ? std::move(session) : std::make_shared<SharedViewerSession>()),
      display_stream_(display_stream) {
    session_generation_ = session_->generation.load();
    texture_sink_ = &session_->primary;
    camera_sink_ = &session_->camera;
    viewer_ = &session_->viewer;
    if (display_stream_) session_->windows[*display_stream_] = window_;
    keyboard_capture_ = std::make_unique<KeyboardCapture>(window_,
        [this](const chast::InputEvent& event) {
            return local_keyboard_.load() ? PostLocalKey(event) : QueueInput(event);
        },
        [this] {
            if (local_keyboard_.load()) PostLocalKey({}, true);
            else ReleaseInput();
        },
        [this] { PostMessageW(window_, kKeyboardEscapeMessage, 0, 0); },
        [this](const KBDLLHOOKSTRUCT& key, bool down, const chast::InputEvent& event, bool consumed) {
            return QueueLocalKey(key, down, event, consumed);
        });
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
    SetWindowSubclass(window_, &ViewerBridge::WindowSubclassProc, kWindowSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));
    view_ = FindWindowExW(window_, nullptr, L"FLUTTERVIEW", nullptr);
    if (view_ != nullptr) {
        local_text_input_ = std::make_unique<LocalTextInput>(view_,
            [this](const std::string& text) {
                const bool accepted = session_ && session_->SendText(this,
                    session_generation_.load(), display_stream_, window_, text);
                if (!accepted && channel_) channel_->InvokeMethod("keyboardTextRejected", nullptr);
                return accepted;
            },
            [this](bool composing) { if (keyboard_capture_) keyboard_capture_->SetComposing(composing); });
        SetWindowSubclass(view_, &ViewerBridge::ViewSubclassProc, kViewSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
        DragAcceptFiles(view_, display_stream_ ? FALSE : TRUE);
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
    // A prompt still open will find its completion message dropped and reset
    // the flag itself; clearing it here covers the window already being gone.
    security_key_busy_->store(false);
    SetKeyCapture(false);
    local_text_input_.reset();
    keyboard_capture_.reset();
    SetRelativeMouse(false, 1.0);
    ClearCursor();
    if (view_ != nullptr) {
        if (IsWindow(view_)) {
            DragAcceptFiles(view_, FALSE);
            RemoveWindowSubclass(view_, &ViewerBridge::ViewSubclassProc, kViewSubclassId);
        }
        view_ = nullptr;
    }
    if (window_ != nullptr) {
        RemoveWindowSubclass(window_, &ViewerBridge::WindowSubclassProc, kWindowSubclassId);
    }
    if (channel_) {
        channel_->SetMethodCallHandler(nullptr);
    }
    if (session_ && !display_stream_) {
        session_->InvalidateWindows();
        viewer_->Stop();
        texture_sink_->SetOutput(nullptr);
        camera_sink_->SetOutput(nullptr);
    }
    if (session_) {
        session_->Detach(this);
        if (display_stream_) {
            const auto it = session_->windows.find(*display_stream_);
            if (it != session_->windows.end() && it->second == window_) session_->windows.erase(it);
        }
    }
    viewer_ = nullptr;
    texture_sink_ = nullptr;
    camera_sink_ = nullptr;
    session_.reset();
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

    if (display_stream_) {
        // Child engines can only render and control their assigned stream.
        // In particular they cannot stop, authenticate or reconfigure the session.
        if (method != "displayStreams" && method != "bindDisplayOutput" &&
            method != "takeDisplayCursors" && method != "focusDisplay" &&
            method != "status" && method != "sendInput" && method != "releaseInput" &&
            method != "setKeyCapture" && method != "keyboardState" &&
            method != "setKeyboardMode" &&
            method != "setFullscreen" && method != "closeSessionWindow") {
            result->Error("display_window", "This window shares its parent session");
            return;
        }
        if (session_generation_.load() != session_->generation.load() &&
            method != "bindDisplayOutput" && method != "releaseInput" &&
            method != "setKeyCapture" && method != "closeSessionWindow") {
            PostMessageW(window_, WM_CLOSE, 0, 0);
            result->Error("session_ended", "The parent session has ended");
            return;
        }
    }

    if (method == "openDisplayWindow") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t value = -1;
        std::string peer_id;
        if (args != nullptr) {
            map_int(*args, "stream", &value);
            if (map_string(*args, "peerId", &peer_id) && peer_id.size() > 256) {
                result->Error("peer_id", "The peer ID is too long");
                return;
            }
        }
        const auto status = viewer_->Snapshot();
        const auto found = std::find_if(status.display_streams.begin(),
                                        status.display_streams.end(),
                                        [value](const auto& s) {
                                            return s.stream.stream_id == value;
                                        });
        if (value < 0 || value > UINT32_MAX ||
            status.state != ViewerState::kStreaming ||
            !status.multiple_displays || found == status.display_streams.end()) {
            result->Success(flutter::EncodableValue(false)); return;
        }
        const auto stream = static_cast<uint32_t>(value);
        const uint32_t display_index = found->stream.display_index;
        const auto it = session_->windows.find(stream);
        if (it != session_->windows.end() && IsWindow(it->second)) {
            ShowWindow(it->second, SW_RESTORE);
            SetForegroundWindow(it->second);
            result->Success(flutter::EncodableValue(true)); return;
        }
        result->Success(flutter::EncodableValue(open_display_window && open_display_window(session_, stream, peer_id, display_index)));
        return;
    }

    if (method == "openSessionWindow") {
        const auto* payload = std::get_if<std::string>(call.arguments());
        if (!payload || !open_session_window || !open_session_window(*payload)) {
            result->Error("window_open_failed",
                          "Could not open a session window (limit 16)");
        } else {
            result->Success(flutter::EncodableValue(true));
        }
        return;
    }
    if (method == "setWindowCloseHandler") {
        const auto* enabled = std::get_if<bool>(call.arguments());
        window_close_handler_ = enabled != nullptr && *enabled;
        result->Success();
        return;
    }
    if (method == "closeSessionWindow") {
        window_close_handler_ = false;
        result->Success();
        PostMessageW(window_, WM_CLOSE, 0, 0);
        return;
    }

    if (method == "start") {
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
        bool file_transfer_only = false;
        if (args != nullptr) {
            map_bool(*args, "fileTransferOnly", &file_transfer_only);
        }
        chast::SessionMode session_mode = chast::SessionMode::Desktop;
        if (args != nullptr) {
            std::string mode_text;
            if (map_string(*args, "sessionMode", &mode_text) && !mode_text.empty()) {
                if (mode_text == "desktop") {
                    session_mode = chast::SessionMode::Desktop;
                } else if (mode_text == "file_transfer") {
                    session_mode = chast::SessionMode::FileTransfer;
                } else if (mode_text == "terminal") {
                    session_mode = chast::SessionMode::Terminal;
                } else if (mode_text == "camera") {
                    session_mode = chast::SessionMode::Camera;
                } else if (mode_text == "port_forward") {
                    session_mode = chast::SessionMode::PortForward;
                } else {
                    result->Error("bad_args", "sessionMode must be desktop, file_transfer, terminal, camera or port_forward");
                    return;
                }
            }
        }
        const bool control_only = file_transfer_only ||
                                  session_mode != chast::SessionMode::Desktop;
        // A control-only session has no picture, so it needs neither the GPU
        // sink nor a texture; a desktop session still does.
        if (!control_only &&
            (texture_sink_ == nullptr || !texture_sink_->ready())) {
            result->Error("no_gpu", texture_sink_ != nullptr
                                        ? texture_sink_->error()
                                        : "the GPU frame sink is not available");
            return;
        }
        if (args == nullptr || !map_string(*args, "host", &host) ||
            !map_int(*args, "port", &port) ||
            (!control_only && !map_int(*args, "gpuOutput", &output)) ||
            host.empty() || port <= 0 || port > 65535 || output < 0 ||
            (!control_only && output == 0)) {
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
        // Managed hosts need a signed ticket and the seed behind its client
        // public key. The key file is created by "ticketIdentity" before the
        // lookup that mints the ticket, so this normally only reads it.
        std::vector<uint8_t> access_ticket;
        std::vector<uint8_t> access_ticket_seed;
        std::string access_ticket_text;
        if (map_string(*args, "accessTicket", &access_ticket_text) &&
            !access_ticket_text.empty()) {
            if (!decode_ticket_text(access_ticket_text, &access_ticket)) {
                result->Error("bad_args", "accessTicket must be a signed access ticket");
                return;
            }
            std::string key_error;
            if (!load_or_create_ticket_seed(&access_ticket_seed, &key_error)) {
                result->Error("ticket_key", key_error);
                return;
            }
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
        if (!viewer_->Start(host, static_cast<uint16_t>(port),
                            static_cast<uint16_t>(display), std::move(fingerprint),
                            allow_unverified, std::move(access_token),
                            std::move(password), std::move(totp), identity_,
                            trust_device, transport, std::move(carrier_host),
                            static_cast<uint16_t>(carrier_port),
                            std::move(carrier_host_id), std::move(carrier_token),
                            file_transfer_only, session_mode,
                            std::move(access_ticket),
                            std::move(access_ticket_seed))) {
            result->Error("busy", "a session is already running");
            return;
        }
        // Attach only after the engine accepted the session: a rejected start
        // must leave the running session's output untouched. The handshake
        // takes longer than this call, so no frame is drawn before the attach.
        SetKeyCapture(false);
        session_->InvalidateWindows();
        session_generation_ = session_->generation.load();
        cursor_revisions_.clear();
        if (output > 0) {
            texture_sink_->SetOutput(
                reinterpret_cast<void*>(static_cast<intptr_t>(output)));
        }
        result->Success(flutter::EncodableValue(true));
        return;
    }

    if (method == "stop") {
        SetKeyCapture(false);
        session_->InvalidateWindows();
        session_generation_ = session_->generation.load();
        cursor_revisions_.clear();
        viewer_->Stop();
        // The plugin textures may be unregistered as soon as this returns.
        texture_sink_->SetOutput(nullptr);
        camera_sink_->SetOutput(nullptr);
        result->Success();
        return;
    }

    if (method == "captureDisplays") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t request_id = 0;
        chast::CaptureDisplaysRequest request;
        auto list = [&](const char* name, std::vector<uint16_t>& out) {
            const auto it = args->find(key(name));
            if (it == args->end()) return true;
            const auto* items = std::get_if<flutter::EncodableList>(&it->second);
            if (!items || items->size() > chast::kMaxDisplayCount) return false;
            for (const auto& item : *items) {
                const auto* index = std::get_if<int32_t>(&item);
                if (!index || *index < 0 || *index >= chast::kMaxDisplayCount) return false;
                out.push_back(static_cast<uint16_t>(*index));
            }
            return true;
        };
        if (!args || !map_int(*args, "requestId", &request_id) || request_id < 0 || request_id > UINT32_MAX ||
            !list("add", request.add) || !list("sub", request.sub) || !list("set", request.set)) {
            result->Error("bad_args", "captureDisplays requires an ID and display index lists"); return;
        }
        request.request_id = static_cast<uint32_t>(request_id);
        result->Success(flutter::EncodableValue(viewer_->CaptureDisplays(std::move(request))));
        return;
    }
    if (method == "bindDisplayOutput") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t id = 0, output = 0;
        if (!args || !map_int(*args, "streamId", &id) || !map_int(*args, "output", &output) ||
            id < 0 || id > UINT32_MAX || output < 0) {
            result->Error("bad_args", "bindDisplayOutput requires a stream ID and output pointer"); return;
        }
        const auto stream_id = static_cast<uint32_t>(id);
        if (display_stream_ && (*display_stream_ != stream_id ||
            (output != 0 && session_generation_.load() != session_->generation.load()))) {
            result->Success(flutter::EncodableValue(false)); return;
        }
        auto& sinks = session_->displays;
        auto it = sinks.find(stream_id);
        if (output == 0) {
            // Clear the borrowed plugin pointer synchronously. Dart can then
            // unregister its texture even if the worker is finishing a frame.
            if (it != sinks.end()) {
                it->second->SetOutputForOwner(this, nullptr);
                if (!it->second->HasOutputs()) {
                    viewer_->BindDisplay(stream_id, nullptr);
                    sinks.erase(it);
                }
            }
            cursor_revisions_.erase(stream_id);
            result->Success(flutter::EncodableValue(true)); return;
        }
        const auto status = viewer_->Snapshot();
        if (status.state != ViewerState::kStreaming || !status.multiple_displays ||
            std::none_of(status.display_streams.begin(), status.display_streams.end(),
                [stream_id](const auto& stream) { return stream.stream.stream_id == stream_id; })) {
            result->Success(flutter::EncodableValue(false)); return;
        }
        auto sink = it == sinks.end() ? std::make_shared<GpuFrameSink>() : it->second;
        if (!sink->ready()) { result->Error("no_gpu", sink->error()); return; }
        // The extra window only adds an output to the existing decoder's sink.
        if (it == sinks.end() && !viewer_->BindDisplay(stream_id, sink)) {
            result->Success(flutter::EncodableValue(false)); return;
        }
        sink->SetOutputForOwner(this, reinterpret_cast<void*>(static_cast<intptr_t>(output)));
        sinks[stream_id] = std::move(sink);
        result->Success(flutter::EncodableValue(true)); return;
    }
    if (method == "focusDisplay") {
        const auto* id = std::get_if<int64_t>(call.arguments());
        const auto* id32 = std::get_if<int32_t>(call.arguments());
        const int64_t value = id ? *id : id32 ? *id32 : -1;
        if (display_stream_) {
            const auto status = viewer_->Snapshot();
            result->Success(flutter::EncodableValue(value == *display_stream_ &&
                status.state == ViewerState::kStreaming && status.multiple_displays &&
                std::any_of(status.display_streams.begin(), status.display_streams.end(),
                    [value](const auto& stream) { return stream.stream.stream_id == value; })));
            return;
        }
        result->Success(flutter::EncodableValue(value >= 0 && value <= UINT32_MAX &&
            viewer_->FocusDisplay(static_cast<uint32_t>(value))));
        return;
    }
    if (method == "displayStreams") {
        const auto status = viewer_->Snapshot();
        session_->Prune(status);
        flutter::EncodableMap state;
        state[key("active")] = flutter::EncodableValue(status.multiple_displays);
        state[key("requestId")] = flutter::EncodableValue(static_cast<int64_t>(status.display_subscription_request));
        state[key("results")] = flutter::EncodableValue(static_cast<int64_t>(status.display_subscription_results));
        state[key("error")] = flutter::EncodableValue(status.display_subscription_error);
        const auto focused = display_stream_ ? display_stream_ : status.focused_display;
        state[key("focused")] = flutter::EncodableValue(focused ? static_cast<int64_t>(*focused) : int64_t(-1));
        flutter::EncodableList streams;
        for (const auto& stream : status.display_streams) {
            if (display_stream_ && stream.stream.stream_id != *display_stream_) continue;
            const auto& info = stream.stream.info;
            flutter::EncodableMap item;
            item[key("streamId")] = flutter::EncodableValue(static_cast<int64_t>(stream.stream.stream_id));
            item[key("displayIndex")] = flutter::EncodableValue(static_cast<int32_t>(stream.stream.display_index));
            item[key("width")] = flutter::EncodableValue(static_cast<int32_t>(info.width));
            item[key("height")] = flutter::EncodableValue(static_cast<int32_t>(info.height));
            item[key("sourceWidth")] = flutter::EncodableValue(static_cast<int32_t>(info.source_width));
            item[key("sourceHeight")] = flutter::EncodableValue(static_cast<int32_t>(info.source_height));
            item[key("originX")] = flutter::EncodableValue(info.origin_x);
            item[key("originY")] = flutter::EncodableValue(info.origin_y);
            item[key("published")] = flutter::EncodableValue(static_cast<int64_t>(stream.published));
            item[key("decoded")] = flutter::EncodableValue(static_cast<int64_t>(stream.decoded));
            item[key("dropped")] = flutter::EncodableValue(static_cast<int64_t>(stream.dropped));
            item[key("bound")] = flutter::EncodableValue(stream.bound);
            item[key("ready")] = flutter::EncodableValue(stream.ready);
            item[key("error")] = flutter::EncodableValue(stream.error);
            streams.emplace_back(std::move(item));
        }
        state[key("streams")] = flutter::EncodableValue(std::move(streams));
        result->Success(flutter::EncodableValue(std::move(state))); return;
    }
    if (method == "takeDisplayCursors") {
        flutter::EncodableList updates;
        session_->CollectCursors();
        for (const auto& [stream_id, cached] : session_->cursors) {
            if ((display_stream_ && stream_id != *display_stream_) ||
                cursor_revisions_[stream_id] == cached.revision) continue;
            cursor_revisions_[stream_id] = cached.revision;
            const auto& cursor = cached.value;
            flutter::EncodableMap item;
            item[key("streamId")] = flutter::EncodableValue(static_cast<int64_t>(stream_id));
            item[key("visible")] = flutter::EncodableValue(cursor.visible);
            item[key("x")] = flutter::EncodableValue(cursor.x); item[key("y")] = flutter::EncodableValue(cursor.y);
            item[key("width")] = flutter::EncodableValue(static_cast<int32_t>(cursor.width));
            item[key("height")] = flutter::EncodableValue(static_cast<int32_t>(cursor.height));
            item[key("hotspotX")] = flutter::EncodableValue(static_cast<int32_t>(cursor.hotspot_x));
            item[key("hotspotY")] = flutter::EncodableValue(static_cast<int32_t>(cursor.hotspot_y));
            item[key("shapeId")] = flutter::EncodableValue(static_cast<int64_t>(cursor.shape_id));
            item[key("bgra")] = flutter::EncodableValue(cursor.sprite);
            updates.emplace_back(std::move(item));
        }
        result->Success(flutter::EncodableValue(std::move(updates))); return;
    }
    if (method == "takeRemoteCursor") {
        // The view-only page polls this at pointer cadence: the newest
        // position every time, the sprite only when a shape arrives.
        DrainCursorUpdates();
        if (!have_cursor_) {
            result->Success(); return;
        }
        flutter::EncodableMap item;
        item[key("visible")] = flutter::EncodableValue(last_cursor_.visible);
        item[key("x")] = flutter::EncodableValue(last_cursor_.x);
        item[key("y")] = flutter::EncodableValue(last_cursor_.y);
        item[key("width")] = flutter::EncodableValue(static_cast<int32_t>(last_cursor_.width));
        item[key("height")] = flutter::EncodableValue(static_cast<int32_t>(last_cursor_.height));
        item[key("hotspotX")] =
            flutter::EncodableValue(static_cast<int32_t>(last_cursor_.hotspot_x));
        item[key("hotspotY")] =
            flutter::EncodableValue(static_cast<int32_t>(last_cursor_.hotspot_y));
        item[key("shapeId")] =
            flutter::EncodableValue(static_cast<int64_t>(last_cursor_.shape_id));
        if (cursor_sprite_sent_ != cursor_sprite_version_ &&
            cursor_sprite_shape_ == last_cursor_.shape_id) {
            item[key("bgra")] = flutter::EncodableValue(cursor_sprite_);
            cursor_sprite_sent_ = cursor_sprite_version_;
        }
        result->Success(flutter::EncodableValue(std::move(item))); return;
    }

    if (method == "status") {
        const ViewerStatus status = viewer_->Snapshot();
        if (key_capture_ && (status.state != ViewerState::kStreaming || !status.input_enabled))
            SetKeyCapture(false);
        flutter::EncodableMap map;
        map[key("state")] = flutter::EncodableValue(std::string(state_name(status.state)));
        map[key("error")] = flutter::EncodableValue(status.error);
        map[key("host")] = flutter::EncodableValue(status.host);
        map[key("port")] = flutter::EncodableValue(static_cast<int32_t>(status.port));
        map[key("transport")] = flutter::EncodableValue(status.transport);
        map[key("transportError")] = flutter::EncodableValue(status.transport_error);
        map[key("codec")] = flutter::EncodableValue(std::string(
            status.codec == chast::VideoCodec::H264
                ? "h264"
                : (status.codec == chast::VideoCodec::Hevc ? "hevc" : status.codec == chast::VideoCodec::Vp9 ? "vp9" : "av1")));
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
        map[key("filesOnly")] = flutter::EncodableValue(status.files_only);
        map[key("controlOnly")] = flutter::EncodableValue(status.control_only);
        map[key("sessionMode")] = flutter::EncodableValue(status.session_mode);
        map[key("fullscreen")] = flutter::EncodableValue(fullscreen_);
        map[key("inputEnabled")] = flutter::EncodableValue(status.input_enabled);
        map[key("permissions")] =
            flutter::EncodableValue(static_cast<int64_t>(status.permissions));
        map[key("permissionUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.permission_updates));
        map[key("originX")] = flutter::EncodableValue(static_cast<int32_t>(status.origin_x));
        map[key("originY")] = flutter::EncodableValue(static_cast<int32_t>(status.origin_y));
        map[key("inputSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.input_sent));
        map[key("inputFailed")] =
            flutter::EncodableValue(static_cast<int64_t>(status.input_failed));
        map[key("clipboardEnabled")] = flutter::EncodableValue(status.clipboard_enabled);
        map[key("clipboardDirection")] = flutter::EncodableValue(static_cast<int>(status.clipboard_direction));
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
        map[key("fileCancels")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_cancels));
        map[key("downloadPhase")] =
            flutter::EncodableValue(static_cast<int32_t>(status.download_phase));
        map[key("downloadName")] = flutter::EncodableValue(status.download_name);
        map[key("downloadPath")] = flutter::EncodableValue(status.download_path);
        map[key("downloadSize")] =
            flutter::EncodableValue(static_cast<int64_t>(status.download_size));
        map[key("downloadReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.download_received));
        map[key("downloadError")] = flutter::EncodableValue(status.download_error);
        map[key("filesReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.files_received));
        map[key("fileReceiveFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_receive_failures));
        map[key("downloadCancels")] =
            flutter::EncodableValue(static_cast<int64_t>(status.download_cancels));
        map[key("downloadCancellable")] =
            flutter::EncodableValue(status.download_cancellable);
        map[key("fileConflictPending")] =
            flutter::EncodableValue(status.file_conflict_pending);
        map[key("fileConflictDownload")] =
            flutter::EncodableValue(status.file_conflict_download);
        map[key("fileConflictTransferId")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_conflict_transfer_id));
        map[key("fileConflictName")] =
            flutter::EncodableValue(status.file_conflict_name);
        map[key("fileConflictSize")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_conflict_size));
        map[key("fileConflictIdentical")] =
            flutter::EncodableValue(status.file_conflict_identical);
        map[key("fileConflictExistingSize")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_conflict_existing_size));
        map[key("fileConflictExistingTime")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_conflict_existing_time));
        map[key("fileConflictPolicy")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_conflict_policy));
        map[key("fileConflicts")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_conflicts));
        map[key("filesSkipped")] =
            flutter::EncodableValue(static_cast<int64_t>(status.files_skipped));
        map[key("downloadsSkipped")] =
            flutter::EncodableValue(static_cast<int64_t>(status.downloads_skipped));
        map[key("fileListAnswers")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_list_answers));
        map[key("fileListDirectory")] =
            flutter::EncodableValue(status.file_list_directory);
        map[key("fileListEntries")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_list_entries));
        map[key("fileListTruncated")] = flutter::EncodableValue(status.file_list_truncated);
        map[key("fileListError")] = flutter::EncodableValue(status.file_list_error);
        map[key("fileDownloadAnswers")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_download_answers));
        map[key("fileDownloadCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_download_code));
        map[key("fileDownloadTransferId")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_download_transfer_id));
        map[key("fileDownloadPath")] = flutter::EncodableValue(status.file_download_path);
        map[key("fileDownloadError")] = flutter::EncodableValue(status.file_download_error);
        map[key("fileOpAnswers")] =
            flutter::EncodableValue(static_cast<int64_t>(status.file_op_answers));
        map[key("fileOpCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.file_op_code));
        map[key("fileOpPath")] = flutter::EncodableValue(status.file_op_path);
        map[key("fileOpError")] = flutter::EncodableValue(status.file_op_error);
        map[key("folderDownloadActive")] =
            flutter::EncodableValue(status.folder_download_active);
        map[key("folderDownloadName")] =
            flutter::EncodableValue(status.folder_download_name);
        map[key("folderDownloadDestination")] =
            flutter::EncodableValue(status.folder_download_destination);
        map[key("folderDownloadFilesTotal")] =
            flutter::EncodableValue(static_cast<int64_t>(status.folder_download_files_total));
        map[key("folderDownloadFilesDone")] =
            flutter::EncodableValue(static_cast<int64_t>(status.folder_download_files_done));
        map[key("folderDownloadRoots")] =
            flutter::EncodableValue(static_cast<int64_t>(status.folder_download_roots));
        map[key("folderDownloadBatch")] =
            flutter::EncodableValue(status.folder_download_batch);
        map[key("folderDownloadCurrent")] =
            flutter::EncodableValue(status.folder_download_current);
        map[key("folderDownloadAnswers")] =
            flutter::EncodableValue(static_cast<int64_t>(status.folder_download_answers));
        map[key("folderDownloadCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.folder_download_code));
        map[key("folderDownloadError")] =
            flutter::EncodableValue(status.folder_download_error);
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
        map[key("screenshotUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.screenshot_updates));
        map[key("screenshotPath")] = flutter::EncodableValue(status.screenshot_path);
        map[key("screenshotError")] = flutter::EncodableValue(status.screenshot_error);
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
        map[key("audioStarvedMs")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_starved_ms));
        map[key("audioDropped")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_dropped));
        map[key("audioRecoveries")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_recoveries));
        map[key("audioRecovering")] = flutter::EncodableValue(status.audio_recovering);
        map[key("audioBufferMs")] =
            flutter::EncodableValue(static_cast<int64_t>(status.audio_buffer_ms));
        map[key("audioLatencyMs")] = flutter::EncodableValue(status.audio_latency_ms);
        map[key("avSkewMs")] = flutter::EncodableValue(status.av_skew_ms);
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
        map[key("voiceAuthorized")] = flutter::EncodableValue(status.voice_authorized);
        map[key("voiceEnabled")] = flutter::EncodableValue(status.voice_enabled);
        map[key("voicePending")] = flutter::EncodableValue(status.voice_pending);
        map[key("voicePackets")] =
            flutter::EncodableValue(static_cast<int64_t>(status.voice_packets));
        map[key("voiceDropped")] =
            flutter::EncodableValue(static_cast<int64_t>(status.voice_dropped));
        map[key("voiceErrors")] =
            flutter::EncodableValue(static_cast<int64_t>(status.voice_errors));
        map[key("voiceError")] = flutter::EncodableValue(status.voice_error);
        map[key("privacySupported")] = flutter::EncodableValue(status.privacy_supported);
        map[key("privacyEnabled")] = flutter::EncodableValue(status.privacy_enabled);
        map[key("privacyCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.privacy_code));
        map[key("privacyError")] = flutter::EncodableValue(status.privacy_error);
        map[key("privacyUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.privacy_updates));
        map[key("displayControlSupported")] =
            flutter::EncodableValue(status.display_control.supported);
        map[key("displayControlManaged")] =
            flutter::EncodableValue(status.display_control.managed);
        map[key("displayControlCount")] =
            flutter::EncodableValue(static_cast<int>(status.display_control.count));
        map[key("displayControlPrivacy")] =
            flutter::EncodableValue(static_cast<int>(status.display_control.privacy));
        map[key("displayControlCode")] =
            flutter::EncodableValue(static_cast<int>(status.display_control.code));
        map[key("displayControlError")] =
            flutter::EncodableValue(status.display_control.reason);
        map[key("displayControlUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_control_updates));
        flutter::EncodableList controlled_outputs;
        for (const auto& output : status.display_control.outputs) {
            controlled_outputs.emplace_back(flutter::EncodableMap{
                {key("id"), flutter::EncodableValue(static_cast<int64_t>(output.id))},
                {key("active"), flutter::EncodableValue(output.active)},
            });
        }
        map[key("displayControlOutputs")] =
            flutter::EncodableValue(std::move(controlled_outputs));
        map[key("displayControlBusy")] =
            flutter::EncodableValue(status.display_control_busy);
        map[key("desktop")] =
            flutter::EncodableValue(static_cast<int32_t>(status.desktop));
        map[key("desktopUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.desktop_updates));
        map[key("sasCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.sas_code));
        map[key("sasError")] = flutter::EncodableValue(status.sas_error);
        map[key("sasUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.sas_updates));
        map[key("restartSupported")] =
            flutter::EncodableValue(status.restart_supported);
        map[key("blockInputSupported")] =
            flutter::EncodableValue(status.block_input_supported);
        map[key("blockInputActive")] =
            flutter::EncodableValue(status.block_input_active);
        map[key("hostActionAction")] =
            flutter::EncodableValue(static_cast<int32_t>(status.host_action_action));
        map[key("hostActionCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.host_action_code));
        map[key("hostActionError")] =
            flutter::EncodableValue(status.host_action_error);
        map[key("hostActionUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.host_action_updates));
        map[key("osLoginSupported")] =
            flutter::EncodableValue(status.os_login_supported);
        map[key("osLoginCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.os_login_code));
        map[key("osLoginError")] = flutter::EncodableValue(status.os_login_error);
        map[key("osLoginUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.os_login_updates));
        map[key("elevationCode")] = flutter::EncodableValue(static_cast<int32_t>(status.elevation.code));
        map[key("elevationRequestId")] = flutter::EncodableValue(static_cast<int64_t>(status.elevation.request_id));
        map[key("elevationReason")] = flutter::EncodableValue(status.elevation.reason);
        map[key("elevationUpdates")] = flutter::EncodableValue(static_cast<int64_t>(status.elevation_updates));
        map[key("sessionSelectSupported")] =
            flutter::EncodableValue(status.session_select_supported);
        map[key("sessionListCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.session_list_code));
        map[key("sessionListError")] = flutter::EncodableValue(status.session_list_error);
        map[key("sessionListUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.session_list_updates));
        map[key("sessionSelectId")] =
            flutter::EncodableValue(static_cast<int32_t>(status.session_select_id));
        map[key("sessionSelectCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.session_select_code));
        map[key("sessionSelectError")] =
            flutter::EncodableValue(status.session_select_error);
        map[key("sessionSelectUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.session_select_updates));
        {
            flutter::EncodableList sessions;
            for (const chast::RemoteSession& session : status.sessions) {
                flutter::EncodableMap entry;
                entry[key("id")] =
                    flutter::EncodableValue(static_cast<int32_t>(session.id));
                entry[key("user")] = flutter::EncodableValue(session.user);
                entry[key("active")] = flutter::EncodableValue(session.active);
                entry[key("current")] = flutter::EncodableValue(session.current);
                sessions.push_back(flutter::EncodableValue(std::move(entry)));
            }
            map[key("sessions")] = flutter::EncodableValue(std::move(sessions));
        }
        map[key("chatSent")] =
            flutter::EncodableValue(static_cast<int64_t>(status.chat_sent));
        map[key("chatReceived")] =
            flutter::EncodableValue(static_cast<int64_t>(status.chat_received));
        map[key("chatFailures")] =
            flutter::EncodableValue(static_cast<int64_t>(status.chat_failures));
        map[key("chatError")] = flutter::EncodableValue(status.chat_error);
        map[key("terminalSupported")] =
            flutter::EncodableValue(status.terminal_supported);
        map[key("terminalPersistSupported")] =
            flutter::EncodableValue(status.terminal_persist_supported);
        map[key("terminalServiceId")] =
            flutter::EncodableValue(static_cast<int64_t>(status.terminal_service_id));
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
        {
            // One entry per console slot; the legacy scalars above stay as the
            // slot 1 mirror so an older page keeps working.
            flutter::EncodableList terminals;
            for (size_t i = 0; i < status.terminals.size(); ++i) {
                const ViewerTerminalStatus& terminal = status.terminals[i];
                flutter::EncodableMap entry;
                entry[key("id")] =
                    flutter::EncodableValue(static_cast<int32_t>(i + 1));
                entry[key("phase")] =
                    flutter::EncodableValue(static_cast<int32_t>(terminal.phase));
                entry[key("code")] =
                    flutter::EncodableValue(static_cast<int32_t>(terminal.code));
                entry[key("reason")] = flutter::EncodableValue(terminal.reason);
                entry[key("error")] = flutter::EncodableValue(terminal.error);
                entry[key("columns")] =
                    flutter::EncodableValue(static_cast<int32_t>(terminal.columns));
                entry[key("rows")] =
                    flutter::EncodableValue(static_cast<int32_t>(terminal.rows));
                entry[key("bytes")] =
                    flutter::EncodableValue(static_cast<int64_t>(terminal.bytes));
                entry[key("updates")] =
                    flutter::EncodableValue(static_cast<int64_t>(terminal.updates));
                entry[key("serviceId")] =
                    flutter::EncodableValue(static_cast<int64_t>(terminal.service_id));
                terminals.push_back(flutter::EncodableValue(std::move(entry)));
            }
            map[key("terminals")] = flutter::EncodableValue(std::move(terminals));
        }
        map[key("cameraSupported")] =
            flutter::EncodableValue(status.camera_supported);
        map[key("cameraPhase")] =
            flutter::EncodableValue(static_cast<int32_t>(status.camera_phase));
        map[key("cameraCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.camera_code));
        map[key("cameraReason")] = flutter::EncodableValue(status.camera_reason);
        map[key("cameraError")] = flutter::EncodableValue(status.camera_error);
        map[key("cameraId")] =
            flutter::EncodableValue(static_cast<int32_t>(status.camera_id));
        map[key("cameraListUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.camera_list_updates));
        map[key("cameraWidth")] =
            flutter::EncodableValue(static_cast<int32_t>(status.camera_width));
        map[key("cameraHeight")] =
            flutter::EncodableValue(static_cast<int32_t>(status.camera_height));
        map[key("cameraFrames")] =
            flutter::EncodableValue(static_cast<int64_t>(status.camera_frames));
        map[key("cameraDecoder")] = flutter::EncodableValue(status.camera_decoder);
        map[key("cameraUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.camera_updates));
        {
            flutter::EncodableList cameras;
            for (const chast::CameraDescription& camera : status.cameras) {
                flutter::EncodableMap entry;
                entry[key("id")] =
                    flutter::EncodableValue(static_cast<int32_t>(camera.id));
                entry[key("name")] = flutter::EncodableValue(camera.name);
                entry[key("default")] = flutter::EncodableValue(camera.is_default);
                cameras.push_back(flutter::EncodableValue(std::move(entry)));
            }
            map[key("cameras")] = flutter::EncodableValue(std::move(cameras));
        }
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
        map[key("displayFollowFlags")] =
            flutter::EncodableValue(static_cast<int32_t>(status.display_follow_flags));
        map[key("displayFollowCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.display_follow_code));
        map[key("displayFollowError")] =
            flutter::EncodableValue(status.display_follow_error);
        map[key("displayFollowResults")] =
            flutter::EncodableValue(static_cast<int64_t>(status.display_follow_results));
        map[key("chroma")] = flutter::EncodableValue(static_cast<int>(status.chroma));
        map[key("streamQuality")] =
            flutter::EncodableValue(static_cast<int32_t>(status.stream_quality));
        map[key("streamQualityPercent")] =
            flutter::EncodableValue(static_cast<int32_t>(status.stream_quality_percent));
        map[key("streamOptionCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.stream_option_code));
        map[key("streamOptionError")] =
            flutter::EncodableValue(status.stream_option_error);
        map[key("streamOptionResults")] =
            flutter::EncodableValue(static_cast<int64_t>(status.stream_option_results));
        map[key("remoteConfigSupported")] =
            flutter::EncodableValue(status.remote_config_supported);
        map[key("remoteConfigFlags")] =
            flutter::EncodableValue(static_cast<int64_t>(status.remote_config_flags));
        map[key("remoteConfigCode")] =
            flutter::EncodableValue(static_cast<int32_t>(status.remote_config_code));
        map[key("remoteConfigError")] =
            flutter::EncodableValue(status.remote_config_error);
        map[key("remoteConfigUpdates")] =
            flutter::EncodableValue(static_cast<int64_t>(status.remote_config_updates));
        map[key("portForwardEnabled")] =
            flutter::EncodableValue(status.port_forward_enabled);
        flutter::EncodableList forward_rules;
        for (const ViewerPortForwardRule& rule : status.port_forward_rules) {
            flutter::EncodableMap entry;
            entry[key("ruleId")] =
                flutter::EncodableValue(static_cast<int32_t>(rule.rule_id));
            entry[key("active")] = flutter::EncodableValue(rule.active);
            entry[key("localPort")] =
                flutter::EncodableValue(static_cast<int32_t>(rule.local_port));
            entry[key("remoteHost")] = flutter::EncodableValue(rule.remote_host);
            entry[key("remotePort")] =
                flutter::EncodableValue(static_cast<int32_t>(rule.remote_port));
            entry[key("connections")] =
                flutter::EncodableValue(static_cast<int64_t>(rule.connections));
            entry[key("failures")] =
                flutter::EncodableValue(static_cast<int64_t>(rule.failures));
            entry[key("bytesUp")] =
                flutter::EncodableValue(static_cast<int64_t>(rule.bytes_up));
            entry[key("bytesDown")] =
                flutter::EncodableValue(static_cast<int64_t>(rule.bytes_down));
            entry[key("error")] = flutter::EncodableValue(rule.error);
            forward_rules.push_back(flutter::EncodableValue(entry));
        }
        map[key("portForwardRules")] = flutter::EncodableValue(forward_rules);
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

    if (method == "setKeyboardMode") {
        const auto* mode = std::get_if<std::string>(call.arguments());
        if (!mode || (*mode != "physical" && *mode != "legacy" && *mode != "local")) {
            result->Error("bad_args", "keyboard mode must be physical, legacy or local"); return;
        }
        if (!SetKeyboardMode(*mode)) {
            result->Error("keyboard_unavailable", "The local input method is not available"); return;
        }
        result->Success(); return;
    }
    if (method == "setKeyCapture") {
        const auto* enable = std::get_if<bool>(call.arguments());
        if (enable == nullptr) {
            result->Error("bad_args", "setKeyCapture wants a bool");
            return;
        }
        SetKeyCapture(*enable);
        if (*enable && local_keyboard_ && (!key_capture_ || !keyboard_capture_ ||
            !keyboard_capture_->enabled())) {
            result->Error("keyboard_unavailable", "The local input method is not available"); return;
        }
        result->Success(
            flutter::EncodableValue(keyboard_capture_ && keyboard_capture_->enabled()));
        return;
    }

    if (method == "setRelativeMouse") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        bool enable = false;
        int64_t scale_milli = 1000;
        if (args == nullptr || !map_bool(*args, "enabled", &enable) ||
            !map_int(*args, "scaleMilli", &scale_milli) || scale_milli <= 0 ||
            scale_milli > 100000) {
            result->Error("bad_args",
                          "setRelativeMouse wants enabled and a positive scaleMilli");
            return;
        }
        SetRelativeMouse(enable, static_cast<double>(scale_milli) / 1000.0);
        result->Success(
            flutter::EncodableValue(relative_mouse_.load(std::memory_order_relaxed)));
        return;
    }

    if (method == "releaseInput") {
        const bool queued = ReleaseInput();
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "keyboardState") {
        if (!keyboard_capture_ || !keyboard_capture_->available()) {
            result->Error("unavailable", "native keyboard capture is unavailable");
            return;
        }
        result->Success(flutter::EncodableValue(static_cast<int32_t>(
            keyboard_capture_->InputState())));
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

    if (method == "setDisplayFollow") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        bool cursor = false;
        bool window = false;
        if (args == nullptr || !map_bool(*args, "cursor", &cursor) ||
            !map_bool(*args, "window", &window)) {
            result->Error("bad_args", "setDisplayFollow wants cursor and window booleans");
            return;
        }
        const bool queued =
            viewer_ != nullptr && viewer_->SetDisplayFollow(cursor, window);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "showCursor") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t x = 0;
        int64_t y = 0;
        bool visible = false;
        if (args == nullptr || !map_int(*args, "x", &x) || !map_int(*args, "y", &y) ||
            !map_bool(*args, "visible", &visible) || x < INT32_MIN || x > INT32_MAX ||
            y < INT32_MIN || y > INT32_MAX) {
            result->Error("bad_args", "showCursor wants x, y and visible");
            return;
        }
        const bool queued = viewer_ != nullptr && viewer_->ShowCursor(
            static_cast<int32_t>(x), static_cast<int32_t>(y), visible);
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
        int64_t quality = 0, chroma = 0, quality_percent = 0;
        map_int(*args, "chroma", &chroma);
        map_int(*args, "qualityPercent", &quality_percent);
        if (!map_int(*args, "codec", &codec) || !map_int(*args, "maxFps", &max_fps) ||
            !map_int(*args, "quality", &quality) || codec < 0 || codec > 4 || chroma < 0 || chroma > 1 ||
            max_fps < 0 || max_fps > chast::kMaxStreamFps || quality < 0 || quality > 3 ||
            quality_percent < 0 || quality_percent > chast::kMaxCustomQualityPercent ||
            (quality == 3 && quality_percent < chast::kMinCustomQualityPercent)) {
            result->Error("bad_args",
                          "setStreamOptions wants codec in 0..4, chroma in 0..1 and quality in 0..3 with "
                          "qualityPercent in 10..100 for custom quality, and maxFps in 0..240");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->SetStreamOptions(static_cast<chast::StreamCodecPreference>(codec),
                                      static_cast<uint16_t>(max_fps),
                                      static_cast<chast::StreamQuality>(quality), static_cast<chast::VideoChroma>(chroma),
                                      static_cast<uint8_t>(quality_percent));
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
            result->Error("unavailable", "recording needs a live session with the host's consent");
            return;
        }
        result->Success(flutter::EncodableValue(enabled));
        return;
    }

    if (method == "takeScreenshot") {
        if (viewer_ == nullptr) {
            result->Error("unavailable", "no viewer session is running");
            return;
        }
        std::string directory;
        if (const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
            args != nullptr) {
            if (const auto it = args->find(flutter::EncodableValue("directory"));
                it != args->end()) {
                if (const auto* value = std::get_if<std::string>(&it->second)) {
                    directory = *value;
                }
            }
        }
        if (!viewer_->SaveScreenshot(std::move(directory))) {
            result->Error("unavailable", "a screenshot needs a live picture stream");
            return;
        }
        result->Success(flutter::EncodableValue(true));
        return;
    }

    if (method == "sendFile" || method == "sendFileAt" || method == "sendFileTo") {
        if (viewer_ == nullptr) {
            result->Error("unavailable", "no viewer session is running");
            return;
        }
        const auto start_upload = [this, &result](const std::vector<std::string>& paths,
                                                  const std::string& target = {}) {
            if (paths.empty() || !viewer_->SendFiles(paths, target)) {
                result->Error("unavailable",
                              "the files cannot be queued now (session ended or not permitted)");
                return;
            }
            result->Success(flutter::EncodableValue(static_cast<int32_t>(paths.size())));
        };
        // sendFileTo carries the directory being browsed, so picked files land
        // in that shared subfolder instead of the receive root.
        std::string target;
        if (method == "sendFileTo") {
            const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
            if (args == nullptr) {
                result->Error("bad_args", "sendFileTo wants a directory argument");
                return;
            }
            const auto directory = args->find(flutter::EncodableValue("directory"));
            if (directory == args->end() ||
                std::get_if<std::string>(&directory->second) == nullptr) {
                result->Error("bad_args", "sendFileTo wants a directory argument");
                return;
            }
            target = *std::get_if<std::string>(&directory->second);
        }
        if (method == "sendFileAt") {
            // The paths come from a drop on the window, so they are already
            // absolute; SendFiles expands folders and skips anything else.
            // The map form adds the directory being browsed so a drop lands
            // beside the picker's uploads instead of always in the root.
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
            } else if (const auto* args =
                           std::get_if<flutter::EncodableMap>(call.arguments())) {
                const auto directory = args->find(flutter::EncodableValue("directory"));
                if (directory != args->end() &&
                    std::get_if<std::string>(&directory->second) != nullptr) {
                    target = *std::get_if<std::string>(&directory->second);
                }
                const auto dropped = args->find(flutter::EncodableValue("paths"));
                if (dropped != args->end()) {
                    if (const auto* items =
                            std::get_if<flutter::EncodableList>(&dropped->second)) {
                        for (const flutter::EncodableValue& item : *items) {
                            if (const auto* item_path = std::get_if<std::string>(&item)) {
                                paths.push_back(*item_path);
                            }
                        }
                    }
                }
            }
            if (paths.empty()) {
                result->Error("bad_args", "sendFileAt wants a path or a list of paths");
                return;
            }
            start_upload(paths, target);
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
        start_upload(selected_paths(selected.data()), target);
        return;
    }

    if (method == "requestFileList" || method == "requestFileDownload") {
        const auto* path = std::get_if<std::string>(call.arguments());
        std::string cursor;
        bool show_hidden = false;
        if (method == "requestFileList") {
            if (const auto* args = std::get_if<flutter::EncodableMap>(call.arguments())) {
                const auto directory = args->find(key("directory"));
                if (directory != args->end()) path = std::get_if<std::string>(&directory->second);
                const auto after = args->find(key("cursor"));
                if (after != args->end()) {
                    const auto* value = std::get_if<std::string>(&after->second);
                    if (!value) { result->Error("bad_args", "cursor must be a string"); return; }
                    cursor = *value;
                }
                const auto hidden = args->find(key("showHidden"));
                if (hidden != args->end()) {
                    const auto* value = std::get_if<bool>(&hidden->second);
                    if (!value) { result->Error("bad_args", "showHidden must be a bool"); return; }
                    show_hidden = *value;
                }
            }
        }
        if (path == nullptr) {
            result->Error("bad_args", "the request wants a '/'-separated relative path");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            (method == "requestFileList"
                 ? viewer_->RequestFileList(*path, std::move(cursor), show_hidden)
                 : viewer_->RequestFileDownload(*path));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "downloadFolder") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        std::string path;
        bool show_hidden = false;
        if (args != nullptr) {
            const auto directory = args->find(key("path"));
            const auto* value =
                directory == args->end() ? nullptr : std::get_if<std::string>(&directory->second);
            if (value == nullptr) {
                result->Error("bad_args", "downloadFolder wants a '/'-separated relative path");
                return;
            }
            path = *value;
            const auto hidden = args->find(key("showHidden"));
            if (hidden != args->end()) {
                const auto* flag = std::get_if<bool>(&hidden->second);
                if (!flag) { result->Error("bad_args", "showHidden must be a bool"); return; }
                show_hidden = *flag;
            }
        } else if (const auto* plain = std::get_if<std::string>(call.arguments())) {
            path = *plain;
        } else {
            result->Error("bad_args", "downloadFolder wants a '/'-separated relative path");
            return;
        }
        const bool queued =
            viewer_ != nullptr && viewer_->RequestDirectoryDownload(path, show_hidden);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "downloadEntries") {
        std::vector<ViewerEngine::DownloadRoot> roots;
        bool show_hidden = false;
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "downloadEntries wants a list of entries");
            return;
        }
        const auto entries = args->find(key("entries"));
        const auto* items =
            entries == args->end() ? nullptr : std::get_if<flutter::EncodableList>(&entries->second);
        if (items == nullptr) {
            result->Error("bad_args", "downloadEntries wants a list of entries");
            return;
        }
        const auto hidden = args->find(key("showHidden"));
        if (hidden != args->end()) {
            const auto* flag = std::get_if<bool>(&hidden->second);
            if (!flag) { result->Error("bad_args", "showHidden must be a bool"); return; }
            show_hidden = *flag;
        }
        roots.reserve(items->size());
        for (const flutter::EncodableValue& item : *items) {
            const auto* map = std::get_if<flutter::EncodableMap>(&item);
            if (map == nullptr) {
                result->Error("bad_args", "downloadEntries wants a list of entries");
                return;
            }
            const auto path = map->find(flutter::EncodableValue("path"));
            const auto* path_value =
                path == map->end() ? nullptr : std::get_if<std::string>(&path->second);
            if (path_value == nullptr) {
                result->Error("bad_args", "downloadEntries wants a path for every entry");
                return;
            }
            const auto directory = map->find(flutter::EncodableValue("directory"));
            const bool* directory_value =
                directory == map->end() ? nullptr : std::get_if<bool>(&directory->second);
            ViewerEngine::DownloadRoot root;
            root.path = *path_value;
            root.directory = directory_value != nullptr && *directory_value;
            roots.push_back(std::move(root));
        }
        const bool queued =
            viewer_ != nullptr && viewer_->RequestBatchDownload(std::move(roots), show_hidden);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "cancelFolderDownload") {
        if (viewer_ != nullptr) {
            viewer_->CancelDirectoryDownload();
        }
        result->Success();
        return;
    }

    if (method == "cancelUploads") {
        if (viewer_ != nullptr) {
            viewer_->CancelUploads();
        }
        result->Success();
        return;
    }

    if (method == "cancelDownload") {
        if (viewer_ != nullptr) {
            viewer_->CancelDownload();
        }
        result->Success();
        return;
    }

    if (method == "answerFileConflict") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "answerFileConflict wants a decision");
            return;
        }
        const auto decision = args->find(flutter::EncodableValue("decision"));
        const auto* decision_value =
            decision == args->end() ? nullptr : std::get_if<int32_t>(&decision->second);
        if (decision_value == nullptr || *decision_value < 0 ||
            *decision_value > static_cast<int32_t>(chast::FileConflictDecision::Cancel)) {
            result->Error("bad_args", "answerFileConflict wants a decision");
            return;
        }
        bool remember = false;
        const auto remember_value = args->find(flutter::EncodableValue("remember"));
        if (remember_value != args->end()) {
            const auto* flag = std::get_if<bool>(&remember_value->second);
            if (flag == nullptr) {
                result->Error("bad_args", "remember must be a bool");
                return;
            }
            remember = *flag;
        }
        const bool accepted =
            viewer_ != nullptr &&
            viewer_->AnswerFileConflict(
                static_cast<chast::FileConflictDecision>(*decision_value), remember);
        result->Success(flutter::EncodableValue(accepted));
        return;
    }

    if (method == "fileOperation") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "fileOperation wants an operation and a path");
            return;
        }
        const auto operation = args->find(flutter::EncodableValue("operation"));
        const auto path = args->find(flutter::EncodableValue("path"));
        const auto* operation_value =
            operation == args->end() ? nullptr : std::get_if<int32_t>(&operation->second);
        const auto* path_value =
            path == args->end() ? nullptr : std::get_if<std::string>(&path->second);
        if (operation_value == nullptr || path_value == nullptr || *operation_value < 0 ||
            *operation_value > static_cast<int32_t>(chast::FileOperation::RemoveTree)) {
            result->Error("bad_args", "fileOperation wants an operation and a path");
            return;
        }
        std::string new_name;
        const auto rename = args->find(flutter::EncodableValue("newName"));
        if (rename != args->end()) {
            if (const auto* name = std::get_if<std::string>(&rename->second)) {
                new_name = *name;
            }
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->RequestFileOperation(
                static_cast<chast::FileOperation>(*operation_value), *path_value,
                std::move(new_name));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "takeFileList") {
        if (viewer_ == nullptr) {
            result->Success();
            return;
        }
        ViewerEngine::FileListAnswer answer;
        if (!viewer_->TakeFileList(answer)) {
            result->Success();
            return;
        }
        flutter::EncodableMap map;
        map[key("directory")] = flutter::EncodableValue(answer.directory);
        map[key("cursor")] = flutter::EncodableValue(answer.cursor);
        map[key("showHidden")] = flutter::EncodableValue(answer.show_hidden);
        map[key("nextCursor")] = flutter::EncodableValue(answer.result.next_cursor);
        map[key("code")] = flutter::EncodableValue(static_cast<int32_t>(answer.result.code));
        map[key("reason")] = flutter::EncodableValue(answer.result.reason);
        map[key("truncated")] = flutter::EncodableValue(answer.result.truncated);
        flutter::EncodableList entries;
        entries.reserve(answer.result.entries.size());
        for (const chast::FileListEntry& entry : answer.result.entries) {
            flutter::EncodableMap item;
            item[key("name")] = flutter::EncodableValue(entry.name);
            item[key("size")] = flutter::EncodableValue(static_cast<int64_t>(entry.size));
            item[key("modifiedMs")] =
                flutter::EncodableValue(static_cast<int64_t>(entry.modified_ms));
            item[key("directory")] = flutter::EncodableValue(entry.directory);
            entries.push_back(flutter::EncodableValue(std::move(item)));
        }
        map[key("entries")] = flutter::EncodableValue(std::move(entries));
        result->Success(flutter::EncodableValue(std::move(map)));
        return;
    }

    if (method == "takeCompletedFiles") {
        if (viewer_ == nullptr) {
            result->Success();
            return;
        }
        std::vector<ViewerEngine::CompletedFile> files;
        if (!viewer_->TakeCompletedFiles(files)) {
            result->Success();
            return;
        }
        flutter::EncodableList list;
        list.reserve(files.size());
        for (const ViewerEngine::CompletedFile& file : files) {
            flutter::EncodableMap entry;
            entry[key("upload")] = flutter::EncodableValue(file.upload);
            entry[key("name")] = flutter::EncodableValue(file.name);
            entry[key("size")] = flutter::EncodableValue(static_cast<int64_t>(file.size));
            entry[key("success")] = flutter::EncodableValue(file.success);
            entry[key("detail")] = flutter::EncodableValue(file.detail);
            entry[key("occurredAtMs")] =
                flutter::EncodableValue(static_cast<int64_t>(file.occurred_at_ms));
            list.push_back(flutter::EncodableValue(std::move(entry)));
        }
        result->Success(flutter::EncodableValue(std::move(list)));
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

    if (method == "setVoiceCall") {
        const auto* enabled = std::get_if<bool>(call.arguments());
        if (enabled == nullptr) {
            result->Error("bad_args", "setVoiceCall wants a bool");
            return;
        }
        const bool applied = viewer_ != nullptr && viewer_->SetVoiceCall(*enabled);
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

    if (method == "controlDisplays") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t action = -1, value = -1, target = 0;
        if (args && args->contains(key("target")) &&
            !map_int(*args, "target", &target)) {
            target = -1;
        }
        if (!args || !map_int(*args, "action", &action) ||
            !map_int(*args, "value", &value) || action < 0 || action > 4 ||
            value < 0 || value > 4 || target < 0 || target > UINT32_MAX) {
            result->Error("bad_args", "controlDisplays wants a valid action and value");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->ControlDisplays(static_cast<chast::DisplayControlAction>(action),
                                     static_cast<uint8_t>(value),
                                     static_cast<uint32_t>(target));
        result->Success(flutter::EncodableValue(queued));
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

    if (method == "requestHostAction") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t action = 0;
        bool block_input = false;
        if (args == nullptr || !map_int(*args, "action", &action) ||
            (action != 1 && action != 2 && action != 3)) {
            result->Error("bad_args", "requestHostAction wants action 1, 2 or 3");
            return;
        }
        if (!map_bool(*args, "blockInput", &block_input)) {
            block_input = false;
        }
        if (action != 3 && block_input) {
            result->Error("bad_args", "only block input carries blockInput");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->RequestHostAction(static_cast<chast::HostAction>(action),
                                       block_input);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "requestOsLogin") {
        const auto* password = std::get_if<std::string>(call.arguments());
        if (password == nullptr || password->empty()) {
            result->Error("bad_args", "requestOsLogin wants the OS password");
            return;
        }
        const bool queued = viewer_ != nullptr && viewer_->RequestOsLogin(*password);
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "requestElevation") {
        result->Success(flutter::EncodableValue(viewer_ != nullptr && viewer_->RequestElevation()));
        return;
    }
    if (method == "requestSessionList") {
        const bool queued = viewer_ != nullptr && viewer_->RequestSessionList();
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "requestSessionSelect") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t id = 0;
        if (args == nullptr || !map_int(*args, "id", &id) || id <= 0 ||
            id >= 0xFFFFFFFFLL) {
            result->Error("bad_args", "requestSessionSelect wants a session id");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->RequestSessionSelect(static_cast<uint32_t>(id));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "requestRemoteConfig") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t keys = 0;
        int64_t values = 0;
        if (args == nullptr || !map_int(*args, "keys", &keys) ||
            !map_int(*args, "values", &values) || keys < 0 ||
            keys > static_cast<int64_t>(chast::kRemoteConfigAll) || values < 0 ||
            values > static_cast<int64_t>(chast::kRemoteConfigAll)) {
            result->Error("bad_args", "requestRemoteConfig wants keys and values masks");
            return;
        }
        const bool queued =
            viewer_ != nullptr &&
            viewer_->RequestRemoteConfig(static_cast<uint32_t>(keys),
                                         static_cast<uint32_t>(values));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "sendText") {
        const auto* text = std::get_if<std::string>(call.arguments());
        if (text == nullptr) {
            result->Error("bad_args", "sendText wants the composed text");
            return;
        }
        const bool queued = viewer_ != nullptr && viewer_->SendText(*text);
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
        // A non-zero serviceId reattaches to a console the host kept; it is
        // optional so older callers keep opening a fresh console. terminalId
        // picks the slot and defaults to the first console.
        int64_t service_id = 0;
        int64_t terminal_id = 1;
        const bool has_service_id =
            args != nullptr && args->find(key("serviceId")) != args->end();
        const bool has_terminal_id =
            args != nullptr && args->find(key("terminalId")) != args->end();
        if (args == nullptr || !map_int(*args, "columns", &columns) ||
            !map_int(*args, "rows", &rows) || columns <= 0 || columns > 500 ||
            rows <= 0 || rows > 200 ||
            (has_service_id && (!map_int(*args, "serviceId", &service_id) ||
                                service_id < 0 || service_id > 0xFFFFFFFFLL)) ||
            (has_terminal_id &&
             (!map_int(*args, "terminalId", &terminal_id) || terminal_id < 1 ||
              terminal_id > static_cast<int64_t>(chast::kMaxTerminals)))) {
            result->Error("bad_args",
                          "openTerminal wants columns 1..500, rows 1..200, an "
                          "optional terminalId in 1..kMaxTerminals and an "
                          "optional serviceId in 0..2^32-1");
            return;
        }
        const bool opened =
            viewer_ != nullptr && viewer_->OpenTerminal(
                                      static_cast<uint16_t>(columns),
                                      static_cast<uint16_t>(rows),
                                      static_cast<uint32_t>(terminal_id),
                                      static_cast<uint32_t>(service_id));
        result->Success(flutter::EncodableValue(opened));
        return;
    }

    if (method == "sendTerminalInput") {
        // The legacy call was a bare string for the first console. The tabs
        // send a map so each one types into its own slot.
        std::string input;
        int64_t terminal_id = 1;
        if (const auto* text = std::get_if<std::string>(call.arguments())) {
            input = *text;
        } else if (const auto* args =
                       std::get_if<flutter::EncodableMap>(call.arguments())) {
            if (!map_string(*args, "text", &input) ||
                !map_int(*args, "terminalId", &terminal_id) || terminal_id < 1 ||
                terminal_id > static_cast<int64_t>(chast::kMaxTerminals)) {
                result->Error("bad_args",
                              "sendTerminalInput wants text and a terminalId in "
                              "1..kMaxTerminals");
                return;
            }
        } else {
            result->Error("bad_args", "sendTerminalInput wants the input text");
            return;
        }
        const bool queued =
            viewer_ != nullptr && viewer_->SendTerminalInput(
                                      input, static_cast<uint32_t>(terminal_id));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "resizeTerminal") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t columns = 0;
        int64_t rows = 0;
        int64_t terminal_id = 1;
        const bool has_terminal_id =
            args != nullptr && args->find(key("terminalId")) != args->end();
        if (args == nullptr || !map_int(*args, "columns", &columns) ||
            !map_int(*args, "rows", &rows) || columns <= 0 || columns > 500 ||
            rows <= 0 || rows > 200 ||
            (has_terminal_id &&
             (!map_int(*args, "terminalId", &terminal_id) || terminal_id < 1 ||
              terminal_id > static_cast<int64_t>(chast::kMaxTerminals)))) {
            result->Error("bad_args",
                          "resizeTerminal wants columns 1..500, rows 1..200 and "
                          "an optional terminalId in 1..kMaxTerminals");
            return;
        }
        const bool queued =
            viewer_ != nullptr && viewer_->ResizeTerminal(
                                      static_cast<uint16_t>(columns),
                                      static_cast<uint16_t>(rows),
                                      static_cast<uint32_t>(terminal_id));
        result->Success(flutter::EncodableValue(queued));
        return;
    }

    if (method == "closeTerminal") {
        const int64_t terminal_id = terminal_id_arg(call.arguments());
        if (viewer_ != nullptr && terminal_id >= 1 &&
            terminal_id <= static_cast<int64_t>(chast::kMaxTerminals)) {
            viewer_->CloseTerminal(static_cast<uint32_t>(terminal_id));
        }
        result->Success();
        return;
    }

    if (method == "takeTerminalOutput") {
        // Bytes, not text: a UTF-8 sequence may be split across chunks, so
        // decoding belongs to the panel that reassembles the console picture.
        const int64_t terminal_id = terminal_id_arg(call.arguments());
        std::vector<uint8_t> data;
        bool ended = false;
        if (viewer_ != nullptr && terminal_id >= 1 &&
            terminal_id <= static_cast<int64_t>(chast::kMaxTerminals)) {
            viewer_->TakeTerminalOutput(data, ended,
                                        static_cast<uint32_t>(terminal_id));
        } else {
            // No engine or an unknown slot: there will never be output, so
            // the panel must see the end instead of polling forever.
            ended = true;
        }
        flutter::EncodableMap reply;
        reply[flutter::EncodableValue("data")] =
            flutter::EncodableValue(std::move(data));
        reply[flutter::EncodableValue("ended")] = flutter::EncodableValue(ended);
        result->Success(flutter::EncodableValue(std::move(reply)));
        return;
    }

    if (method == "setCameraOutput") {
        const auto* output = std::get_if<int64_t>(call.arguments());
        if (output == nullptr) {
            result->Error("bad_args",
                          "setCameraOutput wants the camera texture output, 0 to detach");
            return;
        }
        if (*output < 0) {
            result->Error("bad_args", "setCameraOutput wants a non-negative output");
            return;
        }
        if (camera_sink_ == nullptr) {
            result->Error("no_gpu", "the camera frame sink is not available");
            return;
        }
        camera_sink_->SetOutput(*output == 0
                                    ? nullptr
                                    : reinterpret_cast<void*>(
                                          static_cast<intptr_t>(*output)));
        result->Success(flutter::EncodableValue(true));
        return;
    }

    if (method == "requestCameraList") {
        const bool ok = viewer_ != nullptr && viewer_->RequestCameraList();
        result->Success(flutter::EncodableValue(ok));
        return;
    }

    if (method == "startCamera") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t camera_id = 0;
        if (args == nullptr || !map_int(*args, "cameraId", &camera_id) ||
            camera_id <= 0 || camera_id > 65535) {
            result->Error("bad_args", "startCamera wants a camera id in 1..65535");
            return;
        }
        const bool started =
            viewer_ != nullptr && viewer_->StartCamera(static_cast<uint16_t>(camera_id));
        result->Success(flutter::EncodableValue(started));
        return;
    }

    if (method == "stopCamera") {
        if (viewer_ != nullptr) {
            viewer_->StopCamera();
        }
        result->Success();
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
        const uint32_t rule_id =
            viewer_ != nullptr
                ? viewer_->StartPortForward(static_cast<uint16_t>(local_port),
                                            remote_host,
                                            static_cast<uint16_t>(remote_port))
                : 0;
        result->Success(flutter::EncodableValue(static_cast<int64_t>(rule_id)));
        return;
    }

    if (method == "stopPortForward") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        int64_t rule_id = 0;
        if (args == nullptr || !map_int(*args, "ruleId", &rule_id) || rule_id <= 0 ||
            rule_id > 0xFFFFFFFFll) {
            result->Error("bad_args", "stopPortForward wants a positive ruleId");
            return;
        }
        if (viewer_ != nullptr) {
            viewer_->StopPortForward(static_cast<uint32_t>(rule_id));
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

    if (method == "ticketIdentity") {
        std::vector<uint8_t> seed;
        std::string error;
        if (!load_or_create_ticket_seed(&seed, &error)) {
            result->Error("ticket_key", error);
            return;
        }
        std::vector<uint8_t> public_key;
        const bool derived = chast::derive_access_ticket_public_key(seed, &public_key);
        std::fill(seed.begin(), seed.end(), '\0');
        if (!derived) {
            result->Error("ticket_key", "could not derive the ticket public key");
            return;
        }
        result->Success(flutter::EncodableValue(chast::encode_ticket_base64url(public_key)));
        return;
    }

    if (method == "renewAccessTicket") {
        const auto* text = std::get_if<std::string>(call.arguments());
        std::vector<uint8_t> ticket;
        if (text == nullptr || !decode_ticket_text(*text, &ticket)) {
            result->Error("bad_args", "renewAccessTicket wants a signed access ticket");
            return;
        }
        result->Success(
            flutter::EncodableValue(viewer_->RenewAccessTicket(std::move(ticket))));
        return;
    }

    if (method == "protectSecret" || method == "unprotectSecret") {
        const auto* text = std::get_if<std::string>(call.arguments());
        const bool protect = method == "protectSecret";
        if (text == nullptr || text->empty() ||
            text->size() > (protect ? kMaxProtectPlaintext : kMaxProtectedText)) {
            result->Error("bad_args", "the secret has an unsupported size");
            return;
        }
        std::string output;
        std::string error;
        const bool ok =
            protect ? chast::protect_secret(*text, chast::SecretScope::CurrentUser,
                                            kAccountSecretEntropy, output, &error)
                    : chast::unprotect_secret(*text, chast::SecretScope::CurrentUser,
                                              kAccountSecretEntropy, output, &error);
        if (!ok) {
            result->Error("dpapi", error);
            return;
        }
        result->Success(flutter::EncodableValue(std::move(output)));
        return;
    }

    if (method == "openExternalUrl") {
        const auto* url = std::get_if<std::string>(call.arguments());
        if (url == nullptr || url->empty() || url->size() > 4096) {
            result->Error("bad_args", "the URL has an unsupported size");
            return;
        }
        const std::string& text = *url;
        const bool web = text.rfind("https://", 0) == 0 || text.rfind("http://", 0) == 0;
        if (!web) {
            result->Error("bad_args", "only http and https URLs can be opened");
            return;
        }
        const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                    text.data(),
                                                    static_cast<int>(text.size()),
                                                    nullptr, 0);
        if (wide_length <= 0) {
            result->Error("bad_args", "the URL is not valid UTF-8");
            return;
        }
        std::wstring wide(static_cast<size_t>(wide_length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), wide.data(), wide_length);
        const HINSTANCE launched = ShellExecuteW(nullptr, L"open", wide.c_str(),
                                                 nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32) {
            result->Error("open_failed", "Windows could not open the default browser");
            return;
        }
        result->Success();
        return;
    }

    if (method == "securityKeyAvailable") {
        result->Success(flutter::EncodableValue(QuerySecurityKeyAvailability().available));
        return;
    }

    if (method == "securityKeyMakeCredential" || method == "securityKeyGetAssertion") {
        const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
        if (args == nullptr) {
            result->Error("bad_args", "security key ceremony wants an argument map");
            return;
        }
        StartSecurityKey(*args, method == "securityKeyMakeCredential", std::move(result));
        return;
    }

    result->NotImplemented();
}

void ViewerBridge::StartSecurityKey(
    const flutter::EncodableMap& args, bool make_credential,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    SecurityKeyOptions options;
    std::string user_id;
    std::string username;
    std::string display_name;
    int64_t timeout_ms = 120000;
    if (!map_string(args, "rpId", &options.rp_id) || options.rp_id.empty() ||
        options.rp_id.size() > 253) {
        result->Error("bad_args", "security key ceremony wants a relying party id");
        return;
    }
    if (!map_string(args, "challenge", &options.challenge) || options.challenge.empty() ||
        options.challenge.size() > 128) {
        result->Error("bad_args", "security key ceremony wants a base64url challenge");
        return;
    }
    map_string(args, "origin", &options.origin);
    if (options.origin.size() > 512) {
        result->Error("bad_args", "origin is too long");
        return;
    }
    const auto credentials = args.find(key("credentials"));
    if (credentials != args.end()) {
        const auto* list = std::get_if<flutter::EncodableList>(&credentials->second);
        if (list == nullptr || list->size() > 32) {
            result->Error("bad_args", "credentials must be a list of at most 32 ids");
            return;
        }
        options.credential_ids.reserve(list->size());
        for (const flutter::EncodableValue& item : *list) {
            const auto* id = std::get_if<std::string>(&item);
            if (id == nullptr || id->empty() || id->size() > 1024) {
                result->Error("bad_args", "credential ids must be non-empty base64url");
                return;
            }
            options.credential_ids.push_back(*id);
        }
    }
    if (map_int(args, "timeoutMs", &timeout_ms)) {
        if (timeout_ms < 15000 || timeout_ms > 300000) {
            result->Error("bad_args", "timeoutMs must be between 15000 and 300000");
            return;
        }
        options.timeout_ms = static_cast<int>(timeout_ms);
    }
    if (make_credential) {
        if (!map_string(args, "userId", &user_id) || user_id.empty() || user_id.size() > 128 ||
            !map_string(args, "username", &username) || username.empty() || username.size() > 256) {
            result->Error("bad_args", "registration wants a user id and a user name");
            return;
        }
        map_string(args, "displayName", &display_name);
        if (display_name.size() > 256) {
            result->Error("bad_args", "display name is too long");
            return;
        }
    } else if (options.credential_ids.empty()) {
        result->Error("bad_args", "assertion wants at least one credential id");
        return;
    }

    bool idle = false;
    if (!security_key_busy_->compare_exchange_strong(idle, true)) {
        result->Error("busy", "another Windows Hello prompt is still open");
        return;
    }

    auto completion = std::make_shared<SecurityKeyCompletion>();
    completion->result = std::move(result);
    // The worker may outlive the bridge (a Hello prompt blocks until the user
    // answers), so it holds the shared flag rather than a ViewerBridge pointer.
    auto busy = security_key_busy_;
    const HWND window = window_;
    std::thread([window, busy, completion, make_credential,
                 options = std::move(options), user_id = std::move(user_id),
                 username = std::move(username),
                 display_name = std::move(display_name)]() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string error;
        std::string error_code;
        if (make_credential) {
            const SecurityKeyCredential credential = MakeSecurityKeyCredential(
                window, options, user_id, username, display_name, &error, &error_code);
            if (error_code.empty() && !credential.credential_id.empty()) {
                completion->ok = true;
                completion->value = flutter::EncodableMap{
                    {key("credentialId"), flutter::EncodableValue(credential.credential_id)},
                    {key("clientDataJSON"), flutter::EncodableValue(credential.client_data_json)},
                    {key("attestationObject"),
                     flutter::EncodableValue(credential.attestation_object)},
                };
            }
        } else {
            const SecurityKeyAssertion assertion =
                GetSecurityKeyAssertion(window, options, &error, &error_code);
            if (error_code.empty() && !assertion.credential_id.empty()) {
                completion->ok = true;
                completion->value = flutter::EncodableMap{
                    {key("credentialId"), flutter::EncodableValue(assertion.credential_id)},
                    {key("clientDataJSON"), flutter::EncodableValue(assertion.client_data_json)},
                    {key("authenticatorData"),
                     flutter::EncodableValue(assertion.authenticator_data)},
                    {key("signature"), flutter::EncodableValue(assertion.signature)},
                };
            }
        }
        if (!completion->ok) {
            completion->error_code = error_code.empty() ? "failed" : error_code;
            completion->error =
                error.empty() ? "Windows Hello returned no result" : error;
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        // The handler owns the shared_ptr through a heap copy; a failed post
        // means it will never run, so answer here and clear the flag instead
        // of leaving the bridge permanently busy.
        auto* posted = new std::shared_ptr<SecurityKeyCompletion>(completion);
        if (window == nullptr ||
            !PostMessageW(window, kSecurityKeyDoneMessage, 0,
                          reinterpret_cast<LPARAM>(posted))) {
            delete posted;
            busy->store(false);
            complete_security_key(completion);
        }
    }).detach();
}

LRESULT CALLBACK ViewerBridge::WindowSubclassProc(HWND window, UINT message, WPARAM wparam,
                                                  LPARAM lparam, UINT_PTR id,
                                                  DWORD_PTR ref_data) {
    auto* bridge = reinterpret_cast<ViewerBridge*>(ref_data);
    if (message == kKeyboardEscapeMessage && bridge != nullptr && !bridge->shutting_down_) {
        bridge->SetRelativeMouse(false, 1.0);
        bridge->channel_->InvokeMethod("keyboardEscape", nullptr);
        return 0;
    }
    if (message == kLocalKeyMessage && bridge != nullptr) {
        bridge->DrainLocalKey(static_cast<uintptr_t>(wparam));
        return 0;
    }
    if (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE && bridge != nullptr &&
        bridge->keyboard_capture_ && bridge->keyboard_capture_->enabled()) {
        if (bridge->local_text_input_) bridge->local_text_input_->Cancel();
        bridge->keyboard_capture_->Release();
    }
    if (message == WM_CLOSE && bridge != nullptr && bridge->window_close_handler_ &&
        !bridge->shutting_down_ && !bridge->display_stream_) {
        // The session window must wind its session down before the engine
        // goes away. Tell Dart once; its shutdown asks for the real close.
        if (!bridge->window_close_requested_) {
            bridge->window_close_requested_ = true;
            bridge->channel_->InvokeMethod("windowCloseRequested", nullptr);
        }
        return 0;
    }
    if (message == kSecurityKeyDoneMessage) {
        auto* posted = reinterpret_cast<std::shared_ptr<SecurityKeyCompletion>*>(lparam);
        if (posted != nullptr) {
            const std::shared_ptr<SecurityKeyCompletion> completion = std::move(*posted);
            delete posted;
            if (bridge != nullptr) {
                bridge->security_key_busy_->store(false);
            }
            complete_security_key(completion);
        }
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
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
    int64_t contact = 0;
    int64_t pressure = 0;
    int64_t flags = 0;
    int64_t tilt_x = 0;
    int64_t tilt_y = 0;
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
        bool physical = false;
        map_bool(args, "physical", &physical);
        if (physical && keyboard_capture_ && keyboard_capture_->enabled()) {
            // Native ownership can begin before the asynchronous Dart reply.
            // Do not double-forward the remaining Flutter key notifications.
            result->Success(flutter::EncodableValue(true));
            return;
        }
        event.kind = chast::InputKind::Key;
        if (!map_int(args, "hidUsage", &usage) || usage <= 0 || usage > 0xFFFF ||
            action < 0 || action > 1) {
            result->Error("bad_args", "key event wants action 0/1 and hidUsage");
            return;
        }
        event.key.action = static_cast<chast::KeyAction>(action);
        event.key.hid_usage = static_cast<uint16_t>(usage);
    } else if (kind == 3) {
        event.kind = chast::InputKind::Touch;
        if (!map_int(args, "contact", &contact) || !map_int(args, "pressure", &pressure) ||
            action < 0 || action > 3 || contact < 0 ||
            contact >= chast::kMaxTouchContacts || pressure < 0 || pressure > 1024 ||
            buttons != 0 || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN ||
            y > INT32_MAX) {
            result->Error("bad_args", "touch event out of range");
            return;
        }
        event.touch.action = static_cast<chast::TouchAction>(action);
        event.touch.contact = static_cast<uint8_t>(contact);
        event.touch.x = static_cast<int32_t>(x);
        event.touch.y = static_cast<int32_t>(y);
        event.touch.pressure = static_cast<uint16_t>(pressure);
    } else if (kind == 4) {
        event.kind = chast::InputKind::Pen;
        if (!map_int(args, "pressure", &pressure) || !map_int(args, "flags", &flags) ||
            !map_int(args, "tiltX", &tilt_x) || !map_int(args, "tiltY", &tilt_y) ||
            action < 0 || action > 3 || pressure < 0 || pressure > 1024 || flags < 0 ||
            flags > 0x07 || tilt_x < -90 || tilt_x > 90 || tilt_y < -90 ||
            tilt_y > 90 || buttons != 0 || contact != 0 || x < INT32_MIN ||
            x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
            result->Error("bad_args", "pen event out of range");
            return;
        }
        event.pen.action = static_cast<chast::TouchAction>(action);
        event.pen.x = static_cast<int32_t>(x);
        event.pen.y = static_cast<int32_t>(y);
        event.pen.pressure = static_cast<uint16_t>(pressure);
        event.pen.flags = static_cast<uint8_t>(flags);
        event.pen.tilt_x = static_cast<int16_t>(tilt_x);
        event.pen.tilt_y = static_cast<int16_t>(tilt_y);
    } else if (kind == 5) {
        event.kind = chast::InputKind::Gesture;
        if (action < 0 || action > 1 || buttons != 0 ||
            x < -chast::kMaxGestureDelta || x > chast::kMaxGestureDelta ||
            y < -chast::kMaxGestureDelta || y > chast::kMaxGestureDelta) {
            result->Error("bad_args", "gesture event out of range");
            return;
        }
        event.gesture = {static_cast<chast::GestureAction>(action),
                         static_cast<int32_t>(x), static_cast<int32_t>(y)};
        if (!chast::valid_gesture_event(event.gesture)) {
            result->Error("bad_args", "gesture wants a nonzero pan or vertical zoom delta");
            return;
        }
    } else {
        result->Error(
            "bad_args",
            "input kind must be 1 (mouse), 2 (key), 3 (touch), 4 (pen) or 5 (gesture)");
        return;
    }

    // The hook consumes physical modifiers before Flutter sees them. Pointer
    // events must use that same bitmap; synthetic toolbar chords keep theirs.
    if (event.kind != chast::InputKind::Key && keyboard_capture_ &&
        keyboard_capture_->enabled()) {
        event.modifiers = keyboard_capture_->InputState();
    }
    last_modifiers_ = event.modifiers;
    const bool queued = QueueInput(event);
    result->Success(flutter::EncodableValue(queued));
}

bool ViewerBridge::QueueInput(const chast::InputEvent& event) {
    return session_ && session_->SendInput(this, session_generation_.load(), display_stream_, window_, event);
}

bool ViewerBridge::PostLocalKey(const chast::InputEvent& event, bool release) {
    std::lock_guard lock(local_keys_mutex_);
    // Focus loss can prevent delivery of native keys that were still queued.
    // Discard their barriers and release rather than waiting for another window.
    if (release) {
        local_keys_.clear();
        local_ime_keys_.fill(false);
    }
    if (local_keys_.size() >= 256) return false;
    const auto ticket = ++next_local_ticket_;
    local_keys_.push_back({event, release, session_generation_.load(), ticket});
    if (!PostMessageW(window_, kLocalKeyMessage, ticket, 0)) {
        local_keys_.pop_back();
        return false;
    }
    return true;
}

bool ViewerBridge::QueueLocalKey(const KBDLLHOOKSTRUCT& key, bool down,
    const chast::InputEvent& event, bool consumed) {
    if (consumed) return event.kind != chast::InputKind::Key || PostLocalKey(event);
    std::lock_guard lock(local_keys_mutex_);
    if (local_keys_.size() >= 256) return false;
    LocalKey pending{event, false, session_generation_.load(), ++next_local_ticket_};
    pending.scan = static_cast<uint16_t>((key.scanCode & 0xff) | ((key.flags & LLKHF_EXTENDED) ? 0x100 : 0));
    pending.virtual_key = key.vkCode;
    pending.usage = chast::windows_key_to_hid(key.vkCode, key.scanCode, (key.flags & LLKHF_EXTENDED) != 0);
    pending.down = down;
    pending.native = true;
    pending.completed = false;
    local_keys_.push_back(pending);
    return true;
}

bool ViewerBridge::ObserveLocalKey(UINT message, WPARAM wparam, LPARAM lparam) {
    if (!local_keyboard_ || !key_capture_ ||
        static_cast<ULONG_PTR>(GetMessageExtraInfo()) == chast::kInputInjectionTag) return false;
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const uint16_t scan = static_cast<uint16_t>((lparam >> 16) & 0x1ff);
    const unsigned repeats = (std::max)(1u, static_cast<unsigned>(LOWORD(lparam)));
    bool remote = false;
    std::lock_guard lock(local_keys_mutex_);
    for (unsigned i = 0; i < repeats; ++i) {
        const auto found = std::find_if(local_keys_.begin(), local_keys_.end(), [&](const LocalKey& key) {
            return key.native && !key.received && key.down == down && key.scan == scan &&
                (scan != 0 || key.virtual_key == wparam || wparam == VK_PROCESSKEY);
        });
        if (found == local_keys_.end()) break;
        if (found->usage != 0 && found->usage < 0xe0) {
            auto& ime = local_ime_keys_[found->usage];
            if (down) ime = wparam == VK_PROCESSKEY || (local_text_input_ && local_text_input_->composing());
            if (ime) found->event = {};
            if (!down) ime = false;
        }
        remote |= found->event.kind == chast::InputKind::Key;
        found->received = true;
        // TranslateMessage has already posted this key's WM_CHAR. Complete its
        // barrier only after that character, before allowing later shortcuts.
        // A hook's earlier PostMessage can otherwise overtake native key input.
        if (!PostMessageW(window_, kLocalKeyMessage, found->ticket, 0)) {
            local_keys_.clear();
            ReleaseInput();
            break;
        }
    }
    return remote;
}

void ViewerBridge::DrainLocalKey(uintptr_t ticket) {
    std::deque<LocalKey> ready;
    {
        std::lock_guard lock(local_keys_mutex_);
        const auto found = std::find_if(local_keys_.begin(), local_keys_.end(),
            [ticket](const LocalKey& key) { return key.ticket == ticket; });
        if (found == local_keys_.end()) return;
        if (found->native && found->received) found->completed = true;
        while (!local_keys_.empty() && local_keys_.front().completed) {
            ready.push_back(local_keys_.front());
            local_keys_.pop_front();
        }
    }
    for (const auto& key : ready) {
        if (key.generation != session_generation_.load()) continue;
        if (key.release) ReleaseInput();
        else if (key_capture_ && local_keyboard_ && key.event.kind == chast::InputKind::Key &&
                 !QueueInput(key.event)) ReleaseInput();
    }
}

bool ViewerBridge::ReleaseInput() {
    return session_ && session_->ReleaseInput(this, session_generation_.load());
}

bool ViewerBridge::SetKeyboardMode(const std::string& mode) {
    const bool local = mode == "local";
    const bool legacy = mode == "legacy";
    if (!local && !legacy && mode != "physical") return false;
    if (local_keyboard_ == local && legacy_keyboard_ == legacy) return true;
    if (local && (!local_text_input_ || !keyboard_capture_ || !keyboard_capture_->available())) return false;
    const bool captured = key_capture_.load();
    SetKeyCapture(false);
    ReleaseInput();
    local_keyboard_ = local;
    legacy_keyboard_ = legacy;
    keyboard_capture_->SetTranslated(local);
    keyboard_capture_->SetLegacy(legacy);
    SetKeyCapture(captured);
    if (captured && local && !key_capture_) {
        SetKeyCapture(false);
        local_keyboard_ = false;
        legacy_keyboard_ = false;
        keyboard_capture_->SetTranslated(false);
        keyboard_capture_->SetLegacy(false);
        SetKeyCapture(captured);
        return false;
    }
    return true;
}

void ViewerBridge::SetKeyCapture(bool enable) {
    if (enable && display_stream_ && session_generation_.load() != session_->generation.load()) enable = false;
    if (keyboard_capture_) {
        const bool captured = keyboard_capture_->Enable(enable);
        if (enable && local_keyboard_ && !captured) enable = false;
    }
    if (!enable) {
        std::lock_guard lock(local_keys_mutex_);
        local_keys_.clear();
        local_ime_keys_.fill(false);
        ReleaseInput();
    }
    const bool was = key_capture_.exchange(enable, std::memory_order_relaxed);
    if (was == enable || view_ == nullptr || !IsWindow(view_)) {
        return;
    }
    if (enable && local_keyboard_) {
        if (!local_text_input_ || !local_text_input_->Enable(true)) {
            key_capture_ = false;
            if (keyboard_capture_) keyboard_capture_->Enable(false);
        }
    } else if (enable) {
        saved_himc_ = ImmAssociateContext(view_, nullptr);
    } else {
        if (local_text_input_) local_text_input_->Enable(false);
        if (saved_himc_ != nullptr) {
            ImmAssociateContext(view_, saved_himc_);
            saved_himc_ = nullptr;
        }
    }
}

void ViewerBridge::SetRelativeMouse(bool enable, double scale) {
    if (keyboard_capture_) keyboard_capture_->set_relative(enable);
    relative_scale_ = scale > 0.0 ? scale : 1.0;
    const bool was_enabled = relative_mouse_.exchange(enable, std::memory_order_relaxed);
    if (view_ == nullptr || !IsWindow(view_)) {
        return;
    }
    if (!enable) {
        if (was_enabled) ClipCursor(nullptr);
        return;
    }
    // Park the pointer in the middle so the first relative move starts from a
    // known spot, then confine it so a fast drag cannot leave the video.
    RECT client{};
    GetClientRect(view_, &client);
    if (client.right <= client.left || client.bottom <= client.top) {
        return;
    }
    POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
    ClientToScreen(view_, &center);
    ::SetCursorPos(center.x, center.y);
    ClipRelativePointer();
}

void ViewerBridge::ClipRelativePointer() {
    if (view_ == nullptr || !IsWindow(view_)) {
        return;
    }
    RECT client{};
    if (GetClientRect(view_, &client) == 0) {
        return;
    }
    if (client.right <= client.left || client.bottom <= client.top) {
        // A minimised window has no client area; an empty clip would pin the
        // pointer to one screen pixel.
        ClipCursor(nullptr);
        return;
    }
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (ClientToScreen(view_, &top_left) == 0 ||
        ClientToScreen(view_, &bottom_right) == 0) {
        return;
    }
    const RECT clip{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    ClipCursor(&clip);
}

LRESULT CALLBACK ViewerBridge::ViewSubclassProc(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam, UINT_PTR id, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<ViewerBridge*>(ref_data);
    if (self != nullptr) {
        LRESULT local_result = 0;
        if (self->local_text_input_ && self->local_text_input_->Handle(message, wparam, lparam, local_result))
            return local_result;
        switch (message) {
            case WM_MOUSEMOVE:
                // Relative mode owns the pointer: report the offset from the
                // client centre as host motion and recentre, then swallow the
                // message so Flutter never sees the warp as pointer movement.
                // Only while this window is foreground: after an alt-tab the
                // pointer must be free to leave, even before focus returns.
                if (self->relative_mouse_.load(std::memory_order_relaxed) &&
                    self->viewer_ != nullptr &&
                    GetAncestor(window, GA_ROOT) == GetForegroundWindow()) {
                    const int x = static_cast<short>(LOWORD(lparam));
                    const int y = static_cast<short>(HIWORD(lparam));
                    RECT client{};
                    GetClientRect(window, &client);
                    const int center_x = (client.right - client.left) / 2;
                    const int center_y = (client.bottom - client.top) / 2;
                    const int offset_x = x - center_x;
                    const int offset_y = y - center_y;
                    if (offset_x != 0 || offset_y != 0) {
                        const double scale =
                            self->relative_scale_ > 0.0 ? self->relative_scale_ : 1.0;
                        const int move_x =
                            static_cast<int>(std::lround(offset_x / scale));
                        const int move_y =
                            static_cast<int>(std::lround(offset_y / scale));
                        if (move_x != 0 || move_y != 0) {
                            chast::InputEvent event;
                            event.kind = chast::InputKind::Mouse;
                            event.modifiers = self->keyboard_capture_ &&
                                                      self->keyboard_capture_->enabled()
                                                  ? self->keyboard_capture_->InputState()
                                                  : self->last_modifiers_;
                            event.mouse.action = chast::MouseAction::MoveRelative;
                            event.mouse.buttons = 0;
                            event.mouse.x = move_x;
                            event.mouse.y = move_y;
                            self->QueueInput(event);
                        }
                    }
                    POINT center{center_x, center_y};
                    ClientToScreen(window, &center);
                    ::SetCursorPos(center.x, center.y);
                    return 0;
                }
                break;
            case WM_SIZE:
            case WM_MOVE:
                if (self->relative_mouse_.load(std::memory_order_relaxed)) {
                    self->ClipRelativePointer();
                }
                break;
            case WM_SETFOCUS:
                if (self->relative_mouse_.load(std::memory_order_relaxed)) {
                    self->ClipRelativePointer();
                }
                break;
            case WM_KILLFOCUS:
                if (self->relative_mouse_.load(std::memory_order_relaxed)) {
                    // Another window owns the pointer now; keeping the clip
                    // would trap it outside that window.
                    ClipCursor(nullptr);
                }
                break;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const bool remote = self->ObserveLocalKey(message, wparam, lparam);
                if (self->key_capture_ && self->local_keyboard_) {
                    // IMM owns these keys. Flutter has no active text client
                    // here and must not redispatch them as unhandled input.
                    return remote ? 0 : DefWindowProcW(window, message, wparam, lparam);
                }
                // An active local IME replaces the virtual key with
                // VK_PROCESSKEY and the engine drops such events; while the
                // remote host owns composition, hand it the physical key.
                // ImmGetVirtualKey knows the original for real key presses,
                // but not for synthesized ones, so fall back to the scan code
                // in lParam (the remote side injects by HID usage anyway).
                if (wparam == VK_PROCESSKEY &&
                    self->key_capture_.load(std::memory_order_relaxed) && !self->local_keyboard_) {
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
            }
            case WM_SETCURSOR:
                // The host's pointer shape over the video area. Answering here
                // keeps Flutter from installing its own cursor on top: the
                // page hides the system cursor while control is on, and this
                // is what puts the host's arrow/I-beam/resize grip back.
                if (LOWORD(lparam) == HTCLIENT && self->cursor_override_ &&
                    !self->relative_mouse_.load(std::memory_order_relaxed)) {
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
                if (self->relative_mouse_.exchange(false, std::memory_order_relaxed)) {
                    ClipCursor(nullptr);
                }
                RemoveWindowSubclass(window, &ViewerBridge::ViewSubclassProc, id);
                self->view_ = nullptr;
                break;
            default:
                break;
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

void ViewerBridge::DrainCursorUpdates() {
    if (viewer_ == nullptr) {
        return;
    }
    // Every update the session received since the last poll: register the
    // sprite of each new shape, keep the newest sprite for the view-only
    // overlay, then remember only the newest state.
    chast::CursorUpdate update;
    while (viewer_->TakeCursor(update)) {
        if (!update.sprite.empty() && update.shape_id != 0) {
            cursor_sprite_ = std::move(update.sprite);
            cursor_sprite_shape_ = update.shape_id;
            ++cursor_sprite_version_;
            std::string error;
            if (!remote_cursor_.Register(update.shape_id, update.width, update.height,
                                         update.hotspot_x, update.hotspot_y, cursor_sprite_,
                                         &error)) {
                std::fprintf(stderr, "chast_viewer: remote cursor: %s\n", error.c_str());
            }
        }
        last_cursor_ = std::move(update);
        have_cursor_ = true;
    }
}

void ViewerBridge::ApplyCursor(const flutter::EncodableMap& args) {
    if (viewer_ == nullptr) {
        return;
    }
    DrainCursorUpdates();

    bool enabled = false;
    int64_t scale_milli = 1000;
    map_bool(args, "enabled", &enabled);
    map_int(args, "scaleMilli", &scale_milli);
    if (!enabled || !have_cursor_ || last_cursor_.shape_id == 0) {
        // View-only, or the host has not named a shape yet: Flutter's own
        // cursor is the right one again. A hidden host pointer keeps the last
        // shape, as RustDesk's client does: the local pointer is what the user
        // moves, so hiding the host's must not take it away.
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
    last_cursor_ = {};
    cursor_sprite_.clear();
    cursor_sprite_shape_ = 0;
    cursor_sprite_version_ = 0;
    cursor_sprite_sent_ = 0;
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
