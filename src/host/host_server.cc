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
#include <thread>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/bitrate_controller.h"
#include "chast/capture.h"
#include "chast/carrier.h"
#include "chast/clipboard.h"
#include "chast/desktop.h"
#include "chast/display_mode.h"
#include "chast/file_transfer.h"
#include "chast/privacy.h"
#include "chast/sas.h"

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

// Downloads use the same budget the viewer applies to uploads: enough to move
// a large file quickly without starving video, audio and input.
constexpr auto kDownloadSendBudget = std::chrono::milliseconds(8);
constexpr double kDownloadBytesPerSecond = 32.0 * 1024.0 * 1024.0;
constexpr double kDownloadBurst = 256.0 * 1024.0;

std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
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
    }
    return "unknown";
}

uint32_t stream_quality_percent(StreamQuality quality) {
    switch (quality) {
        case StreamQuality::High:
            return 100;
        case StreamQuality::Balanced:
            return 75;
        case StreamQuality::Smooth:
            return 50;
    }
    return 100;
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

uint16_t settings_permissions(const HostSettings& settings) {
    uint16_t mask = 0;
    if (settings.allow_input) {
        mask |= kPermissionInput;
    }
    if (settings.allow_clipboard) {
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
    if (settings.allow_privacy_mode) {
        mask |= kPermissionPrivacyMode;
    }
    return mask;
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
        if (!frame.nv12_texture) {
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
                           uint16_t permissions, const AudioInfo& audio) {
    const DisplayInfo& display = capturer.display();
    ServerInfo info;
    info.codec = encoder.codec();
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
        if (attached[i].device_name == display.device_name) {
            return i;
        }
    }
    return 0;
}

const char* codec_name(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? "hevc" : "h264";
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

std::string permission_names(uint16_t mask) {
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
    return text.empty() ? "view only" : text;
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
    if (!config.clipboard_read) {
        config.clipboard_read = [](ClipboardContent& content) {
            std::string ignored;
            return read_clipboard(content, &ignored);
        };
    }
    if (!config.clipboard_write) {
        config.clipboard_write = [](const ClipboardContent& content) {
            std::string ignored;
            return write_clipboard(content, &ignored);
        };
    }
    std::unique_ptr<HostServer> server(new HostServer(std::move(config)));
    if (server->settings_.access_token.empty()) {
        server->settings_.access_token = generate_access_token();
        if (server->config_.token_changed) {
            server->config_.token_changed(server->settings_.access_token);
        }
    }
    if (!server->open_session(error)) {
        return nullptr;
    }
    // The policy binds signed Hellos to the certificate just loaded, so it
    // exists only once the session knows its fingerprint.
    server->security_ = std::make_unique<HostSecurity>(
        server->security_config(server->session_->server_fingerprint()));
    server->publish_security_status();
    if (server->config_.service_mode && server->settings_.password_hash.empty()) {
        server->log("service mode has no permanent password; every viewer will be refused "
                    "until one is set");
    }
    return server;
}

HostServer::~HostServer() {
    stop();
    carrier_.reset();
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
    std::lock_guard<std::mutex> lock(status_mutex_);
    HostStatus snapshot = status_;
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
    quic.allow_input = settings_.allow_input;
    quic.allow_clipboard = settings_.allow_clipboard;
    quic.allow_file_transfer = settings_.allow_file_transfer;
    quic.allow_audio = settings_.allow_audio;
    quic.allow_microphone = settings_.allow_microphone;
    quic.allow_privacy_mode = settings_.allow_privacy_mode;
    quic.allow_permission_change = settings_.allow_permission_change;
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
        // host.ini stores the secret DPAPI-protected; an early/hand-written
        // value may still be plain base32.
        std::string plain;
        std::string error;
        if (unprotect_totp_secret(config.totp_secret, secret_scope(config_.identity_scope),
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
    config.default_permissions = settings_permissions(settings_);
    config.allow_permission_change = settings_.allow_permission_change;
    config.ip_whitelist = settings_.ip_whitelist;
    config.service_mode = config_.service_mode;
    config.token_auth = settings_.token_auth;
    config.access_token = settings_.access_token;
    config.host_fingerprint = fingerprint;
    // Once the policy exists it is the authority: the operator may have
    // revoked a device since the settings were loaded.
    config.trusted_devices = security_ ? security_->config().trusted_devices
                                       : settings_.trusted_devices;
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
    pending_security_ = security_->evaluate(request.hello, request.peer_address, unix_now());
    const std::string from =
        request.peer_address.empty() ? std::string("an unknown address") : request.peer_address;
    auth.permissions = pending_security_.permissions;
    switch (pending_security_.decision) {
        case HostSecurity::Decision::Allow:
            auth.action = ClientAuthAction::Allow;
            log("admitted " + from + auth_method_suffix(pending_security_));
            commit_security();
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
    commit_security();
    log("the operator approved the viewer from " + from + "; granting " +
        permission_names(request.permissions & approval.permissions));
    return approval;
}

void HostServer::commit_security() {
    if (!security_) {
        return;
    }
    security_->note_connected(pending_security_, unix_now());
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

void HostServer::set_permissions(uint16_t permissions) {
    pending_permissions_.store(permissions & kPermissionAll);
    permission_update_pending_.store(true);
}

bool HostServer::start_file_download(const std::string& utf8_path, std::string* error) {
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
    const uintmax_t size = std::filesystem::file_size(path, code);
    if (code || !std::filesystem::is_regular_file(path, code) || code) {
        return fail("the path is not a readable file");
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
        ++stats.sessions;
        stats.client_connected = true;
        const std::string peer = session_->client_address();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
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

        std::string error;
        bool peer_left = false;
        const bool ok = stream_session(stats, &error, &peer_left);

        ClientStats peer_stats;
        bool stats_received = false;
        const auto finish_timeout = std::chrono::milliseconds(
            ok ? config_.finish_timeout_ms : std::min<uint32_t>(config_.finish_timeout_ms, 2000));
        Error finished = session_->finish_server(finish_timeout, peer_stats, stats_received);
        stats.peer_left = peer_left;
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
            // fails left on purpose, even though the send then reports the
            // peer gone.
            log(peer_left && !stats_received ? "the viewer disappeared" : "the viewer left");
        }
        if (!finished.ok() && !peer_left && error.empty() && !stopping_.load()) {
            error = "finish: " + finished.to_string();
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

        // A spent one-time token must not guard the next connection, so it is
        // replaced while the listener is between viewers.
        if (settings_.token_mode == HostTokenMode::OneTime && !config_.serve_once) {
            settings_.access_token = generate_access_token();
            session_->set_access_token(settings_.access_token);
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.token = settings_.access_token;
            }
            if (config_.token_changed) {
                config_.token_changed(settings_.access_token);
            }
            // The policy holds its own copy of the token, so automation sees
            // the same rotation the legacy caller does.
            refresh_security_config();
            log("the one-time token was spent; a new token is ready");
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

bool HostServer::stream_session(HostServerStats& stats, std::string* error, bool* peer_left) {
    *peer_left = false;
    stats.audio_active = false;
    stats.privacy_mode_active = false;
    stats.privacy_mode_changes = 0;
    stats.sas_requests = 0;
    stats.sas_failures = 0;
    stats.resolution_percent = 100;
    stats.resolution_changes = 0;
    // The last scale the ladder asked for. A merged canvas may cap it lower,
    // and stats.resolution_percent reports what is actually encoded.
    uint16_t resolution_request = 100;
    std::vector<DisplayInfo> attached = enumerate_displays();
    if (attached.empty()) {
        *error = "no displays are attached";
        return false;
    }

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
        if (!follow_input_desktop(&follow_error)) {
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
    capture.nv12.enabled = true;
    capture.nv12.colorimetry = Nv12Colorimetry::Bt601Limited;

    std::unique_ptr<IDxgiCapturer> capturer;
    if (merged_requested) {
        std::string merged_error;
        capturer = create_merged_capturer(attached, capture, &merged_error);
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
            capturer = create_capturer(display_index, capture, error);
        }
    } else {
        capturer = create_capturer(display_index, capture, error);
    }
    if (!capturer) {
        *error = "capturer creation failed: " + *error;
        return false;
    }
    if (!wait_for_usable_frame(*capturer, true, stopping_, std::chrono::seconds(30), error)) {
        return false;
    }
    const FrameInfo& first = capturer->frame();
    // `first` is a view into the capturer's frame and goes stale once a
    // switch swaps the capturer; this is the only field read later.
    const bool initial_pointer_only = !first.desktop_updated;
    stats.resolution_percent = static_cast<uint16_t>(capturer->nv12_scale_percent());
    auto publish_current = [&](const ServerInfo& info) {
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

    EncoderConfig encoder_config;
    encoder_config.codec = settings_.codec;
    encoder_config.bitrate = settings_.bitrate_kbps * 1000u;
    encoder_config.framerate = settings_.framerate;
    encoder_config.quality_ratio = settings_.quality;
    encoder_config.gop_size = 0;
    encoder_config.width = first.nv12_width;
    encoder_config.height = first.nv12_height;
    encoder_config.backend = config_.encoder_backend;
    const AdapterDevice adapter = capturer->adapter_device();
    std::unique_ptr<IVideoEncoder> encoder =
        create_encoder(adapter.device, encoder_config, error);
    if (!encoder) {
        *error = "encoder creation failed: " + *error;
        return false;
    }
    if (encoder->codec() != settings_.codec) {
        log(std::string("the encoder fell back: requested ") + codec_name(settings_.codec) +
            ", producing " + codec_name(encoder->codec()));
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
    const uint16_t granted_permissions = session_->permissions();

    // Privacy mode is per session and the overlay is owned here: every return
    // path destroys it, which is what puts the operator's screen back when the
    // viewer leaves without turning it off first. The transport drops requests
    // when the host did not opt in, and the permission bit can still be
    // revoked mid-session, so both are checked before acting.
    std::unique_ptr<PrivacyOverlay> privacy_overlay;
    bool privacy_active = false;
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
    auto set_privacy_mode = [&](bool wanted) -> bool {
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
    // this action instead of ending the session.
    auto send_sas = [&]() -> bool {
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
            log("Ctrl+Alt+Del sent to the secure desktop");
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
    };
    // The endpoint stays open for the whole session even if audio was denied
    // at approval: the operator can grant it back live, and rebuilding WASAPI
    // mid-session would cost a fresh announcement for no benefit. The same
    // endpoint plays the viewer's microphone, so its playback feeds the echo
    // canceller the audio that actually comes out of this machine.
    std::shared_ptr<AudioReference> audio_reference;
    const bool audio_aec = settings_.allow_audio && settings_.audio_aec;
    if (settings_.allow_audio) {
        if (audio_aec) {
            audio_reference = std::make_shared<AudioReference>();
        }
        std::string audio_error;
        if (config_.audio_capture) {
            audio_capture = config_.audio_capture(&audio_error);
        } else {
            LoopbackCaptureConfig audio_config;
            audio_config.device_id = settings_.audio_device;
            audio_config.reference = audio_reference;
            audio_config.echo_cancellation = audio_aec;
            audio_capture = create_loopback_capture(audio_config, &audio_error);
        }
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
    }
    stats.audio_active =
        audio_info.enabled && (granted_permissions & kPermissionAudio) != 0;

    // The configured bitrate is the whole session's budget, audio included;
    // the video encoder starts at what is left after reserving it. The
    // controller only ever lowers that and later climbs back to it. The base
    // stays fixed for the session: a viewer quality preset scales it, and a
    // rebuilt encoder reuses it instead of reading back its own rate.
    uint32_t base_video_budget =
        video_budget(encoder->bitrate(), bitrate_reserve(audio_info));
    uint32_t video_floor_bps = video_floor(encoder->bitrate());
    const uint16_t configured_fps = static_cast<uint16_t>(encoder->framerate());
    StreamCodecPreference codec_preference = StreamCodecPreference::Auto;
    StreamQuality stream_quality = StreamQuality::High;
    uint16_t stream_fps_cap = 0;
    auto quality_budget = [&](StreamQuality quality) {
        return std::max(
            video_floor_bps,
            static_cast<uint32_t>(static_cast<uint64_t>(base_video_budget) *
                                  stream_quality_percent(quality) / 100u));
    };
    auto fps_ceiling = [&]() -> uint16_t {
        return stream_fps_cap == 0 ? configured_fps
                                   : std::min(configured_fps, stream_fps_cap);
    };
    uint32_t framerate = encoder->framerate();
    BitrateController abr(base_video_budget, video_floor_bps, configured_fps,
                          resolution_request);
    if (abr.ceiling_bps() != encoder->bitrate()) {
        log("video budget " + std::to_string(abr.ceiling_bps() / 1000u) +
            " kbps after reserving audio out of " + std::to_string(encoder->bitrate() / 1000u) +
            " kbps");
        // The announcement below reads the encoder, and it must describe the
        // rate that is actually running.
        Error trimmed = encoder->set_rate(abr.ceiling_bps(), framerate);
        if (!trimmed.ok()) {
            log("cannot apply the video budget: " + trimmed.to_string());
        }
    }
    stats.target_bitrate = encoder->bitrate();
    stats.framerate = static_cast<uint16_t>(framerate);

    ServerInfo stream_info =
        describe_stream(attached, *capturer, *encoder, first.width, first.height,
                        static_cast<uint16_t>(framerate), granted_permissions, audio_info);
    publish_current(stream_info);
    if (config_.stream_announced) {
        config_.stream_announced(stream_info);
    }
    Error announced = session_->announce(stream_info);
    if (!announced.ok()) {
        *error = "announce: " + announced.to_string();
        return false;
    }
    // A UI request made between sessions is applied once the stream exists:
    // set_permissions() is only valid after the announcement.
    uint16_t announced_permissions = granted_permissions;
    auto apply_pending_permissions = [&]() -> bool {
        if (!permission_update_pending_.exchange(false)) {
            return true;
        }
        Error applied = session_->set_permissions(pending_permissions_.load());
        if (!applied.ok()) {
            if (applied.status() != Status::InvalidCall) {
                log("permission update failed: " + applied.to_string());
            }
            return true;
        }
        const uint16_t now_mask = session_->permissions();
        if (now_mask == announced_permissions) {
            return true;
        }
        announced_permissions = now_mask;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.session_permissions = now_mask;
        }
        // A revoked permission must take the mask down with it: leaving the
        // operator's screen black after they turned the feature off would be
        // a privacy mode they cannot cancel from the viewer.
        if (privacy_active && (now_mask & kPermissionPrivacyMode) == 0) {
            privacy_overlay->disable();
            privacy_active = false;
            stats.privacy_mode_active = false;
            ++stats.privacy_mode_changes;
            log("privacy mode is off: the permission was revoked");
            if (!send_privacy_state(false, PrivacyModeCode::Denied,
                                    "privacy mode was turned off by the host")) {
                return false;
            }
        }
        // Audio and clipboard availability travel in the stream description.
        // A viewer that was denied audio at approval has no AAC config yet,
        // so a fresh description is what makes a restore usable.
        stream_info.audio.enabled = audio_info.enabled;
        publish_current(stream_info);
        Error refreshed = session_->refresh_stream(stream_info);
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
    if (audio_capture) {
        audio_capture->start();
    }
    if (!run_clock_started_) {
        run_clock_started_ = true;
        run_start_ = Clock::now();
    }

    const auto start = run_start_;
    // The capture cadence follows the controller's frame rate; 0 disables
    // pacing entirely (capture() then waits on the desktop itself).
    auto frame_duration = [&](uint32_t fps) {
        // settings_.framerate == 0 means the host did not ask for pacing; a
        // viewer cap still needs it, or the cap would only reach the encoder.
        return (settings_.framerate == 0 && stream_fps_cap == 0) || fps == 0
                   ? Clock::duration::zero()
                   : std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(1.0 / static_cast<double>(fps)));
    };
    auto frame_interval = frame_duration(framerate);
    auto next_frame_due = Clock::now() + frame_interval;
    auto last_keyframe = Clock::now();
    const auto next_reannounce = start + std::chrono::seconds(2);
    bool reannounced = false;
    bool force_keyframe = true;
    // After a resize, one extra IDR follows the forced one: StreamInfo and the
    // video frames travel on different QUIC streams, so the viewer can see the
    // first new-size frame before it knows the new size and has to drop it.
    // The second IDR is decodable once the announcement has landed.
    uint32_t extra_idrs = 0;
    bool have_input = false;
    // Set when the GPU refused a scale: the captured NV12 size then stays put
    // for the rest of the session instead of retrying on every sample.
    bool resolution_fixed = false;
    bool probe_pending = config_.resolution_probe_percent != 0;
    std::vector<EncodedPacket> packets;

    // Clipboard baseline: remember what the host clipboard held when the
    // session started and upload only changes after that, never its history.
    // The operator can revoke the permission mid-session, so the same check
    // runs on every poll; re-enabling starts from a fresh baseline instead of
    // flushing whatever changed while the viewer could not see it.
    auto clipboard_allowed = [&] {
        return settings_.allow_clipboard &&
               (session_->permissions() & kPermissionClipboard) != 0;
    };
    bool clipboard_was_allowed = clipboard_allowed();
    ClipboardContent last_clipboard;
    uint64_t last_clipboard_hash = 0;
    auto remember_local_clipboard = [&] {
        ClipboardContent current;
        if (config_.clipboard_read(current)) {
            last_clipboard_hash = clipboard_content_hash(current);
            last_clipboard = std::move(current);
        }
    };
    if (clipboard_was_allowed) {
        remember_local_clipboard();
    }
    uint64_t clipboard_seq = 1;
    auto next_clipboard_poll = Clock::now() + std::chrono::milliseconds(500);

    auto apply_peer_clipboard = [&](ClipboardContent content) {
        const uint64_t hash = clipboard_content_hash(content);
        if (hash == last_clipboard_hash) {
            return;
        }
        std::string content_error;
        if (!valid_clipboard_content(content, &content_error)) {
            log("clipboard: the viewer sent unusable content: " + content_error);
            return;
        }
        if (!config_.clipboard_write(content)) {
            return;
        }
        // Remembering what was applied is what stops the next poll from
        // sending it straight back to the viewer.
        last_clipboard_hash = hash;
        last_clipboard = std::move(content);
        ++stats.clipboard_received;
    };

    auto drain_peer = [&] {
        for (const InputEvent& event : session_->take_input_events()) {
            ++stats.input_events;
            if (config_.input_event) {
                config_.input_event(event);
            }
        }
        stats.input_stats = session_->input_stats();
        stats.input_releases = session_->input_release_requests();
        if (!clipboard_allowed()) {
            return;
        }
        ClipboardContent content;
        while (session_->take_clipboard_content(content)) {
            apply_peer_clipboard(std::move(content));
        }
        // A viewer that only speaks the v15 text message still works: treat
        // its ClipboardUpdate as a text-only item.
        ClipboardUpdate update;
        while (session_->take_clipboard_update(update)) {
            ClipboardContent legacy;
            legacy.seq = update.seq;
            ClipboardFormatData text;
            text.format = ClipboardFormat::Text;
            text.data.assign(update.text.begin(), update.text.end());
            legacy.formats.push_back(std::move(text));
            apply_peer_clipboard(std::move(legacy));
        }
    };

    auto poll_clipboard = [&](Clock::time_point now) {
        const bool allowed = clipboard_allowed();
        if (!allowed) {
            clipboard_was_allowed = false;
            return;
        }
        if (!clipboard_was_allowed) {
            clipboard_was_allowed = true;
            remember_local_clipboard();
            next_clipboard_poll = now + std::chrono::milliseconds(500);
            return;
        }
        if (now < next_clipboard_poll) {
            return;
        }
        next_clipboard_poll = now + std::chrono::milliseconds(500);
        ClipboardContent current;
        if (!config_.clipboard_read(current)) {
            return;
        }
        const uint64_t hash = clipboard_content_hash(current);
        if (hash == last_clipboard_hash) {
            return;
        }
        // Remember content we cannot put on the wire (oversized or
        // malformed) so every later tick does not retry the same value.
        last_clipboard = current;
        last_clipboard_hash = hash;
        std::string content_error;
        if (!valid_clipboard_content(current, &content_error)) {
            log("clipboard: the local item is not sendable: " + content_error);
            return;
        }
        current.seq = clipboard_seq;
        Error sent = session_->send_clipboard_content(std::move(current));
        if (!sent.ok()) {
            ++stats.send_errors;
            log("clipboard send failed: " + sent.to_string());
            return;
        }
        ++clipboard_seq;
        ++stats.clipboard_sent;
    };

    // Viewer uploads. One transfer at a time: FileReceiver owns the .part
    // file and the answer goes out only after the name, the free space and the
    // resume state checked out.
    const std::string receive_directory = settings_.file_receive_directory.empty()
                                              ? default_receive_directory()
                                              : settings_.file_receive_directory;
    auto file_transfer_allowed = [&] {
        return settings_.allow_file_transfer &&
               (session_->permissions() & kPermissionFileTransfer) != 0;
    };
    struct ActiveUpload {
        FileOffer offer;
        std::unique_ptr<FileReceiver> receiver;
        std::chrono::steady_clock::time_point answer_time;
        uint64_t written = 0;
    };
    std::unique_ptr<ActiveUpload> upload;

    // `report=false` is for a revocation the transport already announced to
    // the viewer; sending a second FileResult would only be noise.
    auto end_upload = [&](FileCode code, const std::string& reason, bool keep_partial,
                          bool report = true) {
        if (!upload) {
            return;
        }
        const uint32_t transfer_id = upload->offer.transfer_id;
        const uint64_t written = upload->written;
        const std::string name =
            upload->receiver ? upload->receiver->name() : upload->offer.name;
        if (upload->receiver) {
            upload->receiver->abort(keep_partial);
        }
        upload.reset();
        if (code != FileCode::Ok) {
            ++stats.file_failures;
            log("file: " + name + " failed: " + reason);
        }
        if (!report) {
            return;
        }
        Error sent = session_->send_file_result(transfer_id, code, written, reason);
        if (!sent.ok()) {
            log("file: cannot report the transfer result: " + sent.to_string());
        }
    };

    auto drain_file_offers = [&] {
        FileOffer offer;
        while (session_->take_file_offer(offer)) {
            if (!file_transfer_allowed()) {
                continue;
            }
            if (upload) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Busy, 0,
                                                 "another transfer is running");
                continue;
            }
            if (receive_directory.empty()) {
                ++stats.file_failures;
                (void)session_->send_file_answer(offer.transfer_id, FileCode::Refused, 0,
                                                 "the host has no receive directory");
                log("file: refused " + offer.name + ": no receive directory is configured");
                continue;
            }
            FileReceiveOptions options;
            options.directory = receive_directory;
            options.name = offer.name;
            options.relative_path = offer.relative_path;
            options.size = offer.size;
            options.sha256 = offer.sha256;
            options.allow_resume = offer.resume;
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
            upload = std::move(active);
            Error answered = session_->send_file_answer(transfer_id, FileCode::Ok,
                                                        resume_offset);
            if (!answered.ok()) {
                upload.reset();
                ++stats.file_failures;
                log("file: cannot accept " + offered_path + ": " + answered.to_string());
                continue;
            }
            log("file: receiving " + offered_path + " (" +
                std::to_string(upload->offer.size) + " bytes" +
                (resume_offset > 0 ? ", resuming at " + std::to_string(resume_offset) : "") +
                ")");
        }
    };

    // Moves every queued chunk into the .part file and turns the transport's
    // FIN into a digest check and the final rename.
    auto drain_file_data = [&] {
        if (!upload) {
            return;
        }
        const uint32_t transfer_id = upload->offer.transfer_id;
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
                    if (Clock::now() - upload->answer_time < std::chrono::seconds(10)) {
                        return;
                    }
                    end_upload(FileCode::IoError, "the viewer never opened the file stream",
                               true);
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
                           true, !revoked);
                return;
            }
            if (!got.ok()) {
                end_upload(FileCode::IoError, got.to_string(), true);
                return;
            }
            if (chunk.transfer_id != transfer_id) {
                end_upload(FileCode::IoError, "the upload stream carried another transfer", true);
                return;
            }
            if (chunk.offset != upload->written) {
                end_upload(FileCode::IoError, "the upload is not contiguous", false);
                return;
            }
            std::string write_error;
            if (!upload->receiver->write(chunk.data.data(), chunk.data.size(), &write_error)) {
                end_upload(FileCode::IoError, write_error, false);
                return;
            }
            upload->written += chunk.data.size();
        }
        FileUploadState state;
        if (!session_->file_upload_state(transfer_id, state)) {
            if (Clock::now() - upload->answer_time < std::chrono::seconds(10)) {
                return;
            }
            end_upload(FileCode::IoError, "the viewer never opened the file stream", true);
            return;
        }
        if (!state.error.empty()) {
            const bool revoked = state.error == "file transfer was disabled";
            end_upload(revoked ? FileCode::Cancelled : FileCode::IoError, state.error, true,
                       !revoked);
            return;
        }
        if (!state.finished) {
            return;
        }
        std::string final_path;
        std::string finish_error;
        if (!upload->receiver->finish(final_path, &finish_error)) {
            // Digest mismatch already removed the partial file; a rename
            // failure kept it for a resume.
            end_upload(FileCode::IoError, finish_error, true);
            return;
        }
        const uint64_t bytes = upload->written;
        upload->receiver.reset();
        upload.reset();
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
    };

    auto drain_file_cancel = [&] {
        FileCancel cancel;
        while (session_->take_file_cancel(cancel)) {
            if (!upload || cancel.transfer_id != upload->offer.transfer_id) {
                continue;
            }
            end_upload(FileCode::Cancelled,
                       cancel.reason.empty() ? "the viewer cancelled the transfer"
                                             : cancel.reason,
                       false);
        }
    };

    // Operator downloads. The tray queues a path; hashing runs on its own
    // thread so the frame loop never waits for a large file, then the offer
    // and the chunks use the same budget as the viewer's uploads.
    std::ifstream download_stream;
    double download_tokens = kDownloadBurst;
    Clock::time_point download_token_time = Clock::now();

    // Joins the hashing thread on every exit path and leaves a truthful
    // terminal state for the tray.
    struct DownloadCleanup {
        HostServer* self;
        ~DownloadCleanup() {
            if (self->send_hasher_.joinable()) {
                self->send_hasher_.join();
            }
            bool marked = false;
            {
                std::lock_guard<std::mutex> lock(self->download_mutex_);
                switch (self->send_.stage) {
                    case FileSend::Stage::kHashing:
                    case FileSend::Stage::kReady:
                    case FileSend::Stage::kOffered:
                    case FileSend::Stage::kSending:
                    case FileSend::Stage::kFinishing:
                        self->send_.stage = FileSend::Stage::kFailed;
                        self->send_.error = "the session ended";
                        self->send_.counted = true;
                        marked = true;
                        break;
                    default:
                        break;
                }
            }
            if (marked) {
                self->publish_download();
            }
        }
    } download_cleanup{this};

    auto send_fail = [&](const std::string& message, bool report) {
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
        download_stream.close();
        ++stats.file_send_failures;
        if (report) {
            // The receiver would otherwise wait for a stream that never ends.
            (void)session_->send_file_cancel(transfer_id, FileCode::Cancelled, message);
            session_->cancel_file_upload(transfer_id);
        }
        log("file: sending " + name + " failed: " + message);
        publish_download();
        publish_stats(stats);
    };

    auto drive_file_download = [&] {
        FileSend snapshot;
        {
            std::lock_guard<std::mutex> lock(download_mutex_);
            snapshot = send_;
        }

        if (snapshot.stage == FileSend::Stage::kNone ||
            snapshot.stage == FileSend::Stage::kDone ||
            snapshot.stage == FileSend::Stage::kFailed) {
            std::string requested;
            {
                std::lock_guard<std::mutex> lock(download_mutex_);
                const bool idle = send_.stage == FileSend::Stage::kNone ||
                                  send_.stage == FileSend::Stage::kDone ||
                                  send_.stage == FileSend::Stage::kFailed;
                if (idle && !pending_download_path_.empty()) {
                    requested = std::move(pending_download_path_);
                    pending_download_path_.clear();
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
                send_.name.assign(reinterpret_cast<const char*>(u8name.data()), u8name.size());
                send_.transfer_id = next_send_id_++;
                transfer_id = send_.transfer_id;
            }
            send_hasher_ = std::thread([this, transfer_id] {
                std::string path;
                {
                    std::lock_guard<std::mutex> lock(download_mutex_);
                    path = send_.path;
                }
                uint64_t size = 0;
                std::array<uint8_t, 32> digest{};
                std::string error;
                bool ok = sha256_file(path, size, digest, &error);
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
            bool first = false;
            {
                std::lock_guard<std::mutex> lock(download_mutex_);
                if (send_.stage == FileSend::Stage::kFailed && !send_.counted) {
                    send_.counted = true;
                    first = true;
                }
            }
            if (first) {
                ++stats.file_send_failures;
                log("file: " + snapshot.name + " failed: " + snapshot.error);
                publish_download();
                publish_stats(stats);
            }
            return;
        }

        if (!file_transfer_allowed()) {
            send_fail("file transfer was disabled", true);
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
            download_stream.close();
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
                send_fail(result.reason.empty() ? "the viewer rejected the file"
                                                : result.reason,
                          false);
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
                send_fail(cancel.reason.empty() ? "the viewer cancelled the download"
                                                : cancel.reason,
                          false);
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
                Error sent = session_->offer_file(offer);
                if (!sent.ok()) {
                    send_fail("offer: " + sent.to_string(), false);
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
                    if (answer.code != FileCode::Ok) {
                        send_fail(answer.reason.empty() ? "the viewer refused the file"
                                                        : answer.reason,
                                  false);
                        return;
                    }
                    if (answer.resume_offset > snapshot.size) {
                        send_fail("the viewer asked to resume past the end of the file",
                                  true);
                        return;
                    }
                    download_stream.open(path_from_utf8(snapshot.path), std::ios::binary);
                    if (!download_stream) {
                        send_fail("the local file cannot be opened", false);
                        return;
                    }
                    download_stream.seekg(static_cast<std::streamoff>(answer.resume_offset),
                                          std::ios::beg);
                    if (!download_stream) {
                        send_fail("the local file cannot seek to the resume offset", false);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(download_mutex_);
                        if (send_.transfer_id == snapshot.transfer_id &&
                            send_.stage == FileSend::Stage::kOffered) {
                            send_.stage = FileSend::Stage::kSending;
                            send_.resume_offset = answer.resume_offset;
                            send_.sent = answer.resume_offset;
                        }
                    }
                    download_tokens = kDownloadBurst;
                    download_token_time = Clock::now();
                    log("file: sending " + snapshot.name + " from " +
                        std::to_string(answer.resume_offset));
                    publish_download();
                    return;
                }
                if (Clock::now() - snapshot.answer_time > std::chrono::seconds(30)) {
                    send_fail("the viewer did not answer the offer", false);
                }
                return;
            }
            case FileSend::Stage::kSending: {
                const auto deadline = Clock::now() + kDownloadSendBudget;
                const auto refill = Clock::now();
                download_tokens = std::min(
                    kDownloadBurst,
                    download_tokens +
                        std::chrono::duration<double>(refill - download_token_time).count() *
                            kDownloadBytesPerSecond);
                download_token_time = refill;
                std::array<uint8_t, kFileChunkBytes> buffer{};
                uint64_t sent_bytes = snapshot.sent;
                while (sent_bytes < snapshot.size && Clock::now() < deadline) {
                    const size_t want = static_cast<size_t>(std::min<uint64_t>(
                        snapshot.size - sent_bytes, buffer.size()));
                    if (download_tokens < static_cast<double>(want)) {
                        break;
                    }
                    download_stream.read(reinterpret_cast<char*>(buffer.data()),
                                         static_cast<std::streamsize>(want));
                    const std::streamsize got = download_stream.gcount();
                    if (got <= 0) {
                        send_fail("the local file ended before its announced size", true);
                        return;
                    }
                    Error wrote = session_->try_send_file_data(
                        snapshot.transfer_id, buffer.data(), static_cast<size_t>(got));
                    if (wrote.status() == Status::WaitTimeout) {
                        // The shared queue is full: leave the piece for the
                        // next pass rather than stall the frame loop.
                        download_stream.clear();
                        download_stream.seekg(static_cast<std::streamoff>(sent_bytes),
                                              std::ios::beg);
                        break;
                    }
                    if (!wrote.ok()) {
                        send_fail("send: " + wrote.to_string(), true);
                        return;
                    }
                    sent_bytes += static_cast<uint64_t>(got);
                    download_tokens -= static_cast<double>(got);
                    std::lock_guard<std::mutex> lock(download_mutex_);
                    if (send_.transfer_id == snapshot.transfer_id &&
                        send_.stage == FileSend::Stage::kSending) {
                        send_.sent = sent_bytes;
                    }
                }
                if (sent_bytes >= snapshot.size) {
                    Error finished = session_->finish_file_upload(snapshot.transfer_id);
                    if (!finished.ok()) {
                        send_fail("finish: " + finished.to_string(), true);
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
    };

    // Audio degrades on its own: a failed endpoint, encoder or send turns the
    // stream off with a StreamInfo and leaves video running.
    auto disable_audio = [&](const std::string& reason) {
        audio_capture->stop();
        audio_capture.reset();
        audio_info = AudioInfo{};
        stats.audio_active = false;
        stream_info.audio = audio_info;
        publish_current(stream_info);
        Error refreshed = session_->refresh_stream(stream_info);
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
        if ((session_->permissions() & kPermissionAudio) == 0) {
            // Keep the endpoint warm so a restore resumes immediately, but
            // encode nothing the viewer may not receive.
            EncodedAudio dropped;
            while (audio_capture->take_packet(dropped)) {
            }
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
            Error sent = session_->send_audio(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == Status::PermissionDenied) {
                    // The operator revoked audio between the check above and
                    // the send; the next tick sees the new mask.
                    continue;
                }
                if (sent.status() == Status::WaitTimeout) {
                    // The shared send queue is full; video owns the path and
                    // this packet is a short gap, not a reason to stop audio.
                    ++audio_send_drops;
                    continue;
                }
                ++stats.audio_errors;
                log("audio send failed: " + sent.to_string());
                disable_audio("send failed");
                return;
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

    // Viewer microphone: CMIC carries Opus packets the viewer captured. The
    // decoder and player follow the stream, not the session: they are created
    // when the viewer switches the microphone on and released when it goes
    // off, so an idle session never holds the render endpoint. There is no
    // echo cancellation; the host simply plays what the viewer sends.
    std::unique_ptr<IAudioDecoder> microphone_decoder;
    std::unique_ptr<IAudioPlayer> microphone_player;
    AudioInfo microphone_info;
    bool microphone_broken = false;
    auto stop_microphone = [&](const std::string& reason) {
        if (audio_reference) {
            audio_reference->set_active(false);
        }
        if (microphone_player) {
            stats.microphone_underruns = microphone_player->stats().underruns;
            microphone_player->stop();
            microphone_player.reset();
        }
        microphone_decoder.reset();
        microphone_info = AudioInfo{};
        microphone_broken = false;
        const bool was_active = stats.microphone_active;
        stats.microphone_active = false;
        if (was_active && !reason.empty()) {
            log("microphone stopped: " + reason);
        }
    };
    auto drain_microphone = [&] {
        stats.microphone_streams = session_->microphone_streams();
        if (!settings_.allow_microphone || stats.microphone_streams == 0) {
            return;
        }
        if ((session_->permissions() & kPermissionMicrophone) == 0) {
            if (microphone_decoder || microphone_player) {
                stop_microphone("the microphone permission was removed");
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
                // No stream is open or the viewer closed it: the microphone
                // was switched off, which is not a failure.
                if (microphone_decoder || microphone_player) {
                    stop_microphone("the viewer closed the stream");
                }
                break;
            }
            if (!next.ok()) {
                ++stats.microphone_errors;
                log("microphone receive failed: " + next.to_string());
                stop_microphone("receive failed");
                break;
            }
            if (format_changed) {
                // A fresh CMIC stream always rebuilds the pipeline, even when
                // the format happens to match the previous one.
                if (microphone_decoder || microphone_player) {
                    stop_microphone({});
                }
                microphone_info = info;
                microphone_broken = false;
                std::string setup_error;
                microphone_decoder = create_audio_decoder(info, &setup_error);
                if (!microphone_decoder) {
                    ++stats.microphone_errors;
                    microphone_broken = true;
                    log("microphone decoder unavailable: " + setup_error);
                } else {
                    AudioPlayerConfig player_config;
                    // The viewer's capture clock is unrelated to the host's;
                    // anchoring blocks on arrival keeps the 80 ms jitter
                    // buffer and recovers after a stall without LatencyReply.
                    player_config.arrival_anchored = true;
                    // The same output as the system-sound capture: what the
                    // reference models has to be what the speakers play.
                    player_config.device_id = settings_.audio_device;
                    if (config_.microphone_player) {
                        microphone_player = config_.microphone_player(&setup_error);
                    } else {
                        microphone_player = create_audio_player(player_config, &setup_error);
                    }
                    if (!microphone_player) {
                        ++stats.microphone_errors;
                        microphone_broken = true;
                        microphone_decoder.reset();
                        log("microphone playback unavailable: " + setup_error);
                    } else if (Error started = microphone_player->start(); !started.ok()) {
                        ++stats.microphone_errors;
                        microphone_broken = true;
                        microphone_player.reset();
                        microphone_decoder.reset();
                        log("microphone playback failed: " + started.to_string());
                    } else {
                        if (audio_reference) {
                            // Only live playback may train the canceller; the
                            // pushed blocks are the reference for loopback.
                            audio_reference->set_active(true);
                        }
                        stats.microphone_active = true;
                        log("microphone: Opus " +
                            std::to_string(info.sample_rate / 1000u) + " kHz, " +
                            std::to_string(info.channels) + " ch, " +
                            std::to_string(info.frame_samples) + " samples/frame" +
                            (audio_reference ? ", feeding loopback AEC" : ""));
                    }
                }
            }
            ++stats.microphone_packets;
            stats.microphone_bytes += payload.size();
            if (payload.empty() || microphone_broken || !microphone_decoder ||
                !microphone_player) {
                continue;
            }
            std::vector<DecodedAudio> decoded;
            Error decode_error =
                microphone_decoder->decode(header, payload.data(), payload.size(), decoded);
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
                if (audio_reference && block.channels >= 1 && block.channels <= 2) {
                    audio_reference->push(block.samples.data(),
                                          block.samples.size() / block.channels,
                                          block.channels);
                }
                Error pushed = microphone_player->push(block);
                if (!pushed.ok()) {
                    ++stats.microphone_errors;
                    log("microphone playback failed: " + pushed.to_string());
                    stop_microphone("playback failed");
                    push_failed = true;
                    break;
                }
            }
            if (push_failed) {
                break;
            }
            stats.microphone_underruns = microphone_player->stats().underruns;
            const std::string render_error = microphone_player->error();
            if (!render_error.empty()) {
                ++stats.microphone_errors;
                log("microphone playback lost the render endpoint: " + render_error);
                stop_microphone("the render endpoint failed");
                break;
            }
        }
    };

    // Pointer state. Desktop Duplication keeps the pointer out of the desktop
    // image, so the viewer is told the sprite and where it is; the sprite
    // travels once per shape id and later updates carry only the position.
    // The cadence comes from capture(): DXGI produces a frame for every
    // pointer move, so no timer is needed and an idle pointer costs nothing.
    bool cursor_sent_once = false;
    CursorUpdate cursor_sent;
    auto publish_cursor = [&]() {
        if (!config_.capture_cursor) {
            return;
        }
        const CursorInfo& info = capturer->cursor();
        if (!info.position_valid) {
            return;
        }
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
        } else if (cursor_sent_once) {
            update.shape_id = cursor_sent.shape_id;
            update.width = cursor_sent.width;
            update.height = cursor_sent.height;
            update.hotspot_x = cursor_sent.hotspot_x;
            update.hotspot_y = cursor_sent.hotspot_y;
        } else {
            return;
        }
        const bool new_shape = shape_known && update.shape_id != cursor_sent.shape_id;
        if (new_shape) {
            update.sprite = info.bitmap;
        }
        if (cursor_sent_once && !new_shape && update.visible == cursor_sent.visible &&
            update.x == cursor_sent.x && update.y == cursor_sent.y) {
            return;
        }
        Error sent = session_->send_cursor(update);
        if (!sent.ok()) {
            if (sent.status() == Status::WaitTimeout) {
                // The control queue is full; the next frame carries the same
                // state again, so nothing is lost by skipping this one.
                ++stats.cursor_dropped;
                return;
            }
            ++stats.cursor_errors;
            if (stats.cursor_errors <= 3) {
                log("cursor send failed: " + sent.to_string());
            }
            return;
        }
        ++stats.cursor_sent;
        if (new_shape) {
            ++stats.cursor_shapes;
            stats.cursor_bytes += update.sprite.size();
        }
        cursor_sent = std::move(update);
        cursor_sent_once = true;
    };

    auto process_frame = [&](uint64_t pts_ms, uint64_t capture_ms) -> bool {
        const FrameInfo& frame = capturer->frame();
        if (!frame.nv12_texture) {
            return true;
        }
        if (frame.rotation_degrees != 0) {
            *error = "capture lost rotation while streaming";
            return false;
        }
        packets.clear();
        Error err = Error::success();
        if (!frame.nv12_updated && have_input) {
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

        for (const EncodedPacket& packet : packets) {
            if (packet.data.empty()) {
                continue;
            }
            FrameHeader header;
            header.pts_ms = packet.pts_ms;
            header.width = encoder->width();
            header.height = encoder->height();
            header.codec = encoder->codec();
            header.keyframe = packet.keyframe;
            header.idr = packet.idr;
            header.capture_ms = capture_ms;
            Error sent = session_->send_frame(header, packet.data.data(), packet.data.size());
            if (!sent.ok()) {
                if (sent.status() == Status::Unavailable ||
                    sent.status() == Status::WaitTimeout) {
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

    // A viewer can ask for another output without reconnecting. The new
    // capturer and encoder are built beside the running pair and swapped in
    // only after both produced a picture, so a failed switch leaves the old
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

        if ((session_->permissions() & kPermissionInput) == 0) {
            return refuse(DisplaySwitchCode::Failed, "input control is disabled");
        }
        if (switched_once && now - last_switch_at < std::chrono::seconds(1)) {
            return answer(DisplaySwitchCode::Busy, "a display switch just happened");
        }

        // Re-enumerate: a hotplug can renumber the outputs since HelloAck.
        std::vector<DisplayInfo> fresh = enumerate_displays();
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
        if (merged_target && capturer->merged()) {
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
        } else if (!merged_target && !capturer->merged()) {
            already_streaming = fresh[target].device_name == capturer->display().device_name;
        }
        if (already_streaming) {
            switched_once = true;
            last_switch_at = now;
            ++stats.display_switches;
            log("display switch: already streaming " +
                streamed_display_name(*capturer));
            return answer(DisplaySwitchCode::Ok, std::string());
        }

        std::string switch_error;
        std::unique_ptr<IDxgiCapturer> next;
        if (merged_target) {
            next = create_merged_capturer(fresh, capture, &switch_error);
            if (!next) {
                return refuse(DisplaySwitchCode::Failed,
                              "cannot merge " + std::to_string(fresh.size()) +
                                  " displays: " + switch_error);
            }
        } else {
            next = create_capturer(target, capture, &switch_error);
            if (!next) {
                return refuse(DisplaySwitchCode::Failed,
                              "cannot capture display " + std::to_string(target) + ": " +
                                  switch_error);
            }
        }
        // Carry the ladder's request across the switch. A merged canvas can
        // cap it lower, so the effective value is read back after the first
        // frame either way.
        if (resolution_request != 100) {
            Error scaled = next->set_nv12_scale(resolution_request);
            if (!scaled.ok()) {
                log("display switch: cannot scale to " + std::to_string(resolution_request) +
                    "%; using 100%: " + scaled.to_string());
                resolution_request = 100;
            }
        }
        if (!wait_for_usable_frame(*next, true, stopping_, std::chrono::milliseconds(3000),
                                   &switch_error)) {
            return refuse(DisplaySwitchCode::Failed,
                          "the new output produced no frame: " + switch_error);
        }
        const uint16_t previous_percent = stats.resolution_percent;
        stats.resolution_percent = static_cast<uint16_t>(next->nv12_scale_percent());
        if (stats.resolution_percent != previous_percent) {
            ++stats.resolution_changes;
        }
        const FrameInfo& next_frame = next->frame();
        EncoderConfig next_config = encoder_config;
        next_config.width = next_frame.nv12_width;
        next_config.height = next_frame.nv12_height;
        const AdapterDevice next_adapter = next->adapter_device();
        std::unique_ptr<IVideoEncoder> next_encoder =
            create_encoder(next_adapter.device, next_config, &switch_error);
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
        // points into the capturer the move below hands to `capturer`.
        capturer = std::move(next);
        encoder = std::move(next_encoder);
        attached = std::move(fresh);
        have_input = false;
        force_keyframe = true;
        extra_idrs = 1;
        // The new output has its own pointer shape and position; the viewer
        // must not keep placing the old sprite.
        cursor_sent_once = false;
        cursor_sent = CursorUpdate{};
        const size_t new_index =
            capturer->merged() ? size_t{0} : find_display(attached, capturer->display());
        stream_info = describe_stream(attached, *capturer, *encoder, next_frame.width,
                                      next_frame.height, static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info);
        publish_current(stream_info);
        if (config_.stream_announced) {
            config_.stream_announced(stream_info);
        }
        if (!answer(DisplaySwitchCode::Ok, std::string())) {
            return false;
        }
        Error refreshed = session_->refresh_stream(stream_info);
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

    // Mode queries and changes for the streamed output. A list request is
    // answered from a fresh enumeration; a change is a synchronous rebuild:
    // the mode is applied, capture is recreated at the new size, a new encoder
    // is built and announced, and only then does the viewer get Ok. Any
    // failure puts the old mode back and leaves the old stream running.
    auto send_mode_list = [&](uint16_t requested_index) -> bool {
        DisplayModeList answer_list;
        answer_list.display = requested_index;
        std::vector<DisplayInfo> fresh = enumerate_displays();
        std::string list_error;
        if (capturer->merged()) {
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
        if (mode_changed_once && now - last_mode_change_at < std::chrono::seconds(1)) {
            return answer(DisplayModeCode::Busy, "a display mode change just happened");
        }
        if (capturer->merged()) {
            return refuse(DisplayModeCode::Failed,
                          "the merged view does not support single display modes");
        }

        // Re-enumerate like the switch path: a hotplug can renumber outputs.
        std::vector<DisplayInfo> fresh = enumerate_displays();
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
            force_keyframe = true;
            attached = enumerate_displays();
            return refuse(DisplayModeCode::Failed, why);
        };

        if (!rebuild_after_mode_change(*wanted, true, &mode_error)) {
            return rollback("the display produced no frame in the new mode: " + mode_error);
        }
        const FrameInfo& mode_frame = capturer->frame();
        EncoderConfig mode_config = encoder_config;
        mode_config.width = mode_frame.nv12_width;
        mode_config.height = mode_frame.nv12_height;
        const AdapterDevice mode_adapter = capturer->adapter_device();
        std::unique_ptr<IVideoEncoder> mode_encoder =
            create_encoder(mode_adapter.device, mode_config, &mode_error);
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

        // `mode_frame` points into the capturer, not the encoder, so it stays
        // valid across the move below.
        encoder = std::move(mode_encoder);
        have_input = false;
        force_keyframe = true;
        extra_idrs = 1;
        // The output was re-created: its pointer position and sprite are new.
        cursor_sent_once = false;
        cursor_sent = CursorUpdate{};
        // `fresh` was read before ChangeDisplaySettingsExW, so its rects
        // describe the old mode. Announce the list as it is now.
        attached = enumerate_displays();
        stream_info = describe_stream(attached, *capturer, *encoder, mode_frame.width,
                                      mode_frame.height, static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info);
        publish_current(stream_info);
        if (config_.stream_announced) {
            config_.stream_announced(stream_info);
        }
        if (!answer(DisplayModeCode::Ok, std::string())) {
            return false;
        }
        Error refreshed = session_->refresh_stream(stream_info);
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

    // The viewer's own picture preferences. A codec change builds the
    // replacement beside the running encoder and swaps only after it produced
    // the requested family; quality and frame rate are set_rate calls that
    // need no rebuild. Every request is answered exactly once.
    bool codec_changed_once = false;
    Clock::time_point last_codec_change_at{};
    auto apply_stream_options = [&](const StreamOptions& options) -> bool {
        ++stats.stream_option_requests;
        const auto now = Clock::now();
        auto answer = [&](StreamOptionsCode code, const std::string& reason) -> bool {
            StreamOptionsResult result;
            result.code = code;
            result.codec = encoder ? encoder->codec() : settings_.codec;
            result.fps = fps_ceiling();
            result.quality = stream_quality;
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

        const bool quality_change = options.quality != stream_quality;
        const bool cap_change = options.max_fps != stream_fps_cap;
        const bool preference_change = options.codec != codec_preference;
        if (!quality_change && !cap_change && !preference_change) {
            return answer(StreamOptionsCode::Ok, std::string());
        }

        VideoCodec wanted_codec = encoder->codec();
        bool codec_rebuild = false;
        if (preference_change) {
            if (options.codec == StreamCodecPreference::Auto) {
                wanted_codec = settings_.codec;
            } else {
                wanted_codec = options.codec == StreamCodecPreference::Hevc
                                   ? VideoCodec::Hevc
                                   : VideoCodec::H264;
            }
            codec_rebuild = wanted_codec != encoder->codec();
        }
        if (codec_rebuild && codec_changed_once &&
            now - last_codec_change_at < std::chrono::seconds(1)) {
            return refuse(StreamOptionsCode::Busy, "a codec change just happened");
        }

        // Work the new rate out on a copy first: a refused codec rebuild must
        // leave the running stream exactly as it was.
        BitrateController next_abr = abr;
        bool rate_change = false;
        if (cap_change) {
            const uint16_t top = options.max_fps == 0
                                     ? configured_fps
                                     : std::min(configured_fps, options.max_fps);
            rate_change = next_abr.set_top_framerate(top).changed || rate_change;
        }
        if (quality_change) {
            rate_change =
                next_abr.set_ceiling(quality_budget(options.quality)).changed || rate_change;
        }

        std::unique_ptr<IVideoEncoder> replacement;
        if (codec_rebuild) {
            EncoderConfig next_config = encoder_config;
            next_config.codec = wanted_codec;
            next_config.width = encoder->width();
            next_config.height = encoder->height();
            // Start the replacement at the rate the stream runs at; runtimes
            // that cannot reprogram a session would otherwise open it twice.
            next_config.bitrate = next_abr.target_bps();
            if (next_abr.framerate() != 0) {
                next_config.framerate = next_abr.framerate();
            }
            const AdapterDevice codec_adapter = capturer->adapter_device();
            std::string codec_error;
            replacement = create_encoder(codec_adapter.device, next_config, &codec_error);
            if (!replacement) {
                return refuse(options.codec == StreamCodecPreference::Hevc
                                  ? StreamOptionsCode::Unsupported
                                  : StreamOptionsCode::Failed,
                              "encoder creation failed: " + codec_error);
            }
            if (options.codec != StreamCodecPreference::Auto &&
                replacement->codec() != wanted_codec) {
                log(std::string("the encoder fell back: requested ") +
                    codec_name(wanted_codec) + ", producing " +
                    codec_name(replacement->codec()));
                return refuse(StreamOptionsCode::Unsupported,
                              std::string("the host cannot produce ") +
                                  codec_name(wanted_codec));
            }
            const uint16_t target_fps = next_abr.framerate();
            if (replacement->bitrate() != next_abr.target_bps() ||
                (target_fps != 0 && replacement->framerate() != target_fps)) {
                Error rated = replacement->set_rate(next_abr.target_bps(), target_fps);
                if (!rated.ok()) {
                    return refuse(StreamOptionsCode::Failed,
                                  "cannot apply the current bitrate: " + rated.to_string());
                }
            }
        }

        bool applied = false;
        // A replacement encoder always starts from a fresh reference state;
        // only set_rate on the running encoder can report a continued
        // sequence that needs no IDR.
        bool keyframe_required = true;
        if (replacement) {
            encoder = std::move(replacement);
            abr = next_abr;
            encoder_config.codec = options.codec == StreamCodecPreference::Auto
                                       ? settings_.codec
                                       : wanted_codec;
            codec_preference = options.codec;
            codec_changed_once = true;
            last_codec_change_at = now;
            // A new encoder starts from a fresh reference state. Two IDRs let
            // the viewer decode once it applied the StreamInfo announcing the
            // codec change.
            have_input = false;
            extra_idrs = 1;
            applied = true;
        } else if (rate_change) {
            Error rated = encoder->set_rate(next_abr.target_bps(), next_abr.framerate(),
                                            &keyframe_required);
            if (!rated.ok()) {
                return refuse(StreamOptionsCode::Failed,
                              "cannot apply the current bitrate: " + rated.to_string());
            }
            abr = next_abr;
            applied = true;
        } else if (preference_change) {
            // No rebuild was needed, but later capture rebuilds must follow
            // the preference that just became current.
            encoder_config.codec = options.codec == StreamCodecPreference::Auto
                                       ? settings_.codec
                                       : wanted_codec;
            codec_preference = options.codec;
        }

        stream_quality = options.quality;
        stream_fps_cap = options.max_fps;

        if (applied) {
            ++stats.stream_option_changes;
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(abr.framerate());
            framerate = abr.framerate();
            frame_interval = frame_duration(framerate);
            next_frame_due = now + frame_interval;
            force_keyframe = force_keyframe || keyframe_required;
            stream_info =
                describe_stream(attached, *capturer, *encoder, stream_info.source_width,
                                stream_info.source_height, static_cast<uint16_t>(framerate),
                                session_->permissions(), audio_info);
            publish_current(stream_info);
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            log("stream options: " + std::string(codec_name(encoder->codec())) + ", " +
                std::to_string(stream_info.bitrate / 1000u) + " kbps at " +
                std::to_string(framerate) + " fps, quality " +
                stream_quality_name(stream_quality) +
                (stream_fps_cap == 0
                     ? ""
                     : " (viewer cap " + std::to_string(stream_fps_cap) + " fps)"));
        }

        if (!answer(StreamOptionsCode::Ok, std::string())) {
            return false;
        }
        if (applied) {
            Error refreshed = session_->refresh_stream(stream_info);
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

    // Once a second: link telemetry for the tray, and the bitrate decision
    // that follows from it. Runs on the host thread between frames so the
    // encoder reset set_rate performs cannot overlap an encode call.
    auto last_link_sample = Clock::now();
    uint64_t last_sample_bytes = stats.bytes;
    uint64_t last_sent_packets = 0;
    uint64_t last_lost_packets = 0;
    auto sample_link = [&](Clock::time_point now) -> bool {
        const uint32_t elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_link_sample)
                .count());
        if (elapsed_ms == 0) {
            return true;
        }
        const uint64_t byte_delta =
            stats.bytes >= last_sample_bytes ? stats.bytes - last_sample_bytes : 0;
        // bytes * 8 / milliseconds is exactly kilobits per second.
        stats.send_kbps =
            static_cast<double>(byte_delta) * 8.0 / static_cast<double>(elapsed_ms);
        last_sample_bytes = stats.bytes;
        last_link_sample = now;

        const TransportStats transport = session_->transport_stats();
        if (!transport.available) {
            return true;
        }
        stats.rtt_ms = static_cast<double>(transport.rtt_us) / 1000.0;
        stats.min_rtt_ms = static_cast<double>(transport.min_rtt_us) / 1000.0;
        const uint64_t sent_delta = transport.sent_packets >= last_sent_packets
                                        ? transport.sent_packets - last_sent_packets
                                        : 0;
        const uint64_t lost_delta = transport.lost_packets >= last_lost_packets
                                        ? transport.lost_packets - last_lost_packets
                                        : 0;
        last_sent_packets = transport.sent_packets;
        last_lost_packets = transport.lost_packets;
        if (!config_.adaptive_bitrate) {
            return true;
        }

        BitrateSample sample;
        sample.rtt_us = transport.rtt_us;
        sample.min_rtt_us = transport.min_rtt_us;
        sample.queued_bytes = transport.queued_bytes;
        sample.congestion_window = transport.congestion_window;
        sample.sent_bytes = transport.send_bytes;
        sample.lost_packets = transport.lost_packets;
        sample.sent_packets = transport.sent_packets;
        const BitrateDecision decision = abr.update(sample, elapsed_ms);
        if (!decision.changed) {
            return true;
        }
        const bool rate_changed = decision.target_bps != stats.target_bitrate ||
                                  decision.framerate != stats.framerate;
        // The ladder names a rung; a merged canvas may not be able to reach it.
        // Compare against the request, not the effective scale, or the host
        // would retry the same capped change every second.
        const bool size_changed =
            !resolution_fixed && decision.resolution_percent != resolution_request;
        if (!rate_changed && !size_changed) {
            return true;
        }

        if (rate_changed) {
            bool keyframe_required = true;
            Error applied =
                encoder->set_rate(decision.target_bps, decision.framerate, &keyframe_required);
            if (!applied.ok()) {
                log("adaptive bitrate: cannot apply " +
                    std::to_string(decision.target_bps / 1000u) + " kbps at " +
                    std::to_string(decision.framerate) + " fps after " + decision.reason + ": " +
                    applied.to_string());
                return true;
            }
            stats.target_bitrate = decision.target_bps;
            stats.framerate = decision.framerate;
            ++stats.abr_changes;
            // The capture cadence has to follow the rate the encoder now
            // divides its budget over, or every frame keeps a budget for a
            // frame nobody sends. A pure bitrate step continues the coded
            // sequence, so an IDR is only forced when the encoder says the
            // references are gone.
            force_keyframe = force_keyframe || keyframe_required;
            framerate = decision.framerate;
            frame_interval = frame_duration(framerate);
            next_frame_due = now + frame_interval;
        }

        bool announce_now = true;
        if (size_changed) {
            Error scaled = capturer->set_nv12_scale(decision.resolution_percent);
            if (!scaled.ok()) {
                resolution_fixed = true;
                log("adaptive resolution: cannot scale to " +
                    std::to_string(decision.resolution_percent) +
                    "%; staying at " + std::to_string(stats.resolution_percent) +
                    "% for this session: " + scaled.to_string());
            } else {
                resolution_request = decision.resolution_percent;
                const uint16_t effective =
                    static_cast<uint16_t>(capturer->nv12_scale_percent());
                if (effective != stats.resolution_percent) {
                    stats.resolution_percent = effective;
                    ++stats.resolution_changes;
                    force_keyframe = true;
                    extra_idrs = 1;
                    // The frame loop rebuilds the encoder at the new captured
                    // size and announces that description once the encoder
                    // accepted it; announcing the intended size here could
                    // disagree with what the encoder lands on.
                    announce_now = false;
                    log("adaptive resolution: " + std::string(decision.reason) + "; " +
                        std::to_string(effective) + "% of the captured frame");
                } else {
                    log("adaptive resolution: " + std::string(decision.reason) +
                        "; requested " + std::to_string(decision.resolution_percent) +
                        "%, the canvas stays at " + std::to_string(effective) + "%");
                }
            }
        }
        if (announce_now) {
            stream_info.bitrate = stats.target_bitrate;
            stream_info.framerate = static_cast<uint16_t>(framerate);
            publish_current(stream_info);
            Error refreshed = session_->refresh_stream(stream_info);
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
        }
        if (rate_changed) {
            log("adaptive bitrate: " + std::string(decision.reason) + "; target " +
                std::to_string(decision.target_bps / 1000u) + " kbps at " +
                std::to_string(framerate) + " fps, RTT " +
                std::to_string(static_cast<int>(stats.rtt_ms + 0.5)) + " ms, loss " +
                std::to_string(lost_delta) + "/" + std::to_string(sent_delta) +
                " pkts, queued " + std::to_string(transport.queued_bytes) + " of cwnd " +
                std::to_string(transport.congestion_window));
        }
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
        const InputDesktopStatus status = input_desktop_status();
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
        if (!follow_input_desktop(&follow_error, &switched)) {
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
        Error rebuilt = capturer->reinitialize();
        if (!rebuilt.ok()) {
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
        encoder.reset();
        have_input = false;
        force_keyframe = true;
        next_frame_due = Clock::now();
        return true;
    };
    if (!sync_desktop(true)) {
        const bool clean = error->empty();
        drain_peer();
        return clean;
    }
    auto next_desktop_check = Clock::now() + std::chrono::seconds(1);

    // The frame captured before the handshake is the first one on the wire.
    ++stats.frames;
    if (initial_pointer_only) {
        ++stats.pointer_only;
    }
    if (!process_frame(0, capture_clock_ms(Clock::now()))) {
        // A vanished viewer is a normal end, not a failure.
        const bool clean = error->empty();
        drain_peer();
        return clean;
    }
    publish_cursor();

    while (!stopping_.load()) {
        const auto now = Clock::now();
        if (config_.run_seconds > 0.0 &&
            std::chrono::duration<double>(now - start).count() >= config_.run_seconds) {
            break;
        }
        uint16_t switch_index = kDisplayAuto;
        if (session_->take_display_switch_request(switch_index) &&
            !switch_display(switch_index)) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        uint16_t modes_index = 0;
        if (session_->take_list_display_modes_request(modes_index) &&
            !send_mode_list(modes_index)) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        uint16_t mode_index = 0;
        uint16_t mode_width = 0;
        uint16_t mode_height = 0;
        uint16_t mode_refresh = 0;
        if (session_->take_set_display_mode_request(mode_index, mode_width, mode_height,
                                                    mode_refresh) &&
            !change_display_mode(mode_index, mode_width, mode_height, mode_refresh)) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        StreamOptions options;
        if (session_->take_stream_options_request(options) &&
            !apply_stream_options(options)) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        if (!apply_pending_permissions()) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        bool privacy_requested = false;
        if (session_->take_privacy_mode_request(privacy_requested) &&
            !set_privacy_mode(privacy_requested)) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        while (session_->take_sas_request()) {
            if (!send_sas()) {
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
        }
        if (now >= next_desktop_check) {
            next_desktop_check = now + std::chrono::seconds(1);
            if (!sync_desktop(false)) {
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
        }
        drain_audio();
        drain_microphone();
        drain_peer();
        poll_clipboard(now);
        if (upload && !file_transfer_allowed()) {
            // The transport already told the viewer and dropped the stream;
            // keeping the part file lets a later offer resume it.
            log("file: stopping " + upload->receiver->name() +
                " because file transfer was revoked");
            end_upload(FileCode::Cancelled, "file transfer was disabled", true, false);
        }
        drain_file_offers();
        drain_file_data();
        drain_file_cancel();
        drive_file_download();
        if (now - last_link_sample >= std::chrono::seconds(1)) {
            if (!sample_link(now)) {
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
        }
        if (session_->take_keyframe_request()) {
            force_keyframe = true;
        }
        if (config_.keyframe_interval_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keyframe).count() >=
                static_cast<long long>(config_.keyframe_interval_ms)) {
            force_keyframe = true;
        }
        if (config_.reannounce && !reannounced && now >= next_reannounce) {
            reannounced = true;
            Error refreshed = session_->refresh_stream(stream_info);
            if (!refreshed.ok()) {
                if (refreshed.status() == Status::Unavailable ||
                    refreshed.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "refresh_stream: " + refreshed.to_string();
                }
                const bool clean = error->empty();
                drain_peer();
                return clean;
            }
            ++stats.stream_updates;
            log("re-announced the stream description");
        }
        if (probe_pending && now - start >= std::chrono::milliseconds(2500)) {
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

        if (frame_interval != Clock::duration::zero()) {
            const auto wait_now = Clock::now();
            if (next_frame_due > wait_now) {
                if (!sleep_unless_stopping(
                        std::chrono::duration_cast<std::chrono::milliseconds>(next_frame_due -
                                                                            wait_now))) {
                    break;
                }
            }
            // Advance on an absolute timeline; capture() itself waits for the
            // next present, so a relative sleep would halve the frame rate.
            next_frame_due += frame_interval;
            if (Clock::now() - next_frame_due > std::chrono::seconds(1)) {
                next_frame_due = Clock::now();
            }
        }

        const CaptureResult result = capturer->capture(std::chrono::milliseconds(100));
        const auto captured_at = Clock::now();
        // The endpoint keeps producing while capture waits for the next
        // present; drain here so audio does not wait behind a slow frame.
        drain_audio();
        drain_microphone();
        switch (result) {
            case CaptureResult::NoNewFrame:
                continue;
            case CaptureResult::ReinitNeeded: {
                log("reinitializing: " + capturer->last_error().to_string());
                // The common cause is a desktop switch: the duplication was
                // attached to the old desktop. Follow first, then rebuild.
                if (!sync_desktop(false)) {
                    const bool clean = error->empty();
                    drain_peer();
                    return clean;
                }
                if (desktop_paused || desktop_reinitialized) {
                    continue;
                }
                Error err = capturer->reinitialize();
                if (!err.ok()) {
                    *error = "reinit failed: " + err.to_string();
                    drain_peer();
                    return false;
                }
                encoder.reset();
                have_input = false;
                force_keyframe = true;
                continue;
            }
            case CaptureResult::FatalError: {
                const std::string failure = capturer->last_error().to_string();
                log("capture failed: " + failure);
                if (!sync_desktop(false)) {
                    const bool clean = error->empty();
                    drain_peer();
                    return clean;
                }
                if (desktop_paused || desktop_reinitialized) {
                    continue;
                }
                *error = "capture failed: " + failure;
                drain_peer();
                return false;
            }
            case CaptureResult::Frame:
                break;
        }

        ++stats.frames;
        const FrameInfo& frame = capturer->frame();
        if (!frame.desktop_updated) {
            ++stats.pointer_only;
        }
        if (!encoder) {
            // The capture rebuilt: the output may have moved, rotated or
            // changed size, and the encoder may land on a different codec.
            encoder_config.width = frame.nv12_width;
            encoder_config.height = frame.nv12_height;
            const AdapterDevice rebuilt_adapter = capturer->adapter_device();
            encoder = create_encoder(rebuilt_adapter.device, encoder_config, error);
            if (!encoder) {
                *error = "encoder recreation failed: " + *error;
                drain_peer();
                return false;
            }
            if (encoder->codec() != encoder_config.codec) {
                log(std::string("the encoder fell back: requested ") +
                    codec_name(encoder_config.codec) + ", producing " +
                    codec_name(encoder->codec()));
            }
            // A rebuilt encoder starts from the configured bitrate again, and
            // an automatic rate may differ after a desktop or size change.
            // Recompute the session budget from it, then reapply the viewer's
            // quality preset and frame-rate cap.
            base_video_budget =
                video_budget(encoder->bitrate(), bitrate_reserve(audio_info));
            video_floor_bps = video_floor(encoder->bitrate());
            // A rebuilt merged canvas can change the cap under the running
            // scale; report what capture now produces before anything reads it.
            stats.resolution_percent = static_cast<uint16_t>(capturer->nv12_scale_percent());
            abr = BitrateController(quality_budget(stream_quality), video_floor_bps,
                                    fps_ceiling(), resolution_request);
            framerate = abr.framerate();
            frame_interval = frame_duration(framerate);
            next_frame_due = Clock::now() + frame_interval;
            if (abr.ceiling_bps() != encoder->bitrate()) {
                // Same reservation as at session start: the rebuilt encoder
                // came back at the configured rate, audio included.
                Error trimmed = encoder->set_rate(abr.ceiling_bps(), framerate);
                if (!trimmed.ok()) {
                    log("cannot apply the video budget: " + trimmed.to_string());
                }
            }
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(framerate);
            const std::vector<DisplayInfo> refreshed = enumerate_displays();
            // The output may have moved or been renumbered while capture was
            // down; StreamInfo and the pointer origin must describe the list
            // the viewer is about to receive.
            attached = refreshed;
            stream_info =
                describe_stream(refreshed, *capturer, *encoder, frame.width, frame.height,
                                static_cast<uint16_t>(framerate), session_->permissions(),
                                audio_info);
            publish_current(stream_info);
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            Error refreshed_err = session_->refresh_stream(stream_info);
            if (!refreshed_err.ok()) {
                if (refreshed_err.status() == Status::Unavailable ||
                    refreshed_err.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    *error = "refresh_stream: " + refreshed_err.to_string();
                }
                const bool clean = error->empty();
                drain_peer();
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
        if (frame.nv12_texture &&
            (frame.nv12_width != encoder->width() || frame.nv12_height != encoder->height())) {
            EncoderConfig resized = encoder_config;
            resized.width = frame.nv12_width;
            resized.height = frame.nv12_height;
            resized.bitrate = stats.target_bitrate;
            resized.framerate = framerate;
            const AdapterDevice resize_adapter = capturer->adapter_device();
            std::string resize_error;
            std::unique_ptr<IVideoEncoder> next =
                create_encoder(resize_adapter.device, resized, &resize_error);
            if (!next) {
                // Keep the stream that is running: put the capture back on the
                // size the current encoder was built for and stop resizing.
                resolution_fixed = true;
                Error reverted = capturer->set_nv12_scale(stats.resolution_percent);
                if (!reverted.ok()) {
                    log("adaptive resolution: revert failed: " + reverted.to_string());
                }
                resolution_request = stats.resolution_percent;
                log("adaptive resolution: encoder recreation at " +
                    std::to_string(frame.nv12_width) + "x" +
                    std::to_string(frame.nv12_height) + " failed; staying at " +
                    std::to_string(stats.resolution_percent) + "%: " + resize_error);
                stream_info.bitrate = stats.target_bitrate;
                stream_info.framerate = static_cast<uint16_t>(framerate);
                publish_current(stream_info);
                Error refreshed = session_->refresh_stream(stream_info);
                if (!refreshed.ok()) {
                    if (refreshed.status() == Status::Unavailable ||
                        refreshed.status() == Status::WaitTimeout) {
                        *peer_left = true;
                    } else {
                        ++stats.send_errors;
                        *error = "refresh_stream: " + refreshed.to_string();
                    }
                    const bool clean = error->empty();
                    drain_peer();
                    return clean;
                }
                ++stats.stream_updates;
                publish_stats(stats);
                // This frame still holds the refused size and cannot feed the
                // old encoder; the reverted pass produces a matching one next.
                continue;
            }
            encoder = std::move(next);
            have_input = false;
            force_keyframe = true;
            extra_idrs = 1;
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(framerate);
            stream_info =
                describe_stream(attached, *capturer, *encoder, frame.width, frame.height,
                                static_cast<uint16_t>(framerate), session_->permissions(),
                                audio_info);
            publish_current(stream_info);
            if (config_.stream_announced) {
                config_.stream_announced(stream_info);
            }
            Error resized_err = session_->refresh_stream(stream_info);
            if (!resized_err.ok()) {
                if (resized_err.status() == Status::Unavailable ||
                    resized_err.status() == Status::WaitTimeout) {
                    *peer_left = true;
                } else {
                    ++stats.send_errors;
                    *error = "refresh_stream: " + resized_err.to_string();
                }
                const bool clean = error->empty();
                drain_peer();
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
        if (!process_frame(pts_ms, capture_clock_ms(captured_at))) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        publish_cursor();
    }

    if (upload) {
        log("file: " + upload->receiver->name() +
            " is incomplete; its .part file is kept for a resume");
        upload->receiver->abort(true);
        upload.reset();
    }
    drain_peer();
    return true;
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

void HostServer::log(const std::string& message) {
    if (config_.log) {
        config_.log(message);
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
