#include "viewer_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/clipboard.h"
#include "chast/decoder.h"
#include "chast/file_transfer.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "chast/quic.h"
#include "chast/security.h"

namespace chast_viewer {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReceiveTimeout = std::chrono::milliseconds(250);
// Input and audio are drained once per receive poll, so an idle desktop (where
// the receive timeout is the only thing pacing the loop) still carries pointer
// motion and audio frames within this bound.
constexpr auto kMediaPoll = std::chrono::milliseconds(10);
constexpr auto kFrameStall = std::chrono::seconds(20);
constexpr auto kKeyframeRetry = std::chrono::seconds(1);
constexpr auto kProbeInterval = std::chrono::seconds(1);
constexpr auto kClipboardPollInterval = std::chrono::milliseconds(500);
// How long one receive-loop pass may spend writing an upload, so a large file
// cannot starve video, audio and input.
constexpr auto kFileSendBudget = std::chrono::milliseconds(8);
// A bulk upload is paced far below the loopback ceiling: unpaced, a 1 GiB file
// fills the 32 MiB send queue and the viewer loop stalls on it, which costs the
// video stream almost all of its frames. The burst lets one pass move a few
// chunks without being reduced to a trickle by the poll cadence.
constexpr double kFileUploadBytesPerSecond = 32.0 * 1024.0 * 1024.0;
constexpr double kFileUploadBurst = 256.0 * 1024.0;
constexpr size_t kMaxPendingInput = 256;

std::string codec_name(chast::VideoCodec codec) {
    return codec == chast::VideoCodec::H264 ? "H.264" : "HEVC";
}

const char* audio_codec_name(chast::AudioCodec codec) {
    switch (codec) {
        case chast::AudioCodec::AAC:
            return "AAC-LC";
        case chast::AudioCodec::Opus:
            return "Opus";
    }
    return "unknown";
}

const char* switch_code_name(chast::DisplaySwitchCode code) {
    switch (code) {
        case chast::DisplaySwitchCode::Ok:
            return "the switch was accepted";
        case chast::DisplaySwitchCode::NotFound:
            return "the display is no longer attached";
        case chast::DisplaySwitchCode::Failed:
            return "the host could not switch displays";
        case chast::DisplaySwitchCode::Busy:
            return "the host is still settling the previous switch";
    }
    return "the host refused the switch";
}

const char* mode_code_name(chast::DisplayModeCode code) {
    switch (code) {
        case chast::DisplayModeCode::Ok:
            return "the change was accepted";
        case chast::DisplayModeCode::NotFound:
            return "the display or the mode is no longer available";
        case chast::DisplayModeCode::Failed:
            return "the host could not change the mode";
        case chast::DisplayModeCode::Busy:
            return "the host is still settling the previous change";
    }
    return "the host refused the change";
}

const char* stream_quality_name(chast::StreamQuality quality) {
    switch (quality) {
        case chast::StreamQuality::High:
            return "high";
        case chast::StreamQuality::Balanced:
            return "balanced";
        case chast::StreamQuality::Smooth:
            return "smooth";
    }
    return "unknown";
}

const char* stream_option_code_name(chast::StreamOptionsCode code) {
    switch (code) {
        case chast::StreamOptionsCode::Ok:
            return "the change was accepted";
        case chast::StreamOptionsCode::Unsupported:
            return "the host cannot provide that encoder or setting";
        case chast::StreamOptionsCode::Failed:
            return "the host could not apply the streaming options";
        case chast::StreamOptionsCode::Busy:
            return "the host is still settling the previous encoder change";
    }
    return "the host refused the streaming options";
}

const char* privacy_code_name(chast::PrivacyModeCode code) {
    switch (code) {
        case chast::PrivacyModeCode::Ok:
            return "the request was accepted";
        case chast::PrivacyModeCode::Denied:
            return "privacy mode is not permitted by the host";
        case chast::PrivacyModeCode::Unsupported:
            return "the host cannot hide its display";
        case chast::PrivacyModeCode::Failed:
            return "the host could not show or remove the mask";
    }
    return "the host refused privacy mode";
}

const char* sas_code_name(chast::SasCode code) {
    switch (code) {
        case chast::SasCode::Ok:
            return "Ctrl+Alt+Del was sent";
        case chast::SasCode::Denied:
            return "input control is not permitted by the host";
        case chast::SasCode::Unsupported:
            return "this host cannot send Ctrl+Alt+Del";
        case chast::SasCode::Failed:
            return "the host could not send Ctrl+Alt+Del";
    }
    return "the host refused Ctrl+Alt+Del";
}

const char* desktop_kind_name(chast::DesktopKind kind) {
    switch (kind) {
        case chast::DesktopKind::Default:
            return "the user desktop";
        case chast::DesktopKind::Secure:
            return "a secure desktop";
        case chast::DesktopKind::Unavailable:
            return "no accessible desktop";
    }
    return "an unknown desktop";
}

uint32_t formats_mask(const chast::ClipboardContent& content) {
    uint32_t mask = 0;
    for (const chast::ClipboardFormatData& format : content.formats) {
        mask |= 1u << static_cast<uint32_t>(format.format);
    }
    return mask;
}

const char* file_code_name(chast::FileCode code) {
    switch (code) {
        case chast::FileCode::Ok:
            return "the transfer completed";
        case chast::FileCode::Refused:
            return "the host refused the file";
        case chast::FileCode::Busy:
            return "the host is receiving another file";
        case chast::FileCode::NotFound:
            return "the host no longer knows this transfer";
        case chast::FileCode::DigestMismatch:
            return "the received bytes did not match the digest";
        case chast::FileCode::IoError:
            return "the host could not store the file";
        case chast::FileCode::Cancelled:
            return "the transfer was cancelled";
    }
    return "the transfer failed";
}

int64_t steady_us(std::chrono::steady_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
}

std::filesystem::path path_from_utf8(const std::string& path) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

}  // namespace

ViewerEngine::ViewerEngine(FrameSink* sink) : sink_(sink) {}

ViewerEngine::~ViewerEngine() {
    Stop();
}

bool ViewerEngine::Start(const std::string& host, uint16_t port, uint16_t display,
                         std::string pinned_fingerprint, bool allow_unverified,
                         std::string access_token, std::string password,
                         std::string totp,
                         std::shared_ptr<chast::DeviceIdentity> device_identity,
                         bool trust_device) {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // A previous session may have ended on its own while its hash was still
    // running; the new one starts from a clean upload slot.
    hash_cancel_.store(true, std::memory_order_release);
    if (hasher_.joinable()) {
        hasher_.join();
    }
    hash_cancel_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        file_ = FileUpload{};
        next_file_id_ = 1;
    }
    device_identity_ = std::move(device_identity);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = ViewerStatus{};
        status_.host = host;
        status_.port = port;
        status_.state = ViewerState::kConnecting;
        started_at_ = Clock::now();
    }
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        pending_input_.clear();
        input_enabled_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        pending_cursor_.reset();
        cursor_updates_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(switch_mutex_);
        pending_switch_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(mode_mutex_);
        pending_mode_list_.reset();
        pending_mode_set_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(stream_option_mutex_);
        pending_stream_options_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(privacy_mutex_);
        pending_privacy_.reset();
    }
    // A SAS request queued for a session that ended must not fire into the
    // next one.
    pending_sas_.store(false, std::memory_order_release);
    // Microphone capture is a per-session privacy decision: every session
    // starts with the uplink off, while the chosen device is remembered.
    microphone_wanted_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ViewerEngine::Run, this, host, port, display,
                          std::move(pinned_fingerprint), allow_unverified,
                          std::move(access_token), std::move(password),
                          std::move(totp), trust_device);
    return true;
}

void ViewerEngine::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    // sha256_file() has no cancellation hook; the flag at least keeps a hash
    // that has not started (or has just finished) from arming the slot again.
    hash_cancel_.store(true, std::memory_order_release);
    if (hasher_.joinable()) {
        hasher_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool ViewerEngine::SendInput(const chast::InputEvent& event) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!input_enabled_ || pending_input_.size() >= kMaxPendingInput) {
        return false;
    }
    PendingInput pending;
    pending.event = event;
    pending_input_.push_back(pending);
    return true;
}

bool ViewerEngine::ReleaseInput() {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!input_enabled_ || pending_input_.size() >= kMaxPendingInput) {
        return false;
    }
    PendingInput pending;
    pending.release = true;
    pending_input_.push_back(pending);
    return true;
}

bool ViewerEngine::SwitchDisplay(uint16_t display) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.input_enabled) {
            return false;
        }
        if (display != chast::kDisplayAuto &&
            static_cast<size_t>(display) >= status_.displays.size()) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(switch_mutex_);
    pending_switch_ = display;
    return true;
}

bool ViewerEngine::RequestDisplayModes(uint16_t display) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            static_cast<size_t>(display) >= status_.displays.size()) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mode_mutex_);
    pending_mode_list_ = display;
    return true;
}

bool ViewerEngine::SetDisplayMode(uint16_t display, uint16_t width, uint16_t height,
                                  uint16_t refresh_hz) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (width == 0 || height == 0 || refresh_hz == 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.input_enabled) {
            return false;
        }
        if (static_cast<size_t>(display) >= status_.displays.size()) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mode_mutex_);
    PendingMode pending;
    pending.display = display;
    pending.width = width;
    pending.height = height;
    pending.refresh_hz = refresh_hz;
    pending_mode_set_ = pending;
    return true;
}

bool ViewerEngine::SetStreamOptions(chast::StreamCodecPreference codec, uint16_t max_fps,
                                    chast::StreamQuality quality) {
    if (!running_.load(std::memory_order_acquire) || max_fps > chast::kMaxStreamFps) {
        return false;
    }
    switch (codec) {
        case chast::StreamCodecPreference::Auto:
        case chast::StreamCodecPreference::H264:
        case chast::StreamCodecPreference::Hevc:
            break;
        default:
            return false;
    }
    switch (quality) {
        case chast::StreamQuality::High:
        case chast::StreamQuality::Balanced:
        case chast::StreamQuality::Smooth:
            break;
        default:
            return false;
    }
    chast::StreamOptions options;
    options.codec = codec;
    options.max_fps = max_fps;
    options.quality = quality;
    std::lock_guard<std::mutex> lock(stream_option_mutex_);
    pending_stream_options_ = options;
    return true;
}

void ViewerEngine::SetClipboardSync(bool enabled) {
    clipboard_sync_.store(enabled, std::memory_order_release);
}

