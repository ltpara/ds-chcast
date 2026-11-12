// The capture and session headers pull in windows.h, whose min/max macros
// collide with std::min/std::max below.
#define NOMINMAX

#include "chast/host_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/audit_spool.h"
#include "chast/camera_relay.h"
#include "chast/clipboard_listener.h"
#include "chast/capture.h"
#include "chast/carrier.h"
#include "chast/clipboard.h"
#include "chast/com_ptr.h"
#include "chast/desktop.h"
#include "host/desktop_stream.h"
#include "chast/user_context.h"
#include "host/display_selection.h"
#include "host/display_topology.h"
#include "host/shared_loopback.h"
#include "chast/display_mode.h"
#include "chast/display_privacy.h"
#include "chast/file_transfer.h"
#include "chast/port_forward_relay.h"
#include "chast/png.h"
#include "chast/privacy.h"
#include "chast/sas.h"
#include "chast/security.h"
#include "chast/show_cursor.h"
#include "chast/terminal_relay.h"
#include "chast/virtual_display.h"
#include "chast/wallpaper.h"

namespace chast {

namespace {
class ScopedFileAccess {
public:
    explicit ScopedFileAccess(const std::shared_ptr<void>& token) {
        if (!token) { ok_ = true; return; }
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &previous_) &&
            GetLastError() != ERROR_NO_TOKEN) return;
        changed_ = SetThreadToken(nullptr, token.get()) != FALSE;
        ok_ = changed_;
    }
    ~ScopedFileAccess() {
        if (changed_ && !SetThreadToken(nullptr, previous_)) std::terminate();
        if (previous_) CloseHandle(previous_);
    }
    bool ok() const { return ok_; }
private:
    HANDLE previous_ = nullptr;
    bool changed_ = false, ok_ = false;
};
}

// Defined here, where the audio pipeline types are complete: the header only
// forward-declares them, so an inline constructor would need their destructors
// in every translation unit that includes it.
HostServer::HostServer(HostServerConfig config)
    : config_(std::move(config)), settings_(config_.settings) {}

// Coordination between a listener and its per-viewer workers. The listener
// owns the object; workers hold the same shared_ptr. All fields are guarded by
// `mutex`; `callback_mutex` serializes the user callbacks that persist
// settings (a viewer thread may otherwise race the parent's).
struct ViewerRegistry {
    mutable std::mutex mutex;
    std::weak_ptr<HostServer> focused;
    std::vector<std::weak_ptr<HostServer>> workers;
    size_t active = 0;
    // Accepted connections still in their handshake. They already hold the
    // token they were accepted with, so rotation has to wait for them too.
    size_t pending = 0;
    uint64_t admitted = 0;
    size_t max_viewers = 1;
    // The desktop viewer allowed to inject input and change host-wide state.
    HostServer* controller = nullptr;
    // Viewer audit references held by live sessions. A second connection that
    // presents the same reference is recorded under a host id instead, so the
    // service can never merge two concurrent sessions into one row.
    std::set<std::string> audit_refs;
    std::mutex callback_mutex;

    // Pipelines outlive individual connections while their display is subscribed.
    DesktopStreamPool desktop_streams;
    SharedLoopback system_audio;

    bool claim_audit_ref(const std::string& ref) {
        std::lock_guard<std::mutex> lock(mutex);
        return audit_refs.insert(ref).second;
    }
    void release_audit_ref(const std::string& ref) {
        std::lock_guard<std::mutex> lock(mutex);
        audit_refs.erase(ref);
    }

    bool acquire_control(HostServer* viewer) {
        std::lock_guard<std::mutex> lock(mutex);
        if (controller == nullptr) {
            controller = viewer;
            return true;
        }
        return controller == viewer;
    }
    bool holds_control(const HostServer* viewer) const {
        std::lock_guard<std::mutex> lock(mutex);
        return controller == viewer;
    }
    void release_control(HostServer* viewer) {
        std::lock_guard<std::mutex> lock(mutex);
        if (controller == viewer) {
            controller = nullptr;
        }
    }
    // Privacy mode changes the operator's physical screens, so it follows the
    // control lease once more than one viewer is connected. A lone viewer may
    // always use it when the host grants the permission.
    bool may_change_host_state(const HostServer* viewer) const {
        std::lock_guard<std::mutex> lock(mutex);
        return active <= 1 || controller == viewer;
    }

    // Remote configuration is process-wide. A worker that changed the exposed
    // switches stores them here so the listener seeds later workers from the
    // current policy instead of its start-time copy. `remote_settings_valid`
    // is set once the listener seeds the copy.
    HostSettings remote_settings;
    bool remote_settings_valid = false;
    // Host microphone chosen from the tray. Like the remote switches it is
    // process-wide: workers seed new sessions from it and live calls are
    // restarted on their own thread when it changes.
    std::string voice_microphone;
    bool voice_microphone_valid = false;
    // Read by the last worker at session teardown, possibly from a different
    // worker than the one that set it.
    std::atomic<bool> lock_after_session_end{false};
    std::atomic<uint64_t> remote_settings_version{0};
};

namespace {

using Clock = std::chrono::steady_clock;

// RustDesk sends the pointer from a 33 ms service that runs beside video, so a
// slow encode or a full video queue never freezes the cursor. Desktop sources
// invoke publish() on an independent pointer thread. The session thread owns
// routing, resets, statistics and
// teardown; send_cursor/send_display_cursor are non-blocking and thread-safe.
class CursorPublisher {
public:
    void bind(QuicSession* session, const IDxgiCapturer* capture) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = session;
        capture_ = capture;
    }
    bool bound_to(const IDxgiCapturer* capture) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capture_ == capture;
    }
    void route(uint32_t stream_id, bool multiple_streams) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stream_id_ != stream_id || multiple_streams_ != multiple_streams) {
            sent_once_ = false;
            sent_ = CursorUpdate{};
            shape_cached_ = false;
            shape_cache_ = CursorUpdate{};
        }
        stream_id_ = stream_id;
        multiple_streams_ = multiple_streams;
    }
    // The session is about to go away. An in-flight send finishes first, so the
    // sink can never touch it after this returns.
    void detach() {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ = nullptr;
        capture_ = nullptr;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        sent_once_ = false;
        sent_ = CursorUpdate{};
        shape_cached_ = false;
        shape_cache_ = CursorUpdate{};
    }
    void publish(const CursorInfo& info) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_ == nullptr || !info.position_valid) return;
        const bool shape_known = info.shape_valid && info.content_id != 0;
        CursorUpdate update;
        update.visible = info.visible;
        update.x = info.position.x;
        update.y = info.position.y;
        // A pointer whose shape the decoder has not seen yet (the first frames
        // of a session) still moves; reuse the geometry already sent so the
        // viewer can keep placing the sprite it holds.
        if (shape_known) {
            update.shape_id = info.content_id;
            update.width = static_cast<uint16_t>(info.width);
            update.height = static_cast<uint16_t>(info.height);
            update.hotspot_x = static_cast<uint16_t>(info.hotspot.x);
            update.hotspot_y = static_cast<uint16_t>(info.hotspot.y);
        } else if (sent_once_) {
            update.shape_id = sent_.shape_id;
            update.width = sent_.width;
            update.height = sent_.height;
            update.hotspot_x = sent_.hotspot_x;
            update.hotspot_y = sent_.hotspot_y;
        } else {
            return;
        }
        const bool new_shape = shape_known && update.shape_id != sent_.shape_id;
        if (new_shape) update.sprite = info.bitmap;
        if (sent_once_ && !new_shape && update.visible == sent_.visible &&
            update.x == sent_.x && update.y == sent_.y) {
            return;
        }
        // Position and shape are 33 ms services in RustDesk. The capture tick
        // is faster than that, and a static desktop would otherwise flood the
        // control queue with identical state.
        const auto now = Clock::now();
        if (sent_once_ && now - sent_at_ < std::chrono::milliseconds(33)) return;
        deliver(update, new_shape);
    }
    // The viewer lost a sprite it had already been sent (its cache evicted it).
    // Reply from the cached copy instead of waiting for the pointer to change
    // shape again. A request for a shape the host has already left is ignored:
    // the next pointer sample carries the current one.
    void resend_shape(uint64_t shape_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_ == nullptr || !shape_cached_ || !sent_once_ ||
            sent_.shape_id != shape_cache_.shape_id ||
            (shape_id != 0 && shape_id != shape_cache_.shape_id)) {
            return;
        }
        CursorUpdate update = sent_;
        update.width = shape_cache_.width;
        update.height = shape_cache_.height;
        update.hotspot_x = shape_cache_.hotspot_x;
        update.hotspot_y = shape_cache_.hotspot_y;
        update.sprite = shape_cache_.sprite;
        deliver(update, true);
    }
    struct Snapshot {
        uint64_t sent = 0, shapes = 0, bytes = 0, dropped = 0, errors = 0;
        std::string last_error;
    };
    Snapshot take() {
        std::lock_guard<std::mutex> lock(mutex_);
        Snapshot result{sent_count_, shape_count_, byte_count_, dropped_, errors_,
                        std::move(last_error_)};
        sent_count_ = shape_count_ = byte_count_ = dropped_ = errors_ = 0;
        last_error_.clear();
        return result;
    }
private:
    bool deliver(CursorUpdate& update, bool new_shape) {
        const Error sent = multiple_streams_
            ? session_->send_display_cursor({stream_id_, update})
            : session_->send_cursor(update);
        if (!sent.ok()) {
            if (sent.status() == Status::WaitTimeout) {
                // The control queue is full; the next sample carries the same
                // state again, so nothing is lost by skipping this one.
                ++dropped_;
            } else {
                ++errors_;
                if (last_error_.empty()) last_error_ = sent.to_string();
            }
            return false;
        }
        const auto now = Clock::now();
        ++sent_count_;
        if (new_shape) {
            ++shape_count_;
            byte_count_ += update.sprite.size();
            shape_cache_ = update;
            shape_cached_ = true;
        }
        sent_ = std::move(update);
        sent_once_ = true;
        sent_at_ = now;
        return true;
    }
    mutable std::mutex mutex_;
    QuicSession* session_ = nullptr;
    const IDxgiCapturer* capture_ = nullptr;
    uint32_t stream_id_ = 0;
    bool multiple_streams_ = false;
    bool sent_once_ = false;
    Clock::time_point sent_at_{};
    CursorUpdate sent_;
    // The last update that actually carried a sprite, kept for the viewer's
    // v73 re-request. Only one is needed: a request for any other shape is
    // answered by the next pointer sample.
    CursorUpdate shape_cache_;
    bool shape_cached_ = false;
    uint64_t sent_count_ = 0, shape_count_ = 0, byte_count_ = 0, dropped_ = 0, errors_ = 0;
    std::string last_error_;
};

// Downloads use the same budget the viewer applies to uploads: enough to move
// a large file quickly without starving video, audio and input.
constexpr auto kDownloadSendBudget = std::chrono::milliseconds(8);
constexpr double kDownloadBytesPerSecond = 32.0 * 1024.0 * 1024.0;
constexpr double kDownloadBurst = 256.0 * 1024.0;

std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// A viewer may omit its audit reference; the host still records the session,
// under an id of its own that has the lowercase-hex shape the service accepts.
// Time, a per-process counter and eight random bytes keep the ids distinct.
std::string host_audit_ref() {
    static std::atomic<uint32_t> sequence{0};
    uint8_t random[8] = {};
    static_cast<void>(secure_random(random, sizeof(random)));
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    char head[20] = {};
    std::snprintf(head, sizeof(head), "%08llx%08x",
                  static_cast<unsigned long long>(now_ms) & 0xFFFFFFFFull,
                  sequence.fetch_add(1, std::memory_order_relaxed));
    std::string ref = head;
    char byte[3] = {};
    for (const uint8_t value : random) {
        std::snprintf(byte, sizeof(byte), "%02x", value);
        ref += byte;
    }
    return ref;
}

// Viewer-controlled names, reasons and protocol errors reach the line log.
// Escaping control characters keeps an embedded newline from forging a second
// timestamped record.
std::string log_safe(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        switch (byte) {
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20 || byte == 0x7F) {
                    char escaped[5] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\x%02x", byte);
                    out += escaped;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

// The viewer downloads a file the host copied to the clipboard through
// ".chast-clipboard/<index>", the index being its position in the announced
// list. Any other name is an ordinary receive-directory path.
bool clipboard_file_index(const std::string& path, size_t& index) {
    constexpr char kPrefix[] = ".chast-clipboard/";
    constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
    if (path.size() <= kPrefixLength || path.compare(0, kPrefixLength, kPrefix) != 0) {
        return false;
    }
    index = 0;
    for (size_t i = kPrefixLength; i < path.size(); ++i) {
        const char digit = path[i];
        if (digit < '0' || digit > '9' || index > 1000000) {
            return false;
        }
        index = index * 10 + static_cast<size_t>(digit - '0');
    }
    return true;
}

// FrameHeader::capture_ms is 48 bits: milliseconds since the steady clock's
// epoch, which is the same clock the latency probe reports.
uint64_t capture_clock_ms(Clock::time_point time) {
    return static_cast<uint64_t>(
               std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch())
                   .count()) &
           0xFFFFFFFFFFFFull;
}

uint32_t bitrate_reserve(const AudioInfo& audio) {
    return audio.enabled ? static_cast<uint32_t>(static_cast<uint64_t>(audio.bitrate) * 11u / 10u)
                         : 0u;
}

int64_t unix_now() {
    return static_cast<int64_t>(std::time(nullptr));
}

// Audit timestamps are wall-clock milliseconds: the server stores them next to
// the viewer's own report, which uses the same epoch.
int64_t unix_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

SecretScope secret_scope(IdentityScope scope) {
    return scope == IdentityScope::LocalMachine ? SecretScope::LocalMachine
                                                : SecretScope::CurrentUser;
}

const char* stream_quality_name(StreamQuality quality) {
    switch (quality) {
        case StreamQuality::High:
            return "high";
        case StreamQuality::Balanced:
            return "balanced";
        case StreamQuality::Smooth:
            return "smooth";
        case StreamQuality::Custom:
            return "custom";
    }
    return "unknown";
}

std::optional<VideoCodec> codec_vote(StreamCodecPreference preference) {
    switch (preference) {
        case StreamCodecPreference::H264: return VideoCodec::H264;
        case StreamCodecPreference::Hevc: return VideoCodec::Hevc;
        case StreamCodecPreference::Av1: return VideoCodec::Av1;
        case StreamCodecPreference::Vp9: return VideoCodec::Vp9;
        default: return std::nullopt;
    }
}

const char* sas_code_name(SasCode code) {
    switch (code) {
        case SasCode::Ok:
            return "ok";
        case SasCode::Denied:
            return "denied";
        case SasCode::Unsupported:
            return "unsupported";
        case SasCode::Failed:
            return "failed";
    }
    return "unknown";
}

const char* os_login_code_name(OsLoginCode code) {
    switch (code) {
        case OsLoginCode::Ok:
            return "ok";
        case OsLoginCode::Denied:
            return "denied";
        case OsLoginCode::Unsupported:
            return "unsupported";
        case OsLoginCode::Busy:
            return "busy";
        case OsLoginCode::Failed:
            return "failed";
    }
    return "unknown";
}

const char* session_list_code_name(SessionListCode code) {
    switch (code) {
        case SessionListCode::Ok:
            return "ok";
        case SessionListCode::Denied:
            return "denied";
        case SessionListCode::Unsupported:
            return "unsupported";
        case SessionListCode::Failed:
            return "failed";
    }
    return "unknown";
}

const char* session_select_code_name(SessionSelectCode code) {
    switch (code) {
        case SessionSelectCode::Ok:
            return "ok";
        case SessionSelectCode::NotFound:
            return "not found";
        case SessionSelectCode::Denied:
            return "denied";
        case SessionSelectCode::Unsupported:
            return "unsupported";
        case SessionSelectCode::Busy:
            return "busy";
        case SessionSelectCode::Failed:
            return "failed";
    }
    return "unknown";
}

PermissionMask settings_permissions(const HostSettings& settings) {
    PermissionMask mask = 0;
    if (settings.allow_input) {
        mask |= kPermissionInput;
    }
    if (settings.allow_clipboard && settings.clipboard_direction != ClipboardDirection::Disabled) {
        mask |= kPermissionClipboard;
    }
    if (settings.allow_audio) {
        mask |= kPermissionAudio;
    }
    if (settings.allow_microphone) {
        mask |= kPermissionMicrophone;
    }
    if (settings.allow_file_transfer) {
        mask |= kPermissionFileTransfer;
    }
    if (settings.allow_port_forward) {
        mask |= kPermissionPortForward;
    }
    if (settings.allow_terminal) {
        mask |= kPermissionTerminal;
    }
    if (settings.allow_camera) {
        mask |= kPermissionCamera;
    }
    if (settings.allow_privacy_mode) {
        mask |= kPermissionPrivacyMode;
    }
    if (settings.allow_record) {
        mask |= kPermissionRecord;
    }
    if (settings.allow_restart) {
        mask |= kPermissionRestart;
    }
    if (settings.allow_block_input) {
        mask |= kPermissionBlockInput;
    }
    if (settings.allow_os_login) {
        mask |= kPermissionOsLogin;
    }
    if (settings.allow_session_select) {
        mask |= kPermissionSessionSelect;
    }
    if (settings.allow_remote_config) {
        mask |= kPermissionRemoteConfig;
    }
    return mask;
}

// The switches a viewer holding kPermissionRemoteConfig may read (keys = 0)
// and change. allow_input and allow_remote_config itself are deliberately
// absent: remote configuration must not hand its holder either of them.
uint32_t remote_config_flags(const HostSettings& settings) {
    uint32_t flags = 0;
    auto set = [&flags](uint32_t bit, bool value) {
        if (value) {
            flags |= bit;
        }
    };
    set(kRemoteConfigLockAfterSessionEnd, settings.lock_after_session_end);
    set(kRemoteConfigClipboard, settings.allow_clipboard);
    set(kRemoteConfigFileTransfer, settings.allow_file_transfer);
    set(kRemoteConfigAudio, settings.allow_audio);
    set(kRemoteConfigMicrophone, settings.allow_microphone);
    set(kRemoteConfigPrivacyMode, settings.allow_privacy_mode);
    set(kRemoteConfigRestart, settings.allow_restart);
    set(kRemoteConfigBlockInput, settings.allow_block_input);
    set(kRemoteConfigOsLogin, settings.allow_os_login);
    set(kRemoteConfigSessionSelect, settings.allow_session_select);
    set(kRemoteConfigTerminal, settings.allow_terminal);
    set(kRemoteConfigPortForward, settings.allow_port_forward);
    set(kRemoteConfigCamera, settings.allow_camera);
    set(kRemoteConfigRecord, settings.allow_record);
    return flags;
}

// Copies only the switches a viewer may change remotely; credentials, ports
// and the remote-config gate itself never enter the shared registry copy.
void copy_remote_config_fields(const HostSettings& from, HostSettings& to) {
    to.lock_after_session_end = from.lock_after_session_end;
    to.allow_clipboard = from.allow_clipboard;
    to.allow_file_transfer = from.allow_file_transfer;
    to.allow_audio = from.allow_audio;
    to.allow_microphone = from.allow_microphone;
    to.allow_privacy_mode = from.allow_privacy_mode;
    to.allow_restart = from.allow_restart;
    to.allow_block_input = from.allow_block_input;
    to.allow_os_login = from.allow_os_login;
    to.allow_session_select = from.allow_session_select;
    to.allow_terminal = from.allow_terminal;
    to.allow_port_forward = from.allow_port_forward;
    to.allow_camera = from.allow_camera;
    to.allow_record = from.allow_record;
}

// Permissions the current session loses when a switch it just watched turn
// off. Only fields that actually changed are revoked; a switch that was
// already off has no grant to take back.
PermissionMask remote_config_revocations(const HostSettings& settings, uint32_t changed) {
    PermissionMask revoke = 0;
    auto take = [&revoke, &settings, changed](uint32_t bit, bool enabled,
                                              PermissionMask permission) {
        if ((changed & bit) != 0 && !enabled) {
            revoke |= permission;
        }
    };
    take(kRemoteConfigClipboard, settings.allow_clipboard, kPermissionClipboard);
    take(kRemoteConfigFileTransfer, settings.allow_file_transfer,
         kPermissionFileTransfer);
    take(kRemoteConfigAudio, settings.allow_audio, kPermissionAudio);
    take(kRemoteConfigMicrophone, settings.allow_microphone, kPermissionMicrophone);
    take(kRemoteConfigPrivacyMode, settings.allow_privacy_mode, kPermissionPrivacyMode);
    take(kRemoteConfigRestart, settings.allow_restart, kPermissionRestart);
    take(kRemoteConfigBlockInput, settings.allow_block_input, kPermissionBlockInput);
    take(kRemoteConfigOsLogin, settings.allow_os_login, kPermissionOsLogin);
    take(kRemoteConfigSessionSelect, settings.allow_session_select,
         kPermissionSessionSelect);
    take(kRemoteConfigTerminal, settings.allow_terminal, kPermissionTerminal);
    take(kRemoteConfigPortForward, settings.allow_port_forward, kPermissionPortForward);
    take(kRemoteConfigCamera, settings.allow_camera, kPermissionCamera);
    take(kRemoteConfigRecord, settings.allow_record, kPermissionRecord);
    return revoke;
}

bool wait_for_usable_frame(IDxgiCapturer& capturer, bool quiet, const std::atomic<bool>& stopping,
                           std::chrono::milliseconds timeout, std::string* error) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline && !stopping.load()) {
        const CaptureResult result = capturer.capture(std::chrono::milliseconds(100));
        switch (result) {
            case CaptureResult::NoNewFrame:
                continue;
            case CaptureResult::ReinitNeeded:
                if (!quiet) {
                    std::printf("reinitializing: %s\n",
                                capturer.last_error().to_string().c_str());
                }
                if (Error err = capturer.reinitialize(); !err.ok()) {
                    *error = "reinit failed: " + err.to_string();
                    return false;
                }
                continue;
            case CaptureResult::FatalError:
                *error = "capture failed: " + capturer.last_error().to_string();
                return false;
            case CaptureResult::Frame:
                break;
        }
        const FrameInfo& frame = capturer.frame();
        if (!frame.has_video_image()) {
            continue;
        }
        if (frame.rotation_degrees != 0) {
            *error = "the encoder cannot carry " + std::to_string(frame.rotation_degrees) +
                     "-degree output and capture could not rotate the desktop";
            return false;
        }
        return true;
    }
    *error = "no frame with an NV12 texture arrived in " +
             std::to_string(timeout.count() / 1000) + " s";
    return false;
}

// The outputs a merged stream actually composites: every attached display on
// the capturer's adapter. Enumeration also reports outputs on other adapters,
// whose frames cannot be copied into this canvas; leaving them in the
// announced list would make its union disagree with the canvas geometry.
std::vector<DisplayInfo> merged_members(const std::vector<DisplayInfo>& attached,
                                        const DisplayInfo& merged) {
    std::vector<DisplayInfo> members;
    for (const DisplayInfo& listed : attached) {
        if (listed.adapter_luid.LowPart == merged.adapter_luid.LowPart &&
            listed.adapter_luid.HighPart == merged.adapter_luid.HighPart) {
            members.push_back(listed);
        }
    }
    // A hot-unplug can empty the adapter before the capturer notices; the
    // synthetic union still describes the picture that is running.
    if (members.empty()) {
        members.push_back(merged);
    }
    return members;
}

// Label for HostStatus and logs. A merged stream has no single device name.
std::string streamed_display_name(const IDxgiCapturer& capturer) {
    return capturer.merged() ? std::string("all displays (merged)")
                             : display_name_utf8(capturer.display());
}

// The description HelloAck and StreamInfo carry. `attached` is the full list
// from the moment of the call; a merged capturer publishes its members instead
// of the whole list. `source_width`/`source_height` are the captured frame (the
// merged canvas); the encoder's size may be smaller when adaptive resolution
// scaled it. `permissions` is the session mask currently in force.
ServerInfo describe_stream(const std::vector<DisplayInfo>& attached,
                           const IDxgiCapturer& capturer, const IVideoEncoder& encoder,
                           UINT source_width, UINT source_height, uint16_t framerate,
                           PermissionMask permissions, const AudioInfo& audio,
                           bool terminal_persist) {
    const DisplayInfo& display = capturer.display();
    ServerInfo info;
    info.codec = encoder.codec();
    info.chroma = encoder.chroma();
    info.width = static_cast<uint16_t>(encoder.width());
    info.height = static_cast<uint16_t>(encoder.height());
    info.source_width = static_cast<uint16_t>(source_width);
    info.source_height = static_cast<uint16_t>(source_height);
    info.framerate = framerate;
    info.bitrate = encoder.bitrate();
    info.origin_x = display.rect.left;
    info.origin_y = display.rect.top;
    info.merged = capturer.merged();
    info.clipboard_enabled = (permissions & kPermissionClipboard) != 0;
    info.microphone_enabled = (permissions & kPermissionMicrophone) != 0;
    info.file_transfer_enabled = (permissions & kPermissionFileTransfer) != 0;
    info.privacy_mode_enabled = (permissions & kPermissionPrivacyMode) != 0;
    info.session_select_enabled = (permissions & kPermissionSessionSelect) != 0;
    info.terminal_persist = terminal_persist;
    info.audio = audio;
    info.encoder = encoder.implementation().empty() ? "oneVPL" : encoder.implementation();
    const std::vector<DisplayInfo> listed =
        capturer.merged() ? merged_members(attached, display) : attached;
    for (const DisplayInfo& entry : listed) {
        DisplayDescription described;
        described.name = display_name_utf8(entry);
        described.left = entry.rect.left;
        described.top = entry.rect.top;
        described.right = entry.rect.right;
        described.bottom = entry.rect.bottom;
        described.rotation_degrees = static_cast<uint16_t>(entry.rotation_degrees);
        described.primary = entry.primary;
        info.displays.push_back(std::move(described));
    }
    return info;
}

size_t find_display(const std::vector<DisplayInfo>& attached, const DisplayInfo& display) {
    for (size_t i = 0; i < attached.size(); ++i) {
        if (same_display_identity(attached[i], display)) {
            return i;
        }
    }
    return attached.size();
}

// Index of the output containing a virtual-desktop point, or -1. The host is
// DPI aware, so GetCursorPos and the DXGI rects share the same physical
// virtual-desktop space and containment is enough.
int display_index_at(const std::vector<DisplayInfo>& displays, const POINT& point) {
    for (size_t i = 0; i < displays.size(); ++i) {
        const DisplayRect& rect = displays[i].rect;
        if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
            point.y < rect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Index of the output holding the foreground window's monitor, or -1 when
// there is no foreground window. The monitor rect is matched against the DXGI
// list by intersection instead of device name, because the two enumerations
// spell monitor names differently.
int foreground_display_index(const std::vector<DisplayInfo>& displays) {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return -1;
    }
    const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return -1;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return -1;
    }
    const RECT& rect = info.rcMonitor;
    int best = -1;
    LONG best_area = 0;
    for (size_t i = 0; i < displays.size(); ++i) {
        const DisplayRect& display = displays[i].rect;
        const LONG width =
            std::min(rect.right, display.right) - std::max(rect.left, display.left);
        const LONG height =
            std::min(rect.bottom, display.bottom) - std::max(rect.top, display.top);
        if (width <= 0 || height <= 0) {
            continue;
        }
        const LONG area = width * height;
        if (area > best_area) {
            best_area = area;
            best = static_cast<int>(i);
        }
    }
    return best;
}

const char* codec_name(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264:
            return "h264";
        case VideoCodec::Hevc:
            return "hevc";
        case VideoCodec::Vp9:
            return "VP9";
        case VideoCodec::Av1:
            return "av1";
    }
    return "h264";
}

std::string auth_method_suffix(const HostSecurity::Result& result) {
    if (result.via_token) {
        return " with the access token";
    }
    if (result.used_temporary_password) {
        return " with the temporary password";
    }
    if (result.via_trusted_device) {
        return " with a trusted device";
    }
    return " with a password";
}

std::string permission_names(PermissionMask mask) {
    std::string text;
    auto add = [&text](const char* name) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    };
    if ((mask & kPermissionInput) != 0) {
        add("keyboard and pointer");
    }
    if ((mask & kPermissionClipboard) != 0) {
        add("clipboard");
    }
    if ((mask & kPermissionAudio) != 0) {
        add("audio");
    }
    if ((mask & kPermissionFileTransfer) != 0) {
        add("file transfer");
    }
    if ((mask & kPermissionRestart) != 0) {
        add("restart");
    }
    if ((mask & kPermissionBlockInput) != 0) {
        add("block input");
    }
    if ((mask & kPermissionOsLogin) != 0) {
        add("OS login");
    }
    if ((mask & kPermissionSessionSelect) != 0) {
        add("session select");
    }
    if ((mask & kPermissionRemoteConfig) != 0) {
        add("remote configuration");
    }
    return text.empty() ? "view only" : text;
}

// The host-control actions run in the worker process, which the service starts
// in the interactive user session, so these Win32 calls reach the operator's
// real desktop instead of session 0.
bool enable_process_privilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) {
        return false;
    }
    LUID luid{};
    bool ok = LookupPrivilegeValueW(nullptr, name, &luid) != FALSE;
    if (ok) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) !=
                 FALSE &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

std::string last_error_text() {
    return "error " + std::to_string(GetLastError());
}

bool restart_host_system(std::string* error) {
    // SeShutdownPrivilege is granted to Users by default; an interactive
    // worker can reboot without elevation. A local policy that removes it
    // surfaces as a Failed result instead of silently doing nothing.
    if (!enable_process_privilege(L"SeShutdownPrivilege")) {
        *error = "the shutdown privilege is not available";
        return false;
    }
    const DWORD reason = SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_APPLICATION |
                         SHTDN_REASON_MINOR_MAINTENANCE;
    if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, reason)) {
        *error = "ExitWindowsEx failed (" + last_error_text() + ")";
        return false;
    }
    return true;
}

bool lock_host_workstation(std::string* error) {
    if (!LockWorkStation()) {
        *error = "LockWorkStation failed (" + last_error_text() + ")";
        return false;
    }
    return true;
}

bool block_host_input(bool block, std::string* error) {
    // BlockInput binds the block to the calling thread: the session loop keeps
    // running on this thread, and Windows releases the block if the thread
    // ever exits without calling it again.
    if (!BlockInput(block ? TRUE : FALSE)) {
        *error = std::string("BlockInput(") + (block ? "on" : "off") + ") failed (" +
                 last_error_text() + ")";
        return false;
    }
    return true;
}

const char* host_action_code_name(HostActionCode code) {
    switch (code) {
        case HostActionCode::Ok:
            return "ok";
        case HostActionCode::Denied:
            return "denied";
        case HostActionCode::Unsupported:
            return "unsupported";
        case HostActionCode::Busy:
            return "busy";
        case HostActionCode::Failed:
            return "failed";
    }
    return "unknown";
}

const char* remote_config_code_name(RemoteConfigCode code) {
    switch (code) {
        case RemoteConfigCode::Ok:
            return "ok";
        case RemoteConfigCode::Denied:
            return "denied";
        case RemoteConfigCode::Unsupported:
            return "unsupported";
        case RemoteConfigCode::Busy:
            return "busy";
        case RemoteConfigCode::Failed:
            return "failed";
    }
    return "unknown";
}

std::string remote_config_change_names(uint32_t mask) {
    std::string text;
    auto add = [&text](const char* name) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    };
    if ((mask & kRemoteConfigLockAfterSessionEnd) != 0) {
        add("lock after session end");
    }
    if ((mask & kRemoteConfigClipboard) != 0) {
        add("clipboard");
    }
    if ((mask & kRemoteConfigFileTransfer) != 0) {
        add("file transfer");
    }
    if ((mask & kRemoteConfigAudio) != 0) {
        add("audio");
    }
    if ((mask & kRemoteConfigMicrophone) != 0) {
        add("microphone");
    }
    if ((mask & kRemoteConfigPrivacyMode) != 0) {
        add("privacy mode");
    }
    if ((mask & kRemoteConfigRestart) != 0) {
        add("restart");
    }
    if ((mask & kRemoteConfigBlockInput) != 0) {
        add("block input");
    }
    if ((mask & kRemoteConfigOsLogin) != 0) {
        add("OS login");
    }
    if ((mask & kRemoteConfigSessionSelect) != 0) {
        add("session select");
    }
    if ((mask & kRemoteConfigTerminal) != 0) {
        add("terminal");
    }
    if ((mask & kRemoteConfigPortForward) != 0) {
        add("port forward");
    }
    if ((mask & kRemoteConfigCamera) != 0) {
        add("camera");
    }
    if ((mask & kRemoteConfigRecord) != 0) {
        add("record");
    }
    return text;
}

// Puts a session's temporary display mode back when the session ends, however
// it ends: the viewer left, the run stopped, or an error unwound the loop.
class ModeRestoreGuard {
public:
    ModeRestoreGuard(std::unique_ptr<DisplayModeChange>& change,
                     std::function<void(const std::string&)> log)
        : change_(change), log_(std::move(log)) {}
    ModeRestoreGuard(const ModeRestoreGuard&) = delete;
    ModeRestoreGuard& operator=(const ModeRestoreGuard&) = delete;
    ~ModeRestoreGuard() {
        if (!change_ || !change_->has_saved()) {
            return;
        }
        std::string error;
        switch (change_->restore(&error)) {
            case ModeRestore::Restored:
                log_("display mode restored to the one from before the session");
                break;
            case ModeRestore::Skipped:
                log_("display mode left alone: it was changed outside the session");
                break;
            case ModeRestore::Failed:
                log_("display mode restore failed: " + error);
                break;
            case ModeRestore::None:
                break;
        }
    }

private:
    std::unique_ptr<DisplayModeChange>& change_;
    std::function<void(const std::string&)> log_;
};

}  // namespace

std::unique_ptr<HostServer> HostServer::create(HostServerConfig config, std::string* error) {
    std::unique_ptr<HostServer> server(new HostServer(std::move(config)));
    if (server->settings_.access_token.empty()) {
        server->settings_.access_token = generate_access_token();
        if (server->config_.token_changed) {
            server->config_.token_changed(server->settings_.access_token);
        }
    }
    server->permission_policy_ = std::make_shared<std::atomic<PermissionMask>>(
        server->policy_permissions());
    if (!server->settings_.wallpaper_state.empty()) {
        // A previous host may have been killed before it put the wallpaper
        // back; the backup file outlives the process for exactly this case.
        std::string wallpaper_error;
        if (!restore_abandoned_wallpaper(server->settings_.wallpaper_state,
                                         &wallpaper_error)) {
            server->log("restore wallpaper from the last run failed: " + wallpaper_error);
        }
    }
    if (!server->open_session(error)) {
        return nullptr;
    }
    // The policy binds signed Hellos to the certificate just loaded, so it
    // exists only once the session knows its fingerprint.
    server->security_ = std::make_shared<HostSecurity>(
        server->security_config(server->session_->server_fingerprint()));
    {
        server->registry_ = std::make_shared<ViewerRegistry>();
        server->registry_->max_viewers = server->settings_.max_viewers;
        copy_remote_config_fields(server->settings_, server->registry_->remote_settings);
        server->registry_->remote_settings_valid = true;
        server->registry_->lock_after_session_end = server->settings_.lock_after_session_end;
    }
    server->publish_security_status();
    if (server->config_.service_mode && server->settings_.password_hash.empty() &&
        (!server->config_.service_approval || server->settings_.auth_mode == HostAuthMode::Password)) {
        server->log("service mode has no permanent password; every viewer will be refused "
                    "until one is set");
    }
    return server;
}

HostServer::~HostServer() {
    stop();
    carrier_.reset();
    if (registry_ && !adopted_) {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        registry_->focused.reset();
    }
    viewers_.clear();
    session_.reset();
}

void HostServer::start() {
    if (started_) {
        return;
    }
    started_ = true;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void HostServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (clipboard_listener_ != 0) {
        ClipboardListener::Unsubscribe(clipboard_listener_);
        clipboard_listener_ = 0;
    }
    if (clipboard_worker_) clipboard_worker_->Cancel();
    if (carrier_) {
        carrier_->stop();
    }
}

void HostServer::wait() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool HostServer::running() const {
    return running_.load();
}

HostStatus HostServer::status() const {
    HostStatus snapshot;
    if (registry_ && !adopted_) {
        size_t active = 0;
        uint64_t admitted = 0;
        std::shared_ptr<HostServer> target;
        {
            std::lock_guard<std::mutex> lock(registry_->mutex);
            active = registry_->active;
            admitted = registry_->admitted;
            target = registry_->focused.lock();
        }
        if (target && target.get() != this) {
            snapshot = target->status();
            // A worker has no listener state of its own; the identity, token,
            // policy and carrier belong to the host and stay the parent's.
            std::lock_guard<std::mutex> lock(status_mutex_);
            snapshot.fingerprint = status_.fingerprint;
            snapshot.token = status_.token;
            snapshot.auth_mode = status_.auth_mode;
            snapshot.password_configured = status_.password_configured;
            snapshot.two_factor_enabled = status_.two_factor_enabled;
            snapshot.temporary_password = status_.temporary_password;
            snapshot.carrier_listening = status_.carrier_listening;
            snapshot.carrier_port = status_.carrier_port;
            snapshot.carrier_error = status_.carrier_error;
        } else {
            std::lock_guard<std::mutex> lock(status_mutex_);
            snapshot = status_;
        }
        snapshot.active_viewers = active;
        // `admitted` is the shared count; the focused session reports one
        // session of its own.
        snapshot.stats.sessions = admitted;
        if (active > 0) {
            snapshot.state = HostState::Streaming;
        }
    } else {
        std::lock_guard<std::mutex> lock(status_mutex_);
        snapshot = status_;
    }
    if (carrier_) {
        const CarrierServerStats stats = carrier_->stats();
        snapshot.carrier_connections = stats.connections;
        snapshot.carrier_active = stats.active;
    }
    return snapshot;
}

std::string HostServer::token() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_.token;
}

bool HostServer::open_session(std::string* error) {
    QuicServerConfig quic;
    quic.bind_address = settings_.bind_address;
    quic.port = settings_.port;
    quic.identity_scope = config_.identity_scope;
    quic.identity_user_token = config_.identity_user_token;
    quic.expected_identity_fingerprint = config_.expected_identity_fingerprint;
    // Above one, the listener hands every connection to its own session
    // worker instead of holding a single slot.
    quic.max_authenticated_clients = config_.serve_once ? 1u : std::max(1u, settings_.max_viewers);
    quic.max_clients = quic.max_authenticated_clients + 8;
    quic.allow_input = settings_.allow_input;
    quic.allow_clipboard = settings_.allow_clipboard;
    quic.clipboard_direction = settings_.clipboard_direction;
    quic.allow_file_transfer = settings_.allow_file_transfer;
    quic.allow_port_forward = settings_.allow_port_forward;
    quic.allow_terminal = settings_.allow_terminal;
    quic.allow_camera = settings_.allow_camera;
    quic.allow_record = settings_.allow_record;
    quic.allow_restart = settings_.allow_restart;
    quic.allow_block_input = settings_.allow_block_input;
    quic.allow_os_login = settings_.allow_os_login;
    // Only the service backend can move the worker; a tray host or a worker
    // started by hand advertises nothing and refuses every request.
    quic.allow_session_select = settings_.allow_session_select && config_.service_mode &&
                                static_cast<bool>(config_.list_sessions) &&
                                static_cast<bool>(config_.select_session);
    quic.allow_audio = settings_.allow_audio;
    quic.allow_microphone = settings_.allow_microphone;
    quic.allow_privacy_mode = settings_.allow_privacy_mode;
    quic.allow_permission_change = settings_.allow_permission_change;
    quic.allow_remote_config = settings_.allow_remote_config;
    quic.permission_policy = permission_policy_;
    quic.dry_run_input = config_.dry_run_input;
    // The one-shot tool reports a refused password instead of waiting for the
    // next viewer; the interactive host keeps listening.
    quic.stop_after_denial = config_.serve_once;
    // The policy owns every credential, including the legacy token; the
    // transport's own token gate stays off.
    quic.authenticate = [this](const ClientRequest& request) {
        return authenticate_client(request);
    };
    quic.approve_client = [this](const ClientRequest& request) {
        return approve_client(request);
    };
    session_ = QuicSession::create_server(quic, error);
    if (!session_) {
        return false;
    }
    const std::string fingerprint = session_->server_fingerprint();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.fingerprint = fingerprint;
        status_.token = settings_.access_token;
    }
    if (!carrier_ && settings_.carrier) {
        if (settings_.port == 65535) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.carrier_error =
                    "QUIC port 65535 leaves no port + 1 for the fallback";
            }
            log("carrier disabled: QUIC port 65535 leaves no port + 1");
        } else {
            CarrierServerConfig carrier;
            carrier.bind_address = settings_.bind_address;
            carrier.port = static_cast<uint16_t>(settings_.port + 1);
            // The fallback's UDP side always dials the QUIC listener from the
            // same machine; a wildcard bind reaches it over loopback.
            const bool wildcard = settings_.bind_address.empty() ||
                                  settings_.bind_address == "0.0.0.0" ||
                                  settings_.bind_address == "::";
            carrier.quic_address = wildcard ? "127.0.0.1" : settings_.bind_address;
            carrier.quic_port = settings_.port;
            carrier.log = [this](const std::string& message) { log(message); };
            std::string carrier_error;
            carrier_ = CarrierServer::create(carrier, &carrier_error);
            uint16_t bound = 0;
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.carrier_listening = carrier_ != nullptr;
                status_.carrier_port = carrier_ ? carrier_->port() : 0;
                status_.carrier_error = carrier_ ? std::string() : carrier_error;
                bound = status_.carrier_port;
            }
            if (carrier_) {
                log("tcp/websocket carrier listening on " + settings_.bind_address + ":" +
                    std::to_string(bound));
            } else {
                log("carrier unavailable, serving QUIC only: " + carrier_error);
            }
        }
    }
    if (security_) {
        // A reopened listener may see a new certificate; signed Hellos must
        // bind to the one now on the wire.
        refresh_security_config();
    }
    return true;
}

HostSecurity::Config HostServer::security_config(const std::string& fingerprint) {
    HostSecurity::Config config;
    config.mode = settings_.auth_mode;
    config.password_hash = settings_.password_hash;
    config.totp_secret = settings_.totp_secret;
    if (!config.totp_secret.empty()) {
        ScopedUserContext original(config_.identity_user_token.get());
        // host.ini stores the secret DPAPI-protected; an early/hand-written
        // value may still be plain base32.
        std::string plain;
        std::string error;
        if (!original.ok()) error = "cannot enter original user context: " + std::to_string(original.error());
        if (original.ok() && unprotect_totp_secret(config.totp_secret, secret_scope(config_.identity_scope),
                                  plain, &error)) {
            config.totp_secret = std::move(plain);
        } else {
            std::vector<uint8_t> key;
            if (!parse_totp_secret(config.totp_secret, key)) {
                // Leave the unusable string in place: HostSecurity marks 2FA
                // invalid and refuses password logins instead of silently
                // dropping the second factor.
                log("two-factor secret cannot be read (" + error +
                    "); password logins will be refused");
            }
        }
    }
    // The listener's settings copy can predate a worker's remote change.
    // Credential/token refreshes must not roll back the shared capability policy.
    config.default_permissions = permission_policy_ ? permission_policy_->load()
                                                     : settings_permissions(settings_);
    // A viewer must not be told about a switch this process cannot perform.
    if (!config_.service_mode || !config_.list_sessions || !config_.select_session) {
        config.default_permissions &=
            ~kPermissionSessionSelect;
    }
    config.allow_permission_change = settings_.allow_permission_change;
    config.ip_whitelist = settings_.ip_whitelist;
    config.service_mode = config_.service_mode;
    config.service_approval = config_.service_approval && static_cast<bool>(config_.approve_client);
    config.token_auth = settings_.token_auth;
    config.access_token = settings_.access_token;
    config.host_fingerprint = fingerprint;
    config.policy_state = settings_.policy_state;
    config.device_id = settings_.tunnel_id;
    config.access_issuer = settings_.access_issuer;
    config.access_kid = settings_.access_kid;
    config.access_public_key = settings_.access_public_key;
    config.access_keys.clear();
    for (const HostSettings::AccessKeyPin& pin : settings_.access_keys) {
        config.access_keys.push_back({pin.kid, pin.public_key});
    }
    // Once the policy exists it is the authority: the operator may have
    // revoked a device since the settings were loaded.
    config.trusted_devices = security_ ? security_->config().trusted_devices
                                       : settings_.trusted_devices;
    config.trusted_device_lifetime_seconds =
        settings_.trusted_device_days == 0
            ? 0
            : static_cast<int64_t>(settings_.trusted_device_days) * 24 * 60 * 60;
    return config;
}

ClientAuthResult HostServer::authenticate_client(const ClientRequest& request) {
    ClientAuthResult auth;
    if (!security_) {
        auth.action = ClientAuthAction::Deny;
        auth.denial = AccessDenial::Declined;
        auth.reason = "the host security policy is not ready";
        return auth;
    }
    HostSecurity::TicketRequest ticket;
    const HostSecurity::TicketRequest* ticket_request = nullptr;
    if (!request.hello.access_ticket.empty()) {
        ticket.signed_ticket = request.hello.access_ticket;
        ticket.host_nonce = request.host_nonce;
        ticket.proof = request.hello.ticket_proof;
        ticket_request = &ticket;
    }
    pending_security_ =
        security_->evaluate(request.hello, request.peer_address, unix_now(), ticket_request);
    // Remember what a later renewal has to re-prove. Cleared for a session
    // without a ticket so a previous viewer's binding cannot leak into this
    // one on a reused single-session host.
    ticket_host_nonce_ = request.host_nonce;
    ticket_client_nonce_.assign(request.hello.nonce.begin(), request.hello.nonce.end());
    ticket_client_key_ = pending_security_.via_access_ticket
                             ? pending_security_.ticket_client_key
                             : std::array<uint8_t, kTicketClientKeySize>{};
    ticket_renew_failed_ = false;
    {
        // Published before the session starts so an online revocation that
        // arrives during approval still matches the right policy and issuer.
        std::lock_guard<std::mutex> lock(ticket_meta_mutex_);
        managed_session_ = pending_security_.via_access_ticket;
        ticket_key_id_ = pending_security_.ticket_key_id;
        ticket_policy_version_ = pending_security_.ticket_policy_version;
    }
    const std::string from =
        request.peer_address.empty() ? std::string("an unknown address") : request.peer_address;
    auth.permissions = pending_security_.permissions;
    approved_permissions_ = 0;
    lease_stripped_input_ = false;
    // A refused Hello must not take the control lease with it.
    if (adopted_ && registry_ && pending_security_.decision != HostSecurity::Decision::Deny) {
        if (request.mode != SessionMode::Desktop) {
            // A files-only connection has no input surface and its worker
            // refuses host actions, so drop those grants here instead of
            // advertising controls that would only be denied.
            auth.permissions &= ~(kPermissionInput | kPermissionRestart |
                                  kPermissionBlockInput | kPermissionSessionSelect);
        } else if (settings_.allow_input && (auth.permissions & kPermissionInput) != 0) {
            if (!registry_->acquire_control(this)) {
                auth.permissions &= ~kPermissionInput;
                lease_stripped_input_ = true;
                log("another viewer already holds control; this viewer joins view-only");
            }
        }
        // The lease strip is temporal, not a policy decision: keep the bit so
        // the transport's ceiling can be raised once control is free. A
        // files-only or policy-stripped mask stays as it is.
        approved_permissions_ = auth.permissions;
        if (lease_stripped_input_) {
            approved_permissions_ |= kPermissionInput;
        }
    }
    switch (pending_security_.decision) {
        case HostSecurity::Decision::Allow:
            auth.action = ClientAuthAction::Allow;
            log("admitted " + from + auth_method_suffix(pending_security_));
            if (!adopted_) commit_security();
            break;
        case HostSecurity::Decision::RequireApproval:
            auth.action = ClientAuthAction::RequireApproval;
            log("a viewer from " + from + " is waiting for approval");
            break;
        case HostSecurity::Decision::Deny:
            auth.action = ClientAuthAction::Deny;
            auth.denial = pending_security_.denial;
            auth.reason = pending_security_.reason;
            log("refused " + from + ": " + pending_security_.reason);
            audit_denial(request.peer_address, pending_security_.denial,
                         pending_security_.reason);
            break;
    }
    return auth;
}

ClientApproval HostServer::approve_client(const ClientRequest& request) {
    if (!config_.approve_client) {
        return ClientApproval{};
    }
    // The approval window explains why it is up; the transport request has no
    // opinion on that, so annotate a copy before handing it over.
    ClientRequest annotated = request;
    annotated.method = settings_.auth_mode == HostAuthMode::Click
                           ? ClientApprovalMethod::Click
                           : ClientApprovalMethod::NoPassword;
    annotated.device_proof = pending_security_.request_trust;
    const ClientApproval approval = config_.approve_client(annotated);
    const std::string from =
        request.peer_address.empty() ? std::string("an unknown address") : request.peer_address;
    if (!approval.allow) {
        if (annotated.peer_alive && !annotated.peer_alive()) {
            log("the viewer from " + from + " left while waiting for approval");
        } else {
            log("the operator declined the viewer from " + from);
            audit_denial(request.peer_address, AccessDenial::Declined,
                         "the operator declined the connection");
        }
        return approval;
    }
    if (annotated.peer_alive && !annotated.peer_alive()) {
        // The viewer stopped waiting while the prompt was open. Do not record
        // a trusted device or spend a password for a session that the
        // transport is about to refuse.
        log("the viewer from " + from + " left while waiting for approval");
        return ClientApproval{};
    }
    // The trust list and a spent temporary password are committed only now:
    // a refused approval is a denial, not a login.
    if (adopted_ && registry_) {
        // The operator's decision also bounds what the lease may grant later.
        approved_permissions_ &= approval.permissions;
    }
    if (!adopted_) commit_security();
    log("the operator approved the viewer from " + from + "; granting " +
        permission_names(request.permissions & approval.permissions));
    return approval;
}

void HostServer::commit_security() {
    if (!security_) {
        return;
    }
    security_->note_connected(pending_security_, unix_now());
    // The login may have rehashed an older password encoding at the current
    // cost; persist it while the plaintext was available.
    const std::string hash = security_->password_hash();
    if (!hash.empty() && hash != settings_.password_hash) {
        settings_.password_hash = hash;
        log("the permanent password hash was upgraded to the current cost");
        if (config_.password_hash_changed) {
            config_.password_hash_changed(hash);
        }
    }
    publish_trusted_devices();
    publish_security_status();
}

void HostServer::refresh_security_config() {
    if (!security_) {
        return;
    }
    std::string fingerprint;
    if (session_) {
        fingerprint = session_->server_fingerprint();
    } else {
        std::lock_guard<std::mutex> lock(status_mutex_);
        fingerprint = status_.fingerprint;
    }
    security_->set_config(security_config(fingerprint));
    publish_security_status();
}

void HostServer::publish_trusted_devices() {
    if (!security_ || !config_.trusted_devices_changed) {
        return;
    }
    const std::vector<TrustedDevice> devices = security_->config().trusted_devices;
    // Sessions that never carry a device key must not rewrite the file.
    if (devices.empty() && settings_.trusted_devices.empty()) {
        return;
    }
    config_.trusted_devices_changed(devices);
}

void HostServer::publish_security_status() {
    if (!security_) {
        return;
    }
    const HostSecurity::Config config = security_->config();
    const std::string temporary = security_->temporary_password();
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.auth_mode = config.mode;
    status_.password_configured = !config.password_hash.empty();
    status_.two_factor_enabled = !config.totp_secret.empty();
    status_.temporary_password = temporary;
}

std::string HostServer::refresh_temporary_password() {
    if (!security_) {
        return {};
    }
    const std::string password = security_->refresh_temporary_password();
    publish_security_status();
    return password;
}

std::vector<TrustedDevice> HostServer::trusted_devices() const {
    if (!security_) {
        return {};
    }
    return security_->trusted_devices(unix_now());
}

bool HostServer::revoke_trusted_device(const std::vector<uint8_t>& public_key) {
    if (!security_ || !security_->revoke_trusted_device(public_key)) {
        return false;
    }
    publish_trusted_devices();
    return true;
}

void HostServer::clear_trusted_devices() {
    if (!security_) {
        return;
    }
    security_->clear_trusted_devices();
    publish_trusted_devices();
}

std::shared_ptr<HostServer> HostServer::focused_viewer() const {
    if (!registry_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(registry_->mutex);
    return registry_->focused.lock();
}

std::shared_ptr<HostServer> HostServer::find_viewer(uint64_t id) const {
    if (!id || !registry_) return {};
    std::lock_guard lock(registry_->mutex);
    for (const auto& weak : registry_->workers)
        if (auto viewer = weak.lock(); viewer && viewer->connection_id_.load() == id) return viewer;
    return {};
}

std::vector<HostViewer> HostServer::viewers() const {
    std::vector<HostViewer> result;
    const auto append = [&](const HostServer& viewer, bool controls) {
        const auto state = viewer.status();
        if (state.connection_id && state.state == HostState::Streaming)
            result.push_back({state.connection_id, state.peer_address, state.session_mode,
                state.session_permissions, state.approved_permissions, controls,
                viewer.disconnect_requested_.load()});
    };
    if (registry_ && !adopted_) {
        std::vector<std::pair<std::shared_ptr<HostServer>, bool>> live;
        {
            std::lock_guard lock(registry_->mutex);
            for (const auto& weak : registry_->workers)
                if (auto viewer = weak.lock()) live.emplace_back(viewer, registry_->controller == viewer.get());
        }
        for (const auto& [viewer, controls] : live) append(*viewer, controls);
    } else {
        const auto state = status();
        append(*this, state.session_mode == SessionMode::Desktop && (state.session_permissions & kPermissionInput));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

bool HostServer::disconnect_viewer(uint64_t id) {
    if (registry_ && !adopted_) {
        auto target = find_viewer(id);
        return target && target->disconnect_viewer(id);
    }
    std::lock_guard lock(operator_mutex_);
    if (!id || connection_id_ != id || stopping_) return false;
    disconnect_requested_ = true;
    return true;
}

void HostServer::revoke_managed_sessions(const std::string& reason, uint32_t policy_version,
                                         const std::string& kid) {
    if (policy_version == 0 && kid.empty()) {
        return;
    }
    if (registry_ && !adopted_) {
        std::vector<std::shared_ptr<HostServer>> workers;
        {
            std::lock_guard lock(registry_->mutex);
            for (const auto& weak : registry_->workers)
                if (auto worker = weak.lock()) workers.push_back(std::move(worker));
        }
        for (const auto& worker : workers)
            worker->revoke_managed_sessions(reason, policy_version, kid);
        return;
    }
    std::lock_guard action(operator_mutex_);
    if (stopping_ || disconnect_requested_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.state != HostState::Streaming) {
            return;
        }
    }
    bool managed = false;
    std::string key_id;
    uint32_t version = 0;
    {
        std::lock_guard<std::mutex> lock(ticket_meta_mutex_);
        managed = managed_session_;
        key_id = ticket_key_id_;
        version = ticket_policy_version_;
    }
    if (!managed) {
        return;
    }
    const bool by_key = !kid.empty() && key_id == kid;
    const bool by_policy = policy_version != 0 && version < policy_version;
    if (!by_key && !by_policy) {
        return;
    }
    disconnect_requested_ = true;
    log("the server ended this managed session: " + reason);
}

void HostServer::set_permissions(PermissionMask permissions) {
    set_permissions(status().connection_id, permissions);
}

bool HostServer::set_permissions(uint64_t id, PermissionMask permissions) {
    if (registry_ && !adopted_) {
        auto target = find_viewer(id);
        return target && target->set_permissions(id, permissions);
    }
    std::lock_guard action(operator_mutex_);
    if (!id || connection_id_ != id || disconnect_requested_ || stopping_) return false;
    std::lock_guard<std::mutex> lock(permission_mutex_);
    PermissionMask mask = permissions & kPermissionAll;
    // Input injection has one owner. Granting input to a viewer that does not
    // hold the lease would put two keyboards on one desktop, so the grant
    // keeps that viewer view-only until control is free.
    if (registry_ && adopted_ && (mask & kPermissionInput) != 0 &&
        !registry_->acquire_control(this)) {
        mask &= ~kPermissionInput;
        log("another viewer controls input; this viewer stays view-only");
    }
    pending_permissions_.store(mask);
    permission_update_pending_.store(true);
    return true;
}

void HostServer::set_lock_after_session_end(bool enabled) {
    settings_.lock_after_session_end = enabled;
    if (!registry_) {
        return;
    }
    std::lock_guard<std::mutex> lock(registry_->mutex);
    registry_->remote_settings.lock_after_session_end = enabled;
    registry_->remote_settings_valid = true;
    registry_->lock_after_session_end.store(enabled);
    ++registry_->remote_settings_version;
}

void HostServer::set_voice_microphone(const std::string& device_id) {
    {
        std::lock_guard<std::mutex> lock(voice_microphone_mutex_);
        pending_voice_microphone_ = device_id;
        voice_microphone_pending_ = true;
    }
    if (!registry_) {
        // A sequential host runs its session in this object; the pending
        // value is picked up by the frame loop.
        return;
    }
    // The listener runs no session of its own, so the choice is published
    // where workers and sessions created later can see it.
    std::lock_guard<std::mutex> lock(registry_->mutex);
    registry_->voice_microphone = device_id;
    registry_->voice_microphone_valid = true;
}

bool HostServer::send_chat(const std::string& text) {
    return send_chat(status().connection_id, text);
}

bool HostServer::send_chat(uint64_t id, const std::string& text) {
    if (registry_ && !adopted_) {
        auto target = find_viewer(id);
        return target && target->send_chat(id, text);
    }
    std::lock_guard action(operator_mutex_);
    if (!id || connection_id_ != id || disconnect_requested_ || stopping_) return false;
    if (!valid_chat_text(text)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.state != HostState::Streaming) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(chat_mutex_);
    if (pending_chat_.size() >= 16) {
        return false;
    }
    pending_chat_.push_back(text);
    return true;
}

bool HostServer::start_file_download(const std::string& utf8_path, std::string* error) {
    return start_file_download(status().connection_id, utf8_path, error);
}

bool HostServer::start_file_download(uint64_t id, const std::string& utf8_path, std::string* error,
                                     std::shared_ptr<void> file_access_token) {
    if (registry_ && !adopted_) {
        auto target = find_viewer(id);
        if (target) return target->start_file_download(id, utf8_path, error, std::move(file_access_token));
        if (error) *error = "the selected viewer is no longer connected";
        return false;
    }
    std::lock_guard action(operator_mutex_);
    if (!id || connection_id_ != id || disconnect_requested_ || stopping_) {
        if (error) *error = "the selected viewer is no longer connected";
        return false;
    }
    const auto fail = [error](const std::string& text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };
    if (!running_.load(std::memory_order_acquire)) {
        return fail("the host is not running");
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.state != HostState::Streaming) {
            return fail("no viewer is streaming");
        }
        if ((status_.session_permissions & kPermissionFileTransfer) == 0) {
            return fail("file transfer is not granted to this viewer");
        }
    }
    if (!settings_.allow_file_transfer) {
        return fail("file transfer is disabled in the settings");
    }
    const std::filesystem::path path = path_from_utf8(utf8_path);
    std::error_code code;
    uintmax_t size = 0;
    {
        ScopedFileAccess access(file_access_token);
        if (!access.ok()) return fail("the operator's file access token is unavailable");
        size = std::filesystem::file_size(path, code);
        if (code || !std::filesystem::is_regular_file(path, code) || code)
            return fail("the path is not a readable file");
        // Directory enumeration can expose size/attributes even when the
        // file's DACL denies reading its contents. Refuse the IPC operation
        // immediately instead of deferring that refusal to the hasher.
        if (file_access_token) {
            std::ifstream readable(path, std::ios::binary);
            if (!readable) return fail("the path is not a readable file");
        }
    }
    if (size > kMaxFileTransferBytes) {
        return fail("the file exceeds the protocol's 16 GiB limit");
    }
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        if (!pending_download_path_.empty()) {
            return fail("another file is already queued");
        }
        switch (send_.stage) {
            case FileSend::Stage::kHashing:
            case FileSend::Stage::kReady:
            case FileSend::Stage::kOffered:
            case FileSend::Stage::kSending:
            case FileSend::Stage::kFinishing:
                return fail("another file is already being sent");
            default:
                break;
        }
        pending_download_path_ = utf8_path;
        pending_download_clipboard_ = false;
        pending_download_token_ = std::move(file_access_token);
    }
    return true;
}

void HostServer::publish_download() {
    std::lock_guard<std::mutex> lock(download_mutex_);
    bool active = false;
    switch (send_.stage) {
        case FileSend::Stage::kHashing:
        case FileSend::Stage::kReady:
        case FileSend::Stage::kOffered:
        case FileSend::Stage::kSending:
        case FileSend::Stage::kFinishing:
            active = true;
            break;
        default:
            break;
    }
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.download_active = active;
    status_.download_name = send_.name;
    status_.download_size = send_.size;
    status_.download_sent = send_.sent;
    status_.download_error = send_.error;
}

void HostServer::run() {
    if (adopted_) {
        run_adopted();
        return;
    }
    run_multi();
}

void HostServer::run_single() {
    HostServerStats stats;
    uint32_t failures = 0;
    while (!stopping_.load()) {
        if (!session_) {
            std::string error;
            if (!open_session(&error)) {
                log("listener failed: " + error);
                publish_state(HostState::Failed, "QUIC server: " + error);
                if (config_.serve_once) {
                    break;
                }
                if (config_.max_consecutive_failures != 0 &&
                    ++failures >= config_.max_consecutive_failures) {
                    break;
                }
                sleep_unless_stopping(std::chrono::seconds(2));
                continue;
            }
        }
        publish_state(HostState::Listening);
        log("listening on " + settings_.bind_address + ":" + std::to_string(settings_.port));

        const bool first = stats.sessions == 0;
        const auto deadline =
            first && config_.first_client_timeout_ms > 0
                ? Clock::now() + std::chrono::milliseconds(config_.first_client_timeout_ms)
                : Clock::time_point::max();
        bool accepted = false;
        std::string wait_error;
        while (!stopping_.load()) {
            auto slice = std::chrono::milliseconds(500);
            if (deadline != Clock::time_point::max()) {
                const auto now = Clock::now();
                if (now >= deadline) {
                    wait_error = "no client connected in " +
                                 std::to_string(config_.first_client_timeout_ms / 1000) + " s";
                    break;
                }
                slice = std::min(slice, std::chrono::duration_cast<std::chrono::milliseconds>(
                                            deadline - now));
            }
            Error waited = session_->wait_for_client(slice);
            if (waited.ok()) {
                accepted = true;
                break;
            }
            if (waited.status() != Status::WaitTimeout) {
                wait_error = "waiting for a client: " + waited.to_string();
                break;
            }
        }
        if (!accepted) {
            if (stopping_.load()) {
                break;
            }
            log(wait_error);
            publish_state(HostState::Failed, wait_error);
            session_.reset();
            if (config_.serve_once) {
                break;
            }
            if (config_.max_consecutive_failures != 0 &&
                ++failures >= config_.max_consecutive_failures) {
                break;
            }
            sleep_unless_stopping(std::chrono::seconds(2));
            continue;
        }

        failures = 0;
        std::string error;
        bool peer_left = false;
        const bool ok = serve_current_session(stats, true, &error, &peer_left);
        if (settings_.lock_after_session_end) {
            lock_after_session_end_workstation();
        }
        if (!ok || !error.empty()) {
            log(error.empty() ? "the session ended" : error);
            publish_state(HostState::Failed, error.empty() ? "the session failed" : error);
            session_.reset();
            if (config_.serve_once) {
                break;
            }
            if (config_.max_consecutive_failures != 0 &&
                ++failures >= config_.max_consecutive_failures) {
                break;
            }
            sleep_unless_stopping(std::chrono::seconds(2));
            continue;
        }

        session_->reset_for_next_client();
        if (config_.serve_once) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.state != HostState::Failed) {
            status_.state = HostState::Stopped;
        }
    }
    running_ = false;
}

bool HostServer::ticket_lease_expired(const Clock::time_point& now) {
    if (ticket_deadline_ == Clock::time_point::max() || now < ticket_deadline_) {
        return false;
    }
    ticket_deadline_ = Clock::time_point::max();
    log("the access ticket expired; closing the session");
    ticket_expired_ = true;
    return true;
}

bool HostServer::apply_ticket_renewals(std::string* error) {
    TicketRenewRequest request;
    while (session_->take_ticket_renew(request)) {
        if (!pending_security_.via_access_ticket || !security_) {
            *error = "the viewer renewed a ticket on a session that has none";
            return false;
        }
        HostSecurity::TicketRenewal renewal;
        renewal.signed_ticket = std::move(request.signed_ticket);
        renewal.proof = std::move(request.proof);
        const HostSecurity::RenewalResult result = security_->renew_ticket(
            renewal, session_->client_address(), ticket_host_nonce_, ticket_client_nonce_,
            ticket_client_key_, pending_security_.ticket_subject, unix_now());
        TicketRenewResult answer;
        if (!result.accepted) {
            answer.code = TicketRenewCode::Refused;
            answer.reason = result.reason;
            log("the ticket renewal was refused: " + result.reason);
            // The answer goes out before the close so the viewer can explain
            // the end instead of reporting a bare transport drop.
            session_->send_ticket_renew_result(answer);
            ticket_renew_failed_ = true;
            error->clear();
            return false;
        }
        // A new ticket can only narrow what is already in force: the operator
        // approved a mask at connect time, and a later policy edit cannot
        // widen it behind their back.
        const PermissionMask current = session_->permissions();
        const PermissionMask next = current & result.permissions;
        if (next != current) {
            session_->revoke_policy_permissions(current & ~next);
            const Error applied = session_->apply_permission_policy();
            if (!applied.ok()) {
                log("the ticket renewal could not narrow the session: " +
                    applied.to_string());
            }
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.session_permissions = session_->permissions();
        }
        // The wall clock is sampled per renewal; the deadline itself stays on
        // the monotonic clock, so a clock jump cannot move it afterwards.
        const int64_t remaining = result.expires_at - unix_now();
        ticket_deadline_ =
            Clock::now() + std::chrono::seconds((std::max)(int64_t{0}, remaining));
        pending_security_.ticket_expires_at = result.expires_at;
        pending_security_.ticket_id = result.ticket_id;
        {
            // The renewed ticket may have been signed by a rotated key and
            // carries the policy version in force when it was issued; a later
            // revocation must match the statement this session actually
            // holds, not the one it was first admitted under.
            std::lock_guard<std::mutex> lock(ticket_meta_mutex_);
            ticket_key_id_ = result.key_id;
            ticket_policy_version_ = result.policy_version;
        }
        answer.code = TicketRenewCode::Accepted;
        answer.expires_at = result.expires_at;
        answer.permissions = session_->permissions();
        const Error sent = session_->send_ticket_renew_result(answer);
        if (!sent.ok()) {
            *error = "ticket_renew_result: " + sent.to_string();
            return false;
        }
        log("the access ticket was renewed until " +
            std::to_string(result.expires_at));
    }
    return true;
}


Clock::duration HostServer::idle_disconnect_timeout() const {    if (config_.auto_disconnect_ms != 0) {
        return std::chrono::duration_cast<Clock::duration>(
            std::chrono::milliseconds(config_.auto_disconnect_ms));
    }
    return std::chrono::duration_cast<Clock::duration>(
        std::chrono::minutes(settings_.auto_disconnect_minutes));
}

long long HostServer::idle_disconnect_seconds() const {
    return (std::max)(1LL, config_.auto_disconnect_ms != 0
                               ? static_cast<long long>(config_.auto_disconnect_ms) / 1000
                               : static_cast<long long>(settings_.auto_disconnect_minutes) * 60);
}

void HostServer::touch_viewer_activity() { last_viewer_activity_ = Clock::now(); }

bool HostServer::serve_current_session(HostServerStats& stats, bool rotate_token,
                                       std::string* error, bool* peer_left) {
    *peer_left = false;
    const SessionMode mode = session_->requested_mode();
    std::unique_ptr<ISessionPowerRequest> power_request;
    if (settings_.keep_awake) {
        std::string power_error;
        power_request = config_.power_request_factory(mode == SessionMode::Desktop, &power_error);
        if (!power_request || !power_error.empty())
            log("keep awake: " + (power_error.empty() ? std::string("power request unavailable") : power_error));
    }
    std::unique_ptr<WallpaperRemoval> wallpaper;
    if (settings_.remove_wallpaper && mode == SessionMode::Desktop) {
        std::string wallpaper_error;
        wallpaper = config_.wallpaper_remover
                        ? config_.wallpaper_remover(&wallpaper_error)
                        : create_wallpaper_removal(settings_.wallpaper_state, &wallpaper_error);
        if (!wallpaper) {
            log("remove wallpaper: " +
                (wallpaper_error.empty() ? std::string("unavailable") : wallpaper_error));
        }
    }
    // A new viewer starts with a fresh idle clock; the time it spent waiting
    // for approval or a first frame is not idleness.
    touch_viewer_activity();
    // v62: a ticket-admitted session lives only until the signed expiry. The
    // wall clock is sampled once here; the deadline itself runs on the
    // monotonic clock, so later clock changes cannot extend or shorten it.
    ticket_deadline_ = Clock::time_point::max();
    ticket_expired_ = false;
    ticket_renew_failed_ = false;
    if (pending_security_.via_access_ticket && pending_security_.ticket_expires_at > 0) {
        const int64_t remaining = pending_security_.ticket_expires_at - unix_now();
        ticket_deadline_ =
            Clock::now() + std::chrono::seconds((std::max)(int64_t{0}, remaining));
    }
    ++stats.sessions;
    stats.client_connected = true;
    const std::string peer = session_->client_address();
    // Captured now: wait_for_client() consumed the Hello, and the reference is
    // cleared when this connection is torn down. A viewer that sent no
    // reference still gets recorded, under a host id that cannot merge with a
    // viewer report.
    const std::string viewer_audit_ref = session_->audit_ref();
    bool viewer_ref_absent = viewer_audit_ref.empty();
    std::string audit_ref = viewer_ref_absent ? host_audit_ref() : viewer_audit_ref;
    bool audit_ref_claimed = false;
    if (registry_ && adopted_ && !viewer_ref_absent) {
        // Two live sessions that present the same reference would merge into
        // one row at the service, so the later one stands alone under a host
        // id instead.
        if (registry_->claim_audit_ref(audit_ref)) {
            audit_ref_claimed = true;
        } else {
            viewer_ref_absent = true;
            audit_ref = host_audit_ref();
        }
    }
    session_audit_ref_ = audit_ref;
    session_audit_ref_absent_ = viewer_ref_absent;
    const int64_t audit_started_ms = unix_now_ms();
    {
        static std::atomic<uint64_t> next_connection{1};
        std::lock_guard action(operator_mutex_);
        { std::lock_guard lock(chat_mutex_); pending_chat_.clear(); }
        { std::lock_guard lock(permission_mutex_); permission_update_pending_ = false; }
        { std::lock_guard lock(download_mutex_);
          pending_download_path_.clear(); pending_download_token_.reset(); pending_download_transfer_id_ = 0; }
        disconnect_requested_ = false;
        connection_id_ = next_connection.fetch_add(1);
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.connection_id = connection_id_;
        status_.session_mode = session_->requested_mode();
        status_.approved_permissions = approved_permissions_;
        status_.peer_address = peer;
        status_.requested_display = session_->requested_display();
        status_.session_permissions = session_->permissions();
        status_.display_index = 0;
        status_.display_name.clear();
        status_.display_merged = false;
        status_.stream = ServerInfo{};
    }
    publish_stats(stats);
    publish_state(HostState::Streaming);
    log(peer.empty() ? "a viewer connected" : "a viewer connected from " + peer);

    struct ElevationGuard {
        std::shared_ptr<IElevationTask>& task;
        ~ElevationGuard() { if (task) { task->cancel(); task.reset(); } }
    } elevation_guard{elevation_task_};
    auto elevation = config_.elevation_state ? config_.elevation_state() :
        ElevationState{0, process_is_elevated() ? ElevationCode::Elevated : ElevationCode::Unsupported, {}};
    if (mode != SessionMode::Desktop) elevation = {};
    elevation.request_id = 0;
    const auto announced = session_->send_elevation_state(elevation);
    if (!announced.ok()) *error = announced.to_string();
    const bool ok = announced.ok() && (mode == SessionMode::FileTransfer
                        ? file_session(stats, error, peer_left)
                        : mode == SessionMode::Desktop
                              ? stream_session(stats, error, peer_left)
                              : control_session(mode, stats, error, peer_left));
    if (wallpaper) {
        std::string restore_error;
        if (!wallpaper->restore(&restore_error)) {
            log("restore wallpaper: " + restore_error);
        }
    }

    bool operator_disconnected = false;
    {
        std::lock_guard action(operator_mutex_);
        // Snapshot the disconnect request while retiring the ID so an
        // accepted operator action is also reflected in finish and audit.
        operator_disconnected = disconnect_requested_.load();
        connection_id_ = 0;
        { std::lock_guard lock(download_mutex_);
          pending_download_path_.clear(); pending_download_token_.reset(); pending_download_transfer_id_ = 0; }
        std::lock_guard lock(status_mutex_);
        status_.connection_id = 0;
    }
    if (operator_disconnected) log("the operator disconnected this viewer");

    ClientStats peer_stats;
    bool stats_received = false;
    const auto finish_timeout = std::chrono::milliseconds(
        operator_disconnected || ticket_expired_ || ticket_renew_failed_
            ? std::min<uint32_t>(config_.finish_timeout_ms, 500)
            : ok ? config_.finish_timeout_ms
                 : std::min<uint32_t>(config_.finish_timeout_ms, 2000));
    Error finished = session_->finish_server(finish_timeout, peer_stats, stats_received);
    stats.peer_left = *peer_left;
    stats.stats_received = stats_received;
    if (stats_received) {
        stats.peer = peer_stats;
    }
    for (const InputEvent& event : session_->take_input_events()) {
        ++stats.input_events;
        if (config_.input_event) {
            config_.input_event(event);
        }
    }
    stats.input_stats = session_->input_stats();
    stats.input_releases = session_->input_release_requests();
    publish_stats(stats);

    if (ok && !stopping_.load()) {
        // A viewer that sends its closing Stats/Bye before the video send
        // fails left on purpose, even though the send then reports the peer
        // gone.
        log(*peer_left && !stats_received ? "the viewer disappeared" : "the viewer left");
    }
    if (!finished.ok() && !*peer_left && error->empty() && !stopping_.load() && !operator_disconnected) {
        *error = "finish: " + finished.to_string();
    }
    // Every session leaves a host-side record. A viewer that sent no reference
    // cannot contribute its own half, so the record is marked and stands
    // alone.
    if (!settings_.audit_spool.empty()) {
        AuditSessionRecord record;
        record.audit_ref = audit_ref;
        record.viewer_ref_absent = viewer_ref_absent;
        record.started_at_ms = audit_started_ms;
        record.ended_at_ms = unix_now_ms();
        record.success = ok && error->empty();
        record.detail = operator_disconnected ? "disconnected by the local operator" : *error;
        std::string audit_error;
        if (!append_audit_session(settings_.audit_spool, record, &audit_error)) {
            log("audit: " + audit_error);
        }
    }
    if (audit_ref_claimed) {
        registry_->release_audit_ref(audit_ref);
    }

    // A spent one-time token must not guard the next connection, so it is
    // replaced once the last viewer of a sequential host has gone. A worker
    // leaves this to its listener, which rotates only when no viewer is left.
    if (rotate_token && ok && error->empty() && !config_.serve_once &&
        settings_.token_mode == HostTokenMode::OneTime) {
        settings_.access_token = generate_access_token();
        session_->set_access_token(settings_.access_token);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.token = settings_.access_token;
        }
        if (config_.token_changed) {
            config_.token_changed(settings_.access_token);
        }
        // The policy holds its own copy of the token, so automation sees the
        // same rotation the legacy caller does.
        refresh_security_config();
        log("the one-time token was spent; a new token is ready");
    }
    disconnect_requested_ = false;
    return ok;
}

void HostServer::run_multi() {
    uint32_t failures = 0;
    const auto first_deadline =
        config_.first_client_timeout_ms > 0
            ? Clock::now() + std::chrono::milliseconds(config_.first_client_timeout_ms)
            : Clock::time_point::max();
    bool listening_logged = false;
    bool stop_after_viewer = false;
    Clock::time_point run_start{};
    // The absolute end of this run, armed when the first viewer is admitted.
    // Workers inherit what remains of it so they stop their own sessions
    // gracefully; the listener only stops accepting.
    Clock::time_point run_deadline = Clock::time_point::max();

    while (!stopping_.load()) {
        reap_viewers();
        if (config_.serve_once) {
            std::lock_guard lock(registry_->mutex);
            stop_after_viewer = registry_->admitted != 0;
        }
        const bool past_deadline =
            run_deadline != Clock::time_point::max() && Clock::now() >= run_deadline;
        if (config_.serve_once && stop_after_viewer) {
            if (viewers_.empty()) {
                break;
            }
            // A one-shot host serves the connection it admitted and accepts
            // nothing more while it runs. The worker ends itself at the
            // deadline; the grace only covers a session that ignores it.
            if (past_deadline &&
                Clock::now() >=
                    run_deadline + std::chrono::milliseconds(config_.finish_timeout_ms)) {
                stop_viewers();
                break;
            }
            sleep_unless_stopping(std::chrono::milliseconds(100));
            continue;
        }
        if (past_deadline) {
            // Let the workers finish their own sessions at the shared
            // deadline; stopping them here would cut audio and video without
            // a FIN.
            if (viewers_.empty()) {
                break;
            }
            if (Clock::now() >=
                run_deadline + std::chrono::milliseconds(config_.finish_timeout_ms)) {
                stop_viewers();
                break;
            }
            sleep_unless_stopping(std::chrono::milliseconds(50));
            continue;
        }
        if (!session_) {
            std::string error;
            if (!open_session(&error)) {
                log("listener failed: " + error);
                publish_state(HostState::Failed, "QUIC server: " + error);
                if (config_.serve_once) {
                    break;
                }
                if (config_.max_consecutive_failures != 0 &&
                    ++failures >= config_.max_consecutive_failures) {
                    break;
                }
                sleep_unless_stopping(std::chrono::seconds(2));
                continue;
            }
            listening_logged = false;
        }
        if (!listening_logged) {
            publish_state(HostState::Listening);
            log("listening on " + settings_.bind_address + ":" + std::to_string(settings_.port) +
                " for up to " + std::to_string(settings_.max_viewers) + " viewers");
            listening_logged = true;
        }

        std::unique_ptr<QuicSession> connection;
        Error accepted = session_->accept(connection, std::chrono::milliseconds(200));
        if (accepted.ok()) {
            failures = 0;
            double run_seconds_left = 0.0;
            if (run_start == Clock::time_point{}) {
                run_start = Clock::now();
                if (config_.run_seconds > 0.0) {
                    run_deadline = run_start + std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double>(
                                                      config_.run_seconds));
                }
            }
            if (run_deadline != Clock::time_point::max()) {
                // Never zero: a zero budget means "unlimited" to the worker.
                run_seconds_left = std::max(
                    0.001, std::chrono::duration<double>(run_deadline - Clock::now()).count());
            }
            start_viewer(std::move(connection), run_seconds_left);
            continue;
        }
        if (accepted.status() != Status::WaitTimeout) {
            // The listener itself failed. Its configuration has to outlive
            // every accepted session, so the workers are stopped before the
            // session is reopened.
            log("accept failed: " + accepted.to_string());
            publish_state(HostState::Failed, "QUIC accept: " + accepted.to_string());
            stop_viewers();
            session_.reset();
            listening_logged = false;
            if (config_.serve_once) {
                break;
            }
            if (config_.max_consecutive_failures != 0 &&
                ++failures >= config_.max_consecutive_failures) {
                break;
            }
            sleep_unless_stopping(std::chrono::seconds(2));
            continue;
        }

        size_t active = 0;
        uint64_t admitted = 0;
        {
            std::lock_guard<std::mutex> lock(registry_->mutex);
            active = registry_->active;
            admitted = registry_->admitted;
        }
        if (admitted == 0 && first_deadline != Clock::time_point::max() &&
            Clock::now() >= first_deadline) {
            const std::string wait_error =
                "no client connected in " +
                std::to_string(config_.first_client_timeout_ms / 1000) + " s";
            log(wait_error);
            publish_state(HostState::Failed, wait_error);
            session_.reset();
            listening_logged = false;
            if (config_.serve_once) {
                break;
            }
            if (config_.max_consecutive_failures != 0 &&
                ++failures >= config_.max_consecutive_failures) {
                break;
            }
            sleep_unless_stopping(std::chrono::seconds(2));
            continue;
        }
        if (active == 0) {
            publish_state(HostState::Listening);
        }
    }

    stop_viewers();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.state != HostState::Failed) {
            status_.state = HostState::Stopped;
        }
    }
    running_ = false;
}

void HostServer::start_viewer(std::unique_ptr<QuicSession> connection,
                              double run_seconds_left) {
    auto viewer = std::shared_ptr<HostServer>(new HostServer(config_));
    viewer->settings_ = settings_;
    // Another worker may have changed the exposed switches since the listener
    // loaded its copy; seed this session from the freshest policy.
    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        if (registry_->remote_settings_valid) {
            copy_remote_config_fields(registry_->remote_settings, viewer->settings_);
        }
        if (registry_->voice_microphone_valid) {
            viewer->settings_.voice_microphone = registry_->voice_microphone;
        }
        viewer->settings_.lock_after_session_end =
            registry_->lock_after_session_end.load();
    }
    viewer->session_ = std::move(connection);
    viewer->security_ = security_;
    viewer->permission_policy_ = permission_policy_;
    viewer->denial_audit_ = denial_audit_;
    viewer->registry_ = registry_;
    viewer->adopted_ = true;
    viewer->self_ = viewer;
    // The listener owns serve_once and token rotation; it passes only what
    // remains of its run budget, so the worker ends its own session instead of
    // being stopped mid-stream.
    viewer->config_.run_seconds = run_seconds_left;
    viewer->config_.serve_once = false;
    HostServer* raw = viewer.get();
    viewer->session_->set_server_callbacks(
        [raw](const ClientRequest& request) { return raw->authenticate_client(request); },
        [raw](const ClientRequest& request) { return raw->approve_client(request); });
    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        ++registry_->pending;
        std::erase_if(registry_->workers, [](const auto& worker) { return worker.expired(); });
        registry_->workers.push_back(viewer);
    }
    viewers_.push_back(viewer);
    viewer->start();
}

void HostServer::reap_viewers() {
    bool dropped = false;
    for (auto it = viewers_.begin(); it != viewers_.end();) {
        if (!(*it)->finished_.load()) {
            ++it;
            continue;
        }
        dropped = true;
        (*it)->stop();
        remember_session((*it)->status());
        it = viewers_.erase(it);
    }
    if (!dropped) {
        return;
    }
    std::shared_ptr<HostServer> next;
    for (auto& viewer : viewers_) {
        if (viewer->status().state == HostState::Streaming) {
            next = viewer;
        }
    }
    size_t active = 0;
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        registry_->focused = next;
        active = registry_->active;
        pending = registry_->pending;
    }
    if (active == 0) {
        // A one-shot run reports how its only session ended; every other host
        // goes back to listening and clears the previous error like the
        // sequential path does.
        bool keep_failure = false;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            keep_failure = config_.serve_once && status_.state == HostState::Failed;
        }
        if (!keep_failure) {
            publish_state(HostState::Listening);
        }
    }
    // A one-time token is replaced only when no viewer is left: rotating while
    // another session is still being admitted would invalidate the token that
    // viewer is about to present.
    if (active == 0 && pending == 0 && settings_.token_mode == HostTokenMode::OneTime &&
        !config_.serve_once) {
        settings_.access_token = generate_access_token();
        session_->set_access_token(settings_.access_token);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.token = settings_.access_token;
        }
        if (config_.token_changed) {
            std::lock_guard<std::mutex> lock(registry_->callback_mutex);
            config_.token_changed(settings_.access_token);
        }
        refresh_security_config();
        log("the one-time token was spent; a new token is ready");
    }
}

void HostServer::stop_viewers() {
    for (auto& viewer : viewers_) {
        viewer->stop();
        remember_session(viewer->status());
    }
    viewers_.clear();
    if (registry_) {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        registry_->focused.reset();
    }
}

void HostServer::remember_session(const HostStatus& session) {
    if (!session.stats.client_connected) {
        return;
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.stats = session.stats;
    status_.stream = session.stream;
    status_.display_index = session.display_index;
    status_.display_name = session.display_name;
    status_.display_merged = session.display_merged;
    status_.requested_display = session.requested_display;
    status_.session_permissions = session.session_permissions;
    status_.peer_address = session.peer_address;
    if (session.state == HostState::Failed) {
        status_.state = session.state;
        status_.error = session.error;
    }
}

void HostServer::run_adopted() {
    HostServerStats stats;
    // TLS is already up; only the Hello is still missing. The transport closes
    // a silent peer after its own hello timeout, so this bound only caps the
    // wait on a connection that is not even completing that.
    const auto hello_timeout = std::chrono::milliseconds(
        config_.first_client_timeout_ms > 0 ? config_.first_client_timeout_ms : 15000);
    Error waited = session_->wait_for_client(hello_timeout);
    if (!waited.ok()) {
        if (!stopping_.load()) {
            log(waited.status() == Status::WaitTimeout ? "the viewer sent no Hello"
                                                       : waited.to_string());
        }
        registry_->release_control(this);
        {
            std::lock_guard<std::mutex> lock(registry_->mutex);
            if (registry_->pending > 0) {
                --registry_->pending;
            }
        }
        {
            // Serialize teardown with operator access to the session.
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_.reset();
        }
        finished_ = true;
        running_ = false;
        return;
    }
    commit_security();
    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        if (registry_->pending > 0) {
            --registry_->pending;
        }
        ++registry_->active;
        ++registry_->admitted;
        registry_->focused = self_;
    }
    log("a viewer was admitted to a shared host; " + std::to_string([this] {
            std::lock_guard<std::mutex> lock(registry_->mutex);
            return registry_->active;
        }()) +
        " viewer(s) active");

    std::string error;
    bool peer_left = false;
    const bool ok = serve_current_session(stats, false, &error, &peer_left);
    if (!ok || !error.empty()) {
        log(error.empty() ? "the session ended" : error);
        publish_state(HostState::Failed, error.empty() ? "the session failed" : error);
    }

    registry_->release_control(this);
    bool last_viewer = false;
    bool lock_after = false;
    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        if (registry_->active > 0) {
            --registry_->active;
        }
        last_viewer = registry_->active == 0;
        lock_after = registry_->lock_after_session_end.load();
        std::shared_ptr<HostServer> focused = registry_->focused.lock();
        if (focused.get() == this) {
            registry_->focused.reset();
        }
    }
    if (last_viewer && lock_after) {
        lock_after_session_end_workstation();
    }
    // Close the connection now instead of in the destructor: the listener can
    // admit the next viewer without waiting for this worker to be reaped.
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.reset();
    }
    if (clipboard_worker_) clipboard_worker_->Cancel();
    finished_ = true;
    clipboard_channel_.reset();
    running_ = false;
}

bool HostServer::stream_session(HostServerStats& stats, std::string* error, bool* peer_left) {
    return stream_desktop(stats, error, peer_left);
}

bool HostServer::stream_desktop(HostServerStats& stats, std::string* error, bool* peer_left) {
    *peer_left = false;
    stats.audio_active = false;
    stats.privacy_mode_active = false;
    stats.privacy_mode_changes = 0;
    stats.sas_requests = 0;
    stats.sas_failures = 0;
    stats.host_actions = 0;
    stats.host_action_failures = 0;
    stats.block_input_active = false;
    stats.resolution_percent = 100;
    stats.resolution_changes = 0;
    // Blocking local input lasts only as long as the session that asked for
    // it; however the loop below ends, the operator's input comes back.
    struct BlockInputGuard {
        HostServer* self;
        HostServerStats* stats;
        ~BlockInputGuard() {
            self->release_block_input_local();
            stats->block_input_active = false;
        }
    } block_input_guard{this, &stats};
    // The last scale the ladder asked for. A merged canvas may cap it lower,
    // and stats.resolution_percent reports what is actually encoded.
    std::vector<DisplayInfo> attached = config_.displays();
    if (attached.empty()) {
        *error = "no displays are attached";
        return false;
    }

    // Crash-safe recovery ownership for a topology this session changed.
    // Declared before the mode guard and every capture object so a session
    // exit restores the original layout after the mode is put back and after
    // the local capturer released its GPU references. A failed restore leaves
    // the independent guardian retrying.
    struct ManagedDisplays {
        std::unique_ptr<DisplayPrivacyBackend> backend;
        std::function<void(const std::string&)> log;
        ~ManagedDisplays() {
            if (backend) {
                std::string error;
                if (!backend->restore(&error)) log("display recovery is pending: " + error);
            }
        }
    } managed_displays{nullptr, [this](const std::string& message) { log(message); }};
    std::wstring managed_original_device;
    bool managed_original_merged = false;
    std::string privacy_implementation = settings_.privacy_mode_impl;
    std::unique_ptr<VirtualDisplayPrivacy> virtual_privacy;

    // A viewer-picked resolution is temporary: this object remembers the mode
    // the output ran before the session, and the guard puts it back on every
    // exit path. Declared before the capturer so the mode is restored even when
    // an early error stops the session.
    std::unique_ptr<DisplayModeChange> mode_change = std::make_unique<DisplayModeChange>();
    ModeRestoreGuard mode_guard(mode_change,
                                [this](const std::string& message) { log(message); });

    // A workstation can already be locked or showing a UAC prompt when the
    // session starts. The service worker runs as SYSTEM in the user session
    // and can attach to that desktop, so unattended access keeps working; a
    // tray host stays on Default and capture creation fails below.
    {
        std::string follow_error;
        if (!config_.follow_desktop(&follow_error, nullptr)) {
            log("cannot follow the input desktop: " + follow_error);
        }
    }

    size_t display_index = attached.size();
    bool merged_requested = false;
    const uint16_t requested = session_->requested_display();
    if (requested == kDisplayAll) {
        merged_requested = true;
    } else if (requested != kDisplayAuto) {
        display_index = requested;
        if (display_index >= attached.size()) {
            *error = "the client asked for display " + std::to_string(display_index) +
                     ", but only " + std::to_string(attached.size()) + " are attached";
            return false;
        }
    } else if (settings_.display != kDisplayAuto) {
        display_index = settings_.display;
        if (display_index >= attached.size()) {
            *error = "display " + std::to_string(display_index) +
                     " from the settings is not attached (" + std::to_string(attached.size()) +
                     " present)";
            return false;
        }
    } else {
        display_index = 0;
        for (size_t i = 0; i < attached.size(); ++i) {
            if (attached[i].primary) {
                display_index = i;
                break;
            }
        }
    }

    CaptureConfig capture;
    capture.want_hdr = config_.want_hdr;
    capture.capture_cursor = config_.capture_cursor;
    capture.backend = config_.capture_backend;
    capture.nv12.enabled = true;
    capture.nv12.colorimetry = Nv12Colorimetry::Bt601Limited;

    DesktopStreamPool standalone_streams;
    auto& streams = registry_ ? registry_->desktop_streams : standalone_streams;
    // One policy participant per connection, retained across display changes.
    auto quality_peer = streams.connect_quality(settings_.framerate, settings_.quality,
                                                config_.adaptive_bitrate, config_.adaptive_fps);
    auto codec_peer = streams.connect_codecs(session_->decoding_codecs(), codec_vote(session_->codec_preference()),
        session_->decoding_444(), session_->chroma_preference());
    auto subscribe_display = [&](const DisplayInfo& display, const CaptureConfig& cfg,
                                 std::string* failure) {
        const auto key = L"display:" + display.device_name + L":" +
            std::to_wstring(display.adapter_luid.HighPart) + L":" +
            std::to_wstring(display.adapter_luid.LowPart);
        return streams.subscribe(key, [&](std::string* out) {
            return config_.capture_factory(display, cfg, out);
        }, config_.encoder_factory, failure, config_.follow_desktop, session_->decoding_codecs(), codec_peer,
           {settings_.record_incoming, settings_.record_directory, config_.log},
           cfg.capture_cursor ? config_.cursor_sampler : DesktopStreamPool::CursorSampler{});
    };
    auto subscribe_merged = [&](const std::vector<DisplayInfo>& displays,
                                const CaptureConfig& cfg, std::string* failure) {
        std::wstring key = L"merged:";
        for (const auto& display : displays) {
            key += display.device_name + L":" + std::to_wstring(display.adapter_luid.HighPart) +
                L":" + std::to_wstring(display.adapter_luid.LowPart) + L":" +
                std::to_wstring(display.rect.left) + L":" + std::to_wstring(display.rect.top) +
                L":" + std::to_wstring(display.rect.right) + L":" +
                std::to_wstring(display.rect.bottom) + L";";
        }
        return streams.subscribe(key, [&](std::string* out) {
            return create_merged_capturer(displays, cfg, out);
        }, config_.encoder_factory, failure, config_.follow_desktop, session_->decoding_codecs(), codec_peer,
           {settings_.record_incoming, settings_.record_directory, config_.log},
           cfg.capture_cursor ? config_.cursor_sampler : DesktopStreamPool::CursorSampler{});
    };

    struct DesktopVideo {
        uint32_t id = 0;
        std::unique_ptr<IDxgiCapturer> capture;
        std::unique_ptr<IVideoEncoder> encoder;
        ServerInfo info;
        Clock::time_point last_keyframe = Clock::now();
        Clock::time_point recovery_at{};
        bool encoder_recovery_only = false;
        bool force_keyframe = true, have_input = false, resolution_fixed = false;
        bool probe_pending = false, needs_resync = false;
        uint32_t extra_idrs = 0, frames_without_nv12 = 0, framerate = 30;
        uint16_t resolution_request = 100, resolution_percent = 100;
        uint32_t target_bitrate = 0;
        ComPtr<ID3D11Texture2D> cached_nv12;
        FrameInfo cached_frame;
        // Pointer delivery belongs to this subscription, not to the video
        // loop: the display service drives it from its capture tick.
        std::shared_ptr<CursorPublisher> cursor;
        std::vector<EncodedPacket> packets;
    } video;
    std::map<uint32_t, DesktopVideo> other_videos;
    bool multiple_streams = false, pumping_extra = false;
    // The display service can invoke a cursor sink at any time, including
    // while this function unwinds. Sever the session pointer before the
    // unique_ptr above outlives the session object.
    struct CursorDetachGuard {
        std::function<void()> detach;
        ~CursorDetachGuard() { detach(); }
    } cursor_detach_guard{[&] {
        if (video.cursor) video.cursor->detach();
        for (auto& [id, entry] : other_videos) {
            if (entry.cursor) entry.cursor->detach();
        }
    }};
    std::wstring selected_device;
    std::function<bool()> publish_subscriptions;
    uint32_t next_video_id = 1, video_round = 0;
    auto& capturer = video.capture;
    auto& encoder = video.encoder;
    auto& stream_info = video.info;
    auto& resolution_request = video.resolution_request;

    struct SavedDisplaySubscription {
        uint32_t id = 0;
        DisplayInfo display;
        uint16_t resolution_request = 100;
        bool resolution_fixed = false;
    };
    struct SavedDisplaySubscriptions {
        bool active = false;
        std::vector<SavedDisplaySubscription> streams;  // focused stream first
    } saved_display_subscriptions;
    std::function<bool(std::string*)> restore_display_subscriptions;
    auto remember_display_subscriptions = [&] {
        if (saved_display_subscriptions.active || !multiple_streams) return;
        // Merged capture already has a dedicated restoration path. This
        // snapshot is for independent per-display subscriptions.
        if ((capturer && capturer->merged()) ||
            std::any_of(other_videos.begin(), other_videos.end(), [](const auto& item) {
                return item.second.capture && item.second.capture->merged();
            })) {
            return;
        }
        saved_display_subscriptions.active = true;
        if (capturer) {
            saved_display_subscriptions.streams.push_back(
                {video.id, capturer->display(), video.resolution_request,
                 video.resolution_fixed});
        }
        for (const auto& [id, entry] : other_videos) {
            if (entry.capture) {
                saved_display_subscriptions.streams.push_back(
                    {id, entry.capture->display(), entry.resolution_request,
                     entry.resolution_fixed});
            }
        }
    };
    auto swap_video = [&](DesktopVideo& other) {
        video.target_bitrate = stats.target_bitrate;
        video.resolution_percent = stats.resolution_percent;
        std::swap(video, other);
        stats.target_bitrate = video.target_bitrate;
        stats.framerate = static_cast<uint16_t>(video.framerate);
        stats.resolution_percent = video.resolution_percent;
    };
    auto video_index = [&](const IDxgiCapturer& source) -> uint16_t {
        return source.merged() ? kDisplayAll : static_cast<uint16_t>(find_display(attached, source.display()));
    };
    auto refresh_video = [&](const ServerInfo& info) -> Error {
        if (multiple_streams && capturer) {
            auto result = session_->refresh_display_stream({video.id, video_index(*capturer), info});
            if (!result.ok()) return result;
        }
        // Legacy StreamInfo still carries connection-wide permissions/audio.
        // Extra displays must never replace that description or those grants.
        return pumping_extra ? Error::success() : session_->refresh_stream(info);
    };
    if (merged_requested) {
        std::string merged_error;
        capturer = subscribe_merged(attached, capture, &merged_error);
        if (!capturer) {
            // A mixed-adapter or oversized layout still lets the viewer in:
            // fall back to the host's default single display and say why.
            display_index = settings_.display != kDisplayAuto &&
                                    settings_.display < attached.size()
                                ? settings_.display
                                : size_t{0};
            if (settings_.display == kDisplayAuto) {
                for (size_t i = 0; i < attached.size(); ++i) {
                    if (attached[i].primary) {
                        display_index = i;
                        break;
                    }
                }
            }
            log("merged view unavailable (" + merged_error + "); falling back to display " +
                std::to_string(display_index));
            capturer = subscribe_display(attached[display_index], capture, error);
        }
    } else {
        capturer = subscribe_display(attached[display_index], capture, error);
    }
    if (!capturer) {
        *error = "capturer creation failed: " + *error;
        return false;
    }
    if (!wait_for_usable_frame(*capturer, true, stopping_, std::chrono::seconds(30), error)) {
        return false;
    }
    const FrameInfo first = capturer->frame();
    // Negotiation can replace the subscription's picture. Retain metadata
    // by value before opening the encoder and building HelloAck.
    const bool initial_pointer_only = !first.desktop_updated;
    stats.resolution_percent = static_cast<uint16_t>(capturer->nv12_scale_percent());
    auto publish_current = [&](const ServerInfo& info) {
        if (pumping_extra || !capturer) return;
        selected_device = capturer->display().device_name;
        const size_t index =
            capturer->merged() ? size_t{0} : find_display(attached, capturer->display());
        publish_stream(info, index, streamed_display_name(*capturer));
    };
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.display_index =
            capturer->merged() ? size_t{0} : find_display(attached, capturer->display());
        status_.display_name = streamed_display_name(*capturer);
        status_.display_merged = capturer->merged();
    }
    if (capturer->merged()) {
        log("capturing " + std::to_string(capturer->merged_display_count()) +
            " displays merged into " + std::to_string(first.width) + "x" +
            std::to_string(first.height) + " at " + std::to_string(capturer->display().rect.left) +
            "," + std::to_string(capturer->display().rect.top) +
            "; encoded at " + std::to_string(stats.resolution_percent) + "%");
    } else {
        log("capturing display " +
            std::to_string(find_display(attached, capturer->display())) + " (" +
            display_name_utf8(capturer->display()) + ")");
    }
    log(std::string("capture backend: ") + capturer->implementation());

    EncoderConfig encoder_config;
    encoder_config.codec = settings_.codec;
    encoder_config.bitrate = settings_.bitrate_kbps * 1000u;
    encoder_config.framerate = settings_.framerate;
    encoder_config.quality_ratio = settings_.quality;
    encoder_config.gop_size = 0;
    encoder_config.width = first.nv12_width;
    encoder_config.height = first.nv12_height;
    encoder_config.backend = config_.encoder_backend;
    encoder = DesktopStreamPool::encoder(*capturer, encoder_config, error);
    if (!encoder) {
        *error = "encoder creation failed: " + *error;
        return false;
    }
    if (encoder->codec() != settings_.codec) {
        log(std::string("global codec negotiation selected ") + codec_name(encoder->codec()));
    }
    log("encoder: " + (encoder->implementation().empty() ? std::string("oneVPL")
                                                         : encoder->implementation()) +
        ", " + std::to_string(encoder->width()) + "x" + std::to_string(encoder->height()) +
        ", target " + std::to_string(encoder->bitrate() / 1000u) + " kbps");

    // System sound is optional: the endpoint and the AAC encoder are opened
    // before the announcement so AudioInfo describes what will actually be
    // encoded, and a machine without a usable endpoint simply streams video.
    std::unique_ptr<ILoopbackCapture> audio_capture;
    AudioInfo audio_info;
    const PermissionMask granted_permissions = session_->permissions();

    // Privacy mode is per session and the overlay is owned here: every return
    // path destroys it, which is what puts the operator's screen back when the
    // viewer leaves without turning it off first. The transport drops requests
    // when the host did not opt in, and the permission bit can still be
    // revoked mid-session, so both are checked before acting.
    std::unique_ptr<PrivacyOverlay> privacy_overlay;
    bool privacy_active = false;
    // The viewer's pointer hint is drawn on the host desktop on demand. A
    // machine without an interactive desktop simply stays without a hint;
    // the failure is logged once and never blocks the session.
    std::unique_ptr<CursorHintOverlay> cursor_overlay;
    bool cursor_overlay_failed = false;
    // Assigned with the capture-rebuild helpers below: restoring a virtual
    // display topology must wait for GPU capture to work again, and those
    // helpers need the pointer state declared later in this function. The
    // revocation path in apply_pending_permissions() only runs once the
    // session loop starts, after the assignment.
    std::function<bool(bool)> set_privacy_mode;
    auto send_privacy_state = [&](bool enabled, PrivacyModeCode code,
                                  const std::string& reason) -> bool {
        Error sent = session_->send_privacy_mode_state(enabled, code, reason);
        if (sent.ok()) {
            return true;
        }
        if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) {
            *peer_left = true;
        } else {
            ++stats.send_errors;
            *error = "privacy_mode_state: " + sent.to_string();
        }
        return false;
    };
    auto set_overlay_privacy = [&](bool wanted) -> bool {
        if (!wanted) {
            if (privacy_active) {
                privacy_overlay->disable();
                privacy_active = false;
                stats.privacy_mode_active = false;
                ++stats.privacy_mode_changes;
                log("privacy mode is off: the local screen is visible again");
            }
            return send_privacy_state(false, PrivacyModeCode::Ok, {});
        }
        if ((session_->permissions() & kPermissionPrivacyMode) == 0) {
            ++stats.privacy_mode_changes;
            return send_privacy_state(false, PrivacyModeCode::Denied,
                                      "privacy mode is not permitted by the host");
        }
        // The overlay covers the operator's physical screens, so with more
        // than one viewer only the one holding control may raise it.
        if (registry_ && !registry_->may_change_host_state(this)) {
            ++stats.privacy_mode_changes;
            return send_privacy_state(false, PrivacyModeCode::Denied,
                                      "another viewer controls this host");
        }
        if (privacy_active) {
            return send_privacy_state(true, PrivacyModeCode::Ok, {});
        }
        if (!privacy_mode_supported()) {
            ++stats.privacy_mode_changes;
            return send_privacy_state(false, PrivacyModeCode::Unsupported,
                                      "privacy mode needs Windows 10 version 2004 or newer");
        }
        if (!privacy_overlay) {
            std::string overlay_error;
            privacy_overlay = PrivacyOverlay::create(&overlay_error);
            if (!privacy_overlay) {
                ++stats.privacy_mode_changes;
                log("privacy mode is unavailable: " + overlay_error);
                return send_privacy_state(false, PrivacyModeCode::Failed, overlay_error);
            }
        }
        std::string enable_error;
        if (!privacy_overlay->enable(&enable_error)) {
            ++stats.privacy_mode_changes;
            log("privacy mode cannot be shown: " + enable_error);
            return send_privacy_state(false, PrivacyModeCode::Failed, enable_error);
        }
        privacy_active = true;
        stats.privacy_mode_active = true;
        ++stats.privacy_mode_changes;
        log("privacy mode is on: the local screen is masked, the viewer keeps the stream");
        return send_privacy_state(true, PrivacyModeCode::Ok, {});
    };
    // Ctrl+Alt+Del is not a key chord here: Windows only accepts it through
    // SendSAS(), which needs the service worker's LocalSystem token. Every
    // request is answered, and a refusal or platform failure stays local to
    // this action instead of ending the session. The body lives in
    // send_sas_result() and is available to every authorized connection.
    auto send_sas = [&]() -> bool { return send_sas_result(stats, error, peer_left); };
    // The endpoint stays open for the whole session even if audio was denied
    // at approval: the operator can grant it back live, and rebuilding WASAPI
    // mid-session would cost a fresh announcement for no benefit. The same
    // endpoint plays the viewer's microphone, so its playback feeds the echo
    // canceller the audio that actually comes out of this machine.
    bool audio_started = false;
    bool audio_policy_enabled = settings_.allow_audio;
    SharedLoopback standalone_audio;
    auto& shared_audio = registry_ ? registry_->system_audio : standalone_audio;
    auto open_system_audio = [&] {
        if (!settings_.allow_audio || audio_capture) return;
        const bool audio_aec = settings_.audio_aec;
        if (audio_aec) {
            audio_reference_ = shared_audio.reference;
        }
        std::string audio_error;
        audio_capture = shared_audio.subscribe([&](std::string* failure) {
            if (config_.audio_capture) return config_.audio_capture(failure);
            LoopbackCaptureConfig audio_config;
            audio_config.device_id = settings_.audio_device;
            audio_config.reference = audio_reference_;
            audio_config.echo_cancellation = audio_aec;
            audio_config.silence_gate_ms = config_.audio_silence_gate_ms;
            return create_loopback_capture(audio_config, failure);
        }, &audio_error);
        if (audio_capture) {
            audio_info = audio_capture->info();
            log("audio: Opus " + std::to_string(audio_info.sample_rate / 1000u) + " kHz, " +
                std::to_string(audio_info.channels) + " ch, " +
                std::to_string(audio_info.bitrate / 1000u) + " kbps" +
                (audio_aec ? ", echo cancellation on" : ", echo cancellation off"));
        } else {
            ++stats.audio_errors;
            log("audio unavailable: " + audio_error);
        }
    };
    open_system_audio();
    DesktopStreamPool::reserve_audio(*capturer, bitrate_reserve(audio_info));
    stats.audio_active =
        audio_info.enabled && (granted_permissions & kPermissionAudio) != 0;

    // Display services own quality and rate. These fields retain only this
    // connection's requested options and pacing of its compressed queue.
    const uint16_t configured_fps = std::clamp<uint16_t>(settings_.framerate ? settings_.framerate : 30, 1, 120);
    StreamCodecPreference codec_preference = session_->codec_preference();
    VideoChroma chroma_preference = session_->chroma_preference();
    StreamQuality stream_quality = settings_.quality >= 1.5f ? StreamQuality::High :
        settings_.quality >= 0.67f ? StreamQuality::Balanced : StreamQuality::Smooth;
    uint8_t stream_quality_percent = 0;
    uint16_t stream_fps_cap = 0;
    auto fps_ceiling = [&]() -> uint16_t {
        return stream_fps_cap == 0 ? configured_fps : std::min(configured_fps, stream_fps_cap);
    };
    auto& framerate = video.framerate;
    framerate = encoder->framerate();
    stats.target_bitrate = encoder->bitrate();
    stats.framerate = static_cast<uint16_t>(framerate);

    stream_info =
        describe_stream(attached, *capturer, *encoder, first.width, first.height,
                        static_cast<uint16_t>(framerate), granted_permissions, audio_info,
                        settings_.allow_terminal_persist);
    publish_current(stream_info);
    if (config_.stream_announced) {
        config_.stream_announced(stream_info);
    }
    Error announced = session_->announce(stream_info);
    if (!announced.ok()) {
        *error = "announce: " + announced.to_string();
        return false;
    }
    if (registry_ && adopted_ && approved_permissions_ != 0) {
        // A viewer can join while another holds the input lease. Preserve
        // its approved ceiling for a later operator grant.
        session_->set_permission_ceiling(approved_permissions_);
    }
    // A UI request made between sessions is applied once the stream exists:
    // set_permissions() is only valid after the announcement.
    PermissionMask announced_permissions = granted_permissions;
    auto apply_pending_permissions = [&]() -> bool {
        bool pending = false;
        PermissionMask requested = 0;
        {
            std::lock_guard<std::mutex> lock(permission_mutex_);
            pending = permission_update_pending_.exchange(false);
            requested = pending_permissions_.load();
        }
        const bool policy_changed = sync_remote_policy();
        bool source_changed = false;
        const bool enable_audio_source = !audio_policy_enabled && settings_.allow_audio;
        audio_policy_enabled = settings_.allow_audio;
        if (enable_audio_source && !audio_capture) {
            open_system_audio();
            if (audio_capture) {
                audio_capture->start();
                audio_started = true;
                source_changed = true;
                stream_info.audio = audio_info;
            }
        }
        if (!pending && !policy_changed && !source_changed) {
            return true;
        }
        Error applied = pending ? session_->set_permissions(requested) : Error::success();
        if (!applied.ok()) {
            if (applied.status() != Status::InvalidCall) {
                log("permission update failed: " + applied.to_string());
            }
            return true;
        }
        const PermissionMask now_mask = session_->permissions();
        if (now_mask == announced_permissions && !source_changed) {
            return true;
        }
        announced_permissions = now_mask;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.session_permissions = now_mask;
        }
        // A controller whose input was revoked gives the lease up, so a later
        // grant to another viewer can take it.
        if (registry_ && adopted_ && (now_mask & kPermissionInput) == 0) {
            registry_->release_control(this);
        }
        // Blocked local input must not outlive the permissions that allowed
        // it: the operator would be locked out of their own machine.
        if (block_input_active_.load() &&
            (now_mask & (kPermissionInput | kPermissionBlockInput)) !=
                (kPermissionInput | kPermissionBlockInput)) {
            if (!release_block_input(stats, error, peer_left, true)) {
                return false;
            }
        }
        // A revoked permission must take the mask down with it: leaving the
        // operator's screen black after they turned the feature off would be
        // a privacy mode they cannot cancel from the viewer. The virtual
        // display path also restores the physical topology and rebuilds the
        // stream before it answers.
        if (privacy_active && (now_mask & kPermissionPrivacyMode) == 0) {
            if (!set_privacy_mode(false)) {
                return false;
            }
            log("privacy mode is off: the permission was revoked");
            if (!send_privacy_state(false, PrivacyModeCode::Denied,
                                    "privacy mode was turned off by the host")) {
                return false;
            }
        }
        // The stream description carries the capability flags as well, and
        // describe_stream() copied the approval-time mask into them. Refresh
        // the local copy with the mask the transport is about to apply.
        stream_info.input_enabled = (now_mask & kPermissionInput) != 0;
        stream_info.clipboard_enabled = (now_mask & kPermissionClipboard) != 0;
        stream_info.file_transfer_enabled =
            (now_mask & kPermissionFileTransfer) != 0;
        stream_info.microphone_enabled = (now_mask & kPermissionMicrophone) != 0;
        stream_info.privacy_mode_enabled =
            (now_mask & kPermissionPrivacyMode) != 0;
        stream_info.port_forward_enabled = (now_mask & kPermissionPortForward) != 0;
        stream_info.terminal_enabled = (now_mask & kPermissionTerminal) != 0;
        stream_info.record_enabled = (now_mask & kPermissionRecord) != 0;
        stream_info.camera_enabled = (now_mask & kPermissionCamera) != 0;
        stream_info.session_select_enabled =
            (now_mask & kPermissionSessionSelect) != 0;
        // A viewer that was denied audio at approval has no AAC config yet,
        // so a fresh description is what makes a restore usable.
        stream_info.audio.enabled = audio_info.enabled;
        publish_current(stream_info);
        Error refreshed = refresh_video(stream_info);
        if (!refreshed.ok()) {
            if (refreshed.status() == Status::Unavailable ||
                refreshed.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "refresh_stream: " + refreshed.to_string();
            }
            return false;
        }
        ++stats.stream_updates;
        stats.audio_active = audio_info.enabled && (now_mask & kPermissionAudio) != 0;
        log("the session permissions are now " + permission_names(now_mask));
        return true;
    };
    if (!apply_pending_permissions()) {
        return error->empty();
    }
    if (audio_capture && !audio_started) {
        audio_capture->start();
        audio_started = true;
    }
    if (!run_clock_started_) {
        run_clock_started_ = true;
        run_start_ = Clock::now();
    }

    const auto start = run_start_;
    auto& last_keyframe = video.last_keyframe;
    const auto next_reannounce = start + std::chrono::seconds(2);
    bool reannounced = false;
    auto& force_keyframe = video.force_keyframe;
    // After a resize, one extra IDR follows the forced one: StreamInfo and the
    // video frames travel on different QUIC streams, so the viewer can see the
    // first new-size frame before it knows the new size and has to drop it.
    // The second IDR is decodable once the announcement has landed.
    auto& extra_idrs = video.extra_idrs;
    auto& have_input = video.have_input;
    // Keep the last valid GPU surface alive across an idle capture call.
    // A forced IDR must not depend on the desktop changing again.
    auto& cached_nv12 = video.cached_nv12;
    auto& cached_frame = video.cached_frame;
    // Set when the GPU refused a scale: the captured NV12 size then stays put
    // for the rest of the session instead of retrying on every sample.
    auto& resolution_fixed = video.resolution_fixed;
    auto& probe_pending = video.probe_pending;
    probe_pending = config_.resolution_probe_percent != 0;
    auto& frames_without_nv12 = video.frames_without_nv12;
    auto& packets = video.packets;

    // Clipboard baseline: remember what the host clipboard held when the
    // session started and upload only changes after that, never its history.
    // The operator can revoke the permission mid-session, so the same check
    // runs on every poll; re-enabling starts from a fresh baseline instead of
    // flushing whatever changed while the viewer could not see it.
    clipboard_was_allowed_ = clipboard_allowed();
    if (clipboard_was_allowed_) {
        remember_local_clipboard();
    }
    clipboard_seq_ = 1;
    next_clipboard_poll_ = Clock::now() + std::chrono::milliseconds(500);


    // Viewer uploads, browsing, file management and operator downloads.
    // The machinery lives on the host because a file-transfer-only session
    // serves exactly the same operations without any capture.
    begin_file_session();
    struct FileSessionEnd {
        HostServer* self;
        ~FileSessionEnd() { self->end_file_session(); }
    } file_session_end{this};


    // Audio degrades on its own: a failed endpoint or encoder turns the
    // stream off with a StreamInfo and leaves video running.
    auto disable_audio = [&](const std::string& reason) {
        audio_capture->stop();
        audio_capture.reset();
        audio_info = AudioInfo{};
        stats.audio_active = false;
        stream_info.audio = audio_info;
        publish_current(stream_info);
        Error refreshed = refresh_video(stream_info);
        if (!refreshed.ok()) {
            log("audio off: could not announce it: " + refreshed.to_string());
        }
        Error stopped = session_->stop_audio();
        if (!stopped.ok() && stopped.status() != Status::Unavailable &&
            stopped.status() != Status::InvalidCall) {
            log("audio off: " + stopped.to_string());
        }
        log("audio stopped: " + reason);
    };

    uint64_t audio_send_drops = 0;
    uint64_t audio_recoveries_logged = 0;
    uint64_t audio_dsp_resets_logged = 0;
    bool audio_recovering_logged = false;
    const uint64_t audio_base_ms = capture_clock_ms(start);
    auto drain_audio = [&] {
        if (!audio_capture) {
            return;
        }
        EncodedAudio packet;
        while (audio_capture->take_packet(packet)) {
            const uint64_t host_ms = packet.capture_ms & 0xFFFFFFFFFFFFull;
            AudioHeader header;
            header.pts_ms = host_ms >= audio_base_ms ? host_ms - audio_base_ms : 0;
            header.capture_ms = host_ms;
            header.sample_rate = audio_info.sample_rate;
            header.frame_samples = audio_info.frame_samples;
            header.channels = audio_info.channels;
            header.codec = audio_info.codec;
            if ((session_->permissions() & kPermissionAudio) == 0) {
                continue;
            }
            Error sent = session_->send_audio(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == Status::PermissionDenied) {
                    // The operator revoked audio between the check above and
                    // the send; the next tick sees the new mask.
                    continue;
                }
                if (sent.status() == Status::WaitTimeout) {
                    // No room on the transport right now (a full shared send
                    // queue, or a datagram that exceeds the current path
                    // budget); video owns the path and this packet is a short
                    // gap, not a reason to stop audio.
                    ++audio_send_drops;
                    continue;
                }
                ++stats.audio_errors;
                log("audio send failed: " + sent.to_string());
                // Other viewers still own independent audio streams.
                continue;
            }
            ++stats.audio_packets;
            stats.audio_bytes += packet.data.size();
        }
        const AudioCaptureStats captured = audio_capture->stats();
        stats.audio_dropped = audio_send_drops + captured.dropped;
        stats.audio_capture_recoveries = captured.recoveries;
        stats.audio_gate_transitions = captured.gate_transitions;
        stats.audio_dsp_frames = captured.processed_frames;
        stats.audio_dsp_resets = captured.dsp_resets;
        stats.audio_recovering = captured.recovering;
        if (captured.recoveries != audio_recoveries_logged) {
            audio_recoveries_logged = captured.recoveries;
            log("audio capture switched to the default output endpoint");
        }
        if (captured.recovering != audio_recovering_logged) {
            audio_recovering_logged = captured.recovering;
            log(captured.recovering ? "audio capture endpoint lost; retrying"
                                    : "audio capture endpoint recovered");
        }
        if (captured.dsp_resets != audio_dsp_resets_logged) {
            audio_dsp_resets_logged = captured.dsp_resets;
            log("audio echo canceller reset (" +
                std::to_string(captured.dsp_resets) + " times)");
        }
        if (audio_capture->failed()) {
            ++stats.audio_errors;
            const std::string reason = audio_capture->error();
            disable_audio(reason.empty() ? "capture failed" : reason);
        }
    };

    // The viewer microphone pipeline lives in the members implemented by
    // drain_microphone(), shared with the other session modes.

    // Pointer state. Desktop Duplication keeps the pointer out of the desktop
    // image, so the viewer is told the sprite and where it is; the sprite
    // travels once per shape id and later updates carry only the position.
    // Like RustDesk's cursor service, the display service invokes this
    // subscription's sink on its capture tick (before encoding and regardless
    // of the video queues), so pointer motion is not serialized behind encode
    // or the session loop. publish_cursor() only seeds the state after resets
    // and drains the counters the sink keeps.
    auto ensure_cursor = [&]() {
        if (!config_.capture_cursor || !capturer) return;
        if (!video.cursor || !video.cursor->bound_to(capturer.get())) {
            video.cursor = std::make_shared<CursorPublisher>();
            video.cursor->bind(session_.get(), capturer.get());
            video.cursor->route(video.id, multiple_streams);
            std::shared_ptr<CursorPublisher> publisher = video.cursor;
            DesktopStreamPool::set_cursor_sink(*capturer, [publisher](const CursorInfo& info) {
                publisher->publish(info);
            });
        }
        video.cursor->route(video.id, multiple_streams);
    };
    auto flush_cursor_stats = [&]() {
        if (!video.cursor) return;
        const auto counts = video.cursor->take();
        stats.cursor_sent += counts.sent;
        stats.cursor_shapes += counts.shapes;
        stats.cursor_bytes += counts.bytes;
        stats.cursor_dropped += counts.dropped;
        stats.cursor_errors += counts.errors;
        if (!counts.last_error.empty() && stats.cursor_errors <= 3) {
            log("cursor send failed: " + counts.last_error);
        }
    };
    auto publish_cursor = [&]() {
        if (!config_.capture_cursor || !capturer) {
            return;
        }
        ensure_cursor();
        const auto snapshot = DesktopStreamPool::cursor_snapshot(*capturer);
        if (snapshot) video.cursor->publish(*snapshot);
        flush_cursor_stats();
    };

    // Physical display privacy and management. Both paths rebuild the stream
    // through the same proof: the new output must produce an NV12 texture and
    // a hardware-encoded key frame before anything is announced. Auto capture
    // may use the GDI uploader when DXGI is unavailable; a virtual output that
    // still fails is rolled back instead of being announced as usable.
    std::wstring privacy_original_device;
    bool privacy_original_merged = false;
    // Set when the rebuilt stream could not be announced; retrying cannot
    // help, so privacy_capture() stops instead of looping until its deadline.
    bool privacy_publish_failed = false;
    auto privacy_capture_once = [&](const std::wstring& monitor, bool commit,
                                    std::string* failure) -> bool {
        std::vector<DisplayInfo> fresh = config_.displays();
        const std::wstring device =
            monitor.empty() ? privacy_original_device : virtual_display_device_name(monitor);
        const bool merged = monitor.empty() && privacy_original_merged;
        size_t target = fresh.size();
        for (size_t i = 0; i < fresh.size(); ++i) {
            if (fresh[i].device_name == device) {
                target = i;
            }
        }
        // Only recovery may choose another surviving physical output after
        // an unplug. Privacy activation never silently captures another one.
        if (monitor.empty() && target == fresh.size() && !fresh.empty()) {
            target = 0;
            for (size_t i = 0; i < fresh.size(); ++i) {
                if (fresh[i].primary) {
                    target = i;
                }
            }
        }
        if (fresh.empty() || (!merged && target == fresh.size())) {
            *failure = "the requested privacy output is not GPU-capturable";
            return false;
        }
        std::unique_ptr<IDxgiCapturer> next =
            merged ? subscribe_merged(fresh, capture, failure)
                   : subscribe_display(fresh[target], capture, failure);
        if (!next) {
            return false;
        }
        UINT next_scale_request = resolution_request;
        if (resolution_request != 100) {
            Error scaled = next->set_nv12_scale(resolution_request);
            if (!scaled.ok()) {
                log("privacy capture: cannot scale to " +
                    std::to_string(resolution_request) +
                    "%; using 100%: " + scaled.to_string());
                next_scale_request = 100;
            }
        }
        if (!wait_for_usable_frame(*next, true, stopping_, std::chrono::seconds(5),
                                   failure)) {
            return false;
        }
        const uint16_t next_effective_percent =
            static_cast<uint16_t>(next->nv12_scale_percent());
        const FrameInfo frame = next->frame();
        EncoderConfig next_config = encoder_config;
        next_config.width = frame.nv12_width;
        next_config.height = frame.nv12_height;
        std::unique_ptr<IVideoEncoder> next_encoder =
            DesktopStreamPool::encoder(*next, next_config, failure);
        if (!next_encoder) {
            return false;
        }
        if (next_encoder->codec() != encoder_config.codec) {
            log(std::string("the encoder fell back: requested ") +
                codec_name(encoder_config.codec) + ", producing " +
                codec_name(next_encoder->codec()));
        }
        // The replacement starts at the configured bitrate, so the current
        // stream rate and frame rate have to be applied before the proof.
        const uint32_t next_fps =
            stats.framerate != 0 ? stats.framerate : static_cast<uint32_t>(framerate);
        if (next_encoder->bitrate() != stats.target_bitrate ||
            (next_fps != 0 && next_encoder->framerate() != next_fps)) {
            Error rated = next_encoder->set_rate(stats.target_bitrate, next_fps);
            if (!rated.ok()) {
                *failure = "cannot apply the current bitrate: " + rated.to_string();
                return false;
            }
        }
        std::vector<EncodedPacket> proof;
        const Error encoded = next_encoder->encode(frame.nv12_texture, 0, true, proof);
        if (!encoded.ok() || proof.empty()) {
            *failure = "virtual display hardware encoding failed: " + encoded.to_string();
            return false;
        }
        if (!commit) {
            return true;
        }
        // The proof frame is discarded. The first transmitted frame is a new
        // IDR at the normal stream timestamp and follows the new StreamInfo.
        // `frame` preserves the source dimensions across the subscription swap.
        const bool new_subscription = !capturer;
        capturer = std::move(next);
        encoder = std::move(next_encoder);
        attached = std::move(fresh);
        have_input = false;
        cached_nv12.reset();
        force_keyframe = true;
        extra_idrs = 1;
        frames_without_nv12 = 0;
        if (video.cursor) video.cursor->reset();
        resolution_request = static_cast<uint16_t>(next_scale_request);
        if (next_effective_percent != stats.resolution_percent) {
            stats.resolution_percent = next_effective_percent;
            ++stats.resolution_changes;
        }
        stream_info = describe_stream(attached, *capturer, *encoder, frame.width, frame.height,
                                      static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info,
                                      settings_.allow_terminal_persist);
        if (multiple_streams && new_subscription) video.id = next_video_id++;
        if (multiple_streams && publish_subscriptions && !publish_subscriptions()) return false;
        publish_current(stream_info);
        if (config_.stream_announced) {
            config_.stream_announced(stream_info);
        }
        Error refreshed = refresh_video(stream_info);
        if (!refreshed.ok()) {
            privacy_publish_failed = true;
            if (refreshed.status() == Status::Unavailable ||
                refreshed.status() == Status::WaitTimeout) {
                *peer_left = true;
            }
            *failure = refreshed.to_string();
            return false;
        }
        ++stats.stream_updates;
        return true;
    };
    auto privacy_capture = [&](const std::wstring& monitor, bool commit,
                               std::string* failure) -> bool {
        // CCD can report the restored topology before DXGI/DWM is ready to
        // duplicate it (including transient E_ACCESSDENIED). Re-enumerate
        // and recreate GPU resources for a bounded interval after switching.
        // The guardian remains armed throughout activation and its rollback.
        privacy_publish_failed = false;
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(8);
        do {
            if (privacy_capture_once(monitor, commit, failure)) {
                return true;
            }
            if (privacy_publish_failed || stopping_.load() || !session_->peer_alive()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (Clock::now() < deadline);
        return false;
    };
    // Discard GPU references to the detached output before restoring CCD. A
    // failed restore leaves the guardian armed and ends the session: retrying
    // from here would race the guardian and the operator's own changes.
    auto stop_virtual_privacy = [&](std::string* failure) -> bool {
        if (!virtual_privacy || !virtual_privacy->recovery_pending()) {
            return true;
        }
        encoder.reset();
        capturer.reset();
        if (!virtual_privacy->disable(failure)) {
            return false;
        }
        privacy_active = false;
        stats.privacy_mode_active = false;
        if (!privacy_capture({}, true, failure)) return false;
        return !restore_display_subscriptions || restore_display_subscriptions(failure);
    };
    set_privacy_mode = [&](bool wanted) -> bool {
        if (wanted && managed_displays.backend) {
            return send_privacy_state(privacy_active, PrivacyModeCode::Denied,
                "restore the managed display layout before enabling privacy");
        }
        if (privacy_implementation == "overlay") {
            return set_overlay_privacy(wanted);
        }
        if (privacy_implementation != "virtual_display") {
            return send_privacy_state(false, PrivacyModeCode::Unsupported,
                                      "invalid privacy mode implementation");
        }
        if (!wanted) {
            if (virtual_privacy && virtual_privacy->recovery_pending()) {
                std::string failure;
                if (!stop_virtual_privacy(&failure)) {
                    send_privacy_state(false, PrivacyModeCode::Failed, failure);
                    *error = failure;
                    return false;  // the guardian keeps retrying
                }
                ++stats.privacy_mode_changes;
                log("virtual display privacy is off; physical outputs are back");
            }
            return send_privacy_state(false, PrivacyModeCode::Ok, {});
        }
        if ((session_->permissions() & kPermissionPrivacyMode) == 0 ||
            (registry_ && !registry_->may_change_host_state(this))) {
            ++stats.privacy_mode_changes;
            return send_privacy_state(
                false, PrivacyModeCode::Denied,
                "virtual display privacy requires permission and a single desktop viewer");
        }
        if (privacy_active) {
            return send_privacy_state(true, PrivacyModeCode::Ok, {});
        }
        if (settings_.virtual_display_provider == "vdd") {
            ++stats.privacy_mode_changes;
            return send_privacy_state(false, PrivacyModeCode::Unsupported,
                                      "the vdd provider is not implemented");
        }
        std::string failure;
        const bool mode_was_active = mode_change->active();
        if (mode_was_active && mode_change->restore(&failure) == ModeRestore::Failed) {
            ++stats.privacy_mode_changes;
            return send_privacy_state(false, PrivacyModeCode::Failed, failure);
        }
        remember_display_subscriptions();
        privacy_original_device = capturer ? capturer->display().device_name : selected_device;
        privacy_original_merged = capturer ? capturer->merged() : stream_info.merged;
        if (!virtual_privacy) {
            virtual_privacy = std::make_unique<VirtualDisplayPrivacy>(
                make_display_privacy_backend(settings_.virtual_display_resolution,
                                             default_virtual_display_package_dir()));
        }
        if (!virtual_privacy->enable(privacy_capture, &failure)) {
            ++stats.privacy_mode_changes;
            log("virtual display privacy failed: " + failure);
            if (virtual_privacy->recovery_pending()) {
                send_privacy_state(false, PrivacyModeCode::Failed, failure);
                *error = failure;
                return false;
            }
            if (!virtual_privacy->topology_may_have_changed() && !mode_was_active) {
                return send_privacy_state(false, PrivacyModeCode::Failed, failure);
            }
            // Even a failed activation can invalidate the old duplication.
            encoder.reset();
            capturer.reset();
            std::string stream_error;
            if (!privacy_capture({}, true, &stream_error) ||
                (restore_display_subscriptions &&
                 !restore_display_subscriptions(&stream_error))) {
                *error = failure + "; stream recovery: " + stream_error;
                return false;
            }
            return send_privacy_state(false, PrivacyModeCode::Failed, failure);
        }
        privacy_active = true;
        stats.privacy_mode_active = true;
        ++stats.privacy_mode_changes;
        log("virtual display privacy is on; physical outputs are disabled");
        return send_privacy_state(true, PrivacyModeCode::Ok, {});
    };

    // IDs survive output reordering during this connection, but are never
    // recycled after an identity disappears. No display-list index is trusted.
    std::vector<std::pair<uint32_t, std::wstring>> display_identities;
    uint32_t next_display_identity = 1;
    auto refresh_display_identities = [&](DisplayControlState* state,
                                          std::string* failure) -> bool {
        std::vector<VirtualDisplayOutput> outputs;
        if (!enumerate_virtual_display_outputs(outputs, failure)) {
            return false;
        }
        std::erase_if(display_identities, [&](const auto& item) {
            return std::none_of(outputs.begin(), outputs.end(), [&](const auto& output) {
                return _wcsicmp(item.second.c_str(), output.identity.c_str()) == 0;
            });
        });
        for (const VirtualDisplayOutput& output : outputs) {
            auto item = std::find_if(
                display_identities.begin(), display_identities.end(),
                [&](const auto& candidate) {
                    return _wcsicmp(candidate.second.c_str(), output.identity.c_str()) == 0;
                });
            if (item == display_identities.end()) {
                if (!next_display_identity) {
                    *failure = "virtual display identity limit reached";
                    return false;
                }
                display_identities.emplace_back(next_display_identity++, output.identity);
                item = std::prev(display_identities.end());
            }
            if (state) {
                state->outputs.push_back({item->first, output.active});
            }
        }
        return true;
    };
    auto send_display_control = [&](DisplayControlCode code,
                                    const std::string& reason) -> bool {
        DisplayControlState state;
        const VirtualDisplayStatus driver =
            classify_virtual_display_probe(probe_virtual_display());
        state.supported = settings_.virtual_display_provider != "vdd" &&
                          (driver.state == VirtualDisplayState::Installed ||
                           driver.state == VirtualDisplayState::Active);
        state.managed = managed_displays.backend != nullptr;
        state.count = static_cast<uint8_t>(
            std::min(attached_virtual_display_count(), kMaxVirtualDisplayMonitors));
        state.privacy = privacy_implementation == "virtual_display"
                            ? PrivacyImplementation::VirtualDisplay
                            : PrivacyImplementation::Overlay;
        state.code = code;
        state.reason = reason;
        std::string scan_error;
        if (!refresh_display_identities(&state, &scan_error)) {
            state.code = DisplayControlCode::Failed;
            state.reason += (state.reason.empty() ? "" : "; ") + scan_error;
        } else {
            state.count = static_cast<uint8_t>(
                std::count_if(state.outputs.begin(), state.outputs.end(),
                              [](const auto& output) { return output.active; }));
        }
        const Error sent = session_->send_display_control_state(state);
        if (!sent.ok()) {
            *error = "display control: " + sent.to_string();
        }
        return sent.ok();
    };
    // Release capture before recovering the CCD topology; the independent
    // guardian retains ownership when recovery cannot finish.
    auto restore_managed_displays = [&](std::string* failure) -> bool {
        if (!managed_displays.backend) {
            return true;
        }
        encoder.reset();
        capturer.reset();
        if (mode_change->active() &&
            mode_change->restore(failure) == ModeRestore::Failed) {
            return false;
        }
        if (!managed_displays.backend->restore(failure)) {
            return false;
        }
        managed_displays.backend.reset();
        privacy_original_device = managed_original_device;
        privacy_original_merged = managed_original_merged;
        if (!privacy_capture({}, true, failure)) return false;
        return !restore_display_subscriptions || restore_display_subscriptions(failure);
    };
    auto control_displays = [&](const DisplayControlRequest& request) -> bool {
        if (request.action == DisplayControlAction::Query) {
            return send_display_control(DisplayControlCode::Ok, {});
        }
        std::string failure;
        if (request.action == DisplayControlAction::Restore) {
            if (!restore_managed_displays(&failure)) {
                send_display_control(DisplayControlCode::Failed, failure);
                *error = "display recovery failed: " + failure;
                return false;
            }
            return send_display_control(DisplayControlCode::Ok, {});
        }
        const uint16_t need =
            (request.action == DisplayControlAction::SetCount ||
             request.action == DisplayControlAction::ToggleOutput)
                ? kPermissionPrivacyMode | kPermissionInput
                : kPermissionPrivacyMode;
        if ((session_->permissions() & need) != need) {
            return send_display_control(DisplayControlCode::Denied,
                                        "display control is not permitted");
        }
        if (privacy_active ||
            (virtual_privacy && virtual_privacy->recovery_pending())) {
            return send_display_control(
                DisplayControlCode::Busy,
                "turn off privacy before changing display settings");
        }
        if (request.action == DisplayControlAction::SelectPrivacy) {
            if (managed_displays.backend) {
                return send_display_control(
                    DisplayControlCode::Busy,
                    "restore the managed layout before selecting privacy");
            }
            if (request.value ==
                static_cast<uint8_t>(PrivacyImplementation::VirtualDisplay)) {
                const VirtualDisplayStatus driver =
                    classify_virtual_display_probe(probe_virtual_display());
                if (settings_.virtual_display_provider == "vdd" ||
                    (driver.state != VirtualDisplayState::Installed &&
                     driver.state != VirtualDisplayState::Active)) {
                    return send_display_control(DisplayControlCode::Unsupported,
                                                driver.detail);
                }
            }
            privacy_implementation =
                request.value == static_cast<uint8_t>(PrivacyImplementation::VirtualDisplay)
                    ? "virtual_display"
                    : "overlay";
            return send_display_control(DisplayControlCode::Ok, {});
        }
        if (registry_ && !registry_->may_change_host_state(this)) {
            return send_display_control(
                DisplayControlCode::Busy,
                "display topology changes require a single desktop viewer");
        }
        if (settings_.virtual_display_provider == "vdd") {
            return send_display_control(DisplayControlCode::Unsupported,
                                        "the vdd provider is not implemented");
        }
        std::wstring target_monitor;
        if (request.action == DisplayControlAction::ToggleOutput) {
            if (!refresh_display_identities(nullptr, &failure)) {
                return send_display_control(DisplayControlCode::Failed, failure);
            }
            const auto target = std::find_if(
                display_identities.begin(), display_identities.end(),
                [&](const auto& item) { return item.first == request.target; });
            if (target == display_identities.end()) {
                return send_display_control(
                    DisplayControlCode::Denied,
                    "unknown or stale virtual display identity; refresh the list");
            }
            target_monitor = target->second;
        }
        if (!managed_displays.backend) {
            std::unique_ptr<DisplayPrivacyBackend> backend =
                make_display_management_backend(default_virtual_display_package_dir());
            if (!backend->arm(&failure)) {
                return send_display_control(DisplayControlCode::Failed, failure);
            }
            remember_display_subscriptions();
            managed_original_device = capturer ? capturer->display().device_name : selected_device;
            managed_original_merged = capturer ? capturer->merged() : stream_info.merged;
            managed_displays.backend = std::move(backend);
        }
        privacy_original_device = capturer ? capturer->display().device_name : selected_device;
        privacy_original_merged = capturer ? capturer->merged() : stream_info.merged;
        std::wstring streamed_virtual_identity;
        if (capturer && !capturer->merged()) {
            for (const auto& item : display_identities) {
                if (virtual_display_device_name(item.second) == privacy_original_device) {
                    streamed_virtual_identity = item.second;
                    break;
                }
            }
        }
        // A newly constructed reader would otherwise reuse the old shared
        // duplication and its cached frame after CCD invalidates the output.
        // Release the encoder and every reader before changing the layout.
        encoder.reset();
        capturer.reset();
        bool changed = !mode_change->active() ||
                       mode_change->restore(&failure) != ModeRestore::Failed;
        std::vector<std::wstring> monitors;
        if (changed) {
            changed = request.action == DisplayControlAction::ToggleOutput
                          ? managed_displays.backend->toggle(target_monitor,
                                                             request.value != 0, &monitors,
                                                             &failure)
                          : managed_displays.backend->configure(request.value, &monitors,
                                                                &failure);
        }
        // Prove capture/conversion/hardware encoding for each active virtual
        // output before reporting success. Auto capture may upload a GDI frame
        // when Desktop Duplication is temporarily unavailable.
        if (changed) {
            for (const std::wstring& monitor : monitors) {
                if (!privacy_capture(monitor, false, &failure)) {
                    changed = false;
                    break;
                }
            }
        }
        if (changed) {
            const bool survives =
                !streamed_virtual_identity.empty() &&
                std::any_of(monitors.begin(), monitors.end(),
                            [&](const std::wstring& monitor) {
                                return _wcsicmp(monitor.c_str(),
                                                streamed_virtual_identity.c_str()) == 0;
                            });
            if (!streamed_virtual_identity.empty() && !survives) {
                // DISPLAYn can be reused after removal. Choose the surviving
                // primary explicitly instead of accidentally capturing that
                // name again.
                privacy_original_device.clear();
                privacy_original_merged = false;
            }
            changed = privacy_capture(survives ? streamed_virtual_identity : std::wstring{},
                                      true, &failure);
        }
        if (!changed) {
            const std::string cause = failure;
            if (!restore_managed_displays(&failure)) {
                send_display_control(DisplayControlCode::Failed,
                                     cause + "; recovery: " + failure);
                *error = "display recovery failed: " + failure;
                return false;
            }
            return send_display_control(DisplayControlCode::Failed,
                                        cause + "; original layout restored");
        }
        return send_display_control(DisplayControlCode::Ok, {});
    };

    auto& video_needs_resync = video.needs_resync;
    auto process_frame = [&](uint64_t pts_ms, bool idle = false) -> bool {
        const FrameInfo& frame = idle ? cached_frame : capturer->frame();
        if (!frame.has_video_image()) {
            have_input = false;
            cached_nv12.reset();
            // A conversion can fail for one frame and recover; a pass that was
            // torn down cannot. Give it a few frames, then end the session
            // instead of stalling silently.
            if (++frames_without_nv12 < 3) {
                return true;
            }
            *error = "capture produced no NV12 image for 3 frames";
            const Error& capture_error = capturer->last_error();
            if (!capture_error.ok()) {
                *error += ": " + capture_error.to_string();
            }
            return false;
        }
        frames_without_nv12 = 0;
        if (frame.rotation_degrees != 0) {
            *error = "capture lost rotation while streaming";
            return false;
        }
        packets.clear();
        Error err = Error::success();
        // A refinement repeats the cached picture; it carries no new desktop
        // content and must not count as motion or as delivery-window backlog.
        const bool refine = (idle || !frame.nv12_updated) && have_input;
        if (refine) {
            err = encoder->encode_repeat(pts_ms, force_keyframe, packets);
            if (err.ok()) {
                ++stats.repeats;
            } else if (err.status() == Status::DeviceRemoved ||
                       err.status() == Status::DeviceReset) {
                *error = "encode_repeat: " + err.to_string();
                return false;
            } else {
                packets.clear();
                err = encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
            }
        } else {
            err = encoder->encode(frame.nv12_texture, pts_ms, force_keyframe, packets);
        }
        if (!err.ok()) {
            *error = "encode: " + err.to_string();
            return false;
        }
        have_input = true;
        if (!idle) {
            if (frame.nv12_texture) frame.nv12_texture->AddRef();
            cached_nv12 = ComPtr<ID3D11Texture2D>(frame.nv12_texture);
            cached_frame = frame;
        }

        for (const EncodedPacket& packet : packets) {
            if (packet.data.empty()) {
                continue;
            }
            FrameHeader header;
            header.stream_id = video.id;
            header.pts_ms = packet.pts_ms;
            header.width = encoder->width();
            header.height = encoder->height();
            header.codec = encoder->codec();
            header.chroma = encoder->chroma();
            header.keyframe = packet.keyframe;
            header.idr = packet.idr;
            // Preserve service capture time through a slow subscriber's queue.
            header.capture_ms = DesktopStreamPool::capture_time(*capturer) & 0xFFFFFFFFFFFFull;
            Error sent = refine
                ? session_->send_frame_best_effort(header, packet.data.data(), packet.data.size())
                : session_->send_frame_nowait(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == Status::WaitTimeout) {
                    // The compressed reference chain may now have a hole.
                    // Resume this subscription with an IDR once it can send.
                    video_needs_resync = true;
                    return true;
                }
                if (sent.status() == Status::Unavailable) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "send_frame: " + sent.to_string();
                }
                return false;
            }
            if (config_.packet) {
                config_.packet(header, packet.data.data(), packet.data.size());
            }
            ++stats.encoded;
            stats.bytes += packet.data.size();
            if (packet.keyframe) {
                ++stats.keyframes;
                last_keyframe = Clock::now();
                if (extra_idrs > 0) {
                    --extra_idrs;
                    force_keyframe = true;
                } else {
                    force_keyframe = false;
                }
            }
        }
        publish_stats(stats);
        return true;
    };

    auto describe_video = [&](DesktopVideo& entry) {
        const auto frame = entry.capture->frame();
        entry.framerate = entry.encoder->framerate();
        entry.target_bitrate = entry.encoder->bitrate();
        entry.resolution_percent = static_cast<uint16_t>(entry.capture->nv12_scale_percent());
        entry.info = describe_stream(attached, *entry.capture, *entry.encoder,
            frame.width ? frame.width : entry.info.source_width,
            frame.height ? frame.height : entry.info.source_height,
            static_cast<uint16_t>(entry.framerate), session_->permissions(), audio_info,
            settings_.allow_terminal_persist);
    };
    auto refresh_adapters = [&]() -> bool {
        auto refresh = [&](DesktopVideo& entry) {
            if (!entry.capture || entry.recovery_at != Clock::time_point{}) return true;
            std::string failure;
            auto replacement = DesktopStreamPool::encoder(*entry.capture, encoder_config, &failure);
            if (!replacement) {
                entry.encoder.reset(); entry.cached_nv12.reset(); entry.have_input = false;
                entry.recovery_at = Clock::now() + std::chrono::seconds(1);
                log("display encoder recovery pending: " + failure);
                return true;
            }
            const bool changed = !entry.encoder || replacement->codec() != entry.encoder->codec() ||
                replacement->chroma() != entry.encoder->chroma() ||
                replacement->width() != entry.encoder->width() || replacement->height() != entry.encoder->height();
            entry.encoder = std::move(replacement);
            if (changed) { entry.force_keyframe = true; entry.cached_nv12.reset(); entry.have_input = false; }
            describe_video(entry);
            return true;
        };
        if (!refresh(video)) return false;
        for (auto& [id, entry] : other_videos) if (!refresh(entry)) return false;
        stats.target_bitrate = video.target_bitrate;
        stats.framerate = static_cast<uint16_t>(framerate);
        stats.resolution_percent = video.resolution_percent;
        return true;
    };
    auto send_subscriptions = [&](uint32_t request_id, DisplaySwitchCode code,
                                  const std::string& reason) -> bool {
        CaptureDisplaysResult result;
        result.request_id = request_id; result.code = code; result.reason = reason;
        if (capturer) result.streams.push_back({video.id, video_index(*capturer), stream_info});
        for (const auto& [id, entry] : other_videos)
            result.streams.push_back({id, video_index(*entry.capture), entry.info});
        auto sent = session_->send_capture_displays_result(result);
        if (!sent.ok()) {
            if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) *peer_left = true;
            else *error = "display subscriptions: " + sent.to_string();
            return false;
        }
        return true;
    };
    publish_subscriptions = [&] { return send_subscriptions(0, DisplaySwitchCode::Ok, {}); };
    auto focus_video = [&](uint32_t id) {
        const auto found = other_videos.find(id);
        if (found == other_videos.end()) return;
        DesktopVideo next = std::move(found->second);
        other_videos.erase(found);
        swap_video(next);
        if (next.capture) other_videos.emplace(next.id, std::move(next));
    };
    auto topology_seen = attached;
    auto topology_due = Clock::now();
    bool recover_primary = false;
    auto refresh_topology = [&](bool force) -> bool {
        if (!multiple_streams || (!force && Clock::now() < topology_due)) return true;
        topology_due = Clock::now() + std::chrono::milliseconds(300);
        auto fresh = config_.displays();
        if (fresh.size() > kMaxDisplayCount) fresh.resize(kMaxDisplayCount);
        const bool changed = !same_display_layout(topology_seen, fresh);
        if (!changed && !recover_primary) return true;
        topology_seen = fresh;
        const bool had_video = capturer || !other_videos.empty();
        auto present = [&](const IDxgiCapturer& source) {
            return source.merged() ? !fresh.empty() : find_display(fresh, source.display()) < fresh.size();
        };
        std::set<uint16_t> occupied;
        if (capturer && present(*capturer)) occupied.insert(capturer->merged() ? kDisplayAll :
            static_cast<uint16_t>(find_display(fresh, capturer->display())));
        for (auto it = other_videos.begin(); it != other_videos.end();) {
            const auto& source = *it->second.capture;
            const auto index = source.merged() ? kDisplayAll : static_cast<uint16_t>(find_display(fresh, source.display()));
            if (!present(source) || !occupied.insert(index).second) it = other_videos.erase(it);
            else ++it;
        }
        if (capturer && !present(*capturer)) {
            cached_nv12.reset(); encoder.reset(); capturer.reset(); have_input = false;
        }
        if (!capturer && !other_videos.empty()) focus_video(other_videos.begin()->first);
        if (had_video && !capturer) recover_primary = true;
        attached = fresh;
        std::string fallback_error;
        if (recover_primary && !fresh.empty()) {
            auto primary = std::find_if(fresh.begin(), fresh.end(), [](const auto& d) { return d.primary; });
            if (primary == fresh.end()) primary = fresh.begin();
            DesktopVideo replacement;
            if (!next_video_id) fallback_error = "video stream ID space exhausted";
            else replacement.capture = subscribe_display(*primary, capture, &fallback_error);
            if (replacement.capture && wait_for_usable_frame(*replacement.capture, true, stopping_,
                    std::chrono::seconds(3), &fallback_error)) {
                DesktopStreamPool::reserve_audio(*replacement.capture, bitrate_reserve(audio_info));
                replacement.encoder = DesktopStreamPool::encoder(*replacement.capture, encoder_config, &fallback_error);
                if (replacement.encoder) {
                    replacement.id = next_video_id++;
                    describe_video(replacement);
                    swap_video(replacement);
                    recover_primary = false;
                    fallback_error.clear();
                }
            }
        }
        auto update = [&](DesktopVideo& entry) {
            if (!entry.capture) return;
            if (!entry.capture->merged()) {
                const auto index = find_display(fresh, entry.capture->display());
                if (index >= fresh.size()) return;
                const auto& display = fresh[index];
                bool rebuilt = false;
                auto result = DesktopStreamPool::refresh_display(*entry.capture, display, &rebuilt);
                if (rebuilt || !result.ok()) {
                    entry.encoder.reset(); entry.cached_nv12.reset(); entry.have_input = false;
                    entry.force_keyframe = true; entry.frames_without_nv12 = 0;
                    if (entry.cursor) entry.cursor->reset();
                    entry.recovery_at = result.ok() ? Clock::time_point{} : Clock::now() + std::chrono::seconds(1);
                }
                entry.info.origin_x = display.rect.left; entry.info.origin_y = display.rect.top;
            }
            entry.info.displays.clear();
            for (const auto& display : fresh) entry.info.displays.push_back({
                display_name_utf8(display), display.rect.left, display.rect.top,
                display.rect.right, display.rect.bottom, static_cast<uint16_t>(display.rotation_degrees), display.primary});
        };
        update(video);
        for (auto& [id, entry] : other_videos) update(entry);
        // The display list is connection state even while no video is selected.
        if (!capturer) {
            stream_info.displays.clear();
            for (const auto& display : fresh) stream_info.displays.push_back({
                display_name_utf8(display), display.rect.left, display.rect.top,
                display.rect.right, display.rect.bottom, static_cast<uint16_t>(display.rotation_degrees), display.primary});
        }
        if (!send_subscriptions(0, DisplaySwitchCode::Ok, {})) return false;
        const auto sent = session_->refresh_stream(stream_info);
        if (!sent.ok()) { *error = "display topology: " + sent.to_string(); return false; }
        publish_current(stream_info);
        if (!fallback_error.empty()) log("display recovery pending: " + fallback_error);
        return true;
    };
    publish_subscriptions = [&] {
        topology_due = {};
        if (!refresh_topology(true)) return false;
        return send_subscriptions(0, DisplaySwitchCode::Ok, {});
    };
    auto capture_displays = [&](const CaptureDisplaysRequest& request) -> bool {
        if (!multiple_streams) {
            if (video.cursor) video.cursor->reset();
        }
        multiple_streams = true;
        if (!refresh_topology(true)) return false;
        const auto fresh = topology_seen;
        std::vector<uint16_t> current;
        auto index_in = [&](const IDxgiCapturer& source) -> uint16_t {
            return source.merged() ? kDisplayAll : static_cast<uint16_t>(find_display(fresh, source.display()));
        };
        if (capturer) current.push_back(index_in(*capturer));
        for (const auto& [id, entry] : other_videos) current.push_back(index_in(*entry.capture));
        const auto wanted = select_capture_displays(current, request, fresh.size());
        if (wanted.size() > kMaxDisplayCount)
            return send_subscriptions(request.request_id, DisplaySwitchCode::Failed, "too many simultaneous display streams");
        if (virtual_privacy && virtual_privacy->recovery_pending() && wanted != current)
            return send_subscriptions(request.request_id, DisplaySwitchCode::Busy,
                "turn off virtual display privacy before changing display subscriptions");
        std::map<uint32_t, DesktopVideo> prepared;
        std::string failure;
        for (auto index : wanted) {
            if (std::find(current.begin(), current.end(), index) != current.end()) continue;
            if (!next_video_id) { failure = "video stream ID space exhausted"; break; }
            DesktopVideo next;
            next.id = next_video_id++;
            next.capture = subscribe_display(fresh[index], capture, &failure);
            if (!next.capture || !wait_for_usable_frame(*next.capture, true, stopping_,
                    std::chrono::seconds(3), &failure)) break;
            DesktopStreamPool::reserve_audio(*next.capture, bitrate_reserve(audio_info));
            next.encoder = DesktopStreamPool::encoder(*next.capture, encoder_config, &failure);
            if (!next.encoder) break;
            failure.clear(); // failed codec candidates can precede a usable one
            prepared.emplace(next.id, std::move(next));
        }
        if (!failure.empty()) {
            prepared.clear(); // rollback global source restrictions before describing survivors
            // Existing adapters will follow the surviving source's format on
            // the next tick; a rejected request never removes a subscription.
            return send_subscriptions(request.request_id, DisplaySwitchCode::Failed, failure);
        }
        attached = std::move(fresh);
        recover_primary = false; // An explicit empty set must stay empty after a hotplug.
        auto keep = [&](const IDxgiCapturer& source) {
            const auto index = video_index(source);
            return std::find(wanted.begin(), wanted.end(), index) != wanted.end();
        };
        for (auto it = other_videos.begin(); it != other_videos.end();)
            if (!keep(*it->second.capture)) it = other_videos.erase(it); else ++it;
        other_videos.merge(prepared);
        if (capturer && !keep(*capturer)) {
            cached_nv12.reset(); encoder.reset(); capturer.reset();
            have_input = false;
        }
        if (!capturer && !other_videos.empty()) focus_video(other_videos.begin()->first);
        if (!refresh_adapters()) return false;
        if (!send_subscriptions(request.request_id, DisplaySwitchCode::Ok, {})) return false;
        publish_current(stream_info);
        return true;
    };

    restore_display_subscriptions = [&](std::string* failure) -> bool {
        if (!saved_display_subscriptions.active) return true;

        std::vector<DisplayInfo> fresh = config_.displays();
        if (fresh.size() > kMaxDisplayCount) fresh.resize(kMaxDisplayCount);
        multiple_streams = true;

        if (saved_display_subscriptions.streams.empty()) {
            cached_nv12.reset();
            encoder.reset();
            capturer.reset();
            other_videos.clear();
            have_input = false;
            attached = std::move(fresh);
            recover_primary = false;
            stream_info.displays.clear();
            for (const auto& display : attached) {
                stream_info.displays.push_back(
                    {display_name_utf8(display), display.rect.left, display.rect.top,
                     display.rect.right, display.rect.bottom,
                     static_cast<uint16_t>(display.rotation_degrees), display.primary});
            }
            saved_display_subscriptions = {};
            if (!send_subscriptions(0, DisplaySwitchCode::Ok, {})) return false;
            Error refreshed = session_->refresh_stream(stream_info);
            if (!refreshed.ok()) {
                *failure = "restore empty display subscription: " + refreshed.to_string();
                return false;
            }
            return true;
        }

        std::map<uint32_t, DesktopVideo> rebuilt;
        for (const SavedDisplaySubscription& saved : saved_display_subscriptions.streams) {
            const auto target = std::find_if(fresh.begin(), fresh.end(), [&](const auto& display) {
                return same_display_identity(saved.display, display);
            });
            if (target == fresh.end()) {
                *failure = "previously subscribed display is not attached: " +
                           display_name_utf8(saved.display);
                return false;
            }
            DesktopVideo next;
            next.id = saved.id;
            next.resolution_request = saved.resolution_request;
            next.resolution_fixed = saved.resolution_fixed;
            next.capture = subscribe_display(*target, capture, failure);
            if (!next.capture) return false;
            if (next.resolution_request != 100) {
                Error scaled = next.capture->set_nv12_scale(next.resolution_request);
                if (!scaled.ok()) {
                    *failure = "cannot restore display resolution: " + scaled.to_string();
                    return false;
                }
            }
            if (!wait_for_usable_frame(*next.capture, true, stopping_,
                                       std::chrono::seconds(5), failure)) {
                return false;
            }
            DesktopStreamPool::reserve_audio(*next.capture, bitrate_reserve(audio_info));
            next.encoder = DesktopStreamPool::encoder(*next.capture, encoder_config, failure);
            if (!next.encoder) return false;
            rebuilt.emplace(next.id, std::move(next));
        }

        attached = std::move(fresh);
        for (auto& [id, entry] : rebuilt) describe_video(entry);
        const uint32_t focused_id = saved_display_subscriptions.streams.front().id;
        auto focused = rebuilt.find(focused_id);
        if (focused == rebuilt.end()) {
            *failure = "the focused display subscription could not be restored";
            return false;
        }
        DesktopVideo focused_video = std::move(focused->second);
        rebuilt.erase(focused);
        video = std::move(focused_video);
        other_videos = std::move(rebuilt);
        recover_primary = false;
        stats.target_bitrate = video.target_bitrate;
        stats.framerate = static_cast<uint16_t>(video.framerate);
        stats.resolution_percent = video.resolution_percent;
        selected_device = capturer->display().device_name;
        saved_display_subscriptions = {};

        if (!send_subscriptions(0, DisplaySwitchCode::Ok, {})) return false;
        Error refreshed = session_->refresh_stream(stream_info);
        if (!refreshed.ok()) {
            *failure = "restore display subscriptions: " + refreshed.to_string();
            return false;
        }
        publish_current(stream_info);
        log("restored the complete multi-display subscription set");
        return true;
    };

    // A viewer can ask for another output without reconnecting. The new
    // subscription is prepared beside the running one and committed only
    // after capture and encoding are ready, so a failed switch leaves the old
    // stream untouched. Runs on the host thread at the top of the loop.
    bool switched_once = false;
    Clock::time_point last_switch_at{};
    auto switch_display = [&](uint16_t requested_index) -> bool {
        const auto now = Clock::now();
        auto answer = [&](DisplaySwitchCode code, const std::string& reason) {
            Error sent = session_->send_display_switch_result(requested_index, code, reason);
            if (sent.ok()) {
                return true;
            }
            if (sent.status() == Status::Unavailable ||
                sent.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "send_display_switch_result: " + sent.to_string();
            }
            return false;
        };
        auto refuse = [&](DisplaySwitchCode code, const std::string& reason) {
            ++stats.display_switch_failures;
            log("display switch refused: " + reason);
            return answer(code, reason);
        };

        // Overlay privacy masks the local screen but leaves capture running,
        // so it never blocks a switch. Virtual display privacy does: the
        // session is capturing the privacy output until the guardian restores
        // the physical topology.
        if (virtual_privacy && virtual_privacy->recovery_pending()) {
            return refuse(DisplaySwitchCode::Busy,
                          "turn off virtual display privacy before switching displays");
        }
        if (switched_once && now - last_switch_at < std::chrono::seconds(1)) {
            return answer(DisplaySwitchCode::Busy, "a display switch just happened");
        }

        // Re-enumerate: a hotplug can renumber the outputs since HelloAck.
        std::vector<DisplayInfo> fresh = config_.displays();
        if (fresh.empty()) {
            return refuse(DisplaySwitchCode::NotFound, "no displays are attached");
        }
        const bool merged_target = requested_index == kDisplayAll;
        size_t target = fresh.size();
        if (merged_target) {
            // The union has no index; every attached output is a candidate
            // and create_merged_capturer rejects a mixed-adapter layout.
        } else if (requested_index != kDisplayAuto) {
            target = requested_index;
        } else if (settings_.display != kDisplayAuto) {
            target = settings_.display;
        } else {
            target = 0;
            for (size_t i = 0; i < fresh.size(); ++i) {
                if (fresh[i].primary) {
                    target = i;
                    break;
                }
            }
        }
        if (!merged_target && target >= fresh.size()) {
            return refuse(DisplaySwitchCode::NotFound,
                          "display " + std::to_string(requested_index) + " is not attached");
        }
        bool already_streaming = false;
        if (capturer && merged_target && capturer->merged()) {
            // Members can move or change size without changing count, so the
            // union is compared too, not just the member count.
            const std::vector<DisplayInfo> members = merged_members(fresh, capturer->display());
            LONG left = members[0].rect.left;
            LONG top = members[0].rect.top;
            LONG right = members[0].rect.right;
            LONG bottom = members[0].rect.bottom;
            for (const DisplayInfo& member : members) {
                left = std::min(left, member.rect.left);
                top = std::min(top, member.rect.top);
                right = std::max(right, member.rect.right);
                bottom = std::max(bottom, member.rect.bottom);
            }
            const DisplayRect& running = capturer->display().rect;
            already_streaming = members.size() == capturer->merged_display_count() &&
                                left == running.left && top == running.top &&
                                right == running.right && bottom == running.bottom;
        } else if (capturer && !merged_target && !capturer->merged()) {
            already_streaming = same_display_identity(fresh[target], capturer->display());
        }
        if (already_streaming) {
            switched_once = true;
            last_switch_at = now;
            ++stats.display_switches;
            log("display switch: already streaming " +
                streamed_display_name(*capturer));
            return answer(DisplaySwitchCode::Ok, std::string());
        }

        if (multiple_streams && !merged_target) {
            CaptureDisplaysRequest add;
            add.add = {static_cast<uint16_t>(target)};
            if (!capture_displays(add)) return false;
            auto selected = std::find_if(other_videos.begin(), other_videos.end(), [&](const auto& entry) {
                return same_display_identity(entry.second.capture->display(), fresh[target]);
            });
            if (selected != other_videos.end()) focus_video(selected->first);
            if (!capturer || !same_display_identity(capturer->display(), fresh[target]))
                return refuse(DisplaySwitchCode::Failed, "the requested display could not be subscribed");
            ++stats.display_switches;
            switched_once = true; last_switch_at = now;
            publish_current(stream_info);
            return answer(DisplaySwitchCode::Ok, {}) && refresh_video(stream_info).ok();
        }

        std::string switch_error;
        std::unique_ptr<IDxgiCapturer> next;
        if (merged_target) {
            next = subscribe_merged(fresh, capture, &switch_error);
            if (!next) {
                return refuse(DisplaySwitchCode::Failed,
                              "cannot merge " + std::to_string(fresh.size()) +
                                  " displays: " + switch_error);
            }
        } else {
            next = subscribe_display(fresh[target], capture, &switch_error);
            if (!next) {
                return refuse(DisplaySwitchCode::Failed,
                              "cannot capture display " + std::to_string(target) + ": " +
                                  switch_error);
            }
        }
        // Carry the ladder's request across the switch. A merged canvas can
        // cap it lower, so the effective value is read back after the first
        // frame either way.
        UINT next_scale_request = resolution_request;
        if (resolution_request != 100) {
            Error scaled = next->set_nv12_scale(resolution_request);
            if (!scaled.ok()) {
                log("display switch: cannot scale to " + std::to_string(resolution_request) +
                    "%; using 100%: " + scaled.to_string());
                next_scale_request = 100;
            }
        }
        if (!wait_for_usable_frame(*next, true, stopping_, std::chrono::milliseconds(3000),
                                   &switch_error)) {
            return refuse(DisplaySwitchCode::Failed,
                          "the new output produced no frame: " + switch_error);
        }
        // Read the effective scale now, but publish it only after the swap:
        // a later refusal must leave the running capture's state alone.
        const uint16_t next_effective_percent =
            static_cast<uint16_t>(next->nv12_scale_percent());
        DesktopStreamPool::reserve_audio(*next, bitrate_reserve(audio_info));
        const FrameInfo next_frame = next->frame();
        EncoderConfig next_config = encoder_config;
        next_config.width = next_frame.nv12_width;
        next_config.height = next_frame.nv12_height;
        std::unique_ptr<IVideoEncoder> next_encoder =
            DesktopStreamPool::encoder(*next, next_config, &switch_error);
        if (!next_encoder) {
            return refuse(DisplaySwitchCode::Failed,
                          "encoder creation failed: " + switch_error);
        }
        if (next_encoder->codec() != encoder_config.codec) {
            log(std::string("the encoder fell back: requested ") +
                codec_name(encoder_config.codec) + ", producing " +
                codec_name(next_encoder->codec()));
        }
        // The replacement starts at the configured bitrate, so the audio
        // reserve and any ABR step-down have to be applied again.
        const uint32_t next_fps =
            stats.framerate != 0 ? stats.framerate : static_cast<uint32_t>(framerate);
        if (next_encoder->bitrate() != stats.target_bitrate ||
            (next_fps != 0 && next_encoder->framerate() != next_fps)) {
            Error rated = next_encoder->set_rate(stats.target_bitrate, next_fps);
            if (!rated.ok()) {
                return refuse(DisplaySwitchCode::Failed,
                              "cannot apply the current bitrate: " + rated.to_string());
            }
        }

        // The abandoned output must not keep a mode this session applied; the
        // new output starts from its own current mode.
        if (mode_change->active() &&
            mode_change->device_name() != next->display().device_name) {
            std::string restore_error;
            switch (mode_change->restore(&restore_error)) {
                case ModeRestore::Restored:
                    log("display switch: restored the old output's mode");
                    break;
                case ModeRestore::Skipped:
                    log("display switch: the old output's mode was changed elsewhere; "
                        "keeping it");
                    break;
                case ModeRestore::Failed:
                    log("display switch: could not restore the old output's mode: " +
                        restore_error);
                    break;
                case ModeRestore::None:
                    break;
            }
        }

        // Both sides are ready; swap and describe what now runs. `next_frame`
        // preserves the source dimensions across the subscription swap.
        if (multiple_streams) {
            if (!next_video_id) return refuse(DisplaySwitchCode::Failed, "video stream ID space exhausted");
            DesktopVideo previous;
            swap_video(previous);
            if (previous.capture) other_videos.emplace(previous.id, std::move(previous));
            video.id = next_video_id++;
        }
        capturer = std::move(next);
        encoder = std::move(next_encoder);
        attached = std::move(fresh);
        have_input = false;
        cached_nv12.reset();
        force_keyframe = true;
        extra_idrs = 1;
        // The new capture is live; commit its request and effective scale
        // only now, so a refused switch leaves the old output's state intact.
        resolution_request = static_cast<uint16_t>(next_scale_request);
        if (next_effective_percent != stats.resolution_percent) {
            stats.resolution_percent = next_effective_percent;
            ++stats.resolution_changes;
        }
        // The new output has its own pointer shape and position; the viewer
        // must not keep placing the old sprite.
        if (video.cursor) video.cursor->reset();
        const size_t new_index =
            capturer->merged() ? size_t{0} : find_display(attached, capturer->display());
        stream_info = describe_stream(attached, *capturer, *encoder, next_frame.width,
                                      next_frame.height, static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info,
                                      settings_.allow_terminal_persist);
        if (multiple_streams && !send_subscriptions(0, DisplaySwitchCode::Ok, {})) return false;
        publish_current(stream_info);
        if (config_.stream_announced) {
            config_.stream_announced(stream_info);
        }
        if (!answer(DisplaySwitchCode::Ok, std::string())) {
            return false;
        }
        Error refreshed = refresh_video(stream_info);
        if (!refreshed.ok()) {
            if (refreshed.status() == Status::Unavailable ||
                refreshed.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "refresh_stream: " + refreshed.to_string();
            }
            return false;
        }
        ++stats.stream_updates;
        ++stats.display_switches;
        switched_once = true;
        last_switch_at = now;
        publish_stats(stats);
        log("display switched to " +
            (capturer->merged()
                 ? "the merged view of " + std::to_string(capturer->merged_display_count()) +
                       " displays"
                 : std::to_string(new_index) + " (" +
                       display_name_utf8(capturer->display()) + ")") +
            ", " + std::to_string(stream_info.source_width) + "x" +
            std::to_string(stream_info.source_height) + " source, " +
            std::to_string(stream_info.width) + "x" + std::to_string(stream_info.height) +
            " coded");
        return true;
    };

    // Automatic display following. The viewer toggles it per session; the
    // host polls at a low rate and moves its capture through switch_display,
    // so a followed switch has exactly the same subscription, cooldown and refusal
    // rules as one the viewer asked for. A candidate must stay put for a
    // moment before it is taken, so a pointer crossing two outputs quickly
    // cannot trigger two rebuilds.
    uint8_t follow_flags = 0;
    uint16_t follow_candidate = kDisplayAuto;
    Clock::time_point follow_candidate_since{};
    Clock::time_point follow_polled_at{};
    Clock::time_point follow_switch_at{};
    constexpr auto kFollowPollInterval = std::chrono::milliseconds(200);
    constexpr auto kFollowSettleTime = std::chrono::milliseconds(400);
    auto send_follow_state = [&](uint8_t flags, DisplayFollowCode code,
                                 const std::string& reason) -> bool {
        DisplayFollowState state;
        state.flags = code == DisplayFollowCode::Ok ? flags : 0;
        state.code = code;
        state.reason = reason;
        Error sent = session_->send_display_follow_state(state);
        if (sent.ok()) {
            return true;
        }
        if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) {
            *peer_left = true;
        } else {
            ++stats.send_errors;
            *error = "send_display_follow_state: " + sent.to_string();
        }
        return false;
    };

    // Mode queries and changes for the streamed output. A list request is
    // answered from a fresh enumeration; a change is a synchronous rebuild:
    // the mode is applied, capture is recreated at the new size, a new encoder
    // is built and announced, and only then does the viewer get Ok. Any
    // failure puts the old mode back and leaves the old stream running.
    auto send_mode_list = [&](uint16_t requested_index) -> bool {
        DisplayModeList answer_list;
        answer_list.display = requested_index;
        std::vector<DisplayInfo> fresh = config_.displays();
        std::string list_error;
        if (capturer && capturer->merged()) {
            answer_list.code = DisplayModeCode::Failed;
            answer_list.reason = "the merged view does not support single display modes";
        } else if (fresh.empty()) {
            answer_list.code = DisplayModeCode::NotFound;
            answer_list.reason = "no displays are attached";
        } else if (requested_index >= fresh.size()) {
            answer_list.code = DisplayModeCode::NotFound;
            answer_list.reason =
                "display " + std::to_string(requested_index) + " is not attached";
        } else {
            const std::vector<DisplayMode> modes =
                list_display_modes(fresh[requested_index], &list_error);
            if (modes.empty()) {
                answer_list.code = DisplayModeCode::Failed;
                answer_list.reason =
                    list_error.empty() ? "the display reports no usable modes" : list_error;
            } else {
                for (const DisplayMode& mode : modes) {
                    DisplayModeInfo info;
                    info.width = mode.width;
                    info.height = mode.height;
                    info.refresh_hz = mode.refresh_hz;
                    info.current = mode.current;
                    answer_list.modes.push_back(info);
                }
            }
        }
        Error sent = session_->send_display_modes(answer_list);
        if (sent.ok()) {
            return true;
        }
        if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) {
            *peer_left = true;
        } else {
            ++stats.send_errors;
            *error = "send_display_modes: " + sent.to_string();
        }
        return false;
    };

    bool mode_changed_once = false;
    Clock::time_point last_mode_change_at{};

    // Recreates capture after ChangeDisplaySettingsExW invalidated the
    // duplication and waits for a usable NV12 frame. The desktop takes a
    // moment to settle, so with `exact_size` the output is required to report
    // the wanted size before the frame wait starts.
    auto rebuild_after_mode_change = [&](const DisplayMode& wanted, bool exact_size,
                                         std::string* rebuild_error) -> bool {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        for (;;) {
            Error reinit = capturer->reinitialize();
            if (!reinit.ok()) {
                *rebuild_error = "reinit failed: " + reinit.to_string();
                if (Clock::now() >= deadline) {
                    return false;
                }
                // A display mode change tears the desktop down and back up;
                // DuplicateOutput answers E_ACCESSDENIED until the driver has
                // finished, so retry instead of treating the first refusal as
                // fatal. The lock step also covers the old duplication's
                // asynchronous release.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const DisplayInfo& now_display = capturer->display();
            if (!exact_size ||
                (now_display.width == wanted.width && now_display.height == wanted.height)) {
                break;
            }
            if (Clock::now() >= deadline) {
                *rebuild_error = "the output still reports " +
                                 std::to_string(now_display.width) + "x" +
                                 std::to_string(now_display.height);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // The adaptive-resolution scale survives reinitialize(); applying the
        // same percentage is a no-op, and a failure leaves the old pass up.
        Error scaled = capturer->set_nv12_scale(stats.resolution_percent);
        if (!scaled.ok()) {
            log("mode change: cannot keep " + std::to_string(stats.resolution_percent) +
                "% scaling: " + scaled.to_string());
        }
        return wait_for_usable_frame(*capturer, true, stopping_,
                                     std::chrono::milliseconds(3000), rebuild_error);
    };

    auto change_display_mode = [&](uint16_t requested_index, uint16_t width, uint16_t height,
                                   uint16_t refresh_hz) -> bool {
        const auto now = Clock::now();
        auto answer = [&](DisplayModeCode code, const std::string& reason) {
            Error sent = session_->send_display_mode_result(requested_index, width, height,
                                                            refresh_hz, code, reason);
            if (sent.ok()) {
                return true;
            }
            if (sent.status() == Status::Unavailable ||
                sent.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "send_display_mode_result: " + sent.to_string();
            }
            return false;
        };
        auto refuse = [&](DisplayModeCode code, const std::string& reason) {
            ++stats.display_mode_failures;
            log("display mode refused: " + reason);
            return answer(code, reason);
        };

        if ((session_->permissions() & kPermissionInput) == 0) {
            return refuse(DisplayModeCode::Failed, "input control is disabled");
        }
        if (privacy_active ||
            (virtual_privacy && virtual_privacy->recovery_pending())) {
            return refuse(DisplayModeCode::Busy,
                          "turn off privacy before changing display modes");
        }
        if (managed_displays.backend) {
            return refuse(DisplayModeCode::Busy,
                          "restore the managed display layout before changing modes");
        }
        if (mode_changed_once && now - last_mode_change_at < std::chrono::seconds(1)) {
            return answer(DisplayModeCode::Busy, "a display mode change just happened");
        }
        if (!capturer) {
            return refuse(DisplayModeCode::NotFound, "no desktop display is subscribed");
        }
        if (capturer->merged()) {
            return refuse(DisplayModeCode::Failed,
                          "the merged view does not support single display modes");
        }

        // Re-enumerate like the switch path: a hotplug can renumber outputs.
        std::vector<DisplayInfo> fresh = config_.displays();
        if (fresh.empty()) {
            return refuse(DisplayModeCode::NotFound, "no displays are attached");
        }
        if (requested_index >= fresh.size()) {
            return refuse(DisplayModeCode::NotFound,
                          "display " + std::to_string(requested_index) + " is not attached");
        }
        const DisplayInfo& target_display = fresh[requested_index];
        if (target_display.device_name != capturer->display().device_name) {
            return refuse(DisplayModeCode::Failed,
                          "only the streamed output can change its mode");
        }
        std::string mode_error;
        const std::vector<DisplayMode> modes = list_display_modes(target_display, &mode_error);
        if (modes.empty()) {
            return refuse(DisplayModeCode::Failed, mode_error);
        }
        const auto wanted =
            std::find_if(modes.begin(), modes.end(), [&](const DisplayMode& mode) {
                return mode.width == width && mode.height == height &&
                       mode.refresh_hz == refresh_hz;
            });
        if (wanted == modes.end()) {
            return refuse(DisplayModeCode::Failed,
                          "the display does not report " + std::to_string(width) + "x" +
                              std::to_string(height) + " at " + std::to_string(refresh_hz) +
                              " Hz");
        }
        if (wanted->current) {
            mode_changed_once = true;
            last_mode_change_at = now;
            ++stats.display_mode_changes;
            log("display mode: already " + std::to_string(width) + "x" +
                std::to_string(height) + " at " + std::to_string(refresh_hz) + " Hz");
            return answer(DisplayModeCode::Ok, std::string());
        }

        // One saved original covers one output. The host restores before it
        // switches outputs; a request for a different output starts over.
        if (mode_change->has_saved() &&
            mode_change->device_name() != target_display.device_name) {
            if (mode_change->active()) {
                std::string restore_error;
                if (mode_change->restore(&restore_error) == ModeRestore::Failed) {
                    return refuse(DisplayModeCode::Failed,
                                  "the previous output's mode cannot be restored: " +
                                      restore_error);
                }
            }
            mode_change = std::make_unique<DisplayModeChange>();
        }
        if (!mode_change->save(target_display, &mode_error)) {
            return refuse(DisplayModeCode::Failed,
                          "cannot read the current mode: " + mode_error);
        }
        if (!mode_change->apply(*wanted, &mode_error)) {
            return refuse(DisplayModeCode::Failed, "cannot change the mode: " + mode_error);
        }

        // From here the output runs the new mode; every failure puts the old
        // one back before the answer goes out and before the old stream is
        // touched.
        auto rollback = [&](const std::string& why) {
            std::string restore_error;
            switch (mode_change->restore(&restore_error)) {
                case ModeRestore::Restored:
                    log("display mode: the old mode is back");
                    break;
                case ModeRestore::Skipped:
                    log("display mode: the mode was changed elsewhere; keeping it");
                    break;
                case ModeRestore::Failed:
                    log("display mode: restoring the old mode failed: " + restore_error);
                    break;
                case ModeRestore::None:
                    break;
            }
            std::string rebuild_error;
            if (!rebuild_after_mode_change(DisplayMode{}, false, &rebuild_error)) {
                log("display mode: capture did not recover at the old mode: " + rebuild_error);
            }
            // The old encoder cannot encode whatever size capture now
            // publishes; the frame loop builds one and re-announces.
            encoder.reset();
            have_input = false;
            cached_nv12.reset();
            force_keyframe = true;
            attached = config_.displays();
            return refuse(DisplayModeCode::Failed, why);
        };

        if (!rebuild_after_mode_change(*wanted, true, &mode_error)) {
            return rollback("the display produced no frame in the new mode: " + mode_error);
        }
        const FrameInfo mode_frame = capturer->frame();
        EncoderConfig mode_config = encoder_config;
        mode_config.width = mode_frame.nv12_width;
        mode_config.height = mode_frame.nv12_height;
        std::unique_ptr<IVideoEncoder> mode_encoder =
            DesktopStreamPool::encoder(*capturer, mode_config, &mode_error);
        if (!mode_encoder) {
            return rollback("encoder creation failed: " + mode_error);
        }
        if (mode_encoder->codec() != encoder_config.codec) {
            log(std::string("the encoder fell back: requested ") +
                codec_name(encoder_config.codec) + ", producing " +
                codec_name(mode_encoder->codec()));
        }
        // The replacement starts at the configured bitrate; the audio reserve
        // and the ABR's current step have to be applied again.
        const uint32_t mode_fps =
            stats.framerate != 0 ? stats.framerate : static_cast<uint32_t>(framerate);
        if (mode_encoder->bitrate() != stats.target_bitrate ||
            (mode_fps != 0 && mode_encoder->framerate() != mode_fps)) {
            Error rated = mode_encoder->set_rate(stats.target_bitrate, mode_fps);
            if (!rated.ok()) {
                return rollback("cannot apply the current bitrate: " + rated.to_string());
            }
        }

        encoder = std::move(mode_encoder);
        have_input = false;
        cached_nv12.reset();
        force_keyframe = true;
        extra_idrs = 1;
        // The output was re-created: its pointer position and sprite are new.
        if (video.cursor) video.cursor->reset();
        // `fresh` was read before ChangeDisplaySettingsExW, so its rects
        // describe the old mode. Announce the list as it is now.
        attached = config_.displays();
        stream_info = describe_stream(attached, *capturer, *encoder, mode_frame.width,
                                      mode_frame.height, static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info,
                                      settings_.allow_terminal_persist);
        publish_current(stream_info);
        if (config_.stream_announced) {
            config_.stream_announced(stream_info);
        }
        if (!answer(DisplayModeCode::Ok, std::string())) {
            return false;
        }
        Error refreshed = refresh_video(stream_info);
        if (!refreshed.ok()) {
            if (refreshed.status() == Status::Unavailable ||
                refreshed.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "refresh_stream: " + refreshed.to_string();
            }
            return false;
        }
        ++stats.stream_updates;
        ++stats.display_mode_changes;
        mode_changed_once = true;
        last_mode_change_at = now;
        publish_stats(stats);
        log("display mode changed to " + std::to_string(width) + "x" + std::to_string(height) +
            " at " + std::to_string(refresh_hz) + " Hz, " +
            std::to_string(stream_info.width) + "x" + std::to_string(stream_info.height) +
            " coded");
        return true;
    };

    // Each viewer votes in the shared codec and quality policies. Candidate
    // codecs are proved on every active display before committing; the actual
    // family may differ from this viewer's preference. Quality and FPS changes
    // use set_rate without rebuilding. Every request is answered exactly once.
    auto apply_stream_options = [&](const StreamOptions& options) -> bool {
        ++stats.stream_option_requests;
        auto answer = [&](StreamOptionsCode code, const std::string& reason) -> bool {
            StreamOptionsResult result;
            result.code = code;
            result.codec = encoder ? encoder->codec() : settings_.codec;
            result.chroma = encoder ? encoder->chroma() : VideoChroma::Yuv420;
            result.fps = fps_ceiling();
            result.quality = stream_quality;
            result.quality_percent =
                stream_quality == StreamQuality::Custom ? stream_quality_percent : 0;
            result.reason = reason;
            Error sent = session_->send_stream_options_result(result);
            if (sent.ok()) {
                return true;
            }
            if (sent.status() == Status::Unavailable ||
                sent.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "send_stream_options_result: " + sent.to_string();
            }
            return false;
        };
        auto refuse = [&](StreamOptionsCode code, const std::string& reason) {
            ++stats.stream_option_failures;
            log("stream options refused: " + reason);
            return answer(code, reason);
        };

        const bool cap_change = options.max_fps != stream_fps_cap;
        const bool preference_change = options.codec != codec_preference || options.chroma != chroma_preference;
        // Re-selecting the same preset is an intentional vote: another viewer
        // may have changed global quality since this connection selected it.
        const bool custom_quality = options.quality == StreamQuality::Custom;
        const bool quality_change =
            options.quality != stream_quality ||
            (custom_quality && options.quality_percent != stream_quality_percent) ||
            (!cap_change && !preference_change);
        auto vote_quality = [&] {
            if (!quality_change) {
                quality_peer->options(options.max_fps, std::nullopt);
            } else if (custom_quality) {
                quality_peer->options(options.max_fps, std::nullopt,
                                      options.quality_percent / 50.0f);
            } else {
                quality_peer->options(
                    options.max_fps,
                    std::optional<VideoQos::Quality>(
                        options.quality == StreamQuality::High ? VideoQos::Quality::High :
                        options.quality == StreamQuality::Balanced ? VideoQos::Quality::Balanced :
                        VideoQos::Quality::Smooth));
            }
        };
        if (!quality_change && !cap_change && !preference_change) {
            return answer(StreamOptionsCode::Ok, std::string());
        }

        if (!capturer || !encoder) {
            std::string failure;
            if (!DesktopStreamPool::vote_peer(*codec_peer, codec_vote(options.codec), &failure, options.chroma))
                return refuse(StreamOptionsCode::Failed, failure);
            vote_quality();
            codec_preference = options.codec;
            chroma_preference = options.chroma;
            stream_quality = options.quality;
            stream_quality_percent = custom_quality ? options.quality_percent : 0;
            stream_fps_cap = options.max_fps;
            return answer(StreamOptionsCode::Ok, {});
        }

        bool applied = false;
        bool keyframe_required = false;
        const auto previous_rate = encoder->bitrate();
        const auto previous_fps = encoder->framerate();
        if (preference_change) {
            std::string codec_error;
            auto wanted = encoder_config;
            wanted.chroma = options.chroma;
            auto replacement = DesktopStreamPool::vote_codec(*capturer, wanted,
                codec_vote(options.codec), &codec_error);
            if (!replacement) return refuse(StreamOptionsCode::Failed, codec_error);
            keyframe_required = replacement->codec() != encoder->codec() || replacement->chroma() != encoder->chroma() ||
                replacement->width() != encoder->width() || replacement->height() != encoder->height();
            if (keyframe_required) {
                encoder = std::move(replacement);
                have_input = false;
                cached_nv12.reset();
                extra_idrs = 1;
                applied = true;
            }
            encoder_config.codec = encoder->codec();
            encoder_config.chroma = encoder->chroma();
            codec_preference = options.codec;
            chroma_preference = options.chroma;
        }

        vote_quality();
        auto shared_rate = encoder->set_rate(0, 0);
        if (!shared_rate.ok()) log("shared video rate: " + shared_rate.to_string());
        applied = applied || encoder->bitrate() != previous_rate || encoder->framerate() != previous_fps;
        stream_quality = options.quality;
        stream_quality_percent = custom_quality ? options.quality_percent : 0;
        stream_fps_cap = options.max_fps;

        if (applied) {
            ++stats.stream_option_changes;
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(encoder->framerate());
            framerate = encoder->framerate();
            force_keyframe = force_keyframe || keyframe_required;
            stream_info =
                describe_stream(attached, *capturer, *encoder, stream_info.source_width,
                                stream_info.source_height, static_cast<uint16_t>(framerate),
                                session_->permissions(), audio_info,
                                settings_.allow_terminal_persist);
            publish_current(stream_info);
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            std::string quality_text = stream_quality_name(stream_quality);
            if (stream_quality == StreamQuality::Custom) {
                quality_text += " " + std::to_string(stream_quality_percent) + "%";
            }
            log("stream options: " + std::string(codec_name(encoder->codec())) + ", " +
                std::to_string(stream_info.bitrate / 1000u) + " kbps at " +
                std::to_string(framerate) + " fps, quality " + quality_text +
                (stream_fps_cap == 0
                     ? ""
                     : " (viewer cap " + std::to_string(stream_fps_cap) + " fps)"));
        }

        if (!answer(StreamOptionsCode::Ok, std::string())) {
            return false;
        }
        if (applied) {
            Error refreshed = refresh_video(stream_info);
            if (!refreshed.ok()) {
                if (refreshed.status() == Status::Unavailable ||
                    refreshed.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "refresh_stream: " + refreshed.to_string();
                }
                return false;
            }
            ++stats.stream_updates;
            publish_stats(stats);
        }
        return true;
    };

    // Network observations belong to this connection; only the host-wide QoS
    // policy changes display encoders. A fast viewer cannot overwrite a slow
    // viewer's limit, and a display switch keeps the same policy participant.
    auto last_link_sample = Clock::now();
    uint64_t last_sample_bytes = stats.bytes;
    uint64_t qos_replies = 0;
    uint64_t video_replies = 0;
    // RustDesk's TestDelay timer sends one probe per second and feeds the
    // policy when the reply arrives. The link loop runs at 500 ms for
    // statistics, but the quality controller's two-sample delay history and
    // 20-sample baseline only keep their real-time meaning at the reference
    // cadence.
    auto next_qos_sample = Clock::now() + std::chrono::seconds(1);
    auto next_qos_probe = next_qos_sample;
    auto sample_link = [&](Clock::time_point now) -> bool {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_link_sample).count();
        if (elapsed <= 0) return true;
        stats.send_kbps = static_cast<double>(stats.bytes - last_sample_bytes) * 8.0 / elapsed;
        last_sample_bytes = stats.bytes;
        last_link_sample = now;
        const auto transport = session_->transport_stats();
        if (transport.available) {
            stats.rtt_ms = static_cast<double>(transport.rtt_us) / 1000;
            stats.min_rtt_ms = static_cast<double>(transport.min_rtt_us) / 1000;
        }
        const auto latency = session_->latency();
        stats.application_rtt_ms = static_cast<double>(latency.rtt_us) / 1000;
        stats.qos_probe_replies = latency.replies;
        stats.video_delivery_ms = static_cast<double>(latency.video_delay_us) / 1000;
        stats.video_pending_ms = static_cast<double>(latency.video_pending_us) / 1000;
        stats.video_receipts = latency.video_replies;
        stats.video_pending_frames = latency.video_pending_frames;
        // Do not keep an old video delay alive on a static desktop. A fresh
        // receipt or outstanding frame participates alongside the probe.
        const auto video_delay = latency.video_replies != video_replies ? latency.video_delay_us : 0;
        const auto delay = std::max({latency.rtt_us, video_delay, latency.video_pending_us});
        const bool fresh = latency.replies != qos_replies || latency.video_replies != video_replies;
        const bool sample_due = now >= next_qos_sample;
        quality_peer->feedback(static_cast<uint32_t>(std::min<uint64_t>(delay / 1000, UINT32_MAX)),
                               std::max(latency.pending_us, latency.video_pending_us) / 1000,
                               fresh && sample_due);
        if (fresh && sample_due) {
            qos_replies = latency.replies;
            video_replies = latency.video_replies;
            next_qos_sample = now + std::chrono::seconds(1);
        }
        if (now >= next_qos_probe) {
            next_qos_probe = now + std::chrono::seconds(1);
            auto probed = session_->send_latency_probe();
            if (!probed.ok() && !session_->peer_alive()) {
                *peer_left = true;
                return false;
            }
        }
        if (!encoder) return true;
        auto rated = encoder->set_rate(0, 0); // synchronizes the shared decision
        if (!rated.ok()) log("shared video rate: " + rated.to_string());
        UINT rate = 0, fps = 0;
        if (!DesktopStreamPool::read_rate(*encoder, rate, fps)) { publish_stats(stats); return true; }
        if (rate == stats.target_bitrate && fps == stats.framerate) { publish_stats(stats); return true; }
        stats.target_bitrate = rate;
        stats.framerate = static_cast<uint16_t>(fps);
        if (!stats.lowest_target_bitrate || rate < stats.lowest_target_bitrate) stats.lowest_target_bitrate = rate;
        ++stats.abr_changes;
        framerate = fps;
        stream_info.bitrate = rate;
        stream_info.framerate = static_cast<uint16_t>(fps);
        publish_current(stream_info);
        auto refreshed = refresh_video(stream_info);
        if (!refreshed.ok()) {
            if (refreshed.status() == Status::Unavailable || refreshed.status() == Status::WaitTimeout)
                *peer_left = true;
            else *error = "refresh shared video rate: " + refreshed.to_string();
            return false;
        }
        ++stats.stream_updates;
        publish_stats(stats);
        return true;
    };

    // The viewer is told which desktop is on screen; the first report is sent
    // even when it is Default so a session that starts at the lock screen
    // shows its banner without waiting for a change.
    DesktopKind reported_desktop = static_cast<DesktopKind>(0xFF);
    bool desktop_paused = false;
    bool desktop_reinitialized = false;
    auto send_desktop_state = [&](DesktopKind kind) -> bool {
        if (kind == reported_desktop) {
            return true;
        }
        Error sent = session_->send_desktop_state(kind);
        if (!sent.ok()) {
            if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "send_desktop_state: " + sent.to_string();
            }
            return false;
        }
        reported_desktop = kind;
        return true;
    };
    // Follows the input desktop: a lock screen or UAC prompt moves input to a
    // secure desktop, where the capturer has to be built again. When this
    // process may not open that desktop (a tray host), capture pauses instead
    // of ending the session and the same check resumes it after unlock.
    auto sync_desktop = [&](bool force_report) -> bool {
        if (force_report) {
            reported_desktop = static_cast<DesktopKind>(0xFF);
        }
        const InputDesktopStatus status = config_.desktop_status();
        const DesktopKind kind =
            status.kind == InputDesktopKind::Default
                ? DesktopKind::Default
                : status.kind == InputDesktopKind::Secure ? DesktopKind::Secure
                                                          : DesktopKind::Unavailable;
        if (!send_desktop_state(kind)) {
            return false;
        }
        std::string follow_error;
        bool switched = false;
        if (!config_.follow_desktop(&follow_error, &switched)) {
            if (!desktop_paused) {
                desktop_paused = true;
                log("capture paused: cannot follow the input desktop: " + follow_error);
            }
            return true;
        }
        desktop_reinitialized = false;
        if (!switched && !desktop_paused) {
            return true;
        }
        if (switched) {
            log("the input desktop is now " +
                (status.name.empty() ? std::string("not Default") : "\"" + status.name + "\""));
        }
        for (auto& [id, entry] : other_videos) {
            auto rebuilt = entry.capture->reinitialize();
            if (!rebuilt.ok()) {
                if (!multiple_streams) { desktop_paused = true; return true; }
                entry.recovery_at = Clock::now() + std::chrono::seconds(1);
            } else entry.recovery_at = {};
            entry.encoder.reset(); entry.cached_nv12.reset();
            entry.have_input = false; entry.force_keyframe = true;
            if (entry.cursor) entry.cursor->reset();
        }
        if (!capturer) { desktop_paused = false; return true; }
        Error rebuilt = capturer->reinitialize();
        if (!rebuilt.ok()) {
            if (multiple_streams) {
                encoder.reset(); cached_nv12.reset(); have_input = false;
                video.recovery_at = Clock::now() + std::chrono::seconds(1);
                desktop_paused = false;
                desktop_reinitialized = true;
                return true;
            }
            if (!desktop_paused) {
                log("capture paused: cannot capture the current desktop yet: " +
                    rebuilt.to_string());
            }
            desktop_paused = true;
            return true;
        }
        if (desktop_paused) {
            log("capture resumed");
        }
        desktop_paused = false;
        desktop_reinitialized = true;
        video.recovery_at = {};
        if (video.cursor) video.cursor->reset();
        encoder.reset();
        have_input = false;
        cached_nv12.reset();
        force_keyframe = true;
        return true;
    };
    if (!sync_desktop(true)) {
        const bool clean = error->empty();
        drain_peer(stats);
        return clean;
    }
    auto next_desktop_check = Clock::now() + std::chrono::seconds(1);

    // The frame captured before the handshake is the first one on the wire.
    if (!desktop_paused && !desktop_reinitialized) {
        ++stats.frames;
        if (initial_pointer_only) {
            ++stats.pointer_only;
        }
        if (!process_frame(0)) {
            // A vanished viewer is a normal end, not a failure.
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        publish_cursor();
    }
    // Lives exactly as long as the session: its sockets close on every exit
    // path, before the caller resets the transport for the next viewer.
    PortForwardRelay port_forward(*session_);
    // Same lifetime rule: its destructor terminates every shell still running.
    TerminalRelay terminal(*session_, settings_.allow_terminal_persist,
                           [this] { touch_viewer_activity(); });
    // The camera opens only when the viewer asks, and only if the host allows
    // cameras; it encodes on the capture adapter with its own device so it
    // never shares the capturer's immediate context.
    const auto camera_adapter = capturer->adapter_device().luid;
    CameraRelay camera(
        *session_, [&capturer, camera_adapter] { return capturer ? capturer->adapter_device().luid : camera_adapter; },
        CameraRelayConfig{settings_.codec, settings_.quality, config_.encoder_backend,
                          settings_.framerate > 0 ? settings_.framerate : 30u,
                          config_.cameras, config_.camera_capture_factory},
        [&, peer = codec_peer, decoders = session_->decoding_codecs()](
                const std::wstring& key, CameraRelay::CaptureFactory capture, std::string* failure) {
            return CameraRelay::Subscription{{}, streams.subscribe(key, std::move(capture),
                config_.encoder_factory, failure, {}, decoders, peer,
                {settings_.record_incoming, settings_.record_directory, config_.log})};
        });

    auto pump_video = [&]() -> bool {
        if (!capturer) return true;
        if (video.recovery_at != Clock::time_point{}) {
            if (Clock::now() < video.recovery_at) return true;
            if (!video.encoder_recovery_only) {
                auto result = capturer->reinitialize();
                if (!result.ok()) { video.recovery_at = Clock::now() + std::chrono::seconds(1); return true; }
            }
            video.encoder_recovery_only = false;
            video.recovery_at = {};
            encoder.reset(); cached_nv12.reset(); have_input = false; force_keyframe = true;
            if (video.cursor) video.cursor->reset(); frames_without_nv12 = 0;
        }
        publish_cursor();
        if (config_.keyframe_interval_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last_keyframe).count() >=
                static_cast<long long>(config_.keyframe_interval_ms)) force_keyframe = true;
        if (multiple_streams && encoder) {
            UINT rate = 0, fps = 0;
            if (DesktopStreamPool::read_rate(*encoder, rate, fps) &&
                (rate != stream_info.bitrate || fps != stream_info.framerate)) {
                stream_info.bitrate = rate;
                stream_info.framerate = static_cast<uint16_t>(fps);
                framerate = fps; stats.target_bitrate = rate;
                auto sent = refresh_video(stream_info);
                if (!sent.ok()) { *error = sent.to_string(); return false; }
            }
        }
        if (!session_->video_send_ready()) {
            video_needs_resync = true;
            return true;
        }
        if (video_needs_resync) {
            DesktopStreamPool::discard_backlog(*capturer);
            force_keyframe = true;
            video_needs_resync = false;
        }

        // The display service already paces production. A second frame-rate
        // sleep here would age its queued frames and delay control/QoS when
        // weak links lower FPS. Readers wait briefly only when no frame exists.
        const bool audio_active = audio_capture != nullptr;
        if (force_keyframe) DesktopStreamPool::request_keyframe(*capturer);
        const CaptureResult result = capturer->capture(
            std::chrono::milliseconds(other_videos.empty() ? (audio_active ? 5 : 100) : 0));
        // The endpoint keeps producing while capture waits for the next
        // present; drain here so audio does not wait behind a slow frame.
        drain_audio();
        drain_microphone(stats);
        handle_voice_call_requests(stats);
        drain_voice_call(stats);
        switch (result) {
            case CaptureResult::NoNewFrame:
                // A failed conversion leaves no NV12 behind. If the desktop
                // stays quiet, no Frame event will advance the counter.
                if (frames_without_nv12 > 0 && ++frames_without_nv12 >= 3) {
                    *error = "capture produced no NV12 image for 3 frames";
                    const Error& capture_error = capturer->last_error();
                    if (!capture_error.ok()) {
                        *error += ": " + capture_error.to_string();
                    }
                    drain_peer(stats);
                    return false;
                }
                if (force_keyframe && have_input && encoder && cached_nv12) {
                    const auto repeated_at = Clock::now();
                    const uint64_t pts_ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            repeated_at - start).count());
                    if (!process_frame(pts_ms, true)) {
                        drain_peer(stats);
                        return error->empty();
                    }
                }
                return true;
            case CaptureResult::ReinitNeeded: {
                log("reinitializing: " + capturer->last_error().to_string());
                // The common cause is a desktop switch: the duplication was
                // attached to the old desktop. Follow first, then rebuild.
                if (!sync_desktop(false)) {
                    const bool clean = error->empty();
                    drain_peer(stats);
                    return clean;
                }
                if (desktop_paused || desktop_reinitialized) {
                    return true;
                }
                Error err = capturer->reinitialize();
                if (!err.ok()) {
                    if (multiple_streams) {
                        log("display capture recovery pending: " + err.to_string());
                        encoder.reset(); cached_nv12.reset(); have_input = false;
                        video.recovery_at = Clock::now() + std::chrono::seconds(1);
                        return true;
                    }
                    *error = "reinit failed: " + err.to_string();
                    drain_peer(stats);
                    return false;
                }
                encoder.reset();
                have_input = false;
                cached_nv12.reset();
                force_keyframe = true;
                // The rebuilt duplication has no frame yet; a quiet desktop is
                // not the same as a broken NV12 conversion.
                frames_without_nv12 = 0;
                return true;
            }
            case CaptureResult::FatalError: {
                const std::string failure = capturer->last_error().to_string();
                log("capture failed: " + failure);
                if (!sync_desktop(false)) {
                    const bool clean = error->empty();
                    drain_peer(stats);
                    return clean;
                }
                if (desktop_paused || desktop_reinitialized) {
                    return true;
                }
                if (multiple_streams) {
                    encoder.reset(); cached_nv12.reset(); have_input = false;
                    video.recovery_at = Clock::now() + std::chrono::seconds(1);
                    return true;
                }
                *error = "capture failed: " + failure;
                drain_peer(stats);
                return false;
            }
            case CaptureResult::Frame:
                break;
        }

        ++stats.frames;
        const FrameInfo frame = capturer->frame();
        if (!frame.desktop_updated) {
            ++stats.pointer_only;
        }
        if (!encoder) {
            // The capture rebuilt: the output may have moved, rotated or
            // changed size, and the encoder may land on a different codec.
            encoder_config.width = frame.nv12_width;
            encoder_config.height = frame.nv12_height;
            std::string encoder_error;
            encoder = DesktopStreamPool::encoder(*capturer, encoder_config, &encoder_error);
            if (!encoder) {
                video.encoder_recovery_only = DesktopStreamPool::encoder_recovery_pending(*capturer);
                if (multiple_streams || video.encoder_recovery_only) {
                    video.recovery_at = Clock::now() + std::chrono::seconds(1);
                    log("display encoder recovery pending: " + encoder_error);
                    return true;
                }
                *error = "encoder recreation failed: " + encoder_error;
                drain_peer(stats);
                return false;
            }
            if (encoder->codec() != encoder_config.codec) {
                log(std::string("the encoder fell back: requested ") +
                    codec_name(encoder_config.codec) + ", producing " +
                    codec_name(encoder->codec()));
            }
            // The service retained all viewer votes while capture rebuilt.
            DesktopStreamPool::reserve_audio(*capturer, bitrate_reserve(audio_info));
            stats.resolution_percent = static_cast<uint16_t>(capturer->nv12_scale_percent());
            framerate = encoder->framerate();
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(framerate);
            const std::vector<DisplayInfo> refreshed = config_.displays();
            // The output may have moved or been renumbered while capture was
            // down; StreamInfo and the pointer origin must describe the list
            // the viewer is about to receive.
            if (!multiple_streams) attached = refreshed;
            stream_info =
                describe_stream(attached, *capturer, *encoder, frame.width, frame.height,
                                static_cast<uint16_t>(framerate), session_->permissions(),
                                audio_info, settings_.allow_terminal_persist);
            publish_current(stream_info);
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            Error refreshed_err = refresh_video(stream_info);
            if (!refreshed_err.ok()) {
                if (refreshed_err.status() == Status::Unavailable ||
                    refreshed_err.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    *error = "refresh_stream: " + refreshed_err.to_string();
                }
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
            ++stats.stream_updates;
            log("stream changed to " + std::to_string(stream_info.width) + "x" +
                std::to_string(stream_info.height) + " at " +
                std::to_string(stream_info.origin_x) + "," +
                std::to_string(stream_info.origin_y) + "; re-announced");
            force_keyframe = true;
        }

        // Adaptive resolution: the capture now publishes a smaller NV12 image
        // than the encoder was built for. VPL fixes the coded size at session
        // creation, so a size change is a new encoder -- built with the rate
        // and frame rate the controller currently holds -- not a reset.
        if (frame.has_video_image() &&
            (frame.nv12_width != encoder->width() || frame.nv12_height != encoder->height() ||
             DesktopStreamPool::codec(*capturer) != encoder->codec() ||
             DesktopStreamPool::chroma(*capturer) != encoder->chroma())) {
            EncoderConfig resized = encoder_config;
            resized.width = frame.nv12_width;
            resized.height = frame.nv12_height;
            resized.bitrate = stats.target_bitrate;
            resized.framerate = framerate;
            std::string resize_error;
            std::unique_ptr<IVideoEncoder> next =
                DesktopStreamPool::encoder(*capturer, resized, &resize_error);
            if (!next) {
                if (multiple_streams) {
                    encoder.reset(); cached_nv12.reset(); have_input = false;
                    video.recovery_at = Clock::now() + std::chrono::seconds(1);
                    log("display encoder recovery pending: " + resize_error);
                    return true;
                }
                // Keep the stream that is running: put the capture back on the
                // size the current encoder was built for and stop resizing.
                resolution_fixed = true;
                Error reverted = capturer->set_nv12_scale(stats.resolution_percent);
                if (!reverted.ok()) {
                    log("adaptive resolution: revert failed: " + reverted.to_string());
                }
                resolution_request = stats.resolution_percent;
                log("adaptive resolution: encoder recreation at " +
                    std::to_string(resized.width) + "x" +
                    std::to_string(resized.height) + " failed; staying at " +
                    std::to_string(stats.resolution_percent) + "%: " + resize_error);
                stream_info.bitrate = stats.target_bitrate;
                stream_info.framerate = static_cast<uint16_t>(framerate);
                publish_current(stream_info);
                Error refreshed = refresh_video(stream_info);
                if (!refreshed.ok()) {
                    if (refreshed.status() == Status::Unavailable ||
                        refreshed.status() == Status::WaitTimeout) {
                        *peer_left = true;
                    } else {
                        ++stats.send_errors;
                        *error = "refresh_stream: " + refreshed.to_string();
                    }
                    const bool clean = error->empty();
                    drain_peer(stats);
                    return clean;
                }
                ++stats.stream_updates;
                publish_stats(stats);
                // This frame still holds the refused size and cannot feed the
                // old encoder; the reverted pass produces a matching one next.
                return true;
            }
            encoder = std::move(next);
            have_input = false;
            cached_nv12.reset();
            force_keyframe = true;
            extra_idrs = 1;
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(framerate);
            stream_info =
                describe_stream(attached, *capturer, *encoder, frame.width, frame.height,
                                static_cast<uint16_t>(framerate), session_->permissions(),
                                audio_info, settings_.allow_terminal_persist);
            publish_current(stream_info);
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            Error resized_err = refresh_video(stream_info);
            if (!resized_err.ok()) {
                if (resized_err.status() == Status::Unavailable ||
                    resized_err.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "refresh_stream: " + resized_err.to_string();
                }
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
            ++stats.stream_updates;
            log("stream resized to " + std::to_string(stream_info.width) + "x" +
                std::to_string(stream_info.height) + " (" +
                std::to_string(stats.resolution_percent) + "% of " +
                std::to_string(stream_info.source_width) + "x" +
                std::to_string(stream_info.source_height) + "); re-announced");
            publish_stats(stats);
        }
        const uint64_t pts_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        if (!process_frame(pts_ms)) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        publish_cursor();
        return true;
    };

    const Clock::duration idle_timeout = idle_disconnect_timeout();
    // Host screenshots (v72). Readback runs on the display service thread;
    // PNG encoding runs on a helper while this loop continues processing
    // controls. Bounded slices use a separate unidirectional stream. A slice
    // refused by the send budget is retried on a later pass.
    struct PendingScreenshot {
        ScreenshotResponse response;
        std::vector<uint8_t> png;
        size_t offset = 0;
        std::future<std::pair<ScreenshotResponse, std::vector<uint8_t>>> encoding;
    };
    std::unique_ptr<PendingScreenshot> screenshot;
    bool screenshot_pending = false;
    auto pump_screenshot = [&] {
        if (!screenshot) {
            ScreenshotRequest request;
            if (!session_->take_screenshot_request(request)) {
                return;
            }
            touch_viewer_activity();
            screenshot = std::make_unique<PendingScreenshot>();
            ScreenshotResponse& answer = screenshot->response;
            answer.request_id = request.request_id;
            answer.stream_id = request.stream_id;
            IDxgiCapturer* source = nullptr;
            if (request.stream_id == kScreenshotCurrentStream || (capturer && request.stream_id == video.id)) {
                source = capturer.get();
                answer.stream_id = video.id;
            } else {
                const auto found = other_videos.find(request.stream_id);
                if (found != other_videos.end()) source = found->second.capture.get();
            }
            if (!source) {
                answer.code = ScreenshotCode::Denied;
                answer.reason = "that display is not being streamed to this viewer";
            } else {
                auto raw = DesktopStreamPool::screenshot_async(*source);
                screenshot->encoding = std::async(std::launch::async,
                    [answer, raw = std::move(raw)]() mutable {
                        std::vector<uint8_t> png;
                        try {
                            auto pixels = raw.get();
                            std::string failure;
                            if (pixels.bgra.empty()) {
                                answer.code = ScreenshotCode::Unavailable;
                                answer.reason = std::move(pixels.error);
                            } else if (!encode_png_bgra(pixels.bgra.data(), pixels.width, pixels.height,
                                        pixels.width * 4, png, &failure)) {
                                answer.code = ScreenshotCode::Failed;
                                answer.reason = std::move(failure);
                            } else if (png.size() > kMaxScreenshotBytes) {
                                answer.code = ScreenshotCode::TooLarge;
                                answer.reason = "the desktop picture is too large to send";
                                png.clear();
                            } else {
                                answer.width = pixels.width; answer.height = pixels.height;
                                answer.total = static_cast<uint32_t>(png.size());
                                answer.count = (answer.total + kScreenshotChunkBytes - 1) / kScreenshotChunkBytes;
                            }
                        } catch (const std::exception& exception) {
                            answer.code = ScreenshotCode::Failed;
                            answer.reason = exception.what();
                        }
                        return std::make_pair(std::move(answer), std::move(png));
                    });
            }
        }
        PendingScreenshot& pending = *screenshot;
        if (pending.encoding.valid()) {
            if (pending.encoding.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                screenshot_pending = true;
                return;
            }
            auto result = pending.encoding.get();
            pending.response = std::move(result.first);
            pending.png = std::move(result.second);
        }
        ScreenshotResponse slice = pending.response;
        for (;;) {
            if (pending.response.code != ScreenshotCode::Ok) {
                const Error sent = session_->send_screenshot_response(pending.response);
                if (sent.status() == Status::WaitTimeout) {
                    screenshot_pending = true;
                    return;
                }
                screenshot.reset();
                screenshot_pending = false;
                return;
            }
            if (pending.offset >= pending.png.size()) {
                screenshot.reset();
                screenshot_pending = false;
                return;
            }
            const size_t bytes = std::min<size_t>(kScreenshotChunkBytes,
                                                  pending.png.size() - pending.offset);
            slice.index = static_cast<uint32_t>(pending.offset / kScreenshotChunkBytes);
            slice.data.assign(pending.png.begin() + static_cast<ptrdiff_t>(pending.offset),
                              pending.png.begin() +
                                  static_cast<ptrdiff_t>(pending.offset + bytes));
            const Error sent = session_->send_screenshot_response(slice);
            if (sent.status() == Status::WaitTimeout) {
                screenshot_pending = true;
                return;
            }
            if (!sent.ok()) {
                screenshot.reset();
                screenshot_pending = false;
                return;
            }
            pending.offset += bytes;
        }
    };
    while (!stopping_.load() && !disconnect_requested_.load()) {
        const auto now = Clock::now();
        if (!apply_ticket_renewals(error)) return false;
        if (ticket_lease_expired(now)) {
            drain_peer(stats);
            return error->empty();
        }
        if (!handle_elevation_requests(error)) return false;
        if (now >= next_desktop_check) {
            next_desktop_check = now + std::chrono::seconds(1);
            if (!sync_desktop(false)) {
                drain_peer(stats);
                return error->empty();
            }
        }
        if (config_.run_seconds > 0.0 &&
            std::chrono::duration<double>(now - start).count() >= config_.run_seconds) {
            break;
        }
        // A viewer that closed its side ends the session here, like the shared
        // and file loops. Waiting for a failed send is not enough: a static
        // desktop produces no frames to send, and the session would outlive
        // the viewer -- along with a blocked local input.
        if (!session_->peer_alive()) {
            *peer_left = true;
            break;
        }
        if (session_->viewer_finished()) {
            break;
        }
        // Frames, audio and local input keep flowing on their own; only an
        // explicit viewer request proves somebody is still watching.
        if (idle_timeout > Clock::duration::zero() &&
            now - last_viewer_activity_ >= idle_timeout) {
            ++stats.idle_disconnects;
            log("idle: no viewer activity for " + std::to_string(idle_disconnect_seconds()) +
                " s; closing the session");
            drain_peer(stats);
            return error->empty();
        }
        if (!refresh_topology(false)) { drain_peer(stats); return error->empty(); }
        CaptureDisplaysRequest display_subscriptions;
        if (session_->take_capture_displays_request(display_subscriptions)) {
            touch_viewer_activity();
            if (!capture_displays(display_subscriptions)) { drain_peer(stats); return error->empty(); }
        }
        pump_screenshot();
        CursorShapeRequest cursor_shape_request;
        if (session_->take_cursor_shape_request(cursor_shape_request)) {
            touch_viewer_activity();
            std::shared_ptr<CursorPublisher> publisher;
            if (cursor_shape_request.stream_id == kCursorCurrentStream ||
                cursor_shape_request.stream_id == video.id) {
                publisher = video.cursor;
            } else {
                const auto found = other_videos.find(cursor_shape_request.stream_id);
                if (found != other_videos.end()) publisher = found->second.cursor;
            }
            if (publisher) publisher->resend_shape(cursor_shape_request.shape_id);
        }
        ShowCursorHint cursor_hint;
        if (session_->take_show_cursor_hint(cursor_hint)) {
            touch_viewer_activity();
            if (!cursor_hint.visible) {
                if (cursor_overlay) {
                    cursor_overlay->hide();
                }
            } else {
                if (!cursor_overlay && !cursor_overlay_failed) {
                    std::string overlay_error;
                    cursor_overlay =
                        CursorHintOverlay::create(&overlay_error, pending_security_.device_name);
                    if (!cursor_overlay) {
                        cursor_overlay_failed = true;
                        log("show-cursor hint is unavailable: " + overlay_error);
                    }
                }
                if (cursor_overlay) {
                    std::string overlay_error;
                    if (!cursor_overlay->move(cursor_hint.x, cursor_hint.y, cursor_hint.pressed,
                                              &overlay_error)) {
                        cursor_overlay.reset();
                        cursor_overlay_failed = true;
                        log("show-cursor hint failed: " + overlay_error);
                    }
                }
            }
        }
        uint16_t switch_index = kDisplayAuto;
        if (session_->take_display_switch_request(switch_index)) {
            touch_viewer_activity();
            if (!switch_display(switch_index)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        uint16_t modes_index = 0;
        if (session_->take_list_display_modes_request(modes_index)) {
            touch_viewer_activity();
            if (!send_mode_list(modes_index)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        uint16_t mode_index = 0;
        uint16_t mode_width = 0;
        uint16_t mode_height = 0;
        uint16_t mode_refresh = 0;
        if (session_->take_set_display_mode_request(mode_index, mode_width, mode_height,
                                                    mode_refresh)) {
            touch_viewer_activity();
            // Apply the mode to the requested subscription while retaining the
            // viewer's focused display, including when permission is refused.
            const auto fresh = config_.displays();
            auto mode_target = other_videos.end();
            if (mode_index < fresh.size()) {
                mode_target = std::find_if(other_videos.begin(), other_videos.end(), [&](const auto& entry) {
                    return entry.second.capture->display().device_name == fresh[mode_index].device_name;
                });
            }
            if (mode_target != other_videos.end()) { swap_video(mode_target->second); pumping_extra = true; }
            const bool changed = change_display_mode(mode_index, mode_width, mode_height, mode_refresh);
            if (mode_target != other_videos.end()) { swap_video(mode_target->second); pumping_extra = false; }
            if (!changed) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        uint8_t follow_request = 0;
        if (session_->take_display_follow_request(follow_request)) {
            touch_viewer_activity();
            uint8_t applied = follow_request & kDisplayFollowAll;
            DisplayFollowCode code = DisplayFollowCode::Ok;
            std::string reason;
            if ((session_->permissions() & kPermissionInput) == 0) {
                applied = 0;
                code = DisplayFollowCode::Failed;
                reason = "input control is disabled";
            } else if (applied != 0 && capturer && capturer->merged()) {
                applied = 0;
                code = DisplayFollowCode::Unsupported;
                reason = "the merged view already shows every display";
            }
            follow_flags = applied;
            follow_candidate = kDisplayAuto;
            if (!send_follow_state(follow_flags, code, reason)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
            if (code != DisplayFollowCode::Ok) {
                log("display follow refused: " + reason);
            }
        }
        // Input permission can be revoked while following is on; the viewer
        // must not keep toggles that can no longer move the host's picture.
        if (follow_flags != 0 && (session_->permissions() & kPermissionInput) == 0) {
            follow_flags = 0;
            follow_candidate = kDisplayAuto;
            if (!send_follow_state(0, DisplayFollowCode::Failed,
                                   "input control was disabled")) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        if (follow_flags != 0 && capturer && other_videos.empty() && now - follow_polled_at >= kFollowPollInterval) {
            follow_polled_at = now;
            const int streamed = capturer->merged()
                                     ? -1
                                     : static_cast<int>(
                                           find_display(attached, capturer->display()));
            int target = -1;
            if ((follow_flags & kDisplayFollowWindow) != 0) {
                target = foreground_display_index(attached);
            }
            if (target < 0 && (follow_flags & kDisplayFollowCursor) != 0) {
                POINT point{};
                if (GetCursorPos(&point)) {
                    target = display_index_at(attached, point);
                }
            }
            if (target < 0 || target == streamed) {
                follow_candidate = kDisplayAuto;
                follow_candidate_since = Clock::time_point{};
            } else if (follow_candidate != static_cast<uint16_t>(target)) {
                follow_candidate = static_cast<uint16_t>(target);
                follow_candidate_since = now;
            } else if (now - follow_candidate_since >= kFollowSettleTime &&
                       now - follow_switch_at >= std::chrono::seconds(1) &&
                       (!switched_once ||
                        now - last_switch_at >= std::chrono::seconds(1))) {
                follow_switch_at = now;
                if (!switch_display(static_cast<uint16_t>(target))) {
                    const bool clean = error->empty();
                    drain_peer(stats);
                    return clean;
                }
                follow_candidate = kDisplayAuto;
                follow_candidate_since = Clock::time_point{};
            }
        }
        StreamOptions options;
        if (session_->take_stream_options_request(options)) {
            touch_viewer_activity();
            if (!apply_stream_options(options)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        if (!apply_pending_permissions()) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        DisplayControlRequest display_request;
        if (session_->take_display_control_request(display_request)) {
            touch_viewer_activity();
            if (!control_displays(display_request)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        // A revoked permission or a layout another application changed must
        // return this host's original topology before the session keeps using
        // it. The independent guardian retries when the restore fails here.
        if (managed_displays.backend) {
            std::string recovery_reason;
            const PermissionMask management_need = kPermissionInput | kPermissionPrivacyMode;
            const bool permitted =
                (session_->permissions() & management_need) == management_need &&
                (!registry_ || registry_->may_change_host_state(this));
            if (!permitted || !managed_displays.backend->intact(&recovery_reason)) {
                if (!restore_managed_displays(&recovery_reason)) {
                    *error = recovery_reason;
                    drain_peer(stats);
                    return false;
                }
                if (!send_display_control(
                        permitted ? DisplayControlCode::Failed
                                  : DisplayControlCode::Denied,
                        permitted
                            ? "display management interrupted; original layout restored"
                            : "display permission revoked; original layout restored")) {
                    const bool clean = error->empty();
                    drain_peer(stats);
                    return clean;
                }
            }
        }
        bool privacy_requested = false;
        if (session_->take_privacy_mode_request(privacy_requested)) {
            touch_viewer_activity();
            if (!set_privacy_mode(privacy_requested)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        if (virtual_privacy && privacy_active) {
            std::string integrity_error;
            if (!virtual_privacy->intact(&integrity_error)) {
                log("virtual display privacy interrupted: " + integrity_error);
                // A monitor hotplug or a dead guardian invalidates privacy.
                // Restore immediately and terminate instead of transmitting
                // a desktop that may now be visible on a physical panel.
                virtual_privacy->disable(error);
                send_privacy_state(false, PrivacyModeCode::Failed, integrity_error);
                *error = integrity_error;
                return false;
            }
        }
        while (session_->take_sas_request()) {
            touch_viewer_activity();
            if (!send_sas()) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        if (!handle_host_actions(stats, error, peer_left)) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        if (!handle_os_login_requests(stats, error, peer_left)) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        if (!handle_session_requests(stats, error, peer_left) || stopping_.load()) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        if (!handle_remote_config_requests(stats, error, peer_left)) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        if (!drain_chat(stats, error)) {
            const bool clean = error->empty();
            drain_peer(stats);
            return clean;
        }
        std::string injected_text;
        while (session_->take_text_input(injected_text)) {
            touch_viewer_activity();
            ++stats.text_received;
            log("text input: received " + std::to_string(injected_text.size()) +
                " bytes");
            if (config_.text_input) {
                config_.text_input(injected_text);
            }
        }
        drain_audio();
        drain_microphone(stats);
        handle_voice_call_requests(stats);
        drain_voice_call(stats);
        PortForwardRequest forward_request;
        while (session_->take_port_forward_request(forward_request)) {
            touch_viewer_activity();
            // The transport already checked the session grant; reaching here
            // means the operator enabled port forwarding for this host.
            const Error answered = session_->send_port_forward_result(
                forward_request.forward_id, PortForwardCode::Ok);
            if (!answered.ok()) {
                log("port forward: cannot approve target " + forward_request.host + ":" +
                    std::to_string(forward_request.port) + ": " + answered.to_string());
                break;
            }
            log("port forward: approved " + forward_request.host + ":" +
                std::to_string(forward_request.port));
        }
        PortForwardStop forward_stop;
        while (session_->take_port_forward_stop(forward_stop)) {
            touch_viewer_activity();
            log("port forward: stopped with code " +
                std::to_string(static_cast<int>(forward_stop.code)) +
                (forward_stop.reason.empty() ? std::string() : ": " + forward_stop.reason));
        }
        for (const std::string& event : port_forward.pump()) {
            log(event);
        }
        for (const std::string& event : terminal.pump()) {
            log(event);
        }
        for (const std::string& event : camera.pump()) {
            log(event);
        }
        drain_peer(stats);
        poll_clipboard(now, stats);
        poll_files(stats);
        if (now - last_link_sample >= std::chrono::milliseconds(500)) {
            if (!sample_link(now)) {
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
        }
        if (session_->take_keyframe_request()) {
            force_keyframe = true;
            for (auto& [id, entry] : other_videos) entry.force_keyframe = true;
        }
        uint32_t requested_stream = 0;
        while (session_->take_display_keyframe_request(requested_stream)) {
            if (requested_stream == video.id && capturer) force_keyframe = true;
            const auto found = other_videos.find(requested_stream);
            if (found != other_videos.end()) found->second.force_keyframe = true;
        }
        if (config_.keyframe_interval_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keyframe).count() >=
                static_cast<long long>(config_.keyframe_interval_ms)) {
            force_keyframe = true;
        }
        if (config_.reannounce && !reannounced && now >= next_reannounce) {
            reannounced = true;
            Error refreshed = refresh_video(stream_info);
            if (!refreshed.ok()) {
                if (refreshed.status() == Status::Unavailable ||
                    refreshed.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "refresh_stream: " + refreshed.to_string();
                }
                const bool clean = error->empty();
                drain_peer(stats);
                return clean;
            }
            ++stats.stream_updates;
            log("re-announced the stream description");
        }
        if (capturer && probe_pending && now - start >= std::chrono::milliseconds(2500)) {
            probe_pending = false;
            if (!resolution_fixed &&
                config_.resolution_probe_percent != resolution_request) {
                Error scaled = capturer->set_nv12_scale(config_.resolution_probe_percent);
                if (!scaled.ok()) {
                    resolution_fixed = true;
                    log("resolution probe: cannot scale to " +
                        std::to_string(config_.resolution_probe_percent) + "%: " +
                        scaled.to_string());
                } else {
                    resolution_request = config_.resolution_probe_percent;
                    stats.resolution_percent =
                        static_cast<uint16_t>(capturer->nv12_scale_percent());
                    ++stats.resolution_changes;
                    force_keyframe = true;
                    extra_idrs = 1;
                    log("resolution probe: " + std::to_string(stats.resolution_percent) + "%");
                }
            }
        }

        if (desktop_paused) {
            // The desktop that receives input cannot be captured from here
            // (the tray at the lock screen). Keep the session, input and audio
            // alive and retry from the next desktop check instead of asking a
            // capturer that has no desktop to duplicate.
            sleep_unless_stopping(std::chrono::milliseconds(200));
            continue;
        }

        if (!capturer) {
            sleep_unless_stopping(std::chrono::milliseconds(5));
            continue;
        }
        // Rotate which display goes first when the connection window is tight.
        // All captures/encoders still run on their own service threads.
        std::vector<uint32_t> order{video.id};
        for (const auto& [id, entry] : other_videos) order.push_back(id);
        std::rotate(order.begin(), order.begin() + (video_round++ % order.size()), order.end());
        for (auto id : order) {
            auto found = other_videos.find(id);
            if (found != other_videos.end()) { swap_video(found->second); pumping_extra = true; }
            const bool ok = pump_video();
            if (found != other_videos.end()) { swap_video(found->second); pumping_extra = false; }
            if (!ok || *peer_left) { drain_peer(stats); return error->empty(); }
        }
        if (!other_videos.empty() || screenshot_pending || !session_->video_send_ready())
            sleep_unless_stopping(std::chrono::milliseconds(1));
    }

    end_file_session();
    drain_peer(stats);
    return true;
}

// ----- file transfer shared by both session kinds -----

void HostServer::begin_file_session() {
    last_peer_clipboard_seq_ = 0;
    last_file_source_version_ = 0;
    // Pending operator work was cleared before this connection ID was
    // published. Keep requests made while the initial media is opening.
    file_receive_directory_ = settings_.file_receive_directory.empty()
                                  ? default_receive_directory()
                                  : settings_.file_receive_directory;
    file_shares_.clear();
    if (!settings_.file_share_roots.empty()) {
        std::vector<std::string> configured;
        if (!file_receive_directory_.empty()) {
            configured.push_back(file_receive_directory_);
        }
        configured.insert(configured.end(), settings_.file_share_roots.begin(),
                          settings_.file_share_roots.end());
        for (const FileShareRoot& share : make_file_share_roots(configured)) {
            const DWORD attributes =
                GetFileAttributesW(path_from_utf8(share.path).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                log("file: share '" + share.name +
                    "' is not an accessible folder; skipping it");
                continue;
            }
            file_shares_.push_back(share);
        }
        if (!file_shares_.empty()) {
            std::string names;
            for (const FileShareRoot& share : file_shares_) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += share.name;
            }
            log("file: sharing " + std::to_string(file_shares_.size()) +
                " folders: " + names);
        }
    }
    file_audit_ref_ = session_audit_ref_;
    file_audit_ref_absent_ = session_audit_ref_absent_;
    upload_.reset();
    held_upload_.reset();
    clipboard_incoming_ = ClipboardIncoming{};
    last_file_identity_ = 0;
    last_file_entries_.clear();
    last_file_paths_.clear();
    {
        // A clipboard download path belongs to the session that announced
        // it; a fresh viewer must not reach files an earlier one copied.
        clear_clipboard_file_share();
    }
    download_stream_.close();
    download_tokens_ = kDownloadBurst;
    download_token_time_ = Clock::now();
}

void HostServer::end_file_session() {
    // A read started by this viewer must never be published to the next one.
    cancel_clipboard_work();
    clipboard_was_allowed_ = false;
    {
        // Nothing may apply this mask after the session it belonged to ended;
        // the next connection starts from its own approved mask.
        std::lock_guard<std::mutex> lock(permission_mutex_);
        pending_permissions_.store(0);
        permission_update_pending_.store(false);
    }
    if (upload_) {
        log("file: " + upload_->receiver->name() +
            " is incomplete; its .part file is kept for a resume");
        upload_->receiver->abort(true);
        upload_.reset();
    }
    // The held offer belonged to this viewer; its decision can no longer come.
    held_upload_.reset();
    if (send_hasher_.joinable()) {
        send_hasher_.join();
    }
    bool marked = false;
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        // Operator and viewer requests both belong to this connection. Never
        // offer a queued local file to the next sequential viewer.
        pending_download_path_.clear();
        pending_download_token_.reset();
        pending_download_transfer_id_ = 0;
        send_.file_access_token.reset();
        switch (send_.stage) {
            case FileSend::Stage::kHashing:
            case FileSend::Stage::kReady:
            case FileSend::Stage::kOffered:
            case FileSend::Stage::kSending:
            case FileSend::Stage::kFinishing:
                send_.stage = FileSend::Stage::kFailed;
                send_.error = "the session ended";
                send_.counted = true;
                marked = true;
                break;
            default:
                break;
        }
    }
    download_stream_.close();
    if (marked) {
        publish_download();
    }
    reset_clipboard_incoming(true);
    file_receive_directory_.clear();
    file_shares_.clear();
    file_audit_ref_.clear();
    file_audit_ref_absent_ = false;
    clear_clipboard_file_share();
}

bool HostServer::file_transfer_allowed() const {
    return settings_.allow_file_transfer &&
           (session_->permissions() & kPermissionFileTransfer) != 0;
}

bool HostServer::clipboard_transfer_allowed() const {
    return clipboard_allowed() && clipboard_to_host(settings_.clipboard_direction);
}

void HostServer::reset_clipboard_incoming(bool remove_directory) {
    if (clipboard_write_pending_ && clipboard_write_is_files_) cancel_clipboard_work();
    if (remove_directory && !clipboard_incoming_.directory.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(path_from_utf8(clipboard_incoming_.directory), ec);
    }
    clipboard_incoming_ = ClipboardIncoming{};
}

bool HostServer::begin_clipboard_incoming(const std::vector<ClipboardFileEntry>& entries,
                                          HostServerStats& stats, std::string* error) {
    if (entries.empty() || entries.size() > kMaxClipboardFileEntries) {
        *error = "the clipboard file list is empty or too long";
        return false;
    }
    if (upload_ && upload_->offer.clipboard) {
        // A regular upload can stay: the viewer's offers queue behind it in
        // the upload channel, so the list is simply recorded now.
        end_upload(FileCode::Cancelled, "the viewer replaced its clipboard list", true, true,
                   stats);
    }
    reset_clipboard_incoming(true);
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) {
        *error = "no temporary directory is available";
        return false;
    }
    std::filesystem::path root;
    bool created = false;
    for (int attempt = 0; attempt < 100 && !created; ++attempt) {
        root = base / ("chast-clipboard-" +
                       std::to_string(static_cast<unsigned long long>(std::time(nullptr))) +
                       "-" + std::to_string(++clipboard_stage_seq_) + "-in");
        created = std::filesystem::create_directories(root, ec);
        if (ec) {
            *error = "cannot create the clipboard staging folder: " + ec.message();
            return false;
        }
    }
    if (!created) {
        *error = "cannot create the clipboard staging folder";
        return false;
    }
    const std::u8string u8root = root.u8string();
    clipboard_incoming_.directory.assign(u8root.begin(), u8root.end());
    clipboard_incoming_.entries = entries;
    clipboard_incoming_.landed.assign(entries.size(), std::string());
    clipboard_incoming_.relative.resize(entries.size());
    clipboard_incoming_.top_relative.resize(entries.size());
    // Copied trees rebuild below one hidden directory per top-level folder and
    // flat files below one per entry, so two copied items with the same name
    // never collide on disk. CF_HDROP then gets the folder or file itself.
    std::map<std::string, size_t> roots;
    for (size_t index = 0; index < entries.size(); ++index) {
        const ClipboardFileEntry& entry = entries[index];
        if (entry.rel_path.empty()) {
            const std::string dir = ".item-" + std::to_string(index);
            clipboard_incoming_.relative[index] = dir;
            clipboard_incoming_.top_relative[index] = dir + "/" + entry.name;
            continue;
        }
        const size_t slash = entry.rel_path.find('/');
        const std::string top = entry.rel_path.substr(0, slash);
        const std::string inside =
            slash == std::string::npos ? entry.name : entry.rel_path.substr(slash + 1);
        const size_t last = inside.rfind('/');
        if ((last == std::string::npos ? inside : inside.substr(last + 1)) != entry.name) {
            reset_clipboard_incoming(true);
            *error = "a clipboard entry disagrees with its relative path";
            return false;
        }
        const auto found = roots.find(top);
        const size_t root_index = found == roots.end() ? (roots[top] = index) : found->second;
        const std::string root_dir = ".root-" + std::to_string(root_index);
        // The copied folder itself is part of the staged path, so a file at
        // its root and a file one level down never collide.
        const auto parent_end = entry.rel_path.rfind('/');
        const std::string relative = root_dir + (parent_end == std::string::npos ? "" :
            "/" + entry.rel_path.substr(0, parent_end));
        clipboard_incoming_.relative[index] = relative;
        clipboard_incoming_.top_relative[index] = root_dir + "/" + top;
    }
    for (size_t index = 0; index < entries.size(); ++index) {
        if (!entries[index].directory) continue;
        const auto relative = clipboard_incoming_.relative[index] + "/" + entries[index].name;
        if (!create_receive_directory(clipboard_incoming_.directory, relative, error)) {
            reset_clipboard_incoming(true);
            return false;
        }
        clipboard_incoming_.landed[index] = clipboard_incoming_.directory + "/" + relative;
    }
    clipboard_incoming_.next = 0;
    while (clipboard_incoming_.next < entries.size() && entries[clipboard_incoming_.next].directory)
        ++clipboard_incoming_.next;
    clipboard_incoming_.identity = clipboard_file_identity_hash(entries);
    clipboard_incoming_.active = clipboard_incoming_.identity != 0;
    if (!clipboard_incoming_.active) {
        reset_clipboard_incoming(true);
        *error = "the clipboard file list has no usable identity";
        return false;
    }
    log("clipboard: the viewer announced " + std::to_string(entries.size()) +
        " item(s); staging them for the host clipboard");
    if (clipboard_incoming_.next == entries.size()) finish_clipboard_incoming(stats);
    return true;
}

void HostServer::finish_clipboard_incoming(HostServerStats& stats) {
    if (!clipboard_incoming_.active) {
        return;
    }
    std::vector<std::string> tops;
    tops.reserve(clipboard_incoming_.entries.size());
    for (const std::string& relative : clipboard_incoming_.top_relative) {
        const std::string item = clipboard_incoming_.directory + "/" + relative;
        if (std::find(tops.begin(), tops.end(), item) == tops.end()) {
            tops.push_back(item);
        }
    }
    // Staged files use the same serialized, retryable writer as rich content.
    cancel_clipboard_work();
    pending_clipboard_paths_ = std::move(tops);
    clipboard_write_pending_ = true;
    clipboard_write_is_files_ = true;
    clipboard_write_attempts_ = 0;
    clipboard_write_retry_ = Clock::now();
    (void)stats;

}

void HostServer::audit_upload(const std::string& name, uint64_t bytes, bool success,
                              const std::string& detail) {
    if (settings_.audit_spool.empty() || file_audit_ref_.empty()) {
        return;
    }
    AuditFileRecord record;
    record.audit_ref = file_audit_ref_;
    record.viewer_ref_absent = file_audit_ref_absent_;
    record.direction = "upload";
    record.name = name;
    record.size = bytes;
    record.occurred_at_ms = unix_now_ms();
    record.success = success;
    record.detail = detail;
    std::string audit_error;
    if (!append_audit_file(settings_.audit_spool, record, &audit_error)) {
        log("audit: " + audit_error);
    }
}

// A blocked address can retry dozens of times a second; repeating the same
// refusal would push real events out of the service's bounded audit table.
bool HostServer::should_audit_denial(const std::string& key, int64_t now_unix) {
    constexpr int64_t kRepeatIntervalSeconds = 30;
    constexpr size_t kMaxTrackedDenials = 512;
    std::lock_guard<std::mutex> lock(denial_audit_->mutex);
    auto& records = denial_audit_->records;
    const auto it = records.find(key);
    if (it != records.end() && now_unix - it->second < kRepeatIntervalSeconds) {
        return false;
    }
    if (records.size() >= kMaxTrackedDenials) {
        // The map only suppresses repeats; dropping it costs duplicates, not
        // records, so a wholesale clear is enough to keep it bounded.
        records.clear();
    }
    records[key] = now_unix;
    return true;
}

void HostServer::audit_denial(const std::string& address, AccessDenial denial,
                              const std::string& reason) {
    if (settings_.audit_spool.empty()) {
        return;
    }
    const int64_t now = unix_now();
    // The transport picks a fresh source port per attempt, so the throttle
    // keys on the address only.
    const std::string host = peer_ip_text(address);
    const std::string key =
        (host.empty() ? std::string("unknown") : host) + "|" +
        std::to_string(static_cast<int>(denial));
    if (!should_audit_denial(key, now)) {
        return;
    }
    AuditDenialRecord record;
    record.address = address;
    record.code = static_cast<int>(denial);
    record.occurred_at_ms = unix_now_ms();
    record.detail = reason;
    std::string audit_error;
    if (!append_audit_denial(settings_.audit_spool, record, &audit_error)) {
        log("audit: " + audit_error);
    }
}

// `report=false` is for a revocation the transport already announced to the
// viewer; sending a second FileResult would only be noise.
void HostServer::end_upload(FileCode code, const std::string& reason, bool keep_partial,
                            bool report, HostServerStats& stats) {
    if (!upload_) {
        return;
    }
    const uint32_t transfer_id = upload_->offer.transfer_id;
    const uint64_t written = upload_->written;
    const std::string name =
        upload_->receiver ? upload_->receiver->name() : upload_->offer.name;
    const bool clipboard_transfer = upload_->offer.clipboard;
    if (upload_->receiver) {
        upload_->receiver->abort(keep_partial);
    }
    upload_.reset();
    if (clipboard_transfer) {
        // A clipboard copy is only useful complete; the staged tree is
        // dropped and the viewer purges the rest of its list.
        reset_clipboard_incoming(true);
    }
    if (code != FileCode::Ok) {
        ++stats.file_failures;
        log("file: " + name + " failed: " + reason);
        audit_upload(name, written, false, reason);
    }
    if (!report) {
        return;
    }
    Error sent = session_->send_file_result(transfer_id, code, written, reason);
    if (!sent.ok()) {
        log("file: cannot report the transfer result: " + sent.to_string());
    }
}

void HostServer::drain_file_offers(HostServerStats& stats) {
    FileOffer offer;
    while (session_->take_file_offer(offer)) {
        if (offer.clipboard) {
            // Protocol v34: the bytes of an announced clipboard list. They
            // stage below the list's session directory and never touch the
            // receive root; the clipboard permission gates them.
            if (!clipboard_transfer_allowed()) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                                 "the clipboard is disabled");
                reset_clipboard_incoming(true);
                continue;
            }
            if (!clipboard_incoming_.active ||
                clipboard_incoming_.next >= clipboard_incoming_.entries.size()) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                                 "no clipboard list is waiting");
                continue;
            }
            if (upload_ || held_upload_) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Busy, 0,
                                                 "another transfer is running");
                continue;
            }
            const size_t index = clipboard_incoming_.next;
            const ClipboardFileEntry& expected = clipboard_incoming_.entries[index];
            if (offer.name != expected.name || offer.size != expected.size) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                                 "the offer does not match the clipboard list");
                log("clipboard: the viewer offered '" + offer.name +
                    "' where the list expects '" + expected.name + "'");
                reset_clipboard_incoming(true);
                continue;
            }
            FileReceiveOptions options;
            // Keep .chast-partial outside CF_HDROP's top-level items. A
            // copied tree must not acquire our internal resume directory.
            options.directory = clipboard_incoming_.directory;
            options.relative_path = clipboard_incoming_.relative[index];
            options.name = expected.name;
            options.size = offer.size;
            options.sha256 = offer.sha256;
            options.allow_resume = offer.resume;
            // A staging tree belongs to this list alone, so a leftover name is
            // ours to replace rather than something the viewer must decide.
            options.overwrite = true;
            uint64_t resume_offset = 0;
            std::string receive_error;
            std::unique_ptr<FileReceiver> receiver =
                FileReceiver::create(options, &resume_offset, &receive_error);
            if (!receiver) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                                 receive_error);
                log("clipboard: cannot stage " + expected.name + ": " + receive_error);
                reset_clipboard_incoming(true);
                continue;
            }
            auto active = std::make_unique<ActiveUpload>();
            active->offer = std::move(offer);
            active->receiver = std::move(receiver);
            active->answer_time = Clock::now();
            active->written = resume_offset;
            active->clipboard = true;
            active->clipboard_entry = index;
            const uint32_t transfer_id = active->offer.transfer_id;
            const std::string clip_name = active->receiver->name();
            upload_ = std::move(active);
            Error answered = session_->send_file_answer(transfer_id, FileCode::Ok,
                                                        resume_offset);
            if (!answered.ok()) {
                upload_.reset();
                ++stats.file_failures;
                log("clipboard: cannot accept " + clip_name + ": " + answered.to_string());
                reset_clipboard_incoming(true);
                continue;
            }
            log("clipboard: receiving '" + clip_name + "' (" +
                std::to_string(expected.size) + " bytes)");
            continue;
        }
        if (!file_transfer_allowed()) {
            continue;
        }
        if (upload_ || held_upload_) {
            ++stats.file_failures;
            (void)session_->send_file_answer(offer.transfer_id, FileCode::Busy, 0,
                                             "another transfer is running");
            continue;
        }
        if (file_receive_directory_.empty()) {
            ++stats.file_failures;
            (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                             "the host has no receive directory");
            log("file: refused " + offer.name + ": no receive directory is configured");
            continue;
        }
        std::string upload_root = file_receive_directory_;
        std::string upload_relative = offer.relative_path;
        if (!file_shares_.empty() && !offer.relative_path.empty()) {
            // A drop into a browsed share belongs to that share. The mapped
            // path stays inside the configured root and is sanitized again by
            // FileReceiver before anything is created.
            std::string split_error;
            const FileCode split =
                split_file_share_path(file_shares_, file_receive_directory_,
                                      offer.relative_path, upload_root, upload_relative,
                                      split_error);
            if (split != FileCode::Ok) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, split, 0, split_error);
                log("file: refused " + offer.name + ": " + split_error);
                continue;
            }
        }
        FileReceiveOptions options;
        options.directory = upload_root;
        options.name = offer.name;
        options.relative_path = upload_relative;
        options.size = offer.size;
        options.sha256 = offer.sha256;
        options.allow_resume = offer.resume;
        FileReceiveTarget target;
        std::string target_error;
        if (!inspect_receive_target(options, target, &target_error)) {
            ++stats.file_failures;
            (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                             target_error);
            log("file: refused " + offer.name + ": " + target_error);
            continue;
        }
        if (target.exists) {
            if (target.directory) {
                ++stats.file_failures;
                (void)session_->send_file_answer(
                    offer.transfer_id, FileCode::Refused, 0,
                    "the destination is an existing directory");
                log("file: refused " + offer.name +
                    ": the destination is an existing directory");
                continue;
            }
            auto held = std::make_unique<HeldUpload>();
            held->root = upload_root;
            held->relative = upload_relative;
            FileConflict conflict;
            conflict.transfer_id = offer.transfer_id;
            conflict.identical = offer.modified_time != 0 &&
                                 target.modified_time == offer.modified_time &&
                                 target.size == offer.size;
            conflict.existing_size = target.size;
            conflict.existing_modified_time = target.modified_time;
            held->offer = std::move(offer);
            held_upload_ = std::move(held);
            const Error asked = session_->send_file_conflict(conflict);
            if (!asked.ok()) {
                held_upload_.reset();
                ++stats.file_failures;
                log("file: cannot ask about " +
                    (upload_relative.empty() ? std::string("the file") : upload_relative) +
                    ": " + asked.to_string());
                continue;
            }
            log("file: " + (upload_relative.empty() ? std::string() : upload_relative + "/") +
                held_upload_->offer.name + " already exists; waiting for the viewer" +
                (conflict.identical ? " (identical)" : ""));
            continue;
        }
        uint64_t resume_offset = 0;
        std::string receive_error;
        std::unique_ptr<FileReceiver> receiver =
            FileReceiver::create(options, &resume_offset, &receive_error);
        if (!receiver) {
            ++stats.file_failures;
            (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                             receive_error);
            log("file: refused " + offer.name + ": " + receive_error);
            continue;
        }
        auto active = std::make_unique<ActiveUpload>();
        active->offer = std::move(offer);
        active->receiver = std::move(receiver);
        active->answer_time = Clock::now();
        active->written = resume_offset;
        const uint32_t transfer_id = active->offer.transfer_id;
        const std::string name = active->receiver->name();
        const std::string offered_path =
            active->offer.relative_path.empty()
                ? name
                : active->offer.relative_path + "/" + name;
        upload_ = std::move(active);
        Error answered = session_->send_file_answer(transfer_id, FileCode::Ok,
                                                    resume_offset);
        if (!answered.ok()) {
            upload_.reset();
            ++stats.file_failures;
            log("file: cannot accept " + offered_path + ": " + answered.to_string());
            continue;
        }
        log("file: receiving " + offered_path + " (" +
            std::to_string(upload_->offer.size) + " bytes" +
            (resume_offset > 0 ? ", resuming at " + std::to_string(resume_offset) : "") +
            ")");
    }
}

// Moves every queued chunk into the .part file and turns the transport's
// FIN into a digest check and the final rename.
void HostServer::drain_file_data(HostServerStats& stats) {
    if (!upload_) {
        return;
    }
    const uint32_t transfer_id = upload_->offer.transfer_id;
    for (;;) {
        FileChunk chunk;
        Error got = session_->receive_file_chunk(chunk, std::chrono::milliseconds(0));
        if (got.status() == Status::WaitTimeout) {
            break;
        }
        if (got.status() == Status::Unavailable) {
            FileUploadState state;
            if (!session_->file_upload_state(transfer_id, state)) {
                // The viewer has not opened its CFIL stream yet; a short
                // answer-to-stream window is normal.
                if (Clock::now() - upload_->answer_time < std::chrono::seconds(10)) {
                    return;
                }
                end_upload(FileCode::IoError, "the viewer never opened the file stream",
                           true, true, stats);
                return;
            }
            if (state.error.empty() && state.finished) {
                // The FIN arrived after the last chunk was taken; the state
                // below turns it into the digest check and the rename.
                break;
            }
            const bool revoked = state.error == "file transfer was disabled";
            end_upload(revoked ? FileCode::Cancelled : FileCode::IoError,
                       state.error.empty() ? "the upload stream ended early" : state.error,
                       true, !revoked, stats);
            return;
        }
        if (!got.ok()) {
            end_upload(FileCode::IoError, got.to_string(), true, true, stats);
            return;
        }
        if (chunk.transfer_id != transfer_id) {
            end_upload(FileCode::IoError, "the upload stream carried another transfer",
                       true, true, stats);
            return;
        }
        if (chunk.offset != upload_->written) {
            end_upload(FileCode::IoError, "the upload is not contiguous", false, true, stats);
            return;
        }
        std::string write_error;
        if (!upload_->receiver->write(chunk.data.data(), chunk.data.size(), &write_error)) {
            end_upload(FileCode::IoError, write_error, false, true, stats);
            return;
        }
        upload_->written += chunk.data.size();
    }
    FileUploadState state;
    if (!session_->file_upload_state(transfer_id, state)) {
        if (Clock::now() - upload_->answer_time < std::chrono::seconds(10)) {
            return;
        }
        end_upload(FileCode::IoError, "the viewer never opened the file stream", true, true,
                   stats);
        return;
    }
    if (!state.error.empty()) {
        const bool revoked = state.error == "file transfer was disabled";
        end_upload(revoked ? FileCode::Cancelled : FileCode::IoError, state.error, true,
                   !revoked, stats);
        return;
    }
    if (!state.finished) {
        return;
    }
    std::string final_path;
    std::string finish_error;
    if (!upload_->receiver->finish(final_path, &finish_error)) {
        // Digest mismatch already removed the partial file; a rename
        // failure kept it for a resume.
        end_upload(FileCode::IoError, finish_error, true, true, stats);
        return;
    }
    const uint64_t bytes = upload_->written;
    if (upload_->offer.clipboard) {
        const size_t index = upload_->clipboard_entry;
        if (index < clipboard_incoming_.landed.size()) {
            clipboard_incoming_.landed[index] = final_path;
        }
        if (clipboard_incoming_.active) {
            ++clipboard_incoming_.next;
            while (clipboard_incoming_.next < clipboard_incoming_.entries.size() &&
                   clipboard_incoming_.entries[clipboard_incoming_.next].directory)
                ++clipboard_incoming_.next;
        }
        upload_->receiver.reset();
        upload_.reset();
        ++stats.files_received;
        stats.file_bytes_received += bytes;
        Error sent = session_->send_file_result(transfer_id, FileCode::Ok, bytes);
        if (!sent.ok()) {
            log("clipboard: cannot report the staging result: " + sent.to_string());
        }
        log("clipboard: staged '" + final_path + "' (" + std::to_string(bytes) +
            " bytes)");
        if (clipboard_incoming_.active &&
            clipboard_incoming_.next >= clipboard_incoming_.entries.size()) {
            finish_clipboard_incoming(stats);
        }
        publish_stats(stats);
        return;
    }
    const std::string received_name =
        upload_->receiver ? upload_->receiver->name() : upload_->offer.name;
    upload_->receiver.reset();
    upload_.reset();
    audit_upload(received_name, bytes, true, "");
    ++stats.files_received;
    stats.file_bytes_received += bytes;
    Error sent = session_->send_file_result(transfer_id, FileCode::Ok, bytes);
    if (!sent.ok()) {
        log("file: cannot report the completed transfer: " + sent.to_string());
    }
    log("file: received " + final_path + " (" + std::to_string(bytes) + " bytes)");
    if (config_.file_received) {
        config_.file_received(final_path, bytes);
    }
    publish_stats(stats);
}

void HostServer::drain_file_cancel(HostServerStats& stats) {
    FileCancel cancel;
    while (session_->take_file_cancel(cancel)) {
        if (!upload_ || cancel.transfer_id != upload_->offer.transfer_id) {
            continue;
        }
        end_upload(FileCode::Cancelled,
                   cancel.reason.empty() ? "the viewer cancelled the transfer"
                                         : cancel.reason,
                   false, true, stats);
    }
}

// Viewer-initiated browsing and downloads (protocol v30). The receive
// directory is the whole sandbox: every path is resolved inside it and a
// download joins the same offer/chunk path as an operator download, with
// its transfer id preassigned so the answer can name the coming offer.
void HostServer::drain_file_browse(HostServerStats& stats) {
    (void)stats;
    uint32_t request_id = 0;
    std::string directory;
    std::string cursor;
    bool show_hidden = false;
    while (session_->take_file_list_request(request_id, directory, &cursor, &show_hidden)) {
        FileListResult result;
        result.request_id = request_id;
        if (!file_transfer_allowed()) {
            result.code = FileCode::Refused;
            result.reason = "file transfer is disabled";
        } else if (file_receive_directory_.empty()) {
            result.code = FileCode::Refused;
            result.reason = "the host has no receive directory";
        } else if (directory.empty() && !file_shares_.empty()) {
            list_file_share_roots(file_shares_, result, cursor);
        } else {
            std::string root = file_receive_directory_;
            std::string relative = directory;
            std::string split_error;
            if (!file_shares_.empty()) {
                result.code = split_file_share_path(file_shares_, file_receive_directory_,
                                                    directory, root, relative, split_error);
                if (result.code != FileCode::Ok) {
                    result.reason = split_error;
                }
            }
            if (result.code == FileCode::Ok) {
                list_receive_directory(root, relative, result, cursor, show_hidden);
            }
        }
        const Error sent = session_->send_file_list_result(result);
        if (!sent.ok()) {
            log("file: cannot answer a browse request: " + sent.to_string());
            return;
        }
        if (result.code == FileCode::Ok) {
            log("file: listed " +
                (directory.empty() ? std::string(".") : directory) + " (" +
                std::to_string(result.entries.size()) + " entries" +
                (result.truncated ? ", more pages)" : ")"));
        } else {
            log("file: refused to list " +
                (directory.empty() ? std::string(".") : directory) + ": " +
                result.reason);
        }
    }

    std::string path;
    while (session_->take_file_download_request(request_id, path)) {
        FileDownloadResult answer;
        answer.request_id = request_id;
        std::string full_path;
        bool is_directory = false;
        size_t clipboard_index = 0;
        if (clipboard_file_index(path, clipboard_index)) {
            // A clipboard file is served from the announced absolute path,
            // gated by the clipboard permission rather than file transfer.
            const bool clipboard_ok = clipboard_allowed() &&
                clipboard_from_host(settings_.clipboard_direction);
            std::shared_ptr<ClipboardVirtualFiles> source;
            {
                std::lock_guard<std::mutex> lock(clipboard_files_mutex_);
                if (clipboard_index < clipboard_file_paths_.size() &&
                    clipboard_index < clipboard_file_entries_.size() &&
                    !clipboard_file_entries_[clipboard_index].directory) {
                    full_path = clipboard_file_paths_[clipboard_index];
                    source = clipboard_file_source_;
                }
            }
            // The owner marker: only the capture that produced the announced
            // list may serve its index, and the file must still hold the same
            // bytes (a re-copy or an in-place rewrite invalidates it).
            if (!clipboard_ok) {
                answer.code = FileCode::Refused;
                answer.reason = "clipboard is disabled";
            } else if (full_path.empty() || !source) {
                answer.code = FileCode::Refused;
                answer.reason = "the clipboard does not hold that file";
            } else if (!source->Active() || !source->Unchanged(clipboard_index)) {
                answer.code = FileCode::Refused;
                answer.reason = "the clipboard changed since these files were announced";
            }
        } else if (!file_transfer_allowed()) {
            answer.code = FileCode::Refused;
            answer.reason = "file transfer is disabled";
        } else if (file_receive_directory_.empty()) {
            answer.code = FileCode::Refused;
            answer.reason = "the host has no receive directory";
        } else {
            std::string root = file_receive_directory_;
            std::string relative = path;
            std::string split_error;
            answer.code = split_file_share_path(file_shares_, file_receive_directory_, path,
                                                root, relative, split_error);
            if (answer.code != FileCode::Ok) {
                answer.reason = split_error;
            } else {
                std::string resolve_error;
                answer.code = resolve_receive_path(root, relative, full_path, is_directory,
                                                   resolve_error);
                if (answer.code != FileCode::Ok) {
                    answer.reason = resolve_error;
                }
            }
        }
        if (answer.code == FileCode::Ok && is_directory) {
            answer.code = FileCode::Refused;
            answer.reason = "the path is a directory";
        } else if (answer.code == FileCode::Ok) {
            std::error_code file_error;
            const auto physical = path_from_utf8(full_path);
            uint64_t size = std::filesystem::file_size(physical, file_error);
            if (file_error || !std::filesystem::is_regular_file(physical, file_error)) {
                answer.code = file_error ? FileCode::IoError : FileCode::Refused;
                answer.reason = "the path is not a readable file";
            } else if (size > kMaxFileTransferBytes) {
                answer.code = FileCode::Refused;
                answer.reason = "the file exceeds the protocol's 16 GiB limit";
            } else {
                std::lock_guard<std::mutex> lock(download_mutex_);
                const bool idle =
                    pending_download_path_.empty() &&
                    send_.stage != FileSend::Stage::kHashing &&
                    send_.stage != FileSend::Stage::kReady &&
                    send_.stage != FileSend::Stage::kOffered &&
                    send_.stage != FileSend::Stage::kSending &&
                    send_.stage != FileSend::Stage::kFinishing;
                if (!idle) {
                    answer.code = FileCode::Busy;
                    answer.reason = "another file is already being sent";
                } else {
                    pending_download_path_ = std::move(full_path);
                    pending_download_clipboard_ = clipboard_file_index(path, clipboard_index);
                    pending_download_token_.reset();
                    pending_download_transfer_id_ = next_send_id_++;
                    answer.transfer_id = pending_download_transfer_id_;
                }
            }
        }
        const Error sent = session_->send_file_download_result(answer, clipboard_file_index(path, clipboard_index));
        if (!sent.ok()) {
            log("file: cannot answer a download request: " + sent.to_string());
            return;
        }
        if (answer.code == FileCode::Ok) {
            log("file: viewer requested " + path + " (transfer " +
                std::to_string(answer.transfer_id) + ")");
        } else {
            log("file: refused download " + path + ": " + answer.reason);
        }
    }

    // File management (protocol v31): every change runs through the same
    // resolve/refuse rules, and a refusal leaves the file system alone.
    FileOperation op = FileOperation::MakeDirectory;
    std::string op_path;
    std::string op_new_name;
    while (session_->take_file_op_request(request_id, op, op_path, op_new_name)) {
        FileOpResult answer;
        answer.request_id = request_id;
        if (!file_transfer_allowed()) {
            answer.code = FileCode::Refused;
            answer.reason = "file transfer is disabled";
        } else if (file_receive_directory_.empty()) {
            answer.code = FileCode::Refused;
            answer.reason = "the host has no receive directory";
        } else {
            std::string root = file_receive_directory_;
            std::string relative = op_path;
            std::string split_error;
            answer.code = split_file_share_path(file_shares_, file_receive_directory_, op_path,
                                                root, relative, split_error);
            if (answer.code != FileCode::Ok) {
                answer.reason = split_error;
            } else {
                std::string operation_error;
                answer.code = apply_file_operation(root, op, relative, op_new_name,
                                                   operation_error);
                if (answer.code != FileCode::Ok) {
                    answer.reason = operation_error;
                }
            }
        }
        const Error sent = session_->send_file_op_result(answer);
        if (!sent.ok()) {
            log("file: cannot answer a file operation: " + sent.to_string());
            return;
        }
        if (answer.code == FileCode::Ok) {
            log("file: applied operation " +
                std::to_string(static_cast<int>(op)) + " on " + op_path);
        } else {
            log("file: refused an operation on " + op_path + ": " + answer.reason);
        }
    }
}

void HostServer::send_file_failure(const std::string& message, bool report,
                                   HostServerStats& stats, bool cancelled) {
    uint32_t transfer_id = 0;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        switch (send_.stage) {
            case FileSend::Stage::kHashing:
            case FileSend::Stage::kReady:
            case FileSend::Stage::kOffered:
            case FileSend::Stage::kSending:
            case FileSend::Stage::kFinishing:
                break;
            default:
                return;
        }
        transfer_id = send_.transfer_id;
        name = send_.name;
        send_.stage = FileSend::Stage::kFailed;
        send_.error = message;
        send_.counted = true;
    }
    download_stream_.close();
    if (cancelled) {
        ++stats.file_send_cancels;
    } else {
        ++stats.file_send_failures;
    }
    if (report) {
        // The receiver would otherwise wait for a stream that never ends.
        (void)session_->send_file_cancel(transfer_id, FileCode::Cancelled, message);
        session_->cancel_file_upload(transfer_id);
    }
    log("file: sending " + name + (cancelled ? " cancelled: " : " failed: ") + message);
    publish_download();
    publish_stats(stats);
}

void HostServer::note_file_send_error(const std::string& message, bool report,
                                      HostServerStats& stats) {
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        switch (send_.stage) {
            case FileSend::Stage::kOffered:
            case FileSend::Stage::kSending:
            case FileSend::Stage::kFinishing:
                break;
            default:
                return;
        }
        // A viewer that cancels aborts its receive stream before the host
        // drains its FileResult, so the next write can fail first. Hold the
        // error until the grace expires; the kFailed branch in
        // drive_file_download() claims it as a cancel if one is queued.
        send_.stage = FileSend::Stage::kFailed;
        send_.error = message;
        send_.counted = false;
        send_.report_failure = report;
        send_.failure_deadline = Clock::now() + std::chrono::milliseconds(1000);
    }
    download_stream_.close();
    publish_download();
    publish_stats(stats);
}

// Operator downloads. The tray queues a path; hashing runs on its own
// thread so neither session loop waits for a large file, then the offer
// and the chunks use the same budget as the viewer's uploads.
void HostServer::drive_file_download(HostServerStats& stats) {
    FileSend snapshot;
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        snapshot = send_;
    }

    if (snapshot.stage == FileSend::Stage::kNone ||
        snapshot.stage == FileSend::Stage::kDone ||
        snapshot.stage == FileSend::Stage::kFailed) {
        std::string requested;
        std::shared_ptr<void> file_access_token;
        uint32_t preassigned_transfer_id = 0;
        bool clipboard = false;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            const bool idle = send_.stage == FileSend::Stage::kNone ||
                              send_.stage == FileSend::Stage::kDone ||
                              send_.stage == FileSend::Stage::kFailed;
            if (idle && !pending_download_path_.empty()) {
                requested = std::move(pending_download_path_);
                clipboard = pending_download_clipboard_;
                pending_download_clipboard_ = false;
                file_access_token = std::move(pending_download_token_);
                pending_download_path_.clear();
                preassigned_transfer_id = pending_download_transfer_id_;
                pending_download_transfer_id_ = 0;
            }
        }
        if (requested.empty()) {
            return;
        }
        if (send_hasher_.joinable()) {
            send_hasher_.join();
        }
        const std::u8string u8name = path_from_utf8(requested).filename().u8string();
        uint32_t transfer_id = 0;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            send_ = FileSend{};
            send_.stage = FileSend::Stage::kHashing;
            send_.path = requested;
            send_.clipboard = clipboard;
            send_.file_access_token = std::move(file_access_token);
            send_.name.assign(reinterpret_cast<const char*>(u8name.data()), u8name.size());
            send_.transfer_id = preassigned_transfer_id != 0 ? preassigned_transfer_id
                                                             : next_send_id_++;
            transfer_id = send_.transfer_id;
        }
        send_hasher_ = std::thread([this, transfer_id] {
            std::string path;
            std::shared_ptr<void> file_access_token;
            {
                std::lock_guard<std::mutex> lock(download_mutex_);
                path = send_.path;
                file_access_token = send_.file_access_token;
            }
            uint64_t size = 0;
            uint64_t modified_time = 0;
            std::array<uint8_t, 32> digest{};
            std::string error;
            bool ok = false;
            {
                ScopedFileAccess access(file_access_token);
                ok = access.ok() && sha256_file(path, size, digest, &error);
            }
            if (ok) {
                std::error_code stamp_error;
                const auto stamp = std::filesystem::last_write_time(
                    path_from_utf8(path), stamp_error);
                if (!stamp_error) {
                    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::clock_cast<std::chrono::system_clock>(stamp)
                            .time_since_epoch());
                    if (seconds.count() > 0) {
                        modified_time = static_cast<uint64_t>(seconds.count());
                    }
                }
            }
            if (ok && size > kMaxFileTransferBytes) {
                ok = false;
                error = "the file exceeds the protocol's 16 GiB limit";
            }
            std::lock_guard<std::mutex> lock(download_mutex_);
            if (send_.transfer_id != transfer_id ||
                send_.stage != FileSend::Stage::kHashing) {
                return;
            }
            if (ok) {
                send_.size = size;
                send_.modified_time = modified_time;
                send_.digest = digest;
                send_.stage = FileSend::Stage::kReady;
            } else {
                send_.stage = FileSend::Stage::kFailed;
                send_.error = error.empty() ? "the file could not be hashed" : error;
            }
        });
        publish_download();
        return;
    }

    if (snapshot.stage == FileSend::Stage::kHashing) {
        return;
    }
    if (snapshot.stage == FileSend::Stage::kDone) {
        return;
    }
    if (snapshot.stage == FileSend::Stage::kFailed) {
        // A failed write may be a viewer cancel in flight: its reset reaches
        // us before the FileResult. Collect the queues first, then count a
        // cancel immediately or a real failure once the grace expires.
        uint32_t transfer_id = 0;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            pending = send_.stage == FileSend::Stage::kFailed && !send_.counted;
            transfer_id = send_.transfer_id;
        }
        bool cancelled = false;
        if (pending) {
            FileResult result;
            while (session_->take_file_result(result)) {
                if (result.transfer_id == transfer_id &&
                    result.code == FileCode::Cancelled) {
                    cancelled = true;
                }
            }
            FileCancel cancel;
            while (session_->take_file_cancel(cancel)) {
                if (cancel.transfer_id == transfer_id) {
                    cancelled = true;
                }
            }
        }
        bool first = false;
        bool as_cancel = false;
        bool report = false;
        std::string message;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            if (send_.stage == FileSend::Stage::kFailed && !send_.counted &&
                (cancelled || Clock::now() >= send_.failure_deadline)) {
                send_.counted = true;
                first = true;
                as_cancel = cancelled;
                report = send_.report_failure;
                if (as_cancel) {
                    send_.error = "the viewer cancelled the download";
                }
                message = send_.error;
            }
        }
        if (first) {
            if (as_cancel) {
                ++stats.file_send_cancels;
                log("file: sending " + snapshot.name + " cancelled: " + message);
            } else {
                ++stats.file_send_failures;
                log("file: " + snapshot.name + " failed: " + message);
                if (report) {
                    (void)session_->send_file_cancel(transfer_id, FileCode::Cancelled,
                                                     message);
                    session_->cancel_file_upload(transfer_id);
                }
            }
            publish_download();
            publish_stats(stats);
        }
        return;
    }

    if (snapshot.clipboard ? !(clipboard_allowed() && clipboard_from_host(settings_.clipboard_direction))
                           : !file_transfer_allowed()) {
        send_file_failure("file or clipboard permission was disabled", true, stats);
        return;
    }

    FileResult result;
    while (session_->take_file_result(result)) {
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            current = send_.transfer_id == result.transfer_id &&
                      (send_.stage == FileSend::Stage::kOffered ||
                       send_.stage == FileSend::Stage::kSending ||
                       send_.stage == FileSend::Stage::kFinishing);
        }
        if (!current) {
            continue;
        }
        download_stream_.close();
        if (result.code == FileCode::Ok) {
            {
                std::lock_guard<std::mutex> lock(download_mutex_);
                send_.stage = FileSend::Stage::kDone;
                send_.error.clear();
                send_.sent = result.received;
            }
            ++stats.files_sent;
            stats.file_bytes_sent += result.received;
            log("file: sent " + snapshot.name + " (" +
                std::to_string(result.received) + " bytes)");
        } else {
            const bool cancelled = result.code == FileCode::Cancelled;
            std::string reason = result.reason;
            if (reason.empty()) {
                reason = cancelled ? "the viewer cancelled the download"
                                   : "the viewer rejected the file";
            }
            send_file_failure(reason, false, stats, cancelled);
        }
        publish_download();
        publish_stats(stats);
        return;
    }

    FileCancel cancel;
    while (session_->take_file_cancel(cancel)) {
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            current = send_.transfer_id == cancel.transfer_id &&
                      (send_.stage == FileSend::Stage::kOffered ||
                       send_.stage == FileSend::Stage::kSending ||
                       send_.stage == FileSend::Stage::kFinishing);
        }
        if (current) {
            send_file_failure(
                cancel.reason.empty() ? "the viewer cancelled the download"
                                      : cancel.reason,
                false, stats, true);
            return;
        }
    }

    switch (snapshot.stage) {
        case FileSend::Stage::kReady: {
            FileOffer offer;
            offer.transfer_id = snapshot.transfer_id;
            offer.size = snapshot.size;
            offer.resume = true;
            offer.sha256 = snapshot.digest;
            offer.name = snapshot.name;
            offer.clipboard = snapshot.clipboard;
            offer.modified_time = snapshot.modified_time;
            Error sent = session_->offer_file(offer);
            if (!sent.ok()) {
                send_file_failure("offer: " + sent.to_string(), false, stats);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(download_mutex_);
                if (send_.transfer_id == snapshot.transfer_id &&
                    send_.stage == FileSend::Stage::kReady) {
                    send_.stage = FileSend::Stage::kOffered;
                    send_.answer_time = Clock::now();
                }
            }
            log("file: offering " + snapshot.name + " (" +
                std::to_string(snapshot.size) + " bytes)");
            publish_download();
            return;
        }
        case FileSend::Stage::kOffered: {
            FileAnswer answer;
            while (session_->take_file_answer(answer)) {
                if (answer.transfer_id != snapshot.transfer_id) {
                    continue;
                }
                if (answer.code == FileCode::Conflict) {
                    // The viewer is asking its user; the decision arrives as
                    // FileConflictAnswer and the offer timeout pauses.
                    {
                        std::lock_guard<std::mutex> lock(download_mutex_);
                        if (send_.transfer_id != snapshot.transfer_id ||
                            send_.stage != FileSend::Stage::kOffered) {
                            continue;
                        }
                        send_.conflict_pending = true;
                    }
                    log("file: " + snapshot.name +
                        " already exists on the viewer; waiting for its decision");
                    publish_download();
                    return;
                }
                if (answer.code != FileCode::Ok) {
                    send_file_failure(
                        answer.reason.empty() ? "the viewer refused the file"
                                              : answer.reason,
                        false, stats);
                    return;
                }
                if (answer.resume_offset > snapshot.size) {
                    send_file_failure("the viewer asked to resume past the end of the file",
                                      true, stats);
                    return;
                }
                start_file_send(snapshot.transfer_id, answer.resume_offset, stats);
                return;
            }
            if (!snapshot.conflict_pending &&
                Clock::now() - snapshot.answer_time > std::chrono::seconds(30)) {
                send_file_failure("the viewer did not answer the offer", false, stats);
            }
            return;
        }
        case FileSend::Stage::kSending: {
            const auto deadline = Clock::now() + kDownloadSendBudget;
            const auto refill = Clock::now();
            download_tokens_ = std::min(
                kDownloadBurst,
                download_tokens_ +
                    std::chrono::duration<double>(refill - download_token_time_).count() *
                        kDownloadBytesPerSecond);
            download_token_time_ = refill;
            std::array<uint8_t, kFileChunkBytes> buffer{};
            uint64_t sent_bytes = snapshot.sent;
            while (sent_bytes < snapshot.size && Clock::now() < deadline) {
                const size_t want = static_cast<size_t>(std::min<uint64_t>(
                    snapshot.size - sent_bytes, buffer.size()));
                if (download_tokens_ < static_cast<double>(want)) {
                    break;
                }
                download_stream_.read(reinterpret_cast<char*>(buffer.data()),
                                      static_cast<std::streamsize>(want));
                const std::streamsize got = download_stream_.gcount();
                if (got <= 0) {
                    send_file_failure("the local file ended before its announced size",
                                      true, stats);
                    return;
                }
                Error wrote = session_->try_send_file_data(
                    snapshot.transfer_id, buffer.data(), static_cast<size_t>(got));
                if (wrote.status() == Status::WaitTimeout) {
                    // The shared queue is full: leave the piece for the
                    // next pass rather than stall the frame loop.
                    download_stream_.clear();
                    download_stream_.seekg(static_cast<std::streamoff>(sent_bytes),
                                           std::ios::beg);
                    break;
                }
                if (!wrote.ok()) {
                    note_file_send_error("send: " + wrote.to_string(), true, stats);
                    return;
                }
                sent_bytes += static_cast<uint64_t>(got);
                download_tokens_ -= static_cast<double>(got);
                std::lock_guard<std::mutex> lock(download_mutex_);
                if (send_.transfer_id == snapshot.transfer_id &&
                    send_.stage == FileSend::Stage::kSending) {
                    send_.sent = sent_bytes;
                }
            }
            if (sent_bytes >= snapshot.size) {
                Error finished = session_->finish_file_upload(snapshot.transfer_id);
                if (!finished.ok()) {
                    note_file_send_error("finish: " + finished.to_string(), true, stats);
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(download_mutex_);
                    if (send_.transfer_id == snapshot.transfer_id &&
                        send_.stage == FileSend::Stage::kSending) {
                        send_.stage = FileSend::Stage::kFinishing;
                    }
                }
                log("file: " + snapshot.name + " sent, waiting for the viewer");
            }
            publish_download();
            return;
        }
        default:
            return;
    }
}

void HostServer::start_file_send(uint32_t transfer_id, uint64_t resume_offset,
                                 HostServerStats& stats) {
    FileSend snapshot;
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        if (send_.transfer_id != transfer_id ||
            send_.stage != FileSend::Stage::kOffered) {
            return;
        }
        snapshot = send_;
    }
    if (resume_offset > snapshot.size) {
        send_file_failure("the viewer asked to resume past the end of the file", true,
                          stats);
        return;
    }
    {
        ScopedFileAccess access(snapshot.file_access_token);
        if (access.ok()) {
            download_stream_.open(path_from_utf8(snapshot.path), std::ios::binary);
        } else {
            download_stream_.setstate(std::ios::failbit);
        }
    }
    if (!download_stream_) {
        send_file_failure("the local file cannot be opened", false, stats);
        return;
    }
    download_stream_.seekg(static_cast<std::streamoff>(resume_offset), std::ios::beg);
    if (!download_stream_) {
        send_file_failure("the local file cannot seek to the resume offset", false, stats);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        if (send_.transfer_id == transfer_id &&
            send_.stage == FileSend::Stage::kOffered) {
            send_.stage = FileSend::Stage::kSending;
            send_.resume_offset = resume_offset;
            send_.sent = resume_offset;
            send_.conflict_pending = false;
        }
    }
    download_tokens_ = kDownloadBurst;
    download_token_time_ = Clock::now();
    log("file: sending " + snapshot.name + " from " + std::to_string(resume_offset));
    publish_download();
}

void HostServer::answer_held_upload(const FileConflictAnswer& answer,
                                    HostServerStats& stats) {
    std::unique_ptr<HeldUpload> held = std::move(held_upload_);
    const uint32_t transfer_id = held->offer.transfer_id;
    const std::string offered_path = held->relative.empty()
                                         ? held->offer.name
                                         : held->relative + "/" + held->offer.name;
    if (answer.decision == FileConflictDecision::Skip) {
        (void)session_->send_file_answer(transfer_id, FileCode::Skipped, 0,
                                         "the existing file is kept");
        log("file: skipped " + offered_path + " (the existing file is kept)");
        return;
    }
    if (answer.decision == FileConflictDecision::Cancel) {
        (void)session_->send_file_answer(transfer_id, FileCode::Cancelled, 0,
                                         "the viewer cancelled the transfer");
        log("file: " + offered_path + " cancelled at the overwrite prompt");
        return;
    }
    FileReceiveOptions options;
    options.directory = held->root;
    options.name = held->offer.name;
    options.relative_path = held->relative;
    options.size = held->offer.size;
    options.sha256 = held->offer.sha256;
    options.allow_resume = held->offer.resume;
    options.overwrite = true;
    uint64_t resume_offset = 0;
    std::string receive_error;
    std::unique_ptr<FileReceiver> receiver =
        FileReceiver::create(options, &resume_offset, &receive_error);
    if (!receiver) {
        ++stats.file_failures;
        (void)session_->send_file_answer(transfer_id, FileCode::Refused, 0,
                                         receive_error);
        log("file: refused " + offered_path + ": " + receive_error);
        return;
    }
    auto active = std::make_unique<ActiveUpload>();
    active->offer = std::move(held->offer);
    active->receiver = std::move(receiver);
    active->answer_time = Clock::now();
    active->written = resume_offset;
    const std::string name = active->receiver->name();
    upload_ = std::move(active);
    Error answered = session_->send_file_answer(transfer_id, FileCode::Ok, resume_offset);
    if (!answered.ok()) {
        upload_.reset();
        ++stats.file_failures;
        log("file: cannot accept " + offered_path + ": " + answered.to_string());
        return;
    }
    log("file: receiving " + offered_path + " (" +
        std::to_string(upload_->offer.size) + " bytes, replacing the existing file" +
        (resume_offset > 0 ? ", resuming at " + std::to_string(resume_offset) : "") + ")");
}

void HostServer::answer_held_download(const FileConflictAnswer& answer,
                                      HostServerStats& stats) {
    FileSend snapshot;
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        // The decision can overtake the FileAnswer{Conflict} that pauses the
        // offer timeout, so conflict_pending is not required here.
        current = send_.transfer_id == answer.transfer_id &&
                  send_.stage == FileSend::Stage::kOffered;
        if (current) {
            snapshot = send_;
        }
    }
    if (!current) {
        return;
    }
    switch (answer.decision) {
        case FileConflictDecision::Overwrite: {
            const Error accepted = session_->accept_held_download(answer.transfer_id);
            if (!accepted.ok()) {
                send_file_failure("cannot restart the overwritten download: " +
                                      accepted.to_string(),
                                  true, stats);
                return;
            }
            start_file_send(answer.transfer_id, 0, stats);
            return;
        }
        case FileConflictDecision::Skip:
            {
                std::lock_guard<std::mutex> lock(download_mutex_);
                if (send_.transfer_id == answer.transfer_id &&
                    send_.stage == FileSend::Stage::kOffered) {
                    send_.stage = FileSend::Stage::kDone;
                    send_.error = "the viewer kept its existing file";
                    send_.conflict_pending = false;
                    send_.counted = true;
                }
            }
            session_->cancel_file_upload(answer.transfer_id);
            log("file: " + snapshot.name + " skipped (the viewer kept its existing file)");
            publish_download();
            return;
        case FileConflictDecision::Cancel:
            send_file_failure("the viewer cancelled the transfer", false, stats, true);
            session_->cancel_file_upload(answer.transfer_id);
            return;
    }
}

void HostServer::drain_file_conflicts(HostServerStats& stats) {
    FileConflictAnswer answer;
    while (session_->take_file_conflict_answer(answer)) {
        if (held_upload_ && answer.transfer_id == held_upload_->offer.transfer_id) {
            answer_held_upload(answer, stats);
        } else {
            answer_held_download(answer, stats);
        }
    }
}

void HostServer::poll_files(HostServerStats& stats) {
    if (upload_ && !file_transfer_allowed() && !upload_->offer.clipboard) {
        // The transport already told the viewer and dropped the stream;
        // keeping the part file lets a later offer resume it.
        log("file: stopping " + upload_->receiver->name() +
            " because file transfer was revoked");
        end_upload(FileCode::Cancelled, "file transfer was disabled", true, false, stats);
    }
    if (upload_ && upload_->offer.clipboard && !clipboard_transfer_allowed()) {
        log("clipboard: stopping " + upload_->receiver->name() +
            " because the clipboard was revoked");
        end_upload(FileCode::Cancelled, "the clipboard was disabled", true, false, stats);
    }
    if (held_upload_ && !file_transfer_allowed()) {
        log("file: dropping the held offer for " + held_upload_->offer.name +
            " because file transfer was revoked");
        (void)session_->send_file_answer(held_upload_->offer.transfer_id,
                                         FileCode::Cancelled, 0,
                                         "file transfer was disabled");
        held_upload_.reset();
    }
    if (clipboard_incoming_.active && !clipboard_transfer_allowed()) {
        reset_clipboard_incoming(true);
    }
    drain_file_offers(stats);
    drain_file_conflicts(stats);
    drain_file_data(stats);
    drain_file_cancel(stats);
    drain_file_browse(stats);
    drive_file_download(stats);
}

bool HostServer::reject_desktop_requests(std::string* error, bool camera_options) {
    const std::string reason = "this operation requires a desktop session";
    auto answered = [error](const Error& sent) {
        if (!sent.ok()) {
            *error = "desktop request result: " + sent.to_string();
        }
        return sent.ok();
    };
    uint16_t display = 0, width = 0, height = 0, refresh = 0;
    if (session_->take_display_switch_request(display) &&
        !answered(session_->send_display_switch_result(
            display, DisplaySwitchCode::Failed, reason))) {
        return false;
    }
    if (session_->take_list_display_modes_request(display)) {
        DisplayModeList result;
        result.display = display;
        result.code = DisplayModeCode::Failed;
        result.reason = reason;
        if (!answered(session_->send_display_modes(result))) {
            return false;
        }
    }
    if (session_->take_set_display_mode_request(display, width, height, refresh) &&
        !answered(session_->send_display_mode_result(display, width, height, refresh,
                                                     DisplayModeCode::Failed, reason))) {
        return false;
    }
    uint8_t follow = 0;
    if (session_->take_display_follow_request(follow) &&
        !answered(session_->send_display_follow_state(
            {0, DisplayFollowCode::Unsupported, reason}))) {
        return false;
    }
    StreamOptions options;
    if (!camera_options && session_->take_stream_options_request(options)) {
        StreamOptionsResult result;
        result.code = StreamOptionsCode::Unsupported;
        result.reason = reason;
        if (!answered(session_->send_stream_options_result(result))) {
            return false;
        }
    }
    bool privacy = false;
    if (session_->take_privacy_mode_request(privacy) &&
        !answered(session_->send_privacy_mode_state(false, PrivacyModeCode::Unsupported, reason))) {
        return false;
    }
    while (session_->take_sas_request()) {
        if (!answered(session_->send_sas_result(SasCode::Unsupported, reason))) {
            return false;
        }
    }
    return true;
}

bool HostServer::file_session(HostServerStats& stats, std::string* error, bool* peer_left) {
    *peer_left = false;
    // The policy only lets a files-only Hello through with the file-transfer
    // permission, and the announcement repeats that mask as the gates.
    ServerInfo info;
    info.files_only = true;
    info.mode = SessionMode::FileTransfer;
    publish_stream(info, 0, std::string());
    if (config_.stream_announced) {
        config_.stream_announced(info);
    }
    begin_file_session();
    struct FileSessionEnd {
        HostServer* self;
        ~FileSessionEnd() { self->end_file_session(); }
    } file_session_end{this};

    Error announced = session_->announce(info);
    if (!announced.ok()) {
        *error = "announce: " + announced.to_string();
        return false;
    }
    publish_stats(stats);
    log("file transfer only: no screen stream follows");

    // A files-only session has no frame loop, so the queued operator mask is
    // applied here instead. Revoking file transfer/clipboard reaches the
    // transport (in-flight uploads are aborted) and the gates below, which
    // read session_->permissions() on every poll.
    auto apply_pending_permissions = [&]() {
        bool pending = false;
        PermissionMask requested = 0;
        {
            std::lock_guard<std::mutex> lock(permission_mutex_);
            pending = permission_update_pending_.exchange(false);
            requested = pending_permissions_.load();
        }
        const bool policy_changed = sync_remote_policy();
        if (!pending && !policy_changed) {
            return;
        }
        Error applied = pending ? session_->set_permissions(requested) : Error::success();
        if (!applied.ok()) {
            if (applied.status() != Status::InvalidCall) {
                log("permission update failed: " + applied.to_string());
            }
            return;
        }
        const PermissionMask now_mask = session_->permissions();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.session_permissions = now_mask;
        }
        log("the session permissions are now " + permission_names(now_mask));
    };
    apply_pending_permissions();

    // A file-only session counts only completed or progressed transfers as
    // activity: browsing folders and refused desktop controls do not keep a
    // forgotten viewer alive. `run_single` reuses `stats`, so the baseline is
    // taken per session rather than assumed zero.
    const Clock::duration idle_timeout = idle_disconnect_timeout();
    const auto file_activity = [&stats] {
        return stats.files_received + stats.files_sent + stats.file_bytes_received +
               stats.file_bytes_sent + stats.file_failures + stats.file_send_failures +
               stats.file_send_cancels;
    };
    auto last_activity = Clock::now();
    auto last_file_activity = file_activity();

    while (!stopping_.load() && !disconnect_requested_.load()) {
        apply_pending_permissions();
        if (!apply_ticket_renewals(error)) return false;
        if (ticket_lease_expired(Clock::now())) return true;
        if (!handle_elevation_requests(error)) return false;
        if (!drain_chat(stats, error)) return false;
        if (!reject_desktop_requests(error)) {
            return false;
        }

        if (idle_timeout > Clock::duration::zero() &&
            Clock::now() - last_activity >= idle_timeout) {
            ++stats.idle_disconnects;
            log("idle: no file activity for " + std::to_string(idle_disconnect_seconds()) +
                " s; closing the file session");
            return true;
        }
        HostControlRequest host_action;
        while (session_->take_host_action_request(host_action)) {
            ++stats.host_actions;
            ++stats.host_action_failures;
            HostControlResult denied;
            denied.action = host_action.action;
            denied.code = HostActionCode::Denied;
            denied.reason = "host actions are not available in a file-transfer session";
            const Error sent = session_->send_host_action_result(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "host_action_result: " + sent.to_string();
                return false;
            }
            log("host action refused: file-transfer sessions cannot control the host");
        }
        RemoteConfigRequest config_request;
        while (session_->take_remote_config_request(config_request)) {
            ++stats.remote_config_requests;
            ++stats.remote_config_failures;
            RemoteConfigState denied;
            denied.code = RemoteConfigCode::Unsupported;
            denied.reason =
                "remote configuration is not available in a file-transfer session";
            const Error sent = session_->send_remote_config_state(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "remote_config_state: " + sent.to_string();
                return false;
            }
            log("remote configuration refused: file-transfer sessions cannot change host settings");
        }
        DisplayControlRequest display_request;
        while (session_->take_display_control_request(display_request)) {
            DisplayControlState denied;
            denied.code = DisplayControlCode::Denied;
            denied.reason = "display control requires a desktop session";
            const Error sent = session_->send_display_control_state(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "display_control_state: " + sent.to_string();
                return false;
            }
            log("display control refused: a file-transfer session has no desktop");
        }
        std::string login_password;
        while (session_->take_os_login_request(login_password)) {
            ++stats.os_login_requests;
            ++stats.os_login_failures;
            std::fill(login_password.begin(), login_password.end(), '\0');
            login_password.clear();
            const Error sent = session_->send_os_login_result(
                OsLoginCode::Unsupported,
                "OS login is not available in a file-transfer session");
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "os_login_result: " + sent.to_string();
                return false;
            }
            log("OS login refused: file-transfer sessions cannot type at the logon screen");
        }
        while (session_->take_session_list_request()) {
            ++stats.session_list_requests;
            SessionListResult denied;
            denied.code = SessionListCode::Unsupported;
            denied.reason = "session select is not available in a file-transfer session";
            const Error sent = session_->send_session_list_result(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "session_list_result: " + sent.to_string();
                return false;
            }
            log("session list refused: file-transfer sessions cannot switch sessions");
        }
        uint32_t select_id = 0;
        while (session_->take_session_select_request(select_id)) {
            ++stats.session_select_requests;
            SessionSelectResult denied;
            denied.session_id = select_id;
            denied.code = SessionSelectCode::Unsupported;
            denied.reason = "session select is not available in a file-transfer session";
            const Error sent = session_->send_session_select_result(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "session_select_result: " + sent.to_string();
                return false;
            }
            log("session select refused: file-transfer sessions cannot switch sessions");
        }
        poll_files(stats);
        const uint64_t activity = file_activity();
        if (activity != last_file_activity) {
            last_file_activity = activity;
            last_activity = Clock::now();
        }
        if (session_->viewer_finished()) {
            return true;
        }
        if (!session_->peer_alive()) {
            *peer_left = true;
            return true;
        }
        const Error async = session_->async_error();
        if (!async.ok()) {
            *error = "transport: " + async.to_string();
            return false;
        }
        if (!sleep_unless_stopping(std::chrono::milliseconds(20))) {
            break;
        }
    }
    return error->empty();
}

bool HostServer::control_session(SessionMode mode, HostServerStats& stats,
                                 std::string* error, bool* peer_left) {
    *peer_left = false;
    // No capturer, encoder or media stream: announce() derives the one granted
    // permission from the session mask and opens no video stream for a
    // control-only description.
    ServerInfo info;
    info.control_only = true;
    info.mode = mode;
    info.terminal_persist = settings_.allow_terminal_persist;
    publish_stream(info, 0, std::string());
    if (config_.stream_announced) {
        config_.stream_announced(info);
    }
    Error announced = session_->announce(info);
    if (!announced.ok()) {
        *error = "announce: " + announced.to_string();
        return false;
    }
    publish_stats(stats);
    const char* label = mode == SessionMode::Terminal
                            ? "terminal"
                            : mode == SessionMode::Camera ? "camera" : "port forward";
    log(std::string(label) + " only: no screen stream follows");

    // As in the file session, there is no frame loop to notice an operator
    // change, so the queued mask is applied here. Revoking the mode's
    // permission closes its streams in the transport; the relays below stop
    // serving new requests.
    auto apply_pending_permissions = [&]() {
        bool pending = false;
        PermissionMask requested = 0;
        {
            std::lock_guard<std::mutex> lock(permission_mutex_);
            pending = permission_update_pending_.exchange(false);
            requested = pending_permissions_.load();
        }
        const bool policy_changed = sync_remote_policy();
        if (!pending && !policy_changed) {
            return;
        }
        Error applied = pending ? session_->set_permissions(requested) : Error::success();
        if (!applied.ok()) {
            if (applied.status() != Status::InvalidCall) {
                log("permission update failed: " + applied.to_string());
            }
            return;
        }
        const PermissionMask now_mask = session_->permissions();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.session_permissions = now_mask;
        }
        log("the session permissions are now " + permission_names(now_mask));
    };
    apply_pending_permissions();

    // The relays own every socket and shell they open; their destructors run
    // on every exit path below.
    PortForwardRelay port_forward(*session_);
    TerminalRelay terminal(*session_, settings_.allow_terminal_persist,
                           [this] { touch_viewer_activity(); });
    // A control-only session has no capturer to borrow an adapter from; the
    // camera relay opens its own device instead.
    DesktopStreamPool standalone_streams;
    auto& streams = registry_ ? registry_->desktop_streams : standalone_streams;
    CameraRelay camera(*session_, [] { return LUID{}; },
                       CameraRelayConfig{settings_.codec, settings_.quality,
                                         config_.encoder_backend,
                                         settings_.framerate > 0 ? settings_.framerate : 30u,
                                         config_.cameras, config_.camera_capture_factory,
                                         mode == SessionMode::Camera},
        [&, decoders = session_->decoding_codecs(), preference = codec_vote(session_->codec_preference()),
            decoders_444 = session_->decoding_444(), chroma = session_->chroma_preference()](
                const std::wstring& key, CameraRelay::CaptureFactory capture, std::string* failure) {
            // A camera-only connection participates while its camera is open.
            // Stop/revocation releases this peer with the worker subscription.
            auto peer = streams.connect_codecs(decoders, preference, decoders_444, chroma);
            auto quality = streams.connect_quality(settings_.framerate, settings_.quality,
                config_.adaptive_bitrate, config_.adaptive_fps);
            CameraRelay::Subscription result;
            result.quality = [quality](const StreamOptions& options) {
                if (options.quality == StreamQuality::Custom) {
                    quality->options(options.max_fps, std::nullopt,
                                     options.quality_percent / 50.0f);
                    return;
                }
                quality->options(options.max_fps,
                    options.quality == StreamQuality::High ? VideoQos::Quality::High :
                    options.quality == StreamQuality::Balanced ? VideoQos::Quality::Balanced :
                    VideoQos::Quality::Smooth);
            };
            result.capture = streams.subscribe(key, std::move(capture), config_.encoder_factory,
                failure, {}, decoders, std::move(peer),
                {settings_.record_incoming, settings_.record_directory, config_.log});
            return result;
        });

    while (!stopping_.load() && !disconnect_requested_.load()) {
        apply_pending_permissions();
        if (!apply_ticket_renewals(error)) return false;
        if (ticket_lease_expired(Clock::now())) return true;
        if (!handle_elevation_requests(error)) return false;
        if (!drain_chat(stats, error)) return false;
        if (!reject_desktop_requests(error, mode == SessionMode::Camera)) {
            return false;
        }

        // Desktop-only controls have no surface here. Answering keeps a viewer
        // that races a permission change from waiting for a reply that will
        // never come.
        HostControlRequest host_action;
        while (session_->take_host_action_request(host_action)) {
            ++stats.host_actions;
            ++stats.host_action_failures;
            HostControlResult denied;
            denied.action = host_action.action;
            denied.code = HostActionCode::Denied;
            denied.reason = "host actions are not available in a control-only session";
            const Error sent = session_->send_host_action_result(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "host_action_result: " + sent.to_string();
                return false;
            }
            log("host action refused: a control-only session cannot control the host");
        }
        RemoteConfigRequest config_request;
        while (session_->take_remote_config_request(config_request)) {
            ++stats.remote_config_requests;
            ++stats.remote_config_failures;
            RemoteConfigState denied;
            denied.code = RemoteConfigCode::Unsupported;
            denied.reason =
                "remote configuration is not available in a control-only session";
            const Error sent = session_->send_remote_config_state(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "remote_config_state: " + sent.to_string();
                return false;
            }
            log("remote configuration refused: a control-only session cannot change host settings");
        }
        DisplayControlRequest display_request;
        while (session_->take_display_control_request(display_request)) {
            DisplayControlState denied;
            denied.code = DisplayControlCode::Denied;
            denied.reason = "display control requires a desktop session";
            const Error sent = session_->send_display_control_state(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "display_control_state: " + sent.to_string();
                return false;
            }
            log("display control refused: a control-only session has no desktop");
        }
        std::string login_password;
        while (session_->take_os_login_request(login_password)) {
            ++stats.os_login_requests;
            ++stats.os_login_failures;
            std::fill(login_password.begin(), login_password.end(), '\0');
            login_password.clear();
            const Error sent = session_->send_os_login_result(
                OsLoginCode::Unsupported,
                "OS login is not available in a control-only session");
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "os_login_result: " + sent.to_string();
                return false;
            }
            log("OS login refused: a control-only session cannot type at the logon screen");
        }
        while (session_->take_session_list_request()) {
            ++stats.session_list_requests;
            SessionListResult denied;
            denied.code = SessionListCode::Unsupported;
            denied.reason = "session select is not available in a control-only session";
            const Error sent = session_->send_session_list_result(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "session_list_result: " + sent.to_string();
                return false;
            }
            log("session list refused: a control-only session cannot switch sessions");
        }
        uint32_t select_id = 0;
        while (session_->take_session_select_request(select_id)) {
            ++stats.session_select_requests;
            SessionSelectResult denied;
            denied.session_id = select_id;
            denied.code = SessionSelectCode::Unsupported;
            denied.reason = "session select is not available in a control-only session";
            const Error sent = session_->send_session_select_result(denied);
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "session_select_result: " + sent.to_string();
                return false;
            }
            log("session select refused: a control-only session cannot switch sessions");
        }
        PortForwardRequest forward_request;
        while (session_->take_port_forward_request(forward_request)) {
            // The transport already checked the session grant; reaching here
            // means the operator enabled port forwarding for this host.
            const Error answered = session_->send_port_forward_result(
                forward_request.forward_id, PortForwardCode::Ok);
            if (!answered.ok()) {
                if (answered.status() == Status::Unavailable ||
                    answered.status() == Status::WaitTimeout) {
                    *peer_left = true;
                    return true;
                }
                ++stats.send_errors;
                *error = "port_forward_result: " + answered.to_string();
                return false;
            }
            log("port forward: approved " + forward_request.host + ":" +
                std::to_string(forward_request.port));
        }
        PortForwardStop forward_stop;
        while (session_->take_port_forward_stop(forward_stop)) {
            log("port forward: stopped with code " +
                std::to_string(static_cast<int>(forward_stop.code)) +
                (forward_stop.reason.empty() ? std::string()
                                             : ": " + forward_stop.reason));
        }
        for (const std::string& event : port_forward.pump()) {
            log(event);
        }
        for (const std::string& event : terminal.pump()) {
            log(event);
        }
        for (const std::string& event : camera.pump()) {
            log(event);
        }
        if (session_->viewer_finished()) {
            return true;
        }
        if (!session_->peer_alive()) {
            *peer_left = true;
            return true;
        }
        const Error async = session_->async_error();
        if (!async.ok()) {
            *error = "transport: " + async.to_string();
            return false;
        }
        const auto idle = std::chrono::milliseconds(camera.streaming() ? 10 : 20);
        if (!sleep_unless_stopping(idle)) {
            break;
        }
    }
    return error->empty();
}

// ----- per-viewer clipboard and microphone plumbing -----

bool HostServer::clipboard_allowed() const {
    return settings_.allow_clipboard && settings_.clipboard_direction != ClipboardDirection::Disabled &&
           (session_->permissions() & kPermissionClipboard) != 0;
}

bool HostServer::request_local_clipboard() {
    if (!clipboard_channel_) clipboard_channel_ = std::make_unique<ClipboardFileChannel>();
    if (!clipboard_worker_) {
        ClipboardBackend backend;
        if (config_.clipboard_read) {
            backend.read = [read = config_.clipboard_read](ClipboardContent& content) {
                if (!read(content)) return ClipboardReadState::Failed;
                return content.empty() ? ClipboardReadState::Empty : ClipboardReadState::Content;
            };
        }
        if (config_.clipboard_read_files) {
            backend.read_files = [read = config_.clipboard_read_files](std::vector<std::string>& paths) {
                return read(paths) && !paths.empty() ? ClipboardReadState::Content : ClipboardReadState::Empty;
            };
        }
        backend.write = config_.clipboard_write;
        backend.write_files = config_.clipboard_write_files;
        backend.write_virtual_files = config_.clipboard_write_virtual_files;
        clipboard_worker_ = std::make_unique<ClipboardWorker>(std::move(backend));
    }
    if (clipboard_native() && clipboard_listener_ == 0) {
        clipboard_listener_ = ClipboardListener::Subscribe();
    }
    if (!clipboard_from_host(settings_.clipboard_direction)) return false;
    const uint64_t generation = ClipboardListener::Generation();
    if (!clipboard_worker_->Read(clipboard_epoch_)) return false;
    clipboard_seen_generation_ = generation;
    return true;
}

std::chrono::milliseconds HostServer::clipboard_poll_interval() const {
    return clipboard_native() && ClipboardListener::Running()
        ? std::chrono::milliseconds(5000) : std::chrono::milliseconds(500);
}

void HostServer::cancel_clipboard_work() {
    ++clipboard_epoch_;
    if (clipboard_worker_) clipboard_worker_->Cancel();
    if (clipboard_channel_) clipboard_channel_->CancelRemote();
    pending_virtual_files_.reset();
    clipboard_write_pending_ = false;
    clipboard_write_inflight_ = false;
    pending_clipboard_content_ = {};
    pending_clipboard_paths_.clear();
}

void HostServer::clear_clipboard_file_share() {
    std::lock_guard<std::mutex> lock(clipboard_files_mutex_);
    clipboard_file_entries_.clear();
    clipboard_file_paths_.clear();
    clipboard_file_source_.reset();
}

void HostServer::remember_local_clipboard() {
    cancel_clipboard_work();
    clear_clipboard_file_share();
    clipboard_baseline_pending_ = true;
    clipboard_initial_send_ = true;
    last_clipboard_hash_ = 0;
    last_clipboard_ = {};
    last_file_entries_.clear();
    last_file_paths_.clear();
    last_file_identity_ = 0;
    request_local_clipboard();
}

void HostServer::apply_peer_clipboard(ClipboardContent content, HostServerStats& stats) {
    if (!clipboard_transfer_allowed()) return;
    if (content.seq && content.seq <= last_peer_clipboard_seq_) return;
    if (content.seq) last_peer_clipboard_seq_ = content.seq;
    // A completed write of the previous item may still be waiting for the
    // poll, and cancel_clipboard_work() discards it. Settle it first: the item
    // is already on the local clipboard, so it must be counted and remembered
    // even though a newer one is about to replace it.
    {
        ClipboardWorker::Result settled;
        bool ignored = false;
        take_clipboard_result(settled, ignored, stats, Clock::now());
    }
    if (const ClipboardFormatData* list = content.find(ClipboardFormat::FileList)) {
        // Protocol v34: the viewer copied files. Only the list crosses the
        // clipboard stream; the bytes follow as clipboard-marked FileOffers
        // that stage below a session directory.
        std::vector<ClipboardFileEntry> entries;
        std::string list_error;
        if (!decode_clipboard_file_list(list->data, entries, &list_error)) {
            log("clipboard: the viewer sent an unusable file list: " + list_error);
            return;
        }
        cancel_clipboard_work();
        clear_clipboard_file_share();
        last_clipboard_hash_ = clipboard_content_hash(content);
        last_clipboard_ = ClipboardContent{};
        if (!config_.clipboard_write_files || config_.clipboard_write_virtual_files) {
            if (!clipboard_channel_) clipboard_channel_ = std::make_unique<ClipboardFileChannel>();
            clipboard_channel_->ClearLocal();
            pending_virtual_files_ = clipboard_channel_->Receive(content.seq, std::move(entries));
            if (!pending_virtual_files_) { log("clipboard: the virtual file offer has no generation"); return; }
            clipboard_write_pending_ = true;
            clipboard_write_is_files_ = true;
            clipboard_write_attempts_ = 0;
            clipboard_write_retry_ = Clock::now();
            return;
        }
        std::string begin_error;
        if (!begin_clipboard_incoming(entries, stats, &begin_error)) {
            log("clipboard: cannot stage the viewer's files: " + begin_error);
        }
        return;
    }
    const uint64_t hash = clipboard_content_hash(content);
    if (hash == last_clipboard_hash_ && !clipboard_write_pending_) {
        return;
    }
    std::string content_error;
    if (!valid_clipboard_content(content, &content_error)) {
        log("clipboard: the viewer sent unusable content: " + content_error);
        return;
    }
    // New peer content supersedes a queued retry and invalidates older reads.
    cancel_clipboard_work();
    clear_clipboard_file_share();
    if (clipboard_channel_) clipboard_channel_->ClearLocal();
    if (upload_ && upload_->offer.clipboard) {
        end_upload(FileCode::Cancelled, "the viewer replaced its clipboard list", true, true, stats);
    }
    if (clipboard_incoming_.active) reset_clipboard_incoming(true);
    pending_clipboard_content_ = std::move(content);
    clipboard_write_pending_ = true;
    clipboard_write_is_files_ = false;
    clipboard_write_attempts_ = 0;
    clipboard_write_retry_ = Clock::now();
}

void HostServer::drain_peer(HostServerStats& stats) {
    for (const InputEvent& event : session_->take_input_events()) {
        touch_viewer_activity();
        ++stats.input_events;
        if (config_.input_event) {
            config_.input_event(event);
        }
    }
    stats.input_stats = session_->input_stats();
    stats.input_releases = session_->input_release_requests();
    if (!clipboard_transfer_allowed()) {
        return;
    }
    // Establish the new epoch before accepting peer content on a restored
    // grant, otherwise the following poll would cancel that new write.
    if (!clipboard_was_allowed_) {
        clipboard_was_allowed_ = true;
        remember_local_clipboard();
        next_clipboard_poll_ = Clock::now() + clipboard_poll_interval();
    }
    ClipboardContent content;
    while (session_->take_clipboard_content(content)) {
        touch_viewer_activity();
        apply_peer_clipboard(std::move(content), stats);
    }
    // A viewer that only speaks the v15 text message still works: treat its
    // ClipboardUpdate as a text-only item.
    ClipboardUpdate update;
    while (session_->take_clipboard_update(update)) {
        touch_viewer_activity();
        ClipboardContent legacy;
        legacy.seq = update.seq;
        ClipboardFormatData text;
        text.format = ClipboardFormat::Text;
        text.data.assign(update.text.begin(), update.text.end());
        legacy.formats.push_back(std::move(text));
        apply_peer_clipboard(std::move(legacy), stats);
    }
}

void HostServer::take_clipboard_result(ClipboardWorker::Result& result, bool& have_read,
                                       HostServerStats& stats,
                                       std::chrono::steady_clock::time_point now) {
    have_read = false;
    if (!clipboard_worker_ || !clipboard_worker_->Take(result)) return;
    if (result.epoch != clipboard_epoch_) return;
    if (result.operation == ClipboardWorker::Operation::Read) {
        have_read = !clipboard_write_pending_;
        return;
    }
    clipboard_write_inflight_ = false;
    if (!result.written) {
        ++stats.clipboard_write_failures;
        if (++clipboard_write_attempts_ >= 10) {
            clipboard_write_pending_ = false;
            log("clipboard: write failed after 10 attempts");
            if (pending_virtual_files_ && clipboard_channel_) { clipboard_channel_->CancelRemote(); pending_virtual_files_.reset(); }
            if (clipboard_write_is_files_) reset_clipboard_incoming(true);
        }
        clipboard_write_retry_ = now + std::chrono::milliseconds(250);
        return;
    }
    clipboard_write_pending_ = false;
    clipboard_baseline_pending_ = false;
    // The peer's item is on the local clipboard now; never send it back, even
    // if the opening baseline had not gone out yet.
    clipboard_initial_send_ = false;
    if (clipboard_write_is_files_) {
        last_file_entries_ = pending_virtual_files_ ? pending_virtual_files_->entries() : clipboard_incoming_.entries;
        last_file_identity_ = clipboard_file_identity_hash(last_file_entries_);
        last_file_source_version_ = 0;
        last_file_paths_ = pending_clipboard_paths_;
        clipboard_incoming_.active = false;
        pending_virtual_files_.reset();
    } else {
        last_clipboard_hash_ = clipboard_content_hash(pending_clipboard_content_);
        last_clipboard_ = std::move(pending_clipboard_content_);
        last_file_identity_ = 0;
        last_file_entries_.clear();
        last_file_paths_.clear();
    }
    ++stats.clipboard_received;
}

void HostServer::poll_clipboard(std::chrono::steady_clock::time_point now,
                                HostServerStats& stats) {
    const bool clipboard_send = clipboard_allowed() && clipboard_from_host(settings_.clipboard_direction);
    if (clipboard_channel_) clipboard_channel_->Poll(*session_,
        clipboard_send, clipboard_transfer_allowed());
    // Poll cancels the announced capture once sending stops; drop the
    // whitelist with it so a later request cannot name a dead index.
    if (!clipboard_send) clear_clipboard_file_share();
    if (!clipboard_allowed()) {
        if (clipboard_was_allowed_) cancel_clipboard_work();
        clipboard_was_allowed_ = false;
        clipboard_initial_send_ = false;
        return;
    }
    if (!clipboard_was_allowed_) {
        clipboard_was_allowed_ = true;
        remember_local_clipboard();
        next_clipboard_poll_ = now + clipboard_poll_interval();
        return;
    }
    ClipboardWorker::Result result;
    bool have_read = false;
    take_clipboard_result(result, have_read, stats, now);
    if (clipboard_write_pending_) {
        if (!clipboard_write_inflight_ && now >= clipboard_write_retry_ && clipboard_worker_) {
            clipboard_write_inflight_ = pending_virtual_files_
                ? clipboard_worker_->WriteVirtualFiles(clipboard_epoch_, pending_virtual_files_)
                : clipboard_write_is_files_
                ? clipboard_worker_->WriteFiles(clipboard_epoch_, pending_clipboard_paths_)
                : clipboard_worker_->Write(clipboard_epoch_, pending_clipboard_content_);
        }
        return;
    }
    if (!clipboard_from_host(settings_.clipboard_direction)) return;
    if (!have_read) {
        const uint64_t change = ClipboardListener::Generation();
        if (now >= next_clipboard_poll_ ||
            (clipboard_listener_ != 0 && change != clipboard_seen_generation_)) {
            request_local_clipboard();
        }
        return;
    }
    ClipboardSnapshot snapshot = std::move(result.snapshot);
    next_clipboard_poll_ = now + clipboard_poll_interval();
    if (snapshot.state == ClipboardReadState::Failed) {
        // The opening observation failed, so whatever a later read finds is
        // history that predates the session. It may seed the baseline, but it
        // must not feed the one-shot opening send.
        if (clipboard_baseline_pending_) clipboard_initial_send_ = false;
        return;
    }
    const bool has_content = snapshot.state == ClipboardReadState::Content && !snapshot.has_files;
    bool initial_send = false;
    if (clipboard_baseline_pending_) {
        clipboard_baseline_pending_ = false;
        initial_send = clipboard_initial_send_;
        clipboard_initial_send_ = false;
        last_clipboard_hash_ = has_content
                                   ? clipboard_content_hash(snapshot.content) : 0;
        // Copied, not moved: an initial send still needs this snapshot.
        last_clipboard_ = snapshot.content;
        last_file_identity_ = snapshot.has_files
                                  ? clipboard_file_identity_hash(snapshot.entries) : 0;
        last_file_source_version_ = snapshot.local_files ? snapshot.local_files->SourceVersion() : 0;
        last_file_entries_ = snapshot.entries;
        last_file_paths_ = snapshot.paths;
        if (!initial_send || (!has_content && !snapshot.has_files)) return;
    }
    auto& current_files = snapshot.entries;
    auto& current_paths = snapshot.paths;
    if (snapshot.has_files) {
        const uint64_t identity = clipboard_file_identity_hash(current_files);
        last_clipboard_ = ClipboardContent{};
        last_clipboard_hash_ = 0;
        const auto source_version = snapshot.local_files ? snapshot.local_files->SourceVersion() : 0;
        if (identity == 0 || (!initial_send && identity == last_file_identity_ &&
            (!last_file_source_version_ || last_file_source_version_ == source_version))) {
            return;
        }
        last_file_entries_ = std::move(current_files);
        last_file_paths_ = std::move(current_paths);
        last_file_identity_ = identity;
        ClipboardFormatData entry;
        entry.format = ClipboardFormat::FileList;
        entry.data = encode_clipboard_file_list(last_file_entries_);
        if (entry.data.empty()) {
            log("clipboard: the local file list is not sendable");
            return;
        }
        ClipboardContent files;
        files.seq = clipboard_seq_;
        files.formats.push_back(std::move(entry));
        Error sent = session_->send_clipboard_content(std::move(files));
        if (!sent.ok()) {
            ++stats.send_errors;
            log("clipboard files send failed: " + sent.to_string());
            return;
        }
        // Only a list this viewer actually received may be downloaded by
        // index; publishing after the send keeps a failed announce from
        // leaving paths the viewer never saw. The source travels with the
        // whitelist so an index is only served by the capture that announced
        // it.
        {
            std::lock_guard<std::mutex> lock(clipboard_files_mutex_);
            clipboard_file_entries_ = last_file_entries_;
            clipboard_file_paths_ = last_file_paths_;
            clipboard_file_source_ = snapshot.local_files;
        }
        clipboard_channel_->CancelRemote();
        clipboard_channel_->SetLocal(clipboard_seq_, std::move(snapshot.local_files));
        last_file_source_version_ = source_version;
        ++clipboard_seq_;
        ++stats.clipboard_sent;
        return;
    }
    // No files now, so a later text item is a fresh change again.
    if (clipboard_channel_) clipboard_channel_->ClearLocal();
    clear_clipboard_file_share();
    last_file_identity_ = 0;
    last_file_entries_.clear();
    last_file_paths_.clear();
    if (!has_content) {
        return;
    }
    ClipboardContent current = std::move(snapshot.content);
    const uint64_t hash = clipboard_content_hash(current);
    if (hash == last_clipboard_hash_ && !initial_send) {
        return;
    }
    // Remember content we cannot put on the wire (oversized or malformed) so
    // every later tick does not retry the same value.
    last_clipboard_ = current;
    last_clipboard_hash_ = hash;
    std::string content_error;
    if (!valid_clipboard_content(current, &content_error)) {
        log("clipboard: the local item is not sendable: " + content_error);
        return;
    }
    current.seq = clipboard_seq_;
    Error sent = session_->send_clipboard_content(std::move(current));
    if (!sent.ok()) {
        ++stats.send_errors;
        log("clipboard send failed: " + sent.to_string());
        return;
    }
    ++clipboard_seq_;
    ++stats.clipboard_sent;
}

void HostServer::stop_microphone(HostServerStats& stats, const std::string& reason) {
    if (audio_reference_) {
        audio_reference_->set_active(false);
    }
    if (microphone_player_) {
        stats.microphone_underruns = microphone_player_->stats().underruns;
        microphone_player_->stop();
        microphone_player_.reset();
    }
    microphone_decoder_.reset();
    microphone_info_ = AudioInfo{};
    microphone_broken_ = false;
    const bool was_active = stats.microphone_active;
    stats.microphone_active = false;
    if (was_active && !reason.empty()) {
        log("microphone stopped: " + reason);
    }
}

void HostServer::drain_microphone(HostServerStats& stats) {
    stats.microphone_streams = session_->microphone_streams();
    if (!settings_.allow_microphone || stats.microphone_streams == 0) {
        return;
    }
    if ((session_->permissions() & kPermissionMicrophone) == 0) {
        if (microphone_decoder_ || microphone_player_) {
            stop_microphone(stats, "the microphone permission was removed");
        }
        return;
    }
    for (;;) {
        AudioHeader header;
        std::vector<uint8_t> payload;
        AudioInfo info;
        bool format_changed = false;
        Error next = session_->receive_microphone(header, payload, info, format_changed,
                                                  std::chrono::milliseconds(0));
        if (next.status() == Status::WaitTimeout) {
            break;
        }
        if (next.status() == Status::Unavailable) {
            // No stream is open or the viewer closed it: the microphone was
            // switched off, which is not a failure.
            if (microphone_decoder_ || microphone_player_) {
                stop_microphone(stats, "the viewer closed the stream");
            }
            break;
        }
        if (!next.ok()) {
            ++stats.microphone_errors;
            log("microphone receive failed: " + next.to_string());
            stop_microphone(stats, "receive failed");
            break;
        }
        if (format_changed) {
            // A fresh CMIC stream always rebuilds the pipeline, even when the
            // format happens to match the previous one.
            if (microphone_decoder_ || microphone_player_) {
                stop_microphone(stats, {});
            }
            microphone_info_ = info;
            microphone_broken_ = false;
            std::string setup_error;
            microphone_decoder_ = create_audio_decoder(info, &setup_error);
            if (!microphone_decoder_) {
                ++stats.microphone_errors;
                microphone_broken_ = true;
                log("microphone decoder unavailable: " + setup_error);
            } else {
                AudioPlayerConfig player_config;
                // The viewer's capture clock is unrelated to the host's;
                // anchoring blocks on arrival keeps the 80 ms jitter buffer
                // and recovers after a stall without LatencyReply.
                player_config.arrival_anchored = true;
                // The same output as the system-sound capture: what the
                // reference models has to be what the speakers play.
                player_config.device_id = settings_.audio_device;
                if (config_.microphone_player) {
                    microphone_player_ = config_.microphone_player(&setup_error);
                } else {
                    microphone_player_ = create_audio_player(player_config, &setup_error);
                }
                if (!microphone_player_) {
                    ++stats.microphone_errors;
                    microphone_broken_ = true;
                    microphone_decoder_.reset();
                    log("microphone playback unavailable: " + setup_error);
                } else if (Error started = microphone_player_->start(); !started.ok()) {
                    ++stats.microphone_errors;
                    microphone_broken_ = true;
                    microphone_player_.reset();
                    microphone_decoder_.reset();
                    log("microphone playback failed: " + started.to_string());
                } else {
                    if (audio_reference_) {
                        // Only live playback may train the canceller; the
                        // pushed blocks are the reference for loopback.
                        audio_reference_->set_active(true);
                    }
                    stats.microphone_active = true;
                    log("microphone: Opus " +
                        std::to_string(info.sample_rate / 1000u) + " kHz, " +
                        std::to_string(info.channels) + " ch, " +
                        std::to_string(info.frame_samples) + " samples/frame" +
                        (audio_reference_ ? ", feeding loopback AEC" : ""));
                }
            }
        }
        ++stats.microphone_packets;
        stats.microphone_bytes += payload.size();
        if (payload.empty() || microphone_broken_ || !microphone_decoder_ ||
            !microphone_player_) {
            continue;
        }
        std::vector<DecodedAudio> decoded;
        Error decode_error =
            microphone_decoder_->decode(header, payload.data(), payload.size(), decoded);
        if (!decode_error.ok()) {
            ++stats.microphone_errors;
            log("microphone decode failed: " + decode_error.to_string());
            continue;
        }
        bool push_failed = false;
        for (const DecodedAudio& block : decoded) {
            if (block.channels != 0) {
                stats.microphone_frames += block.samples.size() / block.channels;
            }
            if (audio_reference_ && block.channels >= 1 && block.channels <= 2) {
                audio_reference_->push(block.samples.data(),
                                       block.samples.size() / block.channels, block.channels);
            }
            Error pushed = microphone_player_->push(block);
            if (!pushed.ok()) {
                ++stats.microphone_errors;
                log("microphone playback failed: " + pushed.to_string());
                stop_microphone(stats, "playback failed");
                push_failed = true;
                break;
            }
        }
        if (push_failed) {
            break;
        }
        stats.microphone_underruns = microphone_player_->stats().underruns;
        const std::string render_error = microphone_player_->error();
        if (!render_error.empty()) {
            ++stats.microphone_errors;
            log("microphone playback lost the render endpoint: " + render_error);
            stop_microphone(stats, "the render endpoint failed");
            break;
        }
    }
}

void HostServer::stop_voice_call(HostServerStats& stats, const std::string& reason,
                                 bool notify) {
    if (voice_capture_) {
        voice_capture_->stop();
        voice_capture_.reset();
    }
    voice_info_ = AudioInfo{};
    voice_next_seq_ = 0;
    voice_request_id_ = 0;
    const bool was_active = stats.voice_call_active;
    stats.voice_call_active = false;
    if (was_active) {
        ++stats.voice_call_completed;
        if (!reason.empty()) {
            log("voice call stopped: " + reason);
        }
    }
    if (notify) {
        const Error closed = session_->send_voice_call_close();
        if (!closed.ok()) {
            ++stats.voice_call_errors;
            log("voice call close failed: " + closed.to_string());
        }
    }
    // Status readers (the tray and callers of status()) must see the call end
    // even when a static desktop produces no frames to publish them with.
    publish_stats(stats);
}

std::unique_ptr<IMicrophoneCapture> HostServer::create_voice_capture(
    std::string& device_id, std::string* error) {
    MicrophoneCaptureConfig microphone_config;
    microphone_config.device_id = device_id;
    std::unique_ptr<IMicrophoneCapture> capture =
        config_.voice_capture ? config_.voice_capture(microphone_config, error)
                              : create_microphone_capture(microphone_config, error);
    if (!capture && !microphone_config.device_id.empty()) {
        // A pinned device can be unplugged between calls. Capture falls back to
        // the default communications microphone instead of failing; the
        // operator log names the substitute so the recording route does not
        // silently change.
        log("voice call: microphone '" + microphone_config.device_id +
            "' is unavailable; using the default communications device");
        microphone_config.device_id.clear();
        capture = config_.voice_capture
                      ? config_.voice_capture(microphone_config, error)
                      : create_microphone_capture(microphone_config, error);
    }
    if (capture) {
        capture->start();
        if (capture->failed()) {
            if (error != nullptr) {
                *error = capture->error();
            }
            capture->stop();
            capture.reset();
        }
    }
    device_id = microphone_config.device_id;
    return capture;
}

void HostServer::sync_voice_microphone(HostServerStats& stats) {
    std::string requested = settings_.voice_microphone;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(voice_microphone_mutex_);
        if (voice_microphone_pending_) {
            requested = pending_voice_microphone_;
            voice_microphone_pending_ = false;
            have = true;
        }
    }
    if (registry_) {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        if (registry_->voice_microphone_valid) {
            requested = registry_->voice_microphone;
            have = true;
        }
    }
    if (!have || requested == settings_.voice_microphone) {
        return;
    }
    settings_.voice_microphone = requested;
    if (!voice_capture_) {
        return;
    }
    // A revoked call ends below in handle_voice_call_requests(); restarting
    // its capture first would only open a device that is about to close.
    if (!settings_.allow_microphone ||
        (session_->permissions() & kPermissionMicrophone) == 0) {
        return;
    }
    restart_voice_call_microphone(stats);
}

void HostServer::restart_voice_call_microphone(HostServerStats& stats) {
    std::string device_id = settings_.voice_microphone;
    std::string setup_error;
    std::unique_ptr<IMicrophoneCapture> capture = create_voice_capture(device_id, &setup_error);
    if (!capture) {
        ++stats.voice_call_errors;
        log("voice call: cannot switch the host microphone; keeping the previous device" +
            (setup_error.empty() ? std::string() : ": " + setup_error));
        return;
    }
    // The viewer built its decoder from the format in the start answer and the
    // call keeps its sequence space, so a device that captures in a different
    // format cannot take over mid-call.
    const AudioInfo info = capture->info();
    if (info.codec != voice_info_.codec || info.sample_rate != voice_info_.sample_rate ||
        info.channels != voice_info_.channels ||
        info.frame_samples != voice_info_.frame_samples) {
        capture->stop();
        ++stats.voice_call_errors;
        log("voice call: the selected microphone uses a different format; keeping the "
            "previous device");
        return;
    }
    voice_capture_->stop();
    voice_capture_ = std::move(capture);
    voice_info_ = voice_capture_->info();
    log("voice call: host microphone switched to " +
        (device_id.empty() ? std::string("the default communications device")
                           : "'" + device_id + "'"));
}

void HostServer::handle_voice_call_requests(HostServerStats& stats) {
    sync_voice_microphone(stats);
    VoiceCallRequest request;
    while (session_->take_voice_call_request(request)) {
        if (!request.connect) {
            // The transport cleared the call when it queued this stop, so no
            // second close goes back to the viewer.
            stop_voice_call(stats, "the viewer ended the call", false);
            continue;
        }
        ++stats.voice_call_requests;
        if (voice_capture_) {
            VoiceCallResponse busy;
            busy.request_id = request.request_id;
            busy.code = VoiceCallCode::Busy;
            busy.reason = "a voice call is already running";
            const Error sent = session_->send_voice_call_response(busy);
            if (!sent.ok()) {
                ++stats.voice_call_errors;
                log("voice call: cannot refuse an overlapping call: " + sent.to_string());
            }
            continue;
        }
        const bool allowed = settings_.allow_microphone &&
                             (session_->permissions() & kPermissionMicrophone) != 0;
        std::string setup_error;
        std::unique_ptr<IMicrophoneCapture> capture;
        if (allowed) {
            std::string device_id = settings_.voice_microphone;
            capture = create_voice_capture(device_id, &setup_error);
        } else {
            setup_error = "the microphone is not enabled for this session";
        }
        if (!capture) {
            VoiceCallResponse failed;
            failed.request_id = request.request_id;
            failed.code = allowed ? VoiceCallCode::Failed : VoiceCallCode::Refused;
            failed.reason =
                setup_error.empty() ? "no microphone is available" : setup_error;
            ++stats.voice_call_errors;
            const Error sent = session_->send_voice_call_response(failed);
            if (!sent.ok()) {
                log("voice call: cannot answer the request: " + sent.to_string());
            }
            log("voice call: request failed: " + failed.reason);
            continue;
        }
        VoiceCallResponse accepted;
        accepted.request_id = request.request_id;
        accepted.code = VoiceCallCode::Ok;
        accepted.audio = capture->info();
        const Error sent = session_->send_voice_call_response(accepted);
        if (!sent.ok()) {
            ++stats.voice_call_errors;
            capture->stop();
            log("voice call: cannot accept the call: " + sent.to_string());
            continue;
        }
        if ((session_->permissions() & kPermissionMicrophone) == 0) {
            // The transport downgrades an Ok answer to Refused when the
            // permission disappears while the request is in flight.
            capture->stop();
            continue;
        }
        voice_capture_ = std::move(capture);
        voice_info_ = voice_capture_->info();
        voice_next_seq_ = 0;
        voice_request_id_ = request.request_id;
        voice_send_drops_ = 0;
        ++stats.voice_call_accepted;
        stats.voice_call_active = true;
        publish_stats(stats);
        log("voice call: host microphone sending Opus " +
            std::to_string(voice_info_.sample_rate / 1000u) + " kHz, " +
            std::to_string(voice_info_.channels) + " ch");
    }
    if (voice_capture_ && (!settings_.allow_microphone ||
                           (session_->permissions() & kPermissionMicrophone) == 0)) {
        stop_voice_call(stats, "the microphone permission was removed", true);
    }
}

void HostServer::drain_voice_call(HostServerStats& stats) {
    if (!voice_capture_) {
        return;
    }
    if (voice_capture_->failed()) {
        ++stats.voice_call_errors;
        const std::string reason = voice_capture_->error();
        stop_voice_call(stats,
                        "capture: " + (reason.empty() ? std::string("the device failed")
                                                      : reason),
                        true);
        return;
    }
    EncodedAudio packet;
    while (voice_capture_->take_packet(packet)) {
        AudioHeader header;
        header.codec = voice_info_.codec;
        header.sample_rate = voice_info_.sample_rate;
        header.channels = voice_info_.channels;
        header.frame_samples = voice_info_.frame_samples;
        header.pts_ms = packet.capture_ms;
        header.capture_ms = packet.capture_ms;
        const Error sent = session_->send_voice_audio(header, packet.data.data(),
                                                      packet.data.size());
        if (sent.status() == Status::WaitTimeout) {
            ++voice_send_drops_;
            continue;
        }
        if (!sent.ok()) {
            ++stats.voice_call_errors;
            const bool revoked = sent.status() == Status::PermissionDenied;
            const std::string reason = sent.to_string();
            stop_voice_call(stats,
                            revoked ? "the microphone permission was removed"
                                    : "send failed: " + reason,
                            !revoked);
            return;
        }
        ++stats.voice_call_packets;
        stats.voice_call_bytes += packet.data.size();
    }
    const AudioCaptureStats captured = voice_capture_->stats();
    stats.voice_call_dropped = voice_send_drops_ + captured.dropped;
}

bool HostServer::handle_host_actions(HostServerStats& stats, std::string* error,
                                     bool* peer_left) {
    HostControlRequest request;
    while (session_->take_host_action_request(request)) {
        touch_viewer_activity();
        ++stats.host_actions;
        HostActionCode code = HostActionCode::Ok;
        std::string reason;
        auto refuse = [&](HostActionCode next, std::string why) {
            code = next;
            reason = std::move(why);
        };
        const PermissionMask permissions = session_->permissions();
        if (request.action == HostAction::Restart) {
            if ((permissions & kPermissionRestart) == 0) {
                refuse(HostActionCode::Denied, "restart is not permitted by the host");
            } else if (registry_ && !registry_->may_change_host_state(this)) {
                refuse(HostActionCode::Busy, "another viewer controls this host");
            } else if (!settings_.allow_restart) {
                refuse(HostActionCode::Denied, "the host does not allow remote restarts");
            } else {
                std::string platform_error;
                const bool done =
                    config_.host_control
                        ? config_.host_control(HostAction::Restart, false, &platform_error)
                        : restart_host_system(&platform_error);
                if (!done) {
                    refuse(HostActionCode::Failed,
                           platform_error.empty() ? "the restart failed" : platform_error);
                }
            }
        } else if (request.action == HostAction::LockScreen) {
            if ((permissions & kPermissionInput) == 0) {
                refuse(HostActionCode::Denied,
                       "input control is not permitted by the host");
            } else if (registry_ && !registry_->may_change_host_state(this)) {
                refuse(HostActionCode::Busy, "another viewer controls this host");
            } else {
                std::string platform_error;
                const bool done =
                    config_.host_control
                        ? config_.host_control(HostAction::LockScreen, false,
                                               &platform_error)
                        : lock_host_workstation(&platform_error);
                if (!done) {
                    refuse(HostActionCode::Failed,
                           platform_error.empty() ? "the lock failed" : platform_error);
                }
            }
        } else if (request.action == HostAction::BlockInput) {
            if ((permissions & (kPermissionInput | kPermissionBlockInput)) !=
                (kPermissionInput | kPermissionBlockInput)) {
                refuse(HostActionCode::Denied,
                       "blocking local input is not permitted by the host");
            } else if (registry_ && !registry_->may_change_host_state(this)) {
                refuse(HostActionCode::Busy, "another viewer controls this host");
            } else if (!settings_.allow_block_input) {
                refuse(HostActionCode::Denied,
                       "the host does not allow blocking local input");
            } else if (request.block_input != block_input_active_.load()) {
                std::string platform_error;
                const bool done = config_.host_control
                                      ? config_.host_control(HostAction::BlockInput,
                                                             request.block_input,
                                                             &platform_error)
                                      : block_host_input(request.block_input,
                                                         &platform_error);
                if (!done) {
                    refuse(HostActionCode::Failed,
                           platform_error.empty() ? "block input failed" : platform_error);
                } else {
                    block_input_active_.store(request.block_input);
                    stats.block_input_active = request.block_input;
                    log(request.block_input ? "local input is blocked"
                                            : "local input is released");
                }
            }
        } else {
            refuse(HostActionCode::Unsupported, "unknown host action");
        }
        HostControlResult result;
        result.action = request.action;
        // The protocol keeps the state field zero for restart and lock.
        result.block_input = request.action == HostAction::BlockInput
                                 ? block_input_active_.load()
                                 : false;
        result.code = code;
        result.reason = reason;
        if (code == HostActionCode::Ok) {
            if (request.action == HostAction::Restart) {
                log("remote restart requested");
            } else if (request.action == HostAction::LockScreen) {
                log("the workstation was locked by the viewer");
            }
        } else {
            ++stats.host_action_failures;
            log(std::string("host action refused (") + host_action_code_name(code) +
                "): " + (reason.empty() ? "no reason given" : reason));
        }
        const Error sent = session_->send_host_action_result(result);
        if (sent.ok()) {
            // The tray shows the block state and the counters; the periodic
            // publisher alone would leave them stale until the next ABR step.
            publish_stats(stats);
            continue;
        }
        if (sent.status() == Status::Unavailable ||
            sent.status() == Status::WaitTimeout) {
            *peer_left = true;
        } else {
            ++stats.send_errors;
            *error = "host_action_result: " + sent.to_string();
        }
        return false;
    }
    return true;
}

PermissionMask HostServer::policy_permissions() const {
    PermissionMask mask = settings_permissions(settings_);
    if (!config_.service_mode || !config_.list_sessions || !config_.select_session) {
        mask &= ~kPermissionSessionSelect;
    }
    return mask;
}

bool HostServer::sync_remote_policy() {
    bool changed = false;
    if (registry_ && remote_settings_version_ != registry_->remote_settings_version.load()) {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        copy_remote_config_fields(registry_->remote_settings, settings_);
        settings_.lock_after_session_end = registry_->lock_after_session_end.load();
        remote_settings_version_ = registry_->remote_settings_version.load();
        changed = true;
    }
    const Error applied = session_->apply_permission_policy();
    if (!applied.ok()) {
        log("policy cleanup failed: " + applied.to_string());
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    return changed || status_.session_permissions != session_->permissions();
}

bool HostServer::handle_elevation_requests(std::string* error) {
    const auto allowed = [&] {
        return !stopping_.load() && !disconnect_requested_.load() && session_->peer_alive() &&
               session_->requested_mode() == SessionMode::Desktop && settings_.allow_input &&
               (session_->permissions() & kPermissionInput) &&
               (!registry_ || registry_->may_change_host_state(this));
    };
    const auto send = [&](ElevationState state) {
        const auto sent = session_->send_elevation_state(state);
        if (!sent.ok()) { *error = "elevation: " + sent.to_string(); return false; }
        return true;
    };
    uint32_t request = 0;
    while (session_->take_elevation_request(request)) {
        touch_viewer_activity();
        ElevationState state{request, ElevationCode::Unsupported, {}};
        if (!allowed()) {
            state.code = ElevationCode::Denied;
            state.reason = "elevation requires desktop input control";
        } else if (elevation_task_) {
            state.code = ElevationCode::Busy;
            state.reason = "an elevation request is already pending";
        } else {
            state = config_.elevation_state ? config_.elevation_state() :
                ElevationState{0, process_is_elevated() ? ElevationCode::Elevated : ElevationCode::Unsupported, {}};
            state.request_id = request;
            if (state.code == ElevationCode::Available || state.code == ElevationCode::Failed ||
                state.code == ElevationCode::Cancelled) {
                try {
                    elevation_task_ = config_.request_elevation ? config_.request_elevation() : nullptr;
                    if (elevation_task_) {
                        state = elevation_task_->poll();
                        state.request_id = elevation_request_ = request;
                        // Always send Pending before consuming a completed task.
                        if (state.code == ElevationCode::Restarting) state.code = ElevationCode::Pending;
                        elevation_last_code_ = state.code;
                        elevation_deadline_ = Clock::now() + std::chrono::minutes(3);
                    } else state.code = ElevationCode::Unsupported;
                } catch (const std::exception& failure) {
                    state.code = ElevationCode::Failed; state.reason = failure.what();
                }
            }
        }
        if (!send(state)) return false;
    }
    if (!elevation_task_) return true;
    auto state = elevation_task_->poll();
    state.request_id = elevation_request_;
    if (elevation_last_code_ == ElevationCode::Restarting) return true;
    if (!allowed() || Clock::now() >= elevation_deadline_) {
        elevation_task_->cancel();
        state.code = allowed() ? ElevationCode::Cancelled : ElevationCode::Denied;
        state.reason = allowed() ? "the elevation request timed out" : "input control was revoked";
    }
    if (state.code != elevation_last_code_) {
        if (!send(state)) return false;
        elevation_last_code_ = state.code;
    }
    if (state.code == ElevationCode::Restarting) {
        elevation_task_->commit();
    } else if (state.code != ElevationCode::Pending) {
        elevation_task_->cancel();
        elevation_task_.reset();
    }
    return true;
}

bool HostServer::handle_remote_config_requests(HostServerStats& stats, std::string* error,
                                               bool* peer_left) {
    RemoteConfigRequest request;
    while (session_->take_remote_config_request(request)) {
        touch_viewer_activity();
        ++stats.remote_config_requests;
        const PermissionMask permissions = session_->permissions();
        // keys = 0 is a read; any key bit asks to write that switch.
        const bool changing = request.keys != 0;
        RemoteConfigState state;
        state.code = RemoteConfigCode::Ok;
        if ((permissions & kPermissionRemoteConfig) == 0) {
            state.code = RemoteConfigCode::Denied;
            state.reason = "remote configuration is not permitted by the host";
        } else if (changing && !settings_.allow_remote_config) {
            state.code = RemoteConfigCode::Denied;
            state.reason = "the host does not allow remote configuration changes";
        } else if (changing && registry_ && !registry_->may_change_host_state(this)) {
            state.code = RemoteConfigCode::Busy;
            state.reason = "another viewer controls this host";
        } else if (changing) {
            uint32_t changed = 0;
            auto assign = [&request, &changed](uint32_t bit, bool& field) {
                if ((request.keys & bit) == 0) {
                    return;
                }
                const bool value = (request.values & bit) != 0;
                if (field != value) {
                    field = value;
                    changed |= bit;
                }
            };
            assign(kRemoteConfigLockAfterSessionEnd, settings_.lock_after_session_end);
            assign(kRemoteConfigClipboard, settings_.allow_clipboard);
            assign(kRemoteConfigFileTransfer, settings_.allow_file_transfer);
            assign(kRemoteConfigAudio, settings_.allow_audio);
            assign(kRemoteConfigMicrophone, settings_.allow_microphone);
            assign(kRemoteConfigPrivacyMode, settings_.allow_privacy_mode);
            assign(kRemoteConfigRestart, settings_.allow_restart);
            assign(kRemoteConfigBlockInput, settings_.allow_block_input);
            assign(kRemoteConfigOsLogin, settings_.allow_os_login);
            assign(kRemoteConfigSessionSelect, settings_.allow_session_select);
            assign(kRemoteConfigTerminal, settings_.allow_terminal);
            assign(kRemoteConfigPortForward, settings_.allow_port_forward);
            assign(kRemoteConfigCamera, settings_.allow_camera);
            assign(kRemoteConfigRecord, settings_.allow_record);
            if (changed != 0) {
                const PermissionMask revoke = remote_config_revocations(settings_, changed);
                // Publish the transport gate before any resource cleanup or
                // persistence callback can block. Revocation also removes the
                // raw grant so a quick off/on cannot revive existing sessions.
                permission_policy_->store(policy_permissions());
                security_->set_default_permissions(policy_permissions());
                session_->revoke_policy_permissions(revoke);
                std::vector<std::shared_ptr<HostServer>> workers;
                if (registry_) {
                    std::lock_guard<std::mutex> lock(registry_->mutex);
                    copy_remote_config_fields(settings_, registry_->remote_settings);
                    registry_->remote_settings_valid = true;
                    registry_->lock_after_session_end.store(settings_.lock_after_session_end);
                    ++registry_->remote_settings_version;
                    for (const auto& weak : registry_->workers) {
                        if (auto worker = weak.lock()) {
                            workers.push_back(std::move(worker));
                        }
                    }
                }
                for (const auto& worker : workers) {
                    std::lock_guard<std::mutex> lock(worker->session_mutex_);
                    if (worker->session_) {
                        worker->session_->revoke_policy_permissions(revoke);
                    }
                }
                if (config_.settings_changed) {
                    if (registry_) {
                        // The listener may persist from its own thread; the
                        // registry mutex serializes the two writers.
                        std::lock_guard<std::mutex> lock(registry_->callback_mutex);
                        config_.settings_changed(settings_);
                    } else {
                        config_.settings_changed(settings_);
                    }
                }
                log("remote configuration changed by the viewer: " +
                    remote_config_change_names(changed));
            }
        }
        state.flags = remote_config_flags(settings_);
        if (state.code != RemoteConfigCode::Ok) {
            ++stats.remote_config_failures;
            log(std::string("remote configuration refused (") +
                remote_config_code_name(state.code) +
                "): " + (state.reason.empty() ? "no reason given" : state.reason));
        }
        const Error sent = session_->send_remote_config_state(state);
        if (sent.ok()) {
            publish_stats(stats);
            continue;
        }
        if (sent.status() == Status::Unavailable ||
            sent.status() == Status::WaitTimeout) {
            *peer_left = true;
        } else {
            ++stats.send_errors;
            *error = "remote_config_state: " + sent.to_string();
        }
        return false;
    }
    return true;
}

void HostServer::lock_after_session_end_workstation() {
    if (stopping_.load()) {
        return;
    }
    std::string platform_error;
    const bool done = config_.host_control
                          ? config_.host_control(HostAction::LockScreen, false,
                                                 &platform_error)
                          : lock_host_workstation(&platform_error);
    if (done) {
        log("the workstation was locked after the session ended");
    } else {
        log("cannot lock the workstation after the session: " +
            (platform_error.empty() ? "unknown error" : platform_error));
    }
}

bool HostServer::handle_os_login_requests(HostServerStats& stats, std::string* error,
                                          bool* peer_left) {
    std::string password;
    while (session_->take_os_login_request(password)) {
        touch_viewer_activity();
        ++stats.os_login_requests;
        OsLoginCode code = OsLoginCode::Ok;
        std::string reason;
        bool at_lock_screen = false;
        auto refuse = [&](OsLoginCode next, std::string why) {
            code = next;
            reason = std::move(why);
        };
        const PermissionMask permissions = session_->permissions();
        if ((permissions & (kPermissionInput | kPermissionOsLogin)) !=
            (kPermissionInput | kPermissionOsLogin)) {
            refuse(OsLoginCode::Denied, "OS login is not permitted by the host");
        } else if (registry_ && !registry_->may_change_host_state(this)) {
            refuse(OsLoginCode::Busy, "another viewer controls this host");
        } else if (!settings_.allow_os_login) {
            refuse(OsLoginCode::Denied, "the host does not allow OS login");
        } else {
            // Checked here rather than at the transport: a UAC prompt also
            // lives on Winlogon, so only the session state can tell it apart
            // from a lock screen. This runs right before the password is
            // queued to the injection thread.
            const OsLoginStatus logon =
                config_.os_login_state ? config_.os_login_state() : os_login_status();
            if (logon.state == OsLoginState::Unlocked) {
                refuse(OsLoginCode::Unsupported,
                       "the host is not at a logon or lock screen");
            } else if (logon.state == OsLoginState::Unavailable) {
                refuse(OsLoginCode::Unsupported,
                       logon.error.empty() ? "the host cannot type at its logon screen"
                                           : logon.error);
            } else {
                at_lock_screen = logon.state == OsLoginState::Locked;
                std::string platform_error;
                bool typed = false;
                if (config_.os_login) {
                    typed = config_.os_login(password, &platform_error);
                } else {
                    const Error injected = session_->inject_login_text(password);
                    typed = injected.ok();
                    if (!typed) {
                        platform_error = injected.to_string();
                    }
                }
                if (!typed) {
                    refuse(OsLoginCode::Failed,
                           platform_error.empty() ? "the password could not be typed"
                                                  : platform_error);
                }
            }
        }
        // Drop the credential before the answer travels; nothing below or in
        // the result may carry it.
        std::fill(password.begin(), password.end(), '\0');
        password.clear();
        if (code == OsLoginCode::Ok) {
            log(at_lock_screen ? "the OS password was typed at the lock screen"
                               : "the OS password was typed at the logon screen");
        } else {
            ++stats.os_login_failures;
            log(std::string("OS login refused (") + os_login_code_name(code) +
                "): " + (reason.empty() ? "no reason given" : reason));
        }
        const Error sent = session_->send_os_login_result(code, reason);
        if (sent.ok()) {
            publish_stats(stats);
            continue;
        }
        if (sent.status() == Status::Unavailable ||
            sent.status() == Status::WaitTimeout) {
            *peer_left = true;
        } else {
            ++stats.send_errors;
            *error = "os_login_result: " + sent.to_string();
        }
        return false;
    }
    return true;
}

bool HostServer::handle_session_requests(HostServerStats& stats, std::string* error,
                                         bool* peer_left) {
    while (session_->take_session_list_request()) {
        touch_viewer_activity();
        ++stats.session_list_requests;
        SessionListResult result;
        const PermissionMask permissions = session_->permissions();
        if ((permissions & kPermissionSessionSelect) == 0) {
            result.code = SessionListCode::Denied;
            result.reason = "session select is not permitted by the host";
        } else if (!settings_.allow_session_select) {
            result.code = SessionListCode::Denied;
            result.reason = "the host does not allow session select";
        } else if (!config_.list_sessions || !config_.select_session) {
            result.code = SessionListCode::Unsupported;
            result.reason = "this host cannot switch Windows sessions";
        } else {
            std::string list_error;
            if (!config_.list_sessions(result.sessions, &list_error)) {
                result.code = SessionListCode::Failed;
                result.reason =
                    list_error.empty() ? "the session list could not be read" : list_error;
            }
        }
        if (result.code == SessionListCode::Ok) {
            log("session list sent: " + std::to_string(result.sessions.size()) +
                " active session(s)");
        } else {
            log(std::string("session list refused (") +
                session_list_code_name(result.code) +
                "): " + (result.reason.empty() ? "no reason given" : result.reason));
        }
        const Error sent = session_->send_session_list_result(result);
        if (!sent.ok()) {
            if (sent.status() == Status::Unavailable ||
                sent.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "session_list_result: " + sent.to_string();
            }
            return false;
        }
        publish_stats(stats);
    }

    uint32_t select_id = 0;
    while (session_->take_session_select_request(select_id)) {
        ++stats.session_select_requests;
        SessionSelectResult result;
        result.session_id = select_id;
        bool accepted = false;
        bool current = false;
        auto refuse = [&result](SessionSelectCode code, std::string why) {
            result.code = code;
            result.reason = std::move(why);
        };
        const PermissionMask permissions = session_->permissions();
        if ((permissions & kPermissionSessionSelect) == 0) {
            refuse(SessionSelectCode::Denied, "session select is not permitted by the host");
        } else if (!settings_.allow_session_select) {
            refuse(SessionSelectCode::Denied, "the host does not allow session select");
        } else if (!config_.list_sessions || !config_.select_session) {
            refuse(SessionSelectCode::Unsupported,
                   "this host cannot switch Windows sessions");
        } else if (registry_ && !registry_->may_change_host_state(this)) {
            refuse(SessionSelectCode::Busy, "another viewer controls this host");
        } else {
            std::vector<RemoteSession> sessions;
            std::string list_error;
            if (!config_.list_sessions(sessions, &list_error)) {
                refuse(SessionSelectCode::Failed,
                       list_error.empty() ? "the session list could not be read"
                                          : list_error);
            } else {
                const RemoteSession* target = nullptr;
                for (const RemoteSession& entry : sessions) {
                    if (entry.id == select_id) {
                        target = &entry;
                        break;
                    }
                }
                if (target == nullptr) {
                    refuse(SessionSelectCode::NotFound,
                           "the session is not active any more");
                } else if (target->current) {
                    accepted = true;
                    current = true;
                } else if (!config_.select_session(select_id, &result.reason)) {
                    if (result.reason.empty()) {
                        result.reason = "the session switch could not be requested";
                    }
                    result.code = SessionSelectCode::Failed;
                } else {
                    accepted = true;
                }
            }
        }
        if (accepted) {
            result.code = SessionSelectCode::Ok;
            result.reason.clear();
            if (!current) {
                ++stats.session_switches;
                log("session switch accepted: session " + std::to_string(select_id));
            }
        } else {
            ++stats.session_select_failures;
            log(std::string("session select refused (") +
                session_select_code_name(result.code) +
                "): " + (result.reason.empty() ? "no reason given" : result.reason));
        }
        const Error sent = session_->send_session_select_result(result);
        if (!sent.ok()) {
            if (sent.status() == Status::Unavailable ||
                sent.status() == Status::WaitTimeout) {
                *peer_left = true;
            } else {
                ++stats.send_errors;
                *error = "session_select_result: " + sent.to_string();
            }
            return false;
        }
        publish_stats(stats);
        if (accepted && !current) {
            // The answer is queued on the control stream; the backend now
            // stops this worker and restarts it in the target session, so the
            // frame loop must not start another frame.
            stopping_.store(true);
            break;
        }
    }
    return true;
}

bool HostServer::release_block_input(HostServerStats& stats, std::string* error,
                                     bool* peer_left, bool notify) {
    if (!block_input_active_.exchange(false)) {
        return true;
    }
    stats.block_input_active = false;
    std::string release_error;
    const bool released =
        config_.host_control
            ? config_.host_control(HostAction::BlockInput, false, &release_error)
            : block_host_input(false, &release_error);
    log(released ? "local input is released"
                 : "cannot release local input: " +
                       (release_error.empty() ? std::string("no reason given")
                                              : release_error));
    if (!notify) {
        return true;
    }
    HostControlResult result;
    result.action = HostAction::BlockInput;
    result.block_input = false;
    result.code = HostActionCode::Denied;
    result.reason = "the host released local input";
    const Error sent = session_->send_host_action_result(result);
    if (sent.ok()) {
        publish_stats(stats);
        return true;
    }
    if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) {
        *peer_left = true;
    } else {
        ++stats.send_errors;
        *error = "host_action_result: " + sent.to_string();
    }
    return false;
}

void HostServer::release_block_input_local() {
    if (!block_input_active_.exchange(false)) {
        return;
    }
    std::string release_error;
    const bool released =
        config_.host_control
            ? config_.host_control(HostAction::BlockInput, false, &release_error)
            : block_host_input(false, &release_error);
    log(released ? "local input is released"
                 : "cannot release local input: " +
                       (release_error.empty() ? std::string("no reason given")
                                              : release_error));
}

bool HostServer::send_sas_result(HostServerStats& stats, std::string* error, bool* peer_left) {
    ++stats.sas_requests;
    SasCode code = SasCode::Ok;
    std::string reason;
    if ((session_->permissions() & kPermissionInput) == 0) {
        code = SasCode::Denied;
        reason = "input control is not permitted by the host";
    } else if (config_.send_sas) {
        if (!config_.send_sas(&reason)) {
            code = SasCode::Failed;
            if (reason.empty()) {
                reason = "the SAS callback failed";
            }
        }
    } else if (!config_.service_mode) {
        code = SasCode::Unsupported;
        reason = "Ctrl+Alt+Del is only available through the Windows service";
    } else if (!sas_available()) {
        code = SasCode::Unsupported;
        reason = "this Windows installation does not expose SendSAS";
    } else if (!send_secure_attention(&reason)) {
        code = SasCode::Failed;
    }
    if (code == SasCode::Ok) {
        // SendSAS is fire-and-forget: Windows reports no result, so this only
        // records that the request was made.
        log("Ctrl+Alt+Del requested (Windows does not confirm the secure desktop)");
    } else {
        ++stats.sas_failures;
        log(std::string("Ctrl+Alt+Del refused (") + sas_code_name(code) + "): " +
            (reason.empty() ? "no reason given" : reason));
    }
    Error sent = session_->send_sas_result(code, reason);
    if (sent.ok()) {
        return true;
    }
    if (sent.status() == Status::Unavailable || sent.status() == Status::WaitTimeout) {
        *peer_left = true;
    } else {
        ++stats.send_errors;
        *error = "sas_result: " + sent.to_string();
    }
    return false;
}

bool HostServer::sleep_unless_stopping(std::chrono::milliseconds duration) {
    const auto end = Clock::now() + duration;
    while (!stopping_.load()) {
        const auto left = end - Clock::now();
        if (left <= Clock::duration::zero()) {
            return true;
        }
        std::this_thread::sleep_for(std::min(
            std::chrono::duration_cast<std::chrono::milliseconds>(left),
            std::chrono::milliseconds(100)));
    }
    return false;
}

bool HostServer::drain_chat(HostServerStats& stats, std::string* error) {
    std::string text;
    while (session_->take_chat(text)) {
        touch_viewer_activity();
        ++stats.chat_received;
        if (config_.viewer_chat_message) config_.viewer_chat_message(connection_id_, text);
        if (config_.chat_message) config_.chat_message(text);
    }
    std::deque<std::string> outbox;
    { std::lock_guard lock(chat_mutex_); outbox.swap(pending_chat_); }
    for (const auto& line : outbox) {
        auto sent = session_->send_chat(line);
        if (!sent.ok()) {
            ++stats.send_errors;
            *error = "chat: " + sent.to_string();
            return false;
        }
        ++stats.chat_sent;
    }
    return true;
}

void HostServer::log(const std::string& message) {
    if (config_.log) {
        config_.log(log_safe(message));
    }
}

void HostServer::publish_state(HostState state, const std::string& error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = state;
    if (state == HostState::Listening || state == HostState::Streaming) {
        status_.error.clear();
    } else if (!error.empty()) {
        status_.error = error;
    }
}

void HostServer::publish_stats(const HostServerStats& stats) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.stats = stats;
}

void HostServer::publish_stream(const ServerInfo& info, size_t display_index,
                                const std::string& display_name) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.stream = info;
    status_.display_index = display_index;
    status_.display_name = display_name;
    status_.display_merged = info.merged;
}

}  // namespace chast
