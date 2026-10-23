// The capture and session headers pull in windows.h, whose min/max macros
// collide with std::min/std::max below.
#define NOMINMAX

#include "chast/host_server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/bitrate_controller.h"
#include "chast/capture.h"
#include "chast/clipboard.h"
#include "chast/display_mode.h"
#include "chast/file_transfer.h"

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

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
    if (settings.allow_file_transfer) {
        mask |= kPermissionFileTransfer;
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

// The description HelloAck and StreamInfo carry. `display` is the output the
// capturer is reading, `attached` the full list from the moment of the call.
// `source_width`/`source_height` are the captured frame; the encoder's size may
// be smaller when adaptive resolution scaled it. `permissions` is the session
// mask currently in force.
ServerInfo describe_stream(const std::vector<DisplayInfo>& attached,
                           const DisplayInfo& display, const IVideoEncoder& encoder,
                           UINT source_width, UINT source_height, uint16_t framerate,
                           uint16_t permissions, const AudioInfo& audio) {
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
    info.clipboard_enabled = (permissions & kPermissionClipboard) != 0;
    info.file_transfer_enabled = (permissions & kPermissionFileTransfer) != 0;
    info.audio = audio;
    info.encoder = encoder.implementation().empty() ? "oneVPL" : encoder.implementation();
    for (const DisplayInfo& listed : attached) {
        DisplayDescription described;
        described.name = display_name_utf8(listed);
        described.left = listed.rect.left;
        described.top = listed.rect.top;
        described.right = listed.rect.right;
        described.bottom = listed.rect.bottom;
        described.rotation_degrees = static_cast<uint16_t>(listed.rotation_degrees);
        described.primary = listed.primary;
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
    return status_;
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
    stats.resolution_percent = 100;
    stats.resolution_changes = 0;
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

    size_t display_index = attached.size();
    const uint16_t requested = session_->requested_display();
    if (requested != kDisplayAuto) {
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

    std::unique_ptr<IDxgiCapturer> capturer =
        create_capturer(display_index, capture, error);
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
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.display_index = find_display(attached, capturer->display());
        status_.display_name = display_name_utf8(capturer->display());
    }
    log("capturing display " + std::to_string(find_display(attached, capturer->display())) + " (" +
        display_name_utf8(capturer->display()) + ")");

    EncoderConfig encoder_config;
    encoder_config.codec = settings_.codec;
    encoder_config.bitrate = settings_.bitrate_kbps * 1000u;
    encoder_config.framerate = settings_.framerate;
    encoder_config.quality_ratio = settings_.quality;
    encoder_config.gop_size = 0;
    encoder_config.width = first.nv12_width;
    encoder_config.height = first.nv12_height;
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
    // The endpoint stays open for the whole session even if audio was denied
    // at approval: the operator can grant it back live, and rebuilding WASAPI
    // mid-session would cost a fresh announcement for no benefit.
    if (settings_.allow_audio) {
        std::string audio_error;
        if (config_.audio_capture) {
            audio_capture = config_.audio_capture(&audio_error);
        } else {
            LoopbackCaptureConfig audio_config;
            audio_capture = create_loopback_capture(audio_config, &audio_error);
        }
        if (audio_capture) {
            audio_info = audio_capture->info();
            log("audio: AAC-LC " + std::to_string(audio_info.sample_rate / 1000u) + " kHz, " +
                std::to_string(audio_info.channels) + " ch, " +
                std::to_string(audio_info.bitrate / 1000u) + " kbps");
        } else {
            ++stats.audio_errors;
            log("audio unavailable: " + audio_error);
        }
    }
    stats.audio_active =
        audio_info.enabled && (granted_permissions & kPermissionAudio) != 0;

    // The configured bitrate is the whole session's budget, audio included;
    // the video encoder starts at what is left after reserving it. The
    // controller only ever lowers that and later climbs back to it.
    uint32_t framerate = encoder->framerate();
    BitrateController abr(video_budget(encoder->bitrate(), bitrate_reserve(audio_info)),
                          video_floor(encoder->bitrate()), static_cast<uint16_t>(framerate));
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
        describe_stream(attached, capturer->display(), *encoder, first.width, first.height,
                        static_cast<uint16_t>(framerate), granted_permissions, audio_info);
    publish_stream(stream_info, find_display(attached, capturer->display()),
                   display_name_utf8(capturer->display()));
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
        // Audio and clipboard availability travel in the stream description.
        // A viewer that was denied audio at approval has no AAC config yet,
        // so a fresh description is what makes a restore usable.
        stream_info.audio.enabled = audio_info.enabled;
        publish_stream(stream_info, find_display(attached, capturer->display()),
                       display_name_utf8(capturer->display()));
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
        return settings_.framerate == 0 || fps == 0
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
    // The scale the running encoder was built for. A requested scale only
    // becomes applied once an encoder at that size exists; stats follow it.
    UINT applied_resolution_percent = stats.resolution_percent;
    UINT pending_resolution_percent = applied_resolution_percent;
    uint32_t frames_without_nv12 = 0;
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
            upload = std::move(active);
            Error answered = session_->send_file_answer(transfer_id, FileCode::Ok,
                                                        resume_offset);
            if (!answered.ok()) {
                upload.reset();
                ++stats.file_failures;
                log("file: cannot accept " + name + ": " + answered.to_string());
                continue;
            }
            log("file: receiving " + name + " (" + std::to_string(upload->offer.size) +
                " bytes" +
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

    // Audio degrades on its own: a failed endpoint, encoder or send turns the
    // stream off with a StreamInfo and leaves video running.
    auto disable_audio = [&](const std::string& reason) {
        audio_capture->stop();
        audio_capture.reset();
        audio_info = AudioInfo{};
        stats.audio_active = false;
        stream_info.audio = audio_info;
        publish_stream(stream_info, find_display(attached, capturer->display()),
                       display_name_utf8(capturer->display()));
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
        if (audio_capture->failed()) {
            ++stats.audio_errors;
            const std::string reason = audio_capture->error();
            disable_audio(reason.empty() ? "capture failed" : reason);
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
        size_t target = fresh.size();
        if (requested_index != kDisplayAuto) {
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
        if (target >= fresh.size()) {
            return refuse(DisplaySwitchCode::NotFound,
                          "display " + std::to_string(requested_index) + " is not attached");
        }
        if (fresh[target].device_name == capturer->display().device_name) {
            switched_once = true;
            last_switch_at = now;
            ++stats.display_switches;
            log("display switch: already streaming " +
                display_name_utf8(capturer->display()));
            return answer(DisplaySwitchCode::Ok, std::string());
        }

        std::string switch_error;
        std::unique_ptr<IDxgiCapturer> next = create_capturer(target, capture, &switch_error);
        if (!next) {
            return refuse(DisplaySwitchCode::Failed,
                          "cannot capture display " + std::to_string(target) + ": " +
                              switch_error);
        }
        if (stats.resolution_percent != 100) {
            Error scaled = next->set_nv12_scale(stats.resolution_percent);
            if (!scaled.ok()) {
                // A screen that cannot scale still gets the switch; the ABR
                // sees the new size as 100% from here on.
                log("display switch: " + std::to_string(target) + " cannot scale to " +
                    std::to_string(stats.resolution_percent) + "%; using 100%: " +
                    scaled.to_string());
                stats.resolution_percent = 100;
                ++stats.resolution_changes;
            }
        }
        if (!wait_for_usable_frame(*next, true, stopping_, std::chrono::milliseconds(3000),
                                   &switch_error)) {
            return refuse(DisplaySwitchCode::Failed,
                          "display " + std::to_string(target) + " produced no frame: " +
                              switch_error);
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
        if (next_encoder->codec() != settings_.codec) {
            log(std::string("the encoder fell back: requested ") + codec_name(settings_.codec) +
                ", producing " + codec_name(next_encoder->codec()));
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
        const size_t new_index = find_display(attached, capturer->display());
        stream_info = describe_stream(attached, capturer->display(), *encoder,
                                      next_frame.width, next_frame.height,
                                      static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info);
        publish_stream(stream_info, new_index, display_name_utf8(capturer->display()));
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
        log("display switched to " + std::to_string(new_index) + " (" +
            display_name_utf8(capturer->display()) + "), " +
            std::to_string(stream_info.source_width) + "x" +
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
        if (fresh.empty()) {
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
        if (mode_encoder->codec() != settings_.codec) {
            log(std::string("the encoder fell back: requested ") + codec_name(settings_.codec) +
                ", producing " + codec_name(mode_encoder->codec()));
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
        const size_t mode_index = find_display(attached, capturer->display());
        stream_info = describe_stream(attached, capturer->display(), *encoder,
                                      mode_frame.width, mode_frame.height,
                                      static_cast<uint16_t>(framerate),
                                      session_->permissions(), audio_info);
        publish_stream(stream_info, mode_index, display_name_utf8(capturer->display()));
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
        const bool size_changed = !resolution_fixed &&
                                  decision.resolution_percent != stats.resolution_percent;
        if (!rate_changed && !size_changed) {
            return true;
        }

        if (rate_changed) {
            Error applied = encoder->set_rate(decision.target_bps, decision.framerate);
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
            // The encoder reset invalidates its reference state, and the
            // capture cadence has to follow the rate the encoder now divides
            // its budget over, or every frame keeps a budget for a frame
            // nobody sends.
            force_keyframe = true;
            framerate = decision.framerate;
            frame_interval = frame_duration(framerate);
            next_frame_due = now + frame_interval;
        }

        bool announce_now = true;
        if (size_changed) {
            Error scaled = capturer->set_nv12_scale(decision.resolution_percent);
            if (!scaled.ok()) {
                resolution_fixed = true;
                pending_resolution_percent = applied_resolution_percent;
                Error reverted = capturer->set_nv12_scale(applied_resolution_percent);
                if (!reverted.ok()) {
                    *error = "adaptive resolution: cannot return to " +
                             std::to_string(applied_resolution_percent) +
                             "% after refusing " +
                             std::to_string(decision.resolution_percent) + "%: " +
                             reverted.to_string();
                    return false;
                }
                log("adaptive resolution: cannot scale to " +
                    std::to_string(decision.resolution_percent) +
                    "%; staying at " + std::to_string(applied_resolution_percent) +
                    "% for this session: " + scaled.to_string());
            } else {
                pending_resolution_percent = decision.resolution_percent;
                force_keyframe = true;
                extra_idrs = 1;
                // The frame loop rebuilds the encoder at the new captured size
                // and announces that description once the encoder accepted it;
                // announcing the intended size here could disagree with what
                // the encoder lands on.
                announce_now = false;
                log("adaptive resolution: " + std::string(decision.reason) + "; " +
                    std::to_string(decision.resolution_percent) + "% of the captured frame");
            }
        }
        if (announce_now) {
            stream_info.bitrate = stats.target_bitrate;
            stream_info.framerate = static_cast<uint16_t>(framerate);
            publish_stream(stream_info, find_display(attached, capturer->display()),
                           display_name_utf8(capturer->display()));
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
        if (!apply_pending_permissions()) {
            const bool clean = error->empty();
            drain_peer();
            return clean;
        }
        drain_audio();
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
                config_.resolution_probe_percent != pending_resolution_percent) {
                Error scaled = capturer->set_nv12_scale(config_.resolution_probe_percent);
                if (!scaled.ok()) {
                    resolution_fixed = true;
                    pending_resolution_percent = applied_resolution_percent;
                    Error reverted = capturer->set_nv12_scale(applied_resolution_percent);
                    if (!reverted.ok()) {
                        *error = "resolution probe: cannot return to " +
                                 std::to_string(applied_resolution_percent) +
                                 "% after refusing " +
                                 std::to_string(config_.resolution_probe_percent) + "%: " +
                                 reverted.to_string();
                        drain_peer();
                        return false;
                    }
                    log("resolution probe: cannot scale to " +
                        std::to_string(config_.resolution_probe_percent) +
                        "%; staying at " + std::to_string(applied_resolution_percent) +
                        "% for this session: " + scaled.to_string());
                } else {
                    pending_resolution_percent = config_.resolution_probe_percent;
                    force_keyframe = true;
                    extra_idrs = 1;
                    log("resolution probe: " + std::to_string(pending_resolution_percent) + "%");
                }
            }
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
                    drain_peer();
                    return false;
                }
                continue;
            case CaptureResult::ReinitNeeded: {
                log("reinitializing: " + capturer->last_error().to_string());
                Error err = capturer->reinitialize();
                if (!err.ok()) {
                    *error = "reinit failed: " + err.to_string();
                    drain_peer();
                    return false;
                }
                encoder.reset();
                have_input = false;
                force_keyframe = true;
                // The rebuilt duplication has no frame yet; a quiet desktop is
                // not the same as a broken NV12 conversion.
                frames_without_nv12 = 0;
                continue;
            }
            case CaptureResult::FatalError:
                *error = "capture failed: " + capturer->last_error().to_string();
                drain_peer();
                return false;
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
            // A capture reinit keeps the last requested scale, so the encoder
            // just built at that size is the one the stats must describe.
            if (pending_resolution_percent != stats.resolution_percent) {
                stats.resolution_percent = static_cast<uint16_t>(pending_resolution_percent);
                ++stats.resolution_changes;
            }
            applied_resolution_percent = pending_resolution_percent;
            // A rebuilt encoder starts from the configured bitrate again, so
            // the controller starts over with the budget as its ceiling and
            // the configured frame rate at the top of the ladder.
            abr = BitrateController(video_budget(encoder->bitrate(), bitrate_reserve(audio_info)),
                                    video_floor(encoder->bitrate()),
                                    static_cast<uint16_t>(encoder->framerate()),
                                    stats.resolution_percent);
            framerate = encoder->framerate();
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
                describe_stream(refreshed, capturer->display(), *encoder, frame.width,
                                frame.height, static_cast<uint16_t>(framerate),
                                session_->permissions(), audio_info);
            publish_stream(stream_info, find_display(refreshed, capturer->display()),
                           display_name_utf8(capturer->display()));
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
            if (resolution_fixed) {
                // The refused scale was reverted, so a mismatch here cannot be
                // a pending resize. Retrying would rebuild the encoder once per
                // frame; the stream cannot continue safely.
                *error = "adaptive resolution: capture is " +
                         std::to_string(frame.nv12_width) + "x" +
                         std::to_string(frame.nv12_height) + " but the fixed encoder is " +
                         std::to_string(encoder->width()) + "x" +
                         std::to_string(encoder->height());
                drain_peer();
                return false;
            }
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
                pending_resolution_percent = applied_resolution_percent;
                Error reverted = capturer->set_nv12_scale(applied_resolution_percent);
                if (!reverted.ok()) {
                    *error = "adaptive resolution: cannot restore " +
                             std::to_string(applied_resolution_percent) +
                             "% after encoder recreation failed: " + reverted.to_string();
                    drain_peer();
                    return false;
                }
                log("adaptive resolution: encoder recreation at " +
                    std::to_string(resized.width) + "x" +
                    std::to_string(resized.height) + " failed; staying at " +
                    std::to_string(applied_resolution_percent) + "%: " + resize_error);
                stream_info.bitrate = stats.target_bitrate;
                stream_info.framerate = static_cast<uint16_t>(framerate);
                publish_stream(stream_info, find_display(attached, capturer->display()),
                               display_name_utf8(capturer->display()));
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
            if (pending_resolution_percent != stats.resolution_percent) {
                stats.resolution_percent = static_cast<uint16_t>(pending_resolution_percent);
                ++stats.resolution_changes;
            }
            applied_resolution_percent = pending_resolution_percent;
            stats.target_bitrate = encoder->bitrate();
            stats.framerate = static_cast<uint16_t>(framerate);
            stream_info =
                describe_stream(attached, capturer->display(), *encoder, frame.width,
                                frame.height, static_cast<uint16_t>(framerate),
                                session_->permissions(), audio_info);
            publish_stream(stream_info, find_display(attached, capturer->display()),
                           display_name_utf8(capturer->display()));
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
}

}  // namespace chast