bool ViewerEngine::SendFile(const std::string& path) {
    if (!running_.load(std::memory_order_acquire) || path.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionFileTransfer) == 0) {
            return false;
        }
    }
    const std::filesystem::path source = path_from_utf8(path);
    std::error_code code;
    const uintmax_t size = std::filesystem::file_size(source, code);
    if (code || !std::filesystem::is_regular_file(source, code) || code ||
        size > chast::kMaxFileTransferBytes) {
        return false;
    }
    const std::u8string u8name = source.filename().u8string();
    std::string name(reinterpret_cast<const char*>(u8name.data()), u8name.size());
    if (name.empty() || name.size() > chast::kMaxFileNameBytes) {
        return false;
    }
    if (hasher_.joinable()) {
        // Only a terminal slot reaches this point, so its hash has already
        // published its result.
        hasher_.join();
    }
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (file_.stage != FileUpload::Stage::kNone &&
            file_.stage != FileUpload::Stage::kDone &&
            file_.stage != FileUpload::Stage::kFailed) {
            return false;
        }
        file_ = FileUpload{};
        file_.stage = FileUpload::Stage::kHashing;
        file_.path = path;
        file_.name = std::move(name);
        file_.size = size;
        file_.transfer_id = next_file_id_++;
    }
    hasher_ = std::thread(&ViewerEngine::HashPendingFile, this);
    return true;
}

void ViewerEngine::HashPendingFile() {
    std::string path;
    uint32_t transfer_id = 0;
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        path = file_.path;
        transfer_id = file_.transfer_id;
    }
    uint64_t size = 0;
    std::array<uint8_t, 32> digest{};
    std::string error;
    bool ok = chast::sha256_file(path, size, digest, &error);
    if (ok && size > chast::kMaxFileTransferBytes) {
        ok = false;
        error = "the file exceeds the protocol's 16 GiB limit";
    }
    if (hash_cancel_.load(std::memory_order_acquire)) {
        ok = false;
        error = "the upload was cancelled";
    }
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (file_.transfer_id != transfer_id ||
        file_.stage != FileUpload::Stage::kHashing) {
        return;
    }
    if (!ok) {
        file_.stage = FileUpload::Stage::kFailed;
        file_.error = error.empty() ? "the file could not be hashed" : error;
        std::fprintf(stderr, "viewer: file %s: %s\n", file_.name.c_str(),
                     file_.error.c_str());
        return;
    }
    file_.size = size;
    file_.digest = digest;
    file_.stage = FileUpload::Stage::kReady;
    std::fprintf(stderr, "viewer: file %s is %llu bytes\n", file_.name.c_str(),
                 static_cast<unsigned long long>(size));
}

bool ViewerEngine::TakeCursor(chast::CursorUpdate& update) {
    std::lock_guard<std::mutex> lock(cursor_mutex_);
    if (!pending_cursor_) {
        return false;
    }
    update = std::move(*pending_cursor_);
    pending_cursor_.reset();
    std::lock_guard<std::mutex> status_lock(mutex_);
    status_.cursor_pending = false;
    return true;
}

bool ViewerEngine::SetAudioMuted(bool mute) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.audio_enabled) {
            return false;
        }
    }
    audio_muted_.store(mute, std::memory_order_relaxed);
    return true;
}

bool ViewerEngine::SetMicrophone(bool enabled) {
    if (enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.microphone_authorized) {
            return false;
        }
    }
    microphone_wanted_.store(enabled, std::memory_order_relaxed);
    return true;
}

bool ViewerEngine::SelectMicrophone(std::string device_id) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(microphone_mutex_);
    microphone_device_id_ = std::move(device_id);
    return true;
}

bool ViewerEngine::SetPrivacyMode(bool enabled) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.privacy_supported) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(privacy_mutex_);
    pending_privacy_ = enabled;
    return true;
}

bool ViewerEngine::SendSas() {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.input_enabled) {
            return false;
        }
    }
    pending_sas_.store(true, std::memory_order_release);
    return true;
}

ViewerStatus ViewerEngine::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ViewerStatus status = status_;
    if (status.state == ViewerState::kConnecting || status.state == ViewerState::kHandshaking ||
        status.state == ViewerState::kStreaming) {
        status.elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at_)
                .count());
    }
    return status;
}

void ViewerEngine::SetState(ViewerState state, std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    status_.error = std::move(error);
}

void ViewerEngine::Run(std::string host, uint16_t port, uint16_t display,
                       std::string pinned_fingerprint, bool allow_unverified,
                       std::string access_token, std::string password,
                       std::string totp, bool trust_device) {
    const auto started = Clock::now();
    auto last_frame = started;
    auto last_keyframe_request = started - kKeyframeRetry;
    auto last_probe = started - kProbeInterval;
    auto window_start = started;

    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t keyframes = 0;
    uint64_t published = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t protocol_errors = 0;
    uint64_t gaps = 0;
    uint64_t bytes = 0;
    uint64_t input_sent = 0;
    uint64_t input_failed = 0;
    bool clipboard_enabled = false;
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_failed = 0;
    uint32_t clipboard_sent_formats = 0;
    uint32_t clipboard_received_formats = 0;
    uint64_t files_sent = 0;
    uint64_t file_failures = 0;
    uint64_t files_received = 0;
    uint64_t file_receive_failures = 0;
    uint64_t cursor_updates = 0;
    bool audio_announced = false;
    bool audio_finished = false;
    bool audio_broken = false;
    bool audio_enabled = false;
    bool audio_muted_applied = false;
    uint16_t permissions = 0;
    uint64_t permission_updates = 0;
    uint32_t audio_sample_rate = 0;
    uint32_t audio_channels = 0;
    uint32_t audio_bitrate = 0;
    uint64_t audio_received = 0;
    uint64_t audio_decoded = 0;
    uint64_t audio_errors = 0;
    uint64_t audio_underruns = 0;
    uint64_t audio_recoveries = 0;
    bool audio_recovering = false;
    uint32_t audio_buffer_ms = 0;
    std::string audio_error;
    std::unique_ptr<chast::IMicrophoneCapture> microphone_capture;
    bool microphone_authorized = false;
    bool microphone_enabled = false;
    bool microphone_stream_open = false;
    std::string microphone_device;
    std::string microphone_name;
    uint64_t microphone_packets = 0;
    uint64_t microphone_dropped = 0;
    uint64_t microphone_send_dropped = 0;
    uint64_t microphone_errors = 0;
    uint64_t microphone_recoveries = 0;
    bool microphone_recovering = false;
    std::string microphone_error;
    bool privacy_supported = false;
    bool privacy_enabled = false;
    int privacy_code = 0;
    std::string privacy_error;
    uint64_t privacy_updates = 0;
    chast::DesktopKind desktop = chast::DesktopKind::Default;
    uint64_t desktop_updates = 0;
    int sas_code = 0;
    std::string sas_error;
    uint64_t sas_updates = 0;
    uint64_t display_switches = 0;
    uint64_t display_switch_failures = 0;
    std::string display_switch_error;
    uint64_t display_mode_lists = 0;
    uint64_t display_mode_changes = 0;
    uint64_t display_mode_failures = 0;
    std::string display_mode_error;
    std::string display_modes_error;
    int stream_quality = 0;
    int stream_option_code = 0;
    std::string stream_option_error;
    uint64_t stream_option_results = 0;
    double rtt_ms = 0.0;
    double latency_ms = 0.0;
    bool clock_synced = false;
    int64_t clock_offset_us = 0;
    uint64_t digest = chast::kFrameDigestSeed;
    uint64_t window_frames = 0;
    uint64_t window_bytes = 0;
    uint32_t display_width = 0;
    uint32_t display_height = 0;
    // Pointer and input space: the captured desktop size, unchanged by the
    // host's resolution ladder. Display sizes above follow the decoded
    // picture.
    uint32_t display_source_width = 0;
    uint32_t display_source_height = 0;
    bool video_finished = false;
    bool handshaken = false;
    bool logged_frame = false;
    bool logged_drop = false;
    uint64_t init_errors = 0;
    std::string failure;
    std::unique_ptr<chast::IAudioDecoder> audio_decoder;
    std::unique_ptr<chast::IAudioPlayer> audio_player;

    // Folds the worker's counters into the status the UI polls. Called from
    // this thread only; the window counters are reset once per second.
    auto sync_status = [&](bool final) {
        const auto now = Clock::now();
        const double window_seconds =
            std::chrono::duration<double>(now - window_start).count();
        double fps = 0.0;
        double kbps = 0.0;
        if (audio_player) {
            const chast::AudioPlayerStats audio_stats = audio_player->stats();
            audio_buffer_ms = audio_stats.buffer_ms;
            audio_underruns = audio_stats.underruns;
            audio_recoveries = audio_stats.recoveries;
            audio_recovering = audio_stats.recovering;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (window_seconds >= 1.0) {
                fps = static_cast<double>(window_frames) / window_seconds;
                kbps = static_cast<double>(window_bytes) * 8.0 / 1000.0 / window_seconds;
                status_.fps = fps;
                status_.kbps = kbps;
            }
            status_.received = received;
            status_.decoded = decoded;
            status_.keyframes = keyframes;
            status_.published = published;
            status_.crc_errors = crc_errors;
            status_.decode_errors = decode_errors;
            status_.protocol_errors = protocol_errors;
            status_.gaps = gaps;
            status_.bytes = bytes;
            status_.rtt_ms = rtt_ms;
            status_.latency_ms = latency_ms;
            status_.width = display_width;
            status_.height = display_height;
            status_.source_width = display_source_width;
            status_.source_height = display_source_height;
            status_.input_sent = input_sent;
            status_.input_failed = input_failed;
            status_.clipboard_enabled = clipboard_enabled;
            status_.clipboard_sent = clipboard_sent;
            status_.clipboard_received = clipboard_received;
            status_.clipboard_failed = clipboard_failed;
            status_.clipboard_sent_formats = clipboard_sent_formats;
            status_.clipboard_received_formats = clipboard_received_formats;
            status_.files_sent = files_sent;
            status_.file_failures = file_failures;
            status_.files_received = files_received;
            status_.file_receive_failures = file_receive_failures;
            {
                std::lock_guard<std::mutex> file_lock(file_mutex_);
                status_.download_name = download_.name;
                status_.download_size = download_.size;
                status_.download_received = download_.received;
                status_.download_error = download_.error;
                switch (download_.stage) {
                    case FileDownload::Stage::kNone:
                        status_.download_phase = FilePhase::kIdle;
                        break;
                    case FileDownload::Stage::kReceiving:
                        status_.download_phase = FilePhase::kSending;
                        break;
                    case FileDownload::Stage::kDone:
                        status_.download_phase = FilePhase::kDone;
                        break;
                    case FileDownload::Stage::kFailed:
                        status_.download_phase = FilePhase::kFailed;
                        break;
                }
            }
            {
                std::lock_guard<std::mutex> file_lock(file_mutex_);
                status_.file_name = file_.name;
                status_.file_size = file_.size;
                status_.file_sent = file_.sent;
                status_.file_error = file_.error;
                switch (file_.stage) {
                    case FileUpload::Stage::kNone:
                        status_.file_phase = FilePhase::kIdle;
                        break;
                    case FileUpload::Stage::kHashing:
                        status_.file_phase = FilePhase::kHashing;
                        break;
                    case FileUpload::Stage::kReady:
                    case FileUpload::Stage::kOffered:
                        status_.file_phase = FilePhase::kOffered;
                        break;
                    case FileUpload::Stage::kSending:
                    case FileUpload::Stage::kFinishing:
                        status_.file_phase = FilePhase::kSending;
                        break;
                    case FileUpload::Stage::kDone:
                        status_.file_phase = FilePhase::kDone;
                        break;
                    case FileUpload::Stage::kFailed:
                        status_.file_phase = FilePhase::kFailed;
                        break;
                }
            }
            status_.cursor_updates = cursor_updates;
            status_.audio_announced = audio_announced;
            status_.audio_enabled = audio_enabled;
            status_.audio_muted = audio_muted_.load(std::memory_order_relaxed);
            status_.audio_sample_rate = audio_sample_rate;
            status_.audio_channels = audio_channels;
            status_.audio_bitrate = audio_bitrate;
            status_.audio_received = audio_received;
            status_.audio_decoded = audio_decoded;
            status_.audio_errors = audio_errors;
            status_.audio_underruns = audio_underruns;
            status_.audio_recoveries = audio_recoveries;
            status_.audio_recovering = audio_recovering;
            status_.audio_buffer_ms = audio_buffer_ms;
            status_.audio_error = audio_error;
            status_.microphone_authorized = microphone_authorized;
            status_.microphone_enabled = microphone_enabled;
            status_.microphone_device = microphone_device;
            status_.microphone_name = microphone_name;
            status_.microphone_packets = microphone_packets;
            status_.microphone_dropped = microphone_dropped;
            status_.microphone_errors = microphone_errors;
            status_.microphone_recoveries = microphone_recoveries;
            status_.microphone_recovering = microphone_recovering;
            status_.microphone_error = microphone_error;
            status_.privacy_supported = privacy_supported;
            status_.privacy_enabled = privacy_enabled;
            status_.privacy_code = privacy_code;
            status_.privacy_error = privacy_error;
            status_.privacy_updates = privacy_updates;
            status_.desktop = desktop;
            status_.desktop_updates = desktop_updates;
            status_.sas_code = sas_code;
            status_.sas_error = sas_error;
            status_.sas_updates = sas_updates;
            status_.display_switches = display_switches;
            status_.display_switch_failures = display_switch_failures;
            status_.display_switch_error = display_switch_error;
            status_.display_mode_lists = display_mode_lists;
            status_.display_mode_changes = display_mode_changes;
            status_.display_mode_failures = display_mode_failures;
            status_.display_mode_error = display_mode_error;
            status_.display_modes_error = display_modes_error;
            status_.stream_quality = stream_quality;
            status_.stream_option_code = stream_option_code;
            status_.stream_option_error = stream_option_error;
            status_.stream_option_results = stream_option_results;
            status_.elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
            if (final) {
                status_.video_finished = video_finished;
            }
        }
        if (window_seconds >= 1.0) {
            window_frames = 0;
            window_bytes = 0;
            window_start = now;
        }
    };

    auto fail = [&](std::string message) {
        failure = std::move(message);
    };

    chast::QuicClientConfig client_config;
    client_config.host = host;
    client_config.port = port;
    client_config.pinned_fingerprint = std::move(pinned_fingerprint);
    client_config.allow_any_certificate = allow_unverified;
    client_config.access_token = std::move(access_token);
    client_config.password = std::move(password);
    client_config.totp = std::move(totp);
    client_config.device_identity = device_identity_.get();
    client_config.trust_device = trust_device;
    std::string error;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(client_config, &error);
    if (!session) {
        fail("QUIC client: " + error);
        sync_status(true);
        std::fprintf(stderr, "viewer: %s\n", failure.c_str());
        SetState(ViewerState::kFailed, failure);
        running_.store(false, std::memory_order_release);
        return;
    }

    // Stops the uplink: the capture thread first (it owns the endpoint), then
    // the CMIC stream, so the host sees the switch go off. `switch_off`
    // clears the UI's request too, which is what a failure and a revocation
    // want; closing the stream is also how the host stops decoding.
    auto stop_microphone = [&](const std::string& reason, bool switch_off) {
        if (microphone_capture) {
            const chast::AudioCaptureStats stats = microphone_capture->stats();
            microphone_dropped = stats.dropped;
            microphone_recoveries = stats.recoveries;
            microphone_capture->stop();
            microphone_capture.reset();
        }
        if (microphone_stream_open) {
            session->stop_microphone();
            microphone_stream_open = false;
        }
        microphone_recovering = false;
        microphone_enabled = false;
        if (switch_off) {
            microphone_wanted_.store(false, std::memory_order_relaxed);
        }
        if (!reason.empty()) {
            microphone_error = reason;
            std::fprintf(stderr, "viewer: microphone stopped: %s\n", reason.c_str());
        }
    };

    chast::Error connected = session->connect(std::chrono::seconds(15));
    // The certificate callback records the presented fingerprint before it
    // decides, so even a rejected identity is visible to the UI.
    const std::string server_fingerprint = session->peer_fingerprint();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.server_fingerprint = server_fingerprint;
    }
    if (!server_fingerprint.empty()) {
        std::fprintf(stderr, "viewer: server identity SHA256 %s\n",
                     chast::format_fingerprint(server_fingerprint).c_str());
    }
    if (!connected.ok()) {
        fail("connect: " + connected.to_string());
    } else if (server_fingerprint.empty()) {
        failure = "the server presented no certificate";
    } else {
        SetState(ViewerState::kHandshaking);
        chast::ServerInfo info;
        chast::Error handshake =
            session->handshake(info, std::chrono::seconds(10), display);
        if (!handshake.ok()) {
            if (handshake.status() == chast::Status::PermissionDenied) {
                // The server named its refusal; the UI picks the wording from
                // the code and shows `error` as the server's reason.
                const std::optional<chast::AccessDenial> denial = session->denial();
                std::lock_guard<std::mutex> lock(mutex_);
                status_.denial =
                    denial.has_value() ? static_cast<int>(*denial) : 0;
            }
            fail("handshake: " + handshake.to_string());
        } else {
            handshaken = true;
            display_width = info.width;
            display_height = info.height;
            display_source_width = info.source_width;
            display_source_height = info.source_height;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.codec = info.codec;
                status_.width = info.width;
                status_.height = info.height;
                status_.source_width = info.source_width;
                status_.source_height = info.source_height;
                status_.framerate = info.framerate;
                status_.server_bitrate = info.bitrate;
                status_.server_encoder = info.encoder;
                status_.input_enabled = info.input_enabled;
                status_.clipboard_enabled = info.clipboard_enabled;
                status_.origin_x = info.origin_x;
                status_.origin_y = info.origin_y;
                status_.displays = info.displays;
            }
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                input_enabled_ = info.input_enabled;
            }
            clipboard_enabled = info.clipboard_enabled;

            // Audio is optional end to end: a missing render endpoint or a
            // decoder that cannot start leaves video untouched and reports
            // why through audio_error.
            auto stop_audio_pipeline = [&] {
                if (audio_player) {
                    const chast::AudioPlayerStats stats = audio_player->stats();
                    audio_buffer_ms = stats.buffer_ms;
                    audio_underruns = stats.underruns;
                    audio_recoveries = stats.recoveries;
                    audio_recovering = stats.recovering;
                    audio_player->stop();
                    audio_player.reset();
                }
                audio_decoder.reset();
                audio_enabled = false;
            };
            auto start_audio = [&](const chast::AudioInfo& audio) {
                audio_announced = audio.enabled;
                audio_finished = false;
                audio_broken = false;
                audio_sample_rate = audio.sample_rate;
                audio_channels = audio.channels;
                audio_bitrate = audio.bitrate;
                if (!audio.enabled) {
                    return;
                }
                std::string audio_setup_error;
                // The host chooses the codec (Opus for system sound since
                // protocol v18); dispatch instead of assuming AAC.
                audio_decoder = chast::create_audio_decoder(audio, &audio_setup_error);
                if (!audio_decoder) {
                    audio_error = "decoder: " + audio_setup_error;
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    return;
                }
                chast::AudioPlayerConfig player_config;
                audio_player =
                    chast::create_audio_player(player_config, &audio_setup_error);
                if (!audio_player) {
                    audio_decoder.reset();
                    audio_error = "playback: " + audio_setup_error;
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    return;
                }
                const bool muted = audio_muted_.load(std::memory_order_relaxed);
                audio_player->set_mute(muted);
                audio_muted_applied = muted;
                chast::Error audio_started = audio_player->start();
                if (!audio_started.ok()) {
                    audio_error = "playback: " + audio_started.to_string();
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    audio_player.reset();
                    audio_decoder.reset();
                    return;
                }
                audio_enabled = true;
                audio_error.clear();
                std::fprintf(stderr,
                             "viewer: audio %s %u Hz %u ch %u kbps, decoder and "
                             "renderer running\n",
                             audio_codec_name(audio.codec), audio.sample_rate,
                             audio.channels, audio.bitrate / 1000u);
            };
            start_audio(info.audio);

            // Applies a permission mask to every gate the session exposes:
            // the UI's queue gates, the flags it polls, and the counters it
            // watches for a live change. Queued events that the host no
            // longer accepts are dropped here rather than sent and refused.
            auto apply_permissions = [&](uint16_t mask) {
                if (permission_updates != 0 && mask == permissions) {
                    return;
                }
                permissions = mask;
                ++permission_updates;
                const bool input = (mask & chast::kPermissionInput) != 0;
                const bool clipboard = (mask & chast::kPermissionClipboard) != 0;
                const bool microphone = (mask & chast::kPermissionMicrophone) != 0;
                const bool privacy = (mask & chast::kPermissionPrivacyMode) != 0;
                {
                    std::lock_guard<std::mutex> lock(input_mutex_);
                    input_enabled_ = input;
                    if (!input) {
                        pending_input_.clear();
                    }
                }
                clipboard_enabled = clipboard;
                microphone_authorized = microphone;
                privacy_supported = privacy;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.permissions = mask;
                    status_.permission_updates = permission_updates;
                    status_.input_enabled = input;
                    status_.clipboard_enabled = clipboard;
                    status_.microphone_authorized = microphone;
                    status_.privacy_supported = privacy;
                }
                // The transport aborts the stream on revocation; stopping here
                // releases the microphone at once and tells the UI why.
                if (!microphone && (microphone_capture || microphone_stream_open)) {
                    stop_microphone("the host revoked the microphone", true);
                }
            };
            apply_permissions(session->permissions());

            std::fprintf(stderr,
                         "viewer: display request %s; %zu announced, stream origin %d,%d\n",
                         display == chast::kDisplayAuto ? "auto"
                                                       : std::to_string(display).c_str(),
                         info.displays.size(), info.origin_x, info.origin_y);

            chast::DecoderConfig decoder_config;
            decoder_config.codec = info.codec;
            if (sink_ == nullptr || sink_->device() == nullptr) {
                fail("display: " +
                     (sink_ != nullptr ? sink_->error()
                                       : std::string("the frame sink is not available")));
            }
            // Decode straight into a D3D11 texture and hand each picture to
            // the sink while the decoder still owns the surface: the image
            // never reaches system memory.
            decoder_config.d3d11_device = sink_ != nullptr ? sink_->device() : nullptr;
            decoder_config.gpu_handler =
                [&](const chast::GpuFrame& frame) -> chast::Error {
                ++decoded;
                display_width = frame.width;
                display_height = frame.height;
                if (sink_ != nullptr &&
                    sink_->OnGpuFrame(frame.texture, frame.subresource, frame.width,
                                      frame.height, frame.crop_x, frame.crop_y,
                                      frame.pts_ms)) {
                    ++published;
                    ++window_frames;
                    if (!logged_frame) {
                        logged_frame = true;
                        std::fprintf(stderr, "viewer: first GPU frame %ux%u\n", frame.width,
                                     frame.height);
                    }
                    return chast::Error::success();
                }
                if (sink_ == nullptr) {
                    return chast::Error(chast::Status::Unsupported,
                                        "display: the frame sink is not available");
                }
                if (!sink_->error().empty()) {
                    return chast::Error(chast::Status::CaptureFailed,
                                        "display: " + sink_->error());
                }
                if (!logged_drop) {
                    logged_drop = true;
                    std::fprintf(stderr,
                                 "viewer: the display has no GPU output; dropping frames\n");
                }
                // The plugin texture is gone (for example while the UI is
                // tearing the page down); dropping keeps the decoder running.
                return chast::Error::success();
            };
            std::unique_ptr<chast::IVideoDecoder> decoder =
                decoder_config.d3d11_device != nullptr
                    ? chast::create_decoder(decoder_config, &error)
                    : nullptr;
            if (!decoder && failure.empty()) {
                fail("decoder (" + codec_name(info.codec) + "): " + error);
            } else if (decoder) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.decoder = decoder->implementation();
                }
                std::fprintf(stderr, "viewer: decoder %s\n", decoder->implementation().c_str());
                SetState(ViewerState::kStreaming);

                auto request_keyframe = [&] {
                    const auto now = Clock::now();
                    if (now - last_keyframe_request >= kKeyframeRetry) {
                        last_keyframe_request = now;
                        session->request_keyframe();
                    }
                };

                // Sends everything the UI queued since the last poll, in
                // order. Runs on this thread, which owns the session, so
                // send_input() never races receive_frame().
                auto drain_input = [&] {
                    if ((permissions & chast::kPermissionInput) == 0) {
                        std::lock_guard<std::mutex> lock(input_mutex_);
                        pending_input_.clear();
                        return;
                    }
                    std::deque<PendingInput> events;
                    {
                        std::lock_guard<std::mutex> lock(input_mutex_);
                        events.swap(pending_input_);
                    }
                    for (const PendingInput& pending : events) {
                        chast::Error sent = pending.release
                                                ? session->release_input()
                                                : session->send_input(pending.event);
                        if (sent.ok()) {
                            ++input_sent;
                            continue;
                        }
                        ++input_failed;
                        if (input_failed <= 3) {
                            std::fprintf(stderr, "viewer: %s: %s\n",
                                         pending.release ? "release_input" : "send_input",
                                         sent.to_string().c_str());
                        }
                    }
                };

                uint64_t expected_seq = 0;
                uint64_t clipboard_seq = 1;
                std::vector<chast::DecodedFrame> frames;

                // Rich clipboard runs natively, so a 32 MiB bitmap never
                // crosses the Dart boundary. The baseline is what the local
                // clipboard held when the gate opened; later changes are
                // uploaded, and anything the host sends is written locally
                // and remembered so the next poll cannot echo it back.
                bool clipboard_was_allowed = false;
                uint64_t clipboard_hash = 0;
                auto clipboard_allowed = [&] {
                    return clipboard_enabled &&
                           (permissions & chast::kPermissionClipboard) != 0 &&
                           clipboard_sync_.load(std::memory_order_relaxed);
                };
                auto remember_local_clipboard = [&] {
                    chast::ClipboardContent current;
                    std::string ignored;
                    if (chast::read_clipboard(current, &ignored)) {
                        clipboard_hash = chast::clipboard_content_hash(current);
                    }
                };
                auto next_clipboard_poll = Clock::now();

                auto poll_clipboard = [&] {
                    const auto now = Clock::now();
                    if (!clipboard_allowed()) {
                        clipboard_was_allowed = false;
                        return;
                    }
                    if (!clipboard_was_allowed) {
                        // Re-enabling starts from a fresh baseline instead of
                        // flushing everything that changed while paused.
                        clipboard_was_allowed = true;
                        remember_local_clipboard();
                        next_clipboard_poll = now + kClipboardPollInterval;
                        return;
                    }
                    if (now < next_clipboard_poll) {
                        return;
                    }
                    next_clipboard_poll = now + kClipboardPollInterval;
                    chast::ClipboardContent current;
                    std::string read_error;
                    if (!chast::read_clipboard(current, &read_error)) {
                        return;
                    }
                    const uint64_t hash = chast::clipboard_content_hash(current);
                    if (hash == clipboard_hash) {
                        return;
                    }
                    // Remember content that cannot go on the wire too, so the
                    // same oversized item is not retried every 500 ms.
                    clipboard_hash = hash;
                    std::string content_error;
                    if (!chast::valid_clipboard_content(current, &content_error)) {
                        std::fprintf(stderr,
                                     "viewer: clipboard: the local item is not "
                                     "sendable: %s\n",
                                     content_error.c_str());
                        return;
                    }
                    clipboard_sent_formats = formats_mask(current);
                    current.seq = clipboard_seq;
                    chast::Error sent = session->send_clipboard_content(std::move(current));
                    if (!sent.ok()) {
                        ++clipboard_failed;
                        if (clipboard_failed <= 3) {
                            std::fprintf(stderr, "viewer: send clipboard: %s\n",
                                         sent.to_string().c_str());
                        }
                        return;
                    }
                    ++clipboard_seq;
                    ++clipboard_sent;
                    std::fprintf(stderr, "viewer: clipboard sent (formats 0x%x)\n",
                                 clipboard_sent_formats);
                };

                auto apply_peer_clipboard = [&](chast::ClipboardContent content) {
                    const uint64_t hash = chast::clipboard_content_hash(content);
                    if (hash == clipboard_hash) {
                        return;
                    }
                    std::string content_error;
                    if (!chast::valid_clipboard_content(content, &content_error)) {
                        ++clipboard_failed;
                        std::fprintf(stderr,
                                     "viewer: clipboard: the host sent unusable "
                                     "content: %s\n",
                                     content_error.c_str());
                        return;
                    }
                    std::string write_error;
                    if (!chast::write_clipboard(content, &write_error)) {
                        ++clipboard_failed;
                        if (clipboard_failed <= 3) {
                            std::fprintf(stderr, "viewer: write clipboard: %s\n",
                                         write_error.c_str());
                        }
                        return;
                    }
                    clipboard_hash = hash;
                    clipboard_received_formats = formats_mask(content);
                    ++clipboard_received;
                    std::fprintf(stderr, "viewer: clipboard written (formats 0x%x)\n",
                                 clipboard_received_formats);
                };

                auto receive_clipboard = [&] {
                    chast::ClipboardContent content;
                    while (clipboard_allowed() && session->take_clipboard_content(content)) {
                        apply_peer_clipboard(std::move(content));
                    }
                    // A v15 host may still send the text message; treat it as
                    // a text-only item rather than dropping it.
                    chast::ClipboardUpdate legacy;
                    while (clipboard_allowed() && session->take_clipboard_update(legacy)) {
                        chast::ClipboardContent text_only;
                        text_only.seq = legacy.seq;
                        chast::ClipboardFormatData text;
                        text.format = chast::ClipboardFormat::Text;
                        text.data.assign(legacy.text.begin(), legacy.text.end());
                        text_only.formats.push_back(std::move(text));
                        apply_peer_clipboard(std::move(text_only));
                    }
                };

                // One upload at a time. The hash was computed off this thread;
                // here the file is offered, opened at the host's resume
                // offset and streamed in time-budgeted chunks so video, audio
                // and input keep flowing.
                std::ifstream file_stream;
                double file_tokens = kFileUploadBurst;
                Clock::time_point file_token_time = Clock::now();
                auto file_fail = [&](const std::string& message) {
                    uint32_t transfer_id = 0;
                    std::string name;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        const FileUpload::Stage stage = file_.stage;
                        if (stage == FileUpload::Stage::kNone ||
                            stage == FileUpload::Stage::kDone ||
                            stage == FileUpload::Stage::kFailed) {
                            return;
                        }
                        transfer_id = file_.transfer_id;
                        name = file_.name;
                        file_.stage = FileUpload::Stage::kFailed;
                        file_.error = message;
                    }
                    file_stream.close();
                    // Frees the host's receive slot and its .part; harmless
                    // when the host already ended the transfer itself.
                    session->cancel_file_upload(transfer_id);
                    ++file_failures;
                    std::fprintf(stderr, "viewer: file %s failed: %s\n", name.c_str(),
                                 message.c_str());
                };

                auto drive_file_upload = [&] {
                    FileUpload snapshot;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        snapshot = file_;
                    }
                    // Every result ends the attempt it belongs to, including
                    // a host-side cancellation of a stream we still own.
                    chast::FileResult result;
                    while (session->take_file_result(result)) {
                        if (result.transfer_id != snapshot.transfer_id) {
                            continue;
                        }
                        const FileUpload::Stage stage = snapshot.stage;
                        if (stage != FileUpload::Stage::kOffered &&
                            stage != FileUpload::Stage::kSending &&
                            stage != FileUpload::Stage::kFinishing) {
                            continue;
                        }
                        file_stream.close();
                        if (result.code == chast::FileCode::Ok) {
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                file_.stage = FileUpload::Stage::kDone;
                                file_.error.clear();
                            }
                            ++files_sent;
                            std::fprintf(stderr,
                                         "viewer: file %s delivered (%llu bytes)\n",
                                         snapshot.name.c_str(),
                                         static_cast<unsigned long long>(result.received));
                        } else {
                            const std::string reason =
                                result.reason.empty() ? file_code_name(result.code)
                                                      : result.reason;
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                file_.stage = FileUpload::Stage::kFailed;
                                file_.error = reason;
                            }
                            ++file_failures;
                            std::fprintf(stderr, "viewer: file %s failed: %s\n",
                                         snapshot.name.c_str(), reason.c_str());
                        }
                        return;
                    }
                    switch (snapshot.stage) {
                        case FileUpload::Stage::kNone:
                        case FileUpload::Stage::kHashing:
                        case FileUpload::Stage::kDone:
                        case FileUpload::Stage::kFailed:
                            return;
                        default:
                            break;
                    }
                    if ((permissions & chast::kPermissionFileTransfer) == 0) {
                        file_fail("the host revoked file transfer");
                        return;
                    }
                    if (snapshot.stage == FileUpload::Stage::kReady) {
                        chast::FileOffer offer;
                        offer.transfer_id = snapshot.transfer_id;
                        offer.size = snapshot.size;
                        offer.resume = true;
                        offer.sha256 = snapshot.digest;
                        offer.name = snapshot.name;
                        chast::Error sent = session->offer_file(offer);
                        if (!sent.ok()) {
                            file_fail("offer: " + sent.to_string());
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (file_.transfer_id == snapshot.transfer_id &&
                                file_.stage == FileUpload::Stage::kReady) {
                                file_.stage = FileUpload::Stage::kOffered;
                            }
                        }
                        std::fprintf(stderr, "viewer: file %s offered (%llu bytes)\n",
                                     snapshot.name.c_str(),
                                     static_cast<unsigned long long>(snapshot.size));
                        return;
                    }
                    if (snapshot.stage == FileUpload::Stage::kOffered) {
                        chast::FileAnswer answer;
                        while (session->take_file_answer(answer)) {
                            if (answer.transfer_id != snapshot.transfer_id) {
                                continue;
                            }
                            if (answer.code != chast::FileCode::Ok) {
                                file_fail(answer.reason.empty() ? file_code_name(answer.code)
                                                                : answer.reason);
                                return;
                            }
                            if (answer.resume_offset > snapshot.size) {
                                file_fail("the host asked to resume past the end of the file");
                                return;
                            }
                            file_stream.open(path_from_utf8(snapshot.path), std::ios::binary);
                            if (!file_stream) {
                                file_fail("the local file cannot be opened");
                                return;
                            }
                            file_stream.seekg(static_cast<std::streamoff>(answer.resume_offset),
                                              std::ios::beg);
                            if (!file_stream) {
                                file_fail("the local file cannot seek to the resume offset");
                                return;
                            }
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                if (file_.transfer_id != snapshot.transfer_id ||
                                    file_.stage != FileUpload::Stage::kOffered) {
                                    return;
                                }
                                file_.stage = FileUpload::Stage::kSending;
                                file_.resume_offset = answer.resume_offset;
                                file_.sent = answer.resume_offset;
                            }
                            file_tokens = kFileUploadBurst;
                            file_token_time = Clock::now();
                            std::fprintf(stderr,
                                         "viewer: file %s accepted, sending from %llu\n",
                                         snapshot.name.c_str(),
                                         static_cast<unsigned long long>(
                                             answer.resume_offset));
                            return;
                        }
                        return;
                    }
                    if (snapshot.stage != FileUpload::Stage::kSending) {
                        return;
                    }
                    const auto deadline = Clock::now() + kFileSendBudget;
                    const auto refill = Clock::now();
                    file_tokens = std::min(
                        kFileUploadBurst,
                        file_tokens + std::chrono::duration<double>(refill - file_token_time)
                                          .count() *
                                          kFileUploadBytesPerSecond);
                    file_token_time = refill;
                    std::array<uint8_t, chast::kFileChunkBytes> buffer{};
                    uint64_t sent = snapshot.sent;
                    while (sent < snapshot.size && Clock::now() < deadline) {
                        const size_t want = static_cast<size_t>(std::min<uint64_t>(
                            snapshot.size - sent, buffer.size()));
                        if (file_tokens < static_cast<double>(want)) {
                            break;
                        }
                        file_stream.read(reinterpret_cast<char*>(buffer.data()),
                                         static_cast<std::streamsize>(want));
                        const std::streamsize got = file_stream.gcount();
                        if (got <= 0) {
                            file_fail("the local file ended before its announced size");
                            return;
                        }
                        chast::Error wrote = session->try_send_file_data(
                            snapshot.transfer_id, buffer.data(), static_cast<size_t>(got));
                        if (wrote.status() == chast::Status::WaitTimeout) {
                            // The shared queue is full: leave the piece for the
                            // next pass rather than stall the receive loop.
                            file_stream.clear();
                            file_stream.seekg(static_cast<std::streamoff>(sent),
                                              std::ios::beg);
                            break;
                        }
                        if (!wrote.ok()) {
                            file_fail("send: " + wrote.to_string());
                            return;
                        }
                        sent += static_cast<uint64_t>(got);
                        file_tokens -= static_cast<double>(got);
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (file_.transfer_id == snapshot.transfer_id &&
                            file_.stage == FileUpload::Stage::kSending) {
                            file_.sent = sent;
                        }
                    }
                    if (sent >= snapshot.size) {
                        chast::Error finished =
                            session->finish_file_upload(snapshot.transfer_id);
                        if (!finished.ok()) {
                            file_fail("finish: " + finished.to_string());
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (file_.transfer_id == snapshot.transfer_id &&
                                file_.stage == FileUpload::Stage::kSending) {
                                file_.stage = FileUpload::Stage::kFinishing;
                            }
                        }
                        std::fprintf(stderr, "viewer: file %s sent, waiting for the host\n",
                                     snapshot.name.c_str());
                    }
                };

                // The host's download. Offered files are accepted into the
                // local receive directory while file transfer is granted; the
                // .part file is renamed only after the digest checks out.
                const std::string download_directory = chast::default_receive_directory();
                auto download_fail = [&](const std::string& message, bool report) {
                    uint32_t transfer_id = 0;
                    std::string name;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (download_.stage != FileDownload::Stage::kReceiving) {
                            return;
                        }
                        transfer_id = download_.transfer_id;
                        name = download_.name;
                        if (download_.receiver) {
                            download_.receiver->abort(true);
                            download_.receiver.reset();
                        }
                        download_.stage = FileDownload::Stage::kFailed;
                        download_.error = message;
                    }
                    if (report) {
                        (void)session->send_file_result(transfer_id, chast::FileCode::IoError, 0,
                                                        message);
                    }
                    ++file_receive_failures;
                    std::fprintf(stderr, "viewer: download %s failed: %s\n", name.c_str(),
                                 message.c_str());
                };

                auto drain_downloads = [&] {
                    if ((permissions & chast::kPermissionFileTransfer) == 0) {
                        bool active = false;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            active = download_.stage == FileDownload::Stage::kReceiving;
                        }
                        if (active) {
                            download_fail("the host revoked file transfer", false);
                        }
                    }
                    chast::FileOffer offer;
                    while (session->take_file_offer(offer)) {
                        if ((permissions & chast::kPermissionFileTransfer) == 0) {
                            continue;
                        }
                        bool busy = false;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            busy = download_.stage == FileDownload::Stage::kReceiving;
                        }
                        if (busy || download_directory.empty()) {
                            ++file_receive_failures;
                            (void)session->send_file_answer(
                                offer.transfer_id, chast::FileCode::Busy, 0,
                                busy ? "another download is running"
                                     : "the viewer has no download directory");
                            continue;
                        }
                        chast::FileReceiveOptions options;
                        options.directory = download_directory;
                        options.name = offer.name;
                        options.size = offer.size;
                        options.sha256 = offer.sha256;
                        options.allow_resume = offer.resume;
                        uint64_t resume_offset = 0;
                        std::string receive_error;
                        std::unique_ptr<chast::FileReceiver> receiver = chast::FileReceiver::create(
                            options, &resume_offset, &receive_error);
                        if (!receiver) {
                            ++file_receive_failures;
                            (void)session->send_file_answer(offer.transfer_id, chast::FileCode::Refused,
                                                            0, receive_error);
                            std::fprintf(stderr, "viewer: download %s refused: %s\n",
                                         offer.name.c_str(), receive_error.c_str());
                            continue;
                        }
                        const std::string name = receiver->name();
                        const uint64_t size = offer.size;
                        const uint32_t transfer_id = offer.transfer_id;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            download_ = FileDownload{};
                            download_.stage = FileDownload::Stage::kReceiving;
                            download_.transfer_id = transfer_id;
                            download_.name = name;
                            download_.size = size;
                            download_.received = resume_offset;
                            download_.answer_time = Clock::now();
                            download_.receiver = std::move(receiver);
                        }
                        chast::Error answered = session->send_file_answer(
                            transfer_id, chast::FileCode::Ok, resume_offset);
                        if (!answered.ok()) {
                            download_fail("cannot accept the download: " + answered.to_string(),
                                          false);
                            continue;
                        }
                        std::fprintf(stderr, "viewer: receiving %s (%llu bytes%s)\n", name.c_str(),
                                     static_cast<unsigned long long>(size),
                                     resume_offset > 0 ? ", resuming" : "");
                    }

                    bool receiving = false;
                    uint32_t transfer_id = 0;
                    uint64_t answer_age_ms = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        receiving = download_.stage == FileDownload::Stage::kReceiving;
                        transfer_id = download_.transfer_id;
                        answer_age_ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now() - download_.answer_time)
                                .count());
                    }
                    if (!receiving) {
                        return;
                    }
                    for (;;) {
                        chast::FileChunk chunk;
                        chast::Error got = session->receive_file_chunk(
                            chunk, std::chrono::milliseconds(0));
                        if (got.status() == chast::Status::WaitTimeout) {
                            break;
                        }
                        if (got.status() == chast::Status::Unavailable) {
                            chast::FileUploadState state;
                            if (!session->file_upload_state(transfer_id, state)) {
                                // The host has not opened the stream yet; a
                                // short answer-to-stream window is normal.
                                if (answer_age_ms < 10000) {
                                    return;
                                }
                                download_fail("the host never opened the file stream", false);
                                return;
                            }
                            if (state.error.empty() && state.finished) {
                                break;
                            }
                            download_fail(state.error.empty()
                                              ? "the download stream ended early"
                                              : state.error,
                                          true);
                            return;
                        }
                        if (!got.ok()) {
                            download_fail(got.to_string(), true);
                            return;
                        }
                        if (chunk.transfer_id != transfer_id) {
                            download_fail("the download stream carried another transfer", true);
                            return;
                        }
                        chast::FileReceiver* receiver = nullptr;
                        uint64_t received = 0;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (download_.stage != FileDownload::Stage::kReceiving ||
                                download_.transfer_id != transfer_id) {
                                return;
                            }
                            receiver = download_.receiver.get();
                            received = download_.received;
                        }
                        if (receiver == nullptr) {
                            download_fail("the download has no partial file", false);
                            return;
                        }
                        if (chunk.offset != received) {
                            download_fail("the download is not contiguous", false);
                            return;
                        }
                        std::string write_error;
                        if (!receiver->write(chunk.data.data(), chunk.data.size(),
                                             &write_error)) {
                            download_fail(write_error, true);
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (download_.stage == FileDownload::Stage::kReceiving &&
                                download_.transfer_id == transfer_id) {
                                download_.received = received + chunk.data.size();
                            }
                        }
                    }
                    chast::FileUploadState state;
                    if (!session->file_upload_state(transfer_id, state)) {
                        if (answer_age_ms < 10000) {
                            return;
                        }
                        download_fail("the host never opened the file stream", false);
                        return;
                    }
                    if (!state.error.empty()) {
                        download_fail(state.error, true);
                        return;
                    }
                    if (!state.finished) {
                        return;
                    }
                    chast::FileReceiver* receiver = nullptr;
                    uint64_t bytes = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        receiver = download_.receiver.get();
                        bytes = download_.received;
                    }
                    if (receiver == nullptr) {
                        download_fail("the download has no partial file", false);
                        return;
                    }
                    std::string final_path;
                    std::string finish_error;
                    if (!receiver->finish(final_path, &finish_error)) {
                        download_fail(finish_error, true);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        download_.receiver.reset();
                        download_.stage = FileDownload::Stage::kDone;
                        download_.path = final_path;
                        download_.error.clear();
                    }
                    ++files_received;
                    chast::Error sent =
                        session->send_file_result(transfer_id, chast::FileCode::Ok, bytes);
                    if (!sent.ok()) {
                        std::fprintf(stderr, "viewer: cannot report the download: %s\n",
                                     sent.to_string().c_str());
                    }
                    std::fprintf(stderr, "viewer: received %s (%llu bytes)\n", final_path.c_str(),
                                 static_cast<unsigned long long>(bytes));
                };

                auto drain_download_cancels = [&] {
                    chast::FileCancel cancel;
                    while (session->take_file_cancel(cancel)) {
                        bool matches = false;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            matches = download_.stage == FileDownload::Stage::kReceiving &&
                                      download_.transfer_id == cancel.transfer_id;
                        }
                        if (matches) {
                            download_fail(cancel.reason.empty() ? "the host cancelled the download"
                                                               : cancel.reason,
                                          false);
                        }
                    }
                };

                // Moves the newest pointer state the host sent into the slot
                // the UI takes from. A newer update replaces an older one that
                // was never taken, so the UI always sees the current position.
                auto receive_cursor = [&] {
                    chast::CursorUpdate update;
                    bool any = false;
                    while (session->take_cursor_update(update)) {
                        any = true;
                        ++cursor_updates;
                        std::lock_guard<std::mutex> lock(cursor_mutex_);
                        pending_cursor_ = std::move(update);
                        ++cursor_updates_;
                    }
                    if (any) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.cursor_updates = cursor_updates;
                        status_.cursor_pending = true;
                    }
                };

                // Drains every complete audio packet that arrived since the
                // last poll. Audio never fails the session: a malformed packet
                // or a dead render endpoint stops local playback and video
                // keeps running.
                auto drain_audio = [&] {
                    if (!audio_announced || audio_finished || audio_broken) {
                        return;
                    }
                    chast::AudioHeader audio_header;
                    std::vector<uint8_t> audio_payload;
                    for (;;) {
                        chast::Error next = session->receive_audio(
                            audio_header, audio_payload, std::chrono::milliseconds(0));
                        if (next.status() == chast::Status::WaitTimeout) {
                            break;
                        }
                        if (next.status() == chast::Status::Unavailable) {
                            audio_finished = true;
                            audio_announced = false;
                            break;
                        }
                        if (!next.ok()) {
                            // The parser cannot resynchronise after a bad
                            // header, so stop consuming rather than spin on
                            // the same error.
                            ++audio_errors;
                            audio_broken = true;
                            audio_error = "protocol: " + next.to_string();
                            std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                            break;
                        }
                        ++audio_received;
                        if (!audio_player || !audio_decoder) {
                            // Local playback is unavailable; keep consuming so
                            // the transport queue cannot grow.
                            continue;
                        }
                        if (audio_header.payload_size != audio_payload.size() ||
                            chast::crc32(audio_payload.data(), audio_payload.size()) !=
                                audio_header.payload_crc32) {
                            ++audio_errors;
                            continue;
                        }
                        std::vector<chast::DecodedAudio> audio_blocks;
                        chast::Error audio_decode = audio_decoder->decode(
                            audio_header, audio_payload.data(), audio_payload.size(),
                            audio_blocks);
                        if (!audio_decode.ok()) {
                            ++audio_errors;
                            if (audio_errors <= 3) {
                                std::fprintf(stderr, "viewer: audio decode: %s\n",
                                             audio_decode.to_string().c_str());
                            }
                            continue;
                        }
                        ++audio_decoded;
                        for (const chast::DecodedAudio& block : audio_blocks) {
                            chast::Error pushed = audio_player->push(block);
                            if (!pushed.ok()) {
                                ++audio_errors;
                                if (audio_errors <= 3) {
                                    std::fprintf(stderr, "viewer: audio push: %s\n",
                                                 pushed.to_string().c_str());
                                }
                                break;
                            }
                        }
                        if (!audio_player->error().empty()) {
                            break;
                        }
                    }
                    if (audio_player) {
                        const bool muted = audio_muted_.load(std::memory_order_relaxed);
                        if (muted != audio_muted_applied) {
                            audio_player->set_mute(muted);
                            audio_muted_applied = muted;
                        }
                        const std::string player_error = audio_player->error();
                        if (!player_error.empty()) {
                            audio_error = "playback: " + player_error;
                            std::fprintf(stderr, "viewer: audio stopped: %s\n",
                                         player_error.c_str());
                            stop_audio_pipeline();
                        }
                    }
                    if ((audio_finished || audio_broken) && (audio_player || audio_decoder)) {
                        stop_audio_pipeline();
                    }
                };

                // Microphone uplink. The UI only sets the wanted flag and the
                // device id; this thread opens the capture, opens the CMIC
                // stream and moves packets so a stalled microphone can never
                // stall video or system sound. Switching the device or the
                // permission off rebuilds or ends the stream from here.
                chast::AudioInfo microphone_info;
                auto drain_microphone = [&] {
                    if (!microphone_authorized ||
                        !microphone_wanted_.load(std::memory_order_relaxed)) {
                        if (microphone_capture || microphone_stream_open) {
                            stop_microphone({}, true);
                        }
                        return;
                    }
                    std::string wanted_device;
                    {
                        std::lock_guard<std::mutex> lock(microphone_mutex_);
                        wanted_device = microphone_device_id_;
                    }
                    if (microphone_capture && wanted_device != microphone_device) {
                        // The picker chose another endpoint: close this
                        // stream before opening the next one.
                        stop_microphone({}, false);
                    }
                    if (!microphone_capture) {
                        chast::MicrophoneCaptureConfig config;
                        config.device_id = wanted_device;
                        std::string setup_error;
                        microphone_capture =
                            chast::create_microphone_capture(config, &setup_error);
                        if (!microphone_capture) {
                            ++microphone_errors;
                            microphone_error = "capture: " + setup_error;
                            std::fprintf(stderr, "viewer: microphone %s\n",
                                         microphone_error.c_str());
                            microphone_wanted_.store(false, std::memory_order_relaxed);
                            return;
                        }
                        microphone_info = microphone_capture->info();
                        chast::Error opened = session->start_microphone(microphone_info);
                        if (!opened.ok()) {
                            ++microphone_errors;
                            microphone_error = "stream: " + opened.to_string();
                            std::fprintf(stderr, "viewer: microphone %s\n",
                                         microphone_error.c_str());
                            stop_microphone({}, true);
                            return;
                        }
                        microphone_stream_open = true;
                        microphone_device = wanted_device;
                        microphone_name.clear();
                        std::string list_error;
                        for (const chast::MicrophoneDevice& device :
                             chast::list_microphones(&list_error)) {
                            if ((wanted_device.empty() && device.is_default) ||
                                device.id == wanted_device) {
                                microphone_name = device.name;
                                break;
                            }
                        }
                        microphone_capture->start();
                        microphone_enabled = true;
                        microphone_error.clear();
                        std::fprintf(stderr,
                                     "viewer: microphone %s %u Hz %u ch %u kbps, sending\n",
                                     audio_codec_name(microphone_info.codec),
                                     microphone_info.sample_rate, microphone_info.channels,
                                     microphone_info.bitrate / 1000u);
                    }
                    if (microphone_capture->failed()) {
                        ++microphone_errors;
                        const std::string reason = microphone_capture->error();
                        stop_microphone("capture: " + reason, true);
                        return;
                    }
                    chast::EncodedAudio packet;
                    while (microphone_capture->take_packet(packet)) {
                        chast::AudioHeader header;
                        header.codec = microphone_info.codec;
                        header.sample_rate = microphone_info.sample_rate;
                        header.channels = microphone_info.channels;
                        header.frame_samples = microphone_info.frame_samples;
                        header.pts_ms = packet.capture_ms;
                        header.capture_ms = packet.capture_ms;
                        chast::Error sent = session->send_microphone(
                            header, packet.data.data(), packet.data.size());
                        if (sent.status() == chast::Status::WaitTimeout) {
                            ++microphone_send_dropped;
                            continue;
                        }
                        if (!sent.ok()) {
                            ++microphone_errors;
                            if (sent.status() == chast::Status::PermissionDenied) {
                                stop_microphone("the host revoked the microphone", true);
                            } else {
                                stop_microphone("send: " + sent.to_string(), true);
                            }
                            return;
                        }
                        ++microphone_packets;
                    }
                    const chast::AudioCaptureStats stats = microphone_capture->stats();
                    microphone_dropped = stats.dropped + microphone_send_dropped;
                    microphone_recoveries = stats.recoveries;
                    microphone_recovering = stats.recovering;
                };

                // Adopts every PermissionUpdate the host sent. The transport
                // enforces the same mask on its side; this keeps the UI's
                // gates and the queues consistent with it immediately.
                auto receive_permissions = [&] {
                    uint16_t updated = 0;
                    while (session->take_permission_update(updated)) {
                        apply_permissions(updated);
                        std::fprintf(stderr, "viewer: permissions are now 0x%04x\n",
                                     updated);
                    }
                };

                // Sends the state the operator picked last. A request that
                // fails locally (permission revoked between the queue and the
                // send) is reported like a refusal, so the switch returns to
                // what the host actually has.
                auto drain_privacy = [&] {
                    std::optional<bool> requested;
                    {
                        std::lock_guard<std::mutex> lock(privacy_mutex_);
                        requested.swap(pending_privacy_);
                    }
                    if (!requested) {
                        return;
                    }
                    chast::Error sent = session->set_privacy_mode(*requested);
                    if (!sent.ok()) {
                        privacy_error = "request: " + sent.to_string();
                        ++privacy_updates;
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.privacy_error = privacy_error;
                        status_.privacy_updates = privacy_updates;
                        std::fprintf(stderr, "viewer: privacy mode: %s\n",
                                     sent.to_string().c_str());
                    }
                };

                // Adopts the overlay state. A host-initiated disable (the
                // operator revoked the permission) arrives here too, so the
                // switch always follows what the host actually did.
                auto receive_privacy = [&] {
                    bool enabled = false;
                    chast::PrivacyModeCode code = chast::PrivacyModeCode::Ok;
                    std::string reason;
                    while (session->take_privacy_mode_state(enabled, code, reason)) {
                        privacy_enabled = enabled;
                        privacy_code = static_cast<int>(code);
                        privacy_error = code == chast::PrivacyModeCode::Ok
                                            ? std::string()
                                            : (reason.empty() ? privacy_code_name(code)
                                                              : reason);
                        ++privacy_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.privacy_enabled = privacy_enabled;
                            status_.privacy_code = privacy_code;
                            status_.privacy_error = privacy_error;
                            status_.privacy_updates = privacy_updates;
                        }
                        std::fprintf(stderr, "viewer: privacy mode %s%s%s\n",
                                     enabled ? "on" : "off",
                                     privacy_error.empty() ? "" : ": ",
                                     privacy_error.c_str());
                    }
                };

                // Sends a queued Ctrl+Alt+Del request. A local failure is
                // reported as a SasResult would be, so the UI always gets an
                // answer even when the permission was revoked in between.
                auto drain_sas = [&] {
                    if (!pending_sas_.exchange(false, std::memory_order_acq_rel)) {
                        return;
                    }
                    chast::Error sent = session->request_sas();
                    if (sent.ok()) {
                        return;
                    }
                    sas_code = static_cast<int>(chast::SasCode::Failed);
                    sas_error = "request: " + sent.to_string();
                    ++sas_updates;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.sas_code = sas_code;
                    status_.sas_error = sas_error;
                    status_.sas_updates = sas_updates;
                    std::fprintf(stderr, "viewer: send Ctrl+Alt+Del: %s\n",
                                 sent.to_string().c_str());
                };

                // Adopts the host's answer. Every request gets one, so a
                // refusal explains itself instead of leaving the UI waiting.
                auto receive_sas = [&] {
                    chast::SasCode code = chast::SasCode::Failed;
                    std::string reason;
                    while (session->take_sas_result(code, reason)) {
                        sas_code = static_cast<int>(code);
                        sas_error = code == chast::SasCode::Ok
                                        ? std::string()
                                        : (reason.empty() ? sas_code_name(code) : reason);
                        ++sas_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.sas_code = sas_code;
                            status_.sas_error = sas_error;
                            status_.sas_updates = sas_updates;
                        }
                        std::fprintf(stderr, "viewer: Ctrl+Alt+Del %s%s%s\n",
                                     code == chast::SasCode::Ok ? "sent" : "refused",
                                     sas_error.empty() ? "" : ": ",
                                     sas_error.c_str());
                    }
                };

                // Adopts the desktop the host is capturing. Secure is the lock
                // screen, UAC or a credential prompt; the host pauses video
                // while it cannot follow and sends Default when it resumes.
                auto receive_desktop = [&] {
                    chast::DesktopKind kind = chast::DesktopKind::Default;
                    while (session->take_desktop_state(kind)) {
                        if (desktop_updates != 0 && kind == desktop) {
                            continue;
                        }
                        desktop = kind;
                        ++desktop_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.desktop = desktop;
                            status_.desktop_updates = desktop_updates;
                        }
                        std::fprintf(stderr, "viewer: host desktop is %s\n",
                                     desktop_kind_name(desktop));
                    }
                };

                // Sends the display the picker last pointed at. One slot: a
                // newer target replaces one the loop has not sent yet, the
                // same policy the pointer queue uses.
                auto drain_display_switch = [&] {
                    std::optional<uint16_t> requested;
                    {
                        std::lock_guard<std::mutex> lock(switch_mutex_);
                        requested.swap(pending_switch_);
                    }
                    if (!requested) {
                        return;
                    }
                    chast::Error sent = session->request_display_switch(*requested);
                    if (!sent.ok()) {
                        ++display_switch_failures;
                        display_switch_error = "request: " + sent.to_string();
                        std::fprintf(stderr, "viewer: switch display: %s\n",
                                     sent.to_string().c_str());
                    }
                };

                // Adopts the host's answer. Ok only says the request was
                // accepted; the geometry that actually changed arrives in the
                // StreamInfo following it. A refusal leaves the picture as it
                // was, so the UI can explain it and return the picker to the
                // output that is still streamed.
                auto receive_display_switch = [&] {
                    uint16_t index = 0;
                    chast::DisplaySwitchCode code = chast::DisplaySwitchCode::Failed;
                    std::string reason;
                    while (session->take_display_switch_result(index, code, reason)) {
                        if (code == chast::DisplaySwitchCode::Ok) {
                            ++display_switches;
                            display_switch_error.clear();
                            std::fprintf(stderr,
                                         "viewer: switch to display %u accepted\n", index);
                            continue;
                        }
                        display_switch_error =
                            reason.empty() ? switch_code_name(code) : reason;
                        ++display_switch_failures;
                        std::fprintf(stderr, "viewer: switch to display %u refused: %s\n",
                                     index, display_switch_error.c_str());
                    }
                };

                // Sends the newest mode query and the newest mode change. Both
                // slots keep only the newest request, like the switch slot.
                auto drain_display_modes = [&] {
                    std::optional<uint16_t> list_request;
                    std::optional<PendingMode> set_request;
                    {
                        std::lock_guard<std::mutex> lock(mode_mutex_);
                        list_request.swap(pending_mode_list_);
                        set_request.swap(pending_mode_set_);
                    }
                    if (list_request) {
                        chast::Error sent = session->request_display_modes(*list_request);
                        if (!sent.ok()) {
                            display_modes_error = "request: " + sent.to_string();
                            std::fprintf(stderr, "viewer: list display modes: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                    if (set_request) {
                        chast::Error sent = session->request_set_display_mode(
                            set_request->display, set_request->width, set_request->height,
                            set_request->refresh_hz);
                        if (!sent.ok()) {
                            ++display_mode_failures;
                            display_mode_error = "request: " + sent.to_string();
                            std::fprintf(stderr, "viewer: set display mode: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                };

                // Adopts the newest mode list. A refusal replaces the previous
                // list too: the UI asked about this display and must not see
                // the answer to an earlier one.
                auto receive_display_modes = [&] {
                    chast::DisplayModeList list;
                    while (session->take_display_modes(list)) {
                        ++display_mode_lists;
                        display_modes_error =
                            list.code == chast::DisplayModeCode::Ok
                                ? std::string()
                                : (list.reason.empty() ? mode_code_name(list.code) : list.reason);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.display_modes = list.modes;
                            status_.display_modes_display = list.display;
                            status_.display_modes_code = static_cast<int>(list.code);
                            status_.display_modes_error = display_modes_error;
                        }
                        std::fprintf(stderr, "viewer: display %u reports %zu mode(s)%s%s\n",
                                     list.display, list.modes.size(),
                                     display_modes_error.empty() ? "" : ": ",
                                     display_modes_error.c_str());
                    }
                };

                // Adopts every mode answer. Ok is followed by a StreamInfo
                // with the new source size; a refusal leaves the picture as it
                // was, and the UI returns the menu to the running mode.
                auto receive_display_mode_results = [&] {
                    uint16_t display = 0;
                    uint16_t width = 0;
                    uint16_t height = 0;
                    uint16_t refresh_hz = 0;
                    chast::DisplayModeCode code = chast::DisplayModeCode::Failed;
                    std::string reason;
                    while (session->take_display_mode_result(display, width, height, refresh_hz,
                                                             code, reason)) {
                        if (code == chast::DisplayModeCode::Ok) {
                            ++display_mode_changes;
                            display_mode_error.clear();
                            std::fprintf(stderr, "viewer: display %u now %ux%u at %u Hz\n",
                                         display, width, height, refresh_hz);
                            continue;
                        }
                        display_mode_error = reason.empty() ? mode_code_name(code) : reason;
                        ++display_mode_failures;
                        std::fprintf(stderr,
                                     "viewer: display %u mode %ux%u at %u Hz refused: %s\n",
                                     display, width, height, refresh_hz,
                                     display_mode_error.c_str());
                    }
                };

                // Sends the newest stream preferences. One slot, like the
                // display pickers: the menu's last choice is what matters.
                auto drain_stream_options = [&] {
                    std::optional<chast::StreamOptions> requested;
                    {
                        std::lock_guard<std::mutex> lock(stream_option_mutex_);
                        requested.swap(pending_stream_options_);
                    }
                    if (!requested) {
                        return;
                    }
                    chast::Error sent = session->request_stream_options(*requested);
                    if (sent.ok()) {
                        return;
                    }
                    stream_option_code = static_cast<int>(chast::StreamOptionsCode::Failed);
                    stream_option_error = "request: " + sent.to_string();
                    ++stream_option_results;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.stream_option_code = stream_option_code;
                        status_.stream_option_error = stream_option_error;
                        status_.stream_option_results = stream_option_results;
                    }
                    std::fprintf(stderr, "viewer: set stream options: %s\n",
                                 sent.to_string().c_str());
                };

                // Adopts the host's answer. It reports the values it actually
                // runs even on a refusal, so the menus can return to reality
                // without waiting for a StreamInfo.
                auto receive_stream_options = [&] {
                    chast::StreamOptionsResult result;
                    while (session->take_stream_options_result(result)) {
                        stream_option_code = static_cast<int>(result.code);
                        stream_option_error =
                            result.code == chast::StreamOptionsCode::Ok
                                ? std::string()
                                : (result.reason.empty()
                                       ? stream_option_code_name(result.code)
                                       : result.reason);
                        stream_quality = static_cast<int>(result.quality);
                        ++stream_option_results;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.stream_option_code = stream_option_code;
                            status_.stream_option_error = stream_option_error;
                            status_.stream_option_results = stream_option_results;
                            status_.stream_quality = stream_quality;
                            status_.codec = result.codec;
                            status_.framerate = result.fps;
                        }
                        std::fprintf(stderr,
                                     "viewer: stream options %s: %s, %u fps, quality %s%s%s\n",
                                     result.code == chast::StreamOptionsCode::Ok ? "accepted"
                                                                                 : "refused",
                                     codec_name(result.codec).c_str(), result.fps,
                                     stream_quality_name(result.quality),
                                     stream_option_error.empty() ? "" : ": ",
                                     stream_option_error.c_str());
                    }
                };

                // Applies StreamInfo messages: the captured output changed
                // size, rotation or origin after the server rebuilt its
                // duplication. Frames keep flowing on the same stream, so only
                // the description and (when the codec or size changed) the
                // decoder are replaced. Returns false with `failure` set when
                // the replacement decoder cannot be created.
                auto apply_stream_updates = [&]() -> bool {
                    chast::ServerInfo update;
                    bool applied = false;
                    while (session->take_stream_update(update)) {
                        applied = true;
                        const bool needs_decoder = update.codec != info.codec ||
                                                   update.width != info.width ||
                                                   update.height != info.height;
                        info = update;
                        display_source_width = info.source_width;
                        display_source_height = info.source_height;
                        if (!info.audio.enabled) {
                            if (audio_announced || audio_player || audio_decoder) {
                                audio_announced = false;
                                audio_finished = true;
                                stop_audio_pipeline();
                                std::fprintf(stderr, "viewer: the host stopped audio\n");
                            }
                        } else if (!audio_announced) {
                            start_audio(info.audio);
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.codec = info.codec;
                            status_.framerate = info.framerate;
                            status_.server_bitrate = info.bitrate;
                            status_.server_encoder = info.encoder;
                            status_.input_enabled = info.input_enabled;
                            status_.clipboard_enabled = info.clipboard_enabled;
                            status_.origin_x = info.origin_x;
                            status_.origin_y = info.origin_y;
                            status_.displays = info.displays;
                        }
                        {
                            std::lock_guard<std::mutex> lock(input_mutex_);
                            input_enabled_ = info.input_enabled;
                        }
                        clipboard_enabled = info.clipboard_enabled;
                        // StreamInfo carries the permission mask too (for
                        // example after the host rebuilt its capture under a
                        // reduced grant), so adopt it before announcing.
                        apply_permissions(session->permissions());
                        std::fprintf(stderr,
                                     "viewer: stream is now %s %ux%u at %d,%d\n",
                                     codec_name(info.codec).c_str(), info.width, info.height,
                                     info.origin_x, info.origin_y);
                        if (!needs_decoder) {
                            continue;
                        }
                        decoder.reset();
                        decoder_config.codec = info.codec;
                        decoder = chast::create_decoder(decoder_config, &error);
                        if (!decoder) {
                            fail("decoder (" + codec_name(info.codec) + "): " + error);
                            return false;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.decoder = decoder->implementation();
                        }
                        std::fprintf(stderr, "viewer: decoder %s\n",
                                     decoder->implementation().c_str());
                        // The server forces an IDR when it re-announces, but a
                        // request makes the wait explicit if that one was lost
                        // with the old stream.
                        request_keyframe();
                    }
                    if (applied) {
                        sync_status(false);
                    }
                    return true;
                };

                while (!stop_requested_.load(std::memory_order_acquire)) {
                    if (!apply_stream_updates()) {
                        break;
                    }
                    receive_permissions();
                    drain_privacy();
                    receive_privacy();
                    drain_sas();
                    receive_sas();
                    receive_desktop();
                    receive_display_switch();
                    receive_display_modes();
                    receive_display_mode_results();
                    receive_stream_options();
                    drain_display_switch();
                    drain_display_modes();
                    drain_stream_options();
                    {
                        const auto probe_now = Clock::now();
                        if (probe_now - last_probe >= kProbeInterval) {
                            last_probe = probe_now;
                            session->send_latency_probe();
                            const chast::LatencySample sample = session->latency();
                            if (sample.valid) {
                                rtt_ms = static_cast<double>(sample.rtt_us) / 1000.0;
                                clock_offset_us = sample.offset_us;
                                clock_synced = true;
                                if (audio_player) {
                                    audio_player->set_clock_offset(clock_offset_us / 1000);
                                }
                            }
                        }
                    }
                    drain_input();
                    poll_clipboard();
                    receive_clipboard();
                    drive_file_upload();
                    drain_downloads();
                    drain_download_cancels();
                    receive_cursor();
                    drain_audio();
                    drain_microphone();
                    chast::FrameHeader header;
                    std::vector<uint8_t> payload;
                    FileUpload::Stage upload_stage = FileUpload::Stage::kNone;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        upload_stage = file_.stage;
                    }
                    const bool file_active = upload_stage == FileUpload::Stage::kOffered ||
                                             upload_stage == FileUpload::Stage::kSending ||
                                             upload_stage == FileUpload::Stage::kFinishing;
                    const bool fast_media_poll =
                        (permissions & (chast::kPermissionInput |
                                        chast::kPermissionClipboard)) != 0 ||
                        file_active || audio_announced || microphone_enabled ||
                        microphone_wanted_.load(std::memory_order_relaxed);
                    chast::Error received_frame = session->receive_frame(
                        header, payload, fast_media_poll ? kMediaPoll : kReceiveTimeout);
                    // Audio and microphone packets arrive on their own streams
                    // and never wake receive_frame(), so drain them as soon as
                    // the wait ends.
                    drain_audio();
                    drain_microphone();
                    receive_permissions();
                    drain_privacy();
                    receive_privacy();
                    drain_sas();
                    receive_sas();
                    receive_desktop();
                    receive_display_switch();
                    receive_display_modes();
                    receive_display_mode_results();
                    receive_stream_options();
                    const auto now = Clock::now();

                    if (received_frame.status() == chast::Status::WaitTimeout) {
                        const chast::Error async = session->async_error();
                        if (!async.ok()) {
                            fail("transport: " + async.to_string());
                            break;
                        }
                        if (session->video_finished()) {
                            video_finished = true;
                            break;
                        }
                        if (now - last_frame >= kFrameStall) {
                            fail("no frames arrived for 20 s");
                            break;
                        }
                        if (sink_ != nullptr && !sink_->error().empty()) {
                            fail("display: " + sink_->error());
                            break;
                        }
                        sync_status(false);
                        continue;
                    }
                    if (received_frame.status() == chast::Status::Unavailable) {
                        video_finished = session->video_finished();
                        if (!video_finished) {
                            const chast::Error async = session->async_error();
                            fail(async.ok() ? "the connection closed before the video stream ended"
                                            : async.to_string());
                        }
                        break;
                    }
                    if (!received_frame.ok()) {
                        fail("receive: " + received_frame.to_string());
                        break;
                    }

                    ++received;
                    last_frame = now;
                    bytes += payload.size();
                    window_bytes += payload.size();
                    if (clock_synced && header.capture_ms != 0) {
                        const int64_t host_now_us = steady_us(now) + clock_offset_us;
                        const int64_t age_us =
                            host_now_us - static_cast<int64_t>(header.capture_ms) * 1000;
                        // A wild value means the offset is stale (for example
                        // after a long receive stall); keep the last good one
                        // instead of showing a bogus age.
                        if (age_us > -100000 && age_us < 10000000) {
                            latency_ms = static_cast<double>(std::max<int64_t>(age_us, 0)) / 1000.0;
                        }
                    }
                    if (header.keyframe) {
                        ++keyframes;
                    }
                    if (header.seq != expected_seq) {
                        ++gaps;
                        request_keyframe();
                    }
                    expected_seq = header.seq + 1;

                    if (header.codec != info.codec || header.width != info.width ||
                        header.height != info.height) {
                        // The description and the first picture of a rebuilt
                        // stream travel on different QUIC streams, so the
                        // picture can win the race. Look for the update before
                        // treating the header as a violation.
                        if (!apply_stream_updates()) {
                            break;
                        }
                    }
                    if (header.codec != info.codec || header.width != info.width ||
                        header.height != info.height) {
                        ++protocol_errors;
                        sync_status(false);
                        continue;
                    }
                    if (chast::crc32(payload.data(), payload.size()) != header.payload_crc32) {
                        ++crc_errors;
                        sync_status(false);
                        continue;
                    }

                    frames.clear();
                    chast::Error decode_error = decoder->decode(
                        payload.data(), payload.size(), header.pts_ms, header.keyframe, frames);
                    if (!decode_error.ok()) {
                        if (sink_ != nullptr && !sink_->error().empty()) {
                            fail("display: " + sink_->error());
                            break;
                        }
                        if (!decoder->initialized()) {
                            // No sequence header yet; ask for an IDR instead
                            // of counting delta frames as damage.
                            if (init_errors++ < 3) {
                                std::fprintf(stderr, "viewer: init: %s\n",
                                             decode_error.to_string().c_str());
                            }
                            request_keyframe();
                            sync_status(false);
                            continue;
                        }
                        if (decode_errors < 3) {
                            std::fprintf(stderr, "viewer: decode: %s\n",
                                         decode_error.to_string().c_str());
                        }
                        ++decode_errors;
                        if (decode_errors >= 30) {
                            fail("too many decode failures");
                            break;
                        }
                        sync_status(false);
                        continue;
                    }

                    // Pictures arrive through gpu_handler while decode() runs,
                    // so `frames` stays empty on this path.
                    sync_status(false);
                }
            }
        }
    }

    if (!stop_requested_.load(std::memory_order_acquire) && failure.empty() &&
        !video_finished && handshaken) {
        // Leaving the loop without a failure or an explicit stop means the
        // decoder path itself ended (it should not); keep the state honest.
        fail("the video session ended unexpectedly");
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_enabled_ = false;
        pending_input_.clear();
    }
    // An upload that outlived the stream cannot continue on this connection.
    uint32_t cancelled_upload = 0;
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        const FileUpload::Stage stage = file_.stage;
        if (stage == FileUpload::Stage::kHashing ||
            stage == FileUpload::Stage::kReady || stage == FileUpload::Stage::kOffered ||
            stage == FileUpload::Stage::kSending ||
            stage == FileUpload::Stage::kFinishing) {
            cancelled_upload = file_.transfer_id;
            file_.stage = FileUpload::Stage::kFailed;
            file_.error = "the session ended before the file finished";
        }
    }
    hash_cancel_.store(true, std::memory_order_release);
    if (cancelled_upload != 0) {
        session->cancel_file_upload(cancelled_upload);
    }
    // The uplink cannot outlive the session: capture stops first, then the
    // stream ends so the host tears down its decoder and player.
    stop_microphone({}, true);

    if (handshaken) {
        chast::ClientStats closing;
        closing.received = received;
        closing.decoded = decoded;
        closing.crc_errors = crc_errors;
        closing.decode_errors = decode_errors;
        closing.digest = digest;
        closing.elapsed_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
                .count());
        session->finish_client(closing, std::chrono::milliseconds(1500));
    }

    sync_status(true);
    if (!failure.empty()) {
        std::fprintf(stderr, "viewer: %s\n", failure.c_str());
        SetState(ViewerState::kFailed, failure);
    } else {
        SetState(ViewerState::kEnded);
    }
    running_.store(false, std::memory_order_release);
}

}  // namespace chast_viewer
