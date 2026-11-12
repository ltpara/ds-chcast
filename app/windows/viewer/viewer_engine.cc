#include "viewer_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "mp4_recorder.h"

#include "chast/access_ticket.h"
#include "chast/audio.h"
#include "chast/carrier.h"
#include "chast/clipboard.h"
#include "chast/clipboard_listener.h"
#include "chast/clipboard_worker.h"
#include "chast/clipboard_channel.h"
#include "chast/decoder.h"
#include "chast/file_transfer.h"
#include "chast/identity.h"
#include "chast/png.h"
#include "chast/port_forward_client.h"
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
// Matches GpuFrameSink's cap. Checking the announced size here fails the
// session with a clear reason before a decoder and shared textures exist;
// the sink repeats the check for every decoded frame.
constexpr uint32_t kMaxFrameSide = 8192;
constexpr uint64_t kMaxFramePixels = 7680ull * 4320ull;
// Consecutive sink refusals tolerated before the display stream ends. One
// mode change or TDR recovery can fail a handful of frames; a permanently
// broken pipeline must not look like a frozen desktop forever.
constexpr uint64_t kMaxSinkFailureStreak = 300;
constexpr auto kClipboardPollInterval = std::chrono::milliseconds(500);
// Virtual browse path that the host serves announced clipboard files from;
// the digits after the prefix index the FileList the host sent.
constexpr char kClipboardFilePathPrefix[] = ".chast-clipboard/";
// How long the host may take to answer a clipboard download request. The
// offer itself is not bounded: the host hashes the file first.
constexpr auto kClipboardAnswerTimeout = std::chrono::seconds(10);

// A file name stamp that is unlikely to collide: the local wall clock. The
// recording muxer appends "-N" when a size change rolls over; screenshots use
// the same stamp and pick the first free suffix.
std::string local_stamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return stamp;
}

std::filesystem::path path_from_utf8(const std::string& path) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

std::string utf8_from_path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string record_file_path(const std::string& directory) {
    return (std::filesystem::path(directory) / ("chast-" + local_stamp() + ".mp4"))
        .string();
}

// Where screenshots land when the UI does not name a folder.
std::string default_screenshot_directory() {
    wchar_t* profile = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&profile, &length, L"USERPROFILE") == 0 && profile != nullptr) {
        const std::filesystem::path value(profile);
        std::free(profile);
        if (!value.empty()) {
            return utf8_from_path(value / "Pictures" / "chast");
        }
    }
    return utf8_from_path(std::filesystem::current_path());
}

std::string screenshot_file_path(const std::string& directory) {
    const std::filesystem::path folder = path_from_utf8(directory);
    const std::string stem = "chast-" + local_stamp();
    std::filesystem::path candidate = folder / (stem + ".png");
    std::error_code ec;
    for (int suffix = 1; suffix < 1000 && std::filesystem::exists(candidate, ec); ++suffix) {
        candidate = folder / (stem + "-" + std::to_string(suffix) + ".png");
    }
    return utf8_from_path(candidate);
}

// Where recordings land when the UI does not name a folder. RustDesk keeps
// them in the user's Videos directory; this matches that default.
std::string default_record_directory() {
    char* profile = nullptr;
    size_t length = 0;
    if (_dupenv_s(&profile, &length, "USERPROFILE") == 0 && profile != nullptr) {
        std::string value(profile);
        std::free(profile);
        if (!value.empty()) {
            return (std::filesystem::path(value) / "Videos").string();
        }
    }
    return std::filesystem::current_path().string();
}
// How long one receive-loop pass may spend writing an upload, so a large file
// cannot starve video, audio and input.
constexpr auto kFileSendBudget = std::chrono::milliseconds(8);
// A local recording must not fill the system disk: it only starts with room
// for a useful segment and stops before the volume is exhausted. An unreadable
// free-space query stays permissive so recording is not blocked by a quirk.
constexpr uint64_t kRecordStartFreeBytes = 512ull * 1024 * 1024;
constexpr uint64_t kRecordStopFreeBytes = 64ull * 1024 * 1024;
bool recording_space(const std::string& directory, uint64_t minimum) {
    std::error_code error;
    const std::filesystem::space_info space =
        std::filesystem::space(std::filesystem::path(directory), error);
    return error || space.available >= minimum;
}
// A bulk upload is paced far below the loopback ceiling: unpaced, a 1 GiB file
// fills the 32 MiB send queue and the viewer loop stalls on it, which costs the
// video stream almost all of its frames. The burst lets one pass move a few
// chunks without being reduced to a trickle by the poll cadence.
constexpr double kFileUploadBytesPerSecond = 32.0 * 1024.0 * 1024.0;
constexpr double kFileUploadBurst = 256.0 * 1024.0;
constexpr size_t kMaxPendingInput = 256;
// Safety valve for a folder that expands into a huge tree; the UI would
// otherwise build an unbounded queue on the session thread.
constexpr size_t kMaxQueuedUploads = 10000;

const char* session_mode_name(chast::SessionMode mode) {
    switch (mode) {
        case chast::SessionMode::Desktop:
            return "desktop";
        case chast::SessionMode::FileTransfer:
            return "file_transfer";
        case chast::SessionMode::Terminal:
            return "terminal";
        case chast::SessionMode::Camera:
            return "camera";
        case chast::SessionMode::PortForward:
            return "port_forward";
    }
    return "desktop";
}

std::string codec_name(chast::VideoCodec codec) {
    switch (codec) {
        case chast::VideoCodec::H264:
            return "H.264";
        case chast::VideoCodec::Hevc:
            return "HEVC";
        case chast::VideoCodec::Vp9:
            return "VP9";
        case chast::VideoCodec::Av1:
            return "AV1";
    }
    return "H.264";
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

const char* follow_code_name(chast::DisplayFollowCode code) {
    switch (code) {
        case chast::DisplayFollowCode::Ok:
            return "following is in effect";
        case chast::DisplayFollowCode::Failed:
            return "the host could not apply the follow request";
        case chast::DisplayFollowCode::Unsupported:
            return "the merged view already shows every display";
    }
    return "the host refused the follow request";
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
        case chast::StreamQuality::Custom:
            return "custom";
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

const char* host_action_code_name(chast::HostActionCode code) {
    switch (code) {
        case chast::HostActionCode::Ok:
            return "the host performed the action";
        case chast::HostActionCode::Denied:
            return "the host denied the action";
        case chast::HostActionCode::Unsupported:
            return "this host does not support the action";
        case chast::HostActionCode::Busy:
            return "another viewer controls this host";
        case chast::HostActionCode::Failed:
            return "the host could not perform the action";
    }
    return "the host refused the action";
}

const char* host_action_name(chast::HostAction action) {
    switch (action) {
        case chast::HostAction::Restart:
            return "restart";
        case chast::HostAction::LockScreen:
            return "lock screen";
        case chast::HostAction::BlockInput:
            return "block input";
    }
    return "host action";
}

const char* os_login_code_name(chast::OsLoginCode code) {
    switch (code) {
        case chast::OsLoginCode::Ok:
            return "the OS password was typed";
        case chast::OsLoginCode::Denied:
            return "the host denied OS login";
        case chast::OsLoginCode::Unsupported:
            return "the host is not at a logon or lock screen";
        case chast::OsLoginCode::Busy:
            return "another viewer controls this host";
        case chast::OsLoginCode::Failed:
            return "the host could not type the password";
    }
    return "the host refused OS login";
}

const char* remote_config_code_name(chast::RemoteConfigCode code) {
    switch (code) {
        case chast::RemoteConfigCode::Ok:
            return "the host applied the change";
        case chast::RemoteConfigCode::Denied:
            return "the host denied the change";
        case chast::RemoteConfigCode::Unsupported:
            return "this host cannot change that switch";
        case chast::RemoteConfigCode::Busy:
            return "another viewer controls this host";
        case chast::RemoteConfigCode::Failed:
            return "the host could not store the change";
    }
    return "the host refused the change";
}

const char* session_list_code_name(chast::SessionListCode code) {
    switch (code) {
        case chast::SessionListCode::Ok:
            return "the session list was read";
        case chast::SessionListCode::Denied:
            return "the host denied the session list";
        case chast::SessionListCode::Unsupported:
            return "this host cannot list Windows sessions";
        case chast::SessionListCode::Failed:
            return "the host could not read the session list";
    }
    return "the host refused the session list";
}

const char* session_select_code_name(chast::SessionSelectCode code) {
    switch (code) {
        case chast::SessionSelectCode::Ok:
            return "the session switch was requested";
        case chast::SessionSelectCode::NotFound:
            return "the session is not active any more";
        case chast::SessionSelectCode::Denied:
            return "the host denied the session switch";
        case chast::SessionSelectCode::Unsupported:
            return "this host cannot switch Windows sessions";
        case chast::SessionSelectCode::Busy:
            return "another viewer controls this host";
        case chast::SessionSelectCode::Failed:
            return "the host could not switch sessions";
    }
    return "the host refused the session switch";
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

// Finished transfers kept for the page's audit drain. The page polls four
// times a second, so this only caps a session whose page stopped draining;
// the newest events win.
constexpr size_t kMaxCompletedFiles = 512;

}  // namespace

ViewerEngine::ViewerEngine(FrameSink* sink, FrameSink* camera_sink,
                           DisplayVideoReceiver::DecoderFactory decoder_factory)
    : sink_(sink), camera_sink_(camera_sink), decoder_factory_(std::move(decoder_factory)) {}

ViewerEngine::~ViewerEngine() {
    Stop();
}

bool ViewerEngine::Start(const std::string& host, uint16_t port, uint16_t display,
                         std::string pinned_fingerprint, bool allow_unverified,
                         std::string access_token, std::string password,
                         std::string totp,
                         std::shared_ptr<chast::DeviceIdentity> device_identity,
                         bool trust_device, ViewerTransport transport,
                         std::string carrier_host, uint16_t carrier_port,
                         std::string carrier_host_id, std::string carrier_token,
                         bool file_transfer_only, chast::SessionMode session_mode,
                         std::vector<uint8_t> access_ticket,
                         std::vector<uint8_t> access_ticket_seed) {
    if (running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    // A previous session may have ended on its own while its worker was still
    // walking a folder or hashing a file; the new queue starts clean.
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        file_worker_stop_ = true;
        ++upload_epoch_;
        pending_upload_roots_.clear();
        uploads_.clear();
        last_upload_ = FileUpload{};
        last_upload_.stage = FileUpload::Stage::kDone;
        next_file_id_ = 1;
    }
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_files_.clear();
    }
    file_work_cv_.notify_all();
    if (file_worker_.joinable()) {
        file_worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        file_worker_stop_ = false;
    }
    file_worker_ = std::thread(&ViewerEngine::FileWorker, this);
    device_identity_ = std::move(device_identity);
    // One reference per session ties the viewer's own audit report to the
    // records the host writes for the same session and uploads.
    uint8_t ref_bytes[16]{};
    std::string audit_ref;
    if (chast::secure_random(ref_bytes, sizeof(ref_bytes))) {
        audit_ref = chast::hex_encode(ref_bytes, sizeof(ref_bytes));
    }
    std::string selected_output;
    {
        std::lock_guard<std::mutex> lock(audio_output_mutex_);
        selected_output = audio_output_device_id_;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = ViewerStatus{};
        pending_elevation_.store(0);
        status_.host = host;
        status_.port = port;
        status_.state = ViewerState::kConnecting;
        status_.session_mode = session_mode_name(
            session_mode != chast::SessionMode::Desktop
                ? session_mode
                : file_transfer_only ? chast::SessionMode::FileTransfer
                                     : chast::SessionMode::Desktop);
        status_.transport = transport == ViewerTransport::kTcp
                                ? "tcp"
                                : transport == ViewerTransport::kWebSocket ? "ws" : "quic";
        status_.audio_device = std::move(selected_output);
        status_.audio_aec = microphone_aec_.load(std::memory_order_relaxed);
        status_.audio_noise_suppression =
            microphone_noise_.load(std::memory_order_relaxed);
        status_.audit_ref = audit_ref;
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
        pending_display_cursors_.clear();
        cursor_updates_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(switch_mutex_);
        pending_switch_.reset();
    }
    {
        std::lock_guard lock(displays_mutex_);
        pending_displays_.clear();
        pending_display_sinks_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(mode_mutex_);
        pending_mode_list_.reset();
        pending_mode_set_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(follow_mutex_);
        pending_follow_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(cursor_hint_mutex_);
        pending_show_cursor_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(stream_option_mutex_);
        pending_stream_options_.reset();
    }
    // Browse answers belong to the session that was asked; a listing from the
    // previous host must not appear in the next one's browser.
    {
        std::lock_guard<std::mutex> lock(browse_mutex_);
        pending_browse_.clear();
        browse_paths_.clear();
        latest_list_request_.reset();
        newest_file_list_.reset();
        next_browse_id_ = 1;
    }
    {
        std::lock_guard<std::mutex> lock(privacy_mutex_);
        // A reused viewer starts with a display query: the toolbar needs the
        // output list before the operator can ask for a change.
        pending_display_control_ = chast::DisplayControlRequest{};
        pending_privacy_.reset();
    }
    // A SAS request queued for a session that ended must not fire into the
    // next one.
    pending_sas_.store(false, std::memory_order_release);
    // Nor may a host-control action reach the next host.
    {
        std::lock_guard<std::mutex> lock(host_action_mutex_);
        pending_host_action_.reset();
    }
    // An OS password queued for the old host must never reach the next one.
    {
        std::lock_guard<std::mutex> lock(os_login_mutex_);
        if (pending_os_login_) {
            std::fill(pending_os_login_->begin(), pending_os_login_->end(), '\0');
            pending_os_login_.reset();
        }
    }
    // Nor may a session switch target reach a different host.
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        pending_session_list_ = false;
        pending_session_select_.reset();
        pending_cursor_shape_.reset();
        cursor_request_valid_ = false;
    }
    // Port forwarding belongs to one session too; the listeners live on the
    // session thread and go away with it.
    {
        std::lock_guard<std::mutex> lock(port_forward_mutex_);
        pending_port_forward_.clear();
        next_port_forward_id_ = 1;
    }
    // Chat belongs to one viewer session in both directions.
    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        pending_chat_.clear();
        received_chat_.clear();
    }
    // Consoles belong to one session too; neither their commands nor their
    // unread output may leak into the next connection.
    {
        std::lock_guard<std::mutex> lock(terminal_mutex_);
        pending_terminal_.clear();
        terminals_ = {};
    }
    // A camera request belongs to one session; the host's ids are only valid
    // for its own list.
    {
        std::lock_guard<std::mutex> lock(camera_mutex_);
        pending_camera_.clear();
    }
    // Microphone capture and recording are per-session privacy decisions:
    // every session starts with both off, while the chosen devices and
    // directory are remembered.
    microphone_wanted_.store(false, std::memory_order_relaxed);
    voice_wanted_.store(false, std::memory_order_relaxed);
    record_wanted_.store(false, std::memory_order_relaxed);
    { std::lock_guard lock(record_mutex_); record_stream_.reset(); pending_record_keyframe_.reset(); }
    {
        std::lock_guard lock(screenshot_mutex_);
        pending_screenshot_.reset();
        pending_screenshot_stream_.reset();
        pending_screenshot_sent_ = false;
        pending_screenshot_restart_ = false;
    }
    stop_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(ticket_mutex_);
        pending_ticket_renew_.reset();
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ViewerEngine::Run, this, host, port, display,
                          std::move(pinned_fingerprint), allow_unverified,
                          std::move(access_token), std::move(password),
                          std::move(totp), trust_device, transport,
                          std::move(carrier_host), carrier_port,
                          std::move(carrier_host_id), std::move(carrier_token),
                          std::move(audit_ref), file_transfer_only, session_mode,
                          std::move(access_ticket), std::move(access_ticket_seed));
    return true;
}

bool ViewerEngine::RenewAccessTicket(std::vector<uint8_t> signed_ticket) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    if (signed_ticket.size() <= chast::kTicketSignatureSize ||
        signed_ticket.size() > chast::kTicketMaxSigned) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ticket_mutex_);
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    pending_ticket_renew_ = std::move(signed_ticket);
    return true;
}

void ViewerEngine::Stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    // The worker may be in the middle of a folder walk or a multi-gigabyte
    // hash; the epoch makes its result unusable and the flag ends its wait.
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        file_worker_stop_ = true;
        ++upload_epoch_;
        pending_upload_roots_.clear();
        uploads_.clear();
    }
    file_work_cv_.notify_all();
    if (file_worker_.joinable()) {
        file_worker_.join();
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
    if (!input_enabled_) {
        return false;
    }
    if (pending_input_.size() >= kMaxPendingInput) pending_input_.clear();
    PendingInput pending;
    pending.release = true;
    pending_input_.push_back(pending);
    return true;
}

bool ViewerEngine::CaptureDisplays(chast::CaptureDisplaysRequest request) {
    if (!running_.load(std::memory_order_acquire) || chast::make_capture_displays(request).empty()) return false;
    {
        std::lock_guard lock(mutex_);
        if (status_.state != ViewerState::kStreaming || status_.control_only) return false;
    }
    std::lock_guard lock(displays_mutex_);
    if (pending_displays_.size() >= 32) return false;
    pending_displays_.push_back(std::move(request));
    return true;
}

bool ViewerEngine::BindDisplay(uint32_t id, std::shared_ptr<FrameSink> sink) {
    if (!running_.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard lock(mutex_);
        if (!status_.multiple_displays || std::none_of(status_.display_streams.begin(), status_.display_streams.end(),
                [id](const auto& entry) { return entry.stream.stream_id == id; })) return false;
    }
    std::lock_guard lock(displays_mutex_);
    pending_display_sinks_[id] = std::move(sink);
    return true;
}

bool ViewerEngine::TakeDisplayCursor(chast::DisplayCursorUpdate& update) {
    std::lock_guard lock(cursor_mutex_);
    if (pending_display_cursors_.empty()) return false;
    auto it = pending_display_cursors_.begin();
    update.stream_id = it->first; update.cursor = std::move(it->second);
    pending_display_cursors_.erase(it);
    return true;
}

bool ViewerEngine::FocusDisplay(uint32_t id) {
    std::lock_guard lock(mutex_);
    if (!running_.load() || !status_.multiple_displays ||
        std::none_of(status_.display_streams.begin(), status_.display_streams.end(),
            [id](const auto& stream) { return stream.stream.stream_id == id; })) return false;
    status_.focused_display = id;
    return true;
}

bool ViewerEngine::SwitchDisplay(uint16_t display) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming) {
            return false;
        }
        if (display != chast::kDisplayAuto && display != chast::kDisplayAll &&
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
        if (status_.state != ViewerState::kStreaming || status_.merged_layout ||
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
        if (status_.state != ViewerState::kStreaming || !status_.input_enabled ||
            status_.merged_layout) {
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

bool ViewerEngine::SetDisplayFollow(bool cursor, bool window) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.input_enabled) {
            return false;
        }
    }
    uint8_t flags = 0;
    if (cursor) {
        flags |= chast::kDisplayFollowCursor;
    }
    if (window) {
        flags |= chast::kDisplayFollowWindow;
    }
    std::lock_guard<std::mutex> lock(follow_mutex_);
    pending_follow_ = flags;
    return true;
}

bool ViewerEngine::ShowCursor(int32_t x, int32_t y, bool visible, bool pressed) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming) {
            return false;
        }
    }
    chast::ShowCursorHint hint;
    hint.x = x;
    hint.y = y;
    hint.visible = visible;
    hint.pressed = pressed && visible;
    std::lock_guard<std::mutex> lock(cursor_hint_mutex_);
    pending_show_cursor_ = hint;
    return true;
}

bool ViewerEngine::SetStreamOptions(chast::StreamCodecPreference codec, uint16_t max_fps,
                                    chast::StreamQuality quality, chast::VideoChroma chroma,
                                    uint8_t quality_percent) {
    if (!running_.load(std::memory_order_acquire) || max_fps > chast::kMaxStreamFps || static_cast<unsigned>(chroma) > 1) {
        return false;
    }
    switch (codec) {
        case chast::StreamCodecPreference::Auto:
        case chast::StreamCodecPreference::H264:
        case chast::StreamCodecPreference::Hevc:
        case chast::StreamCodecPreference::Vp9:
        case chast::StreamCodecPreference::Av1:
            break;
        default:
            return false;
    }
    switch (quality) {
        case chast::StreamQuality::High:
        case chast::StreamQuality::Balanced:
        case chast::StreamQuality::Smooth:
            quality_percent = 0;
            break;
        case chast::StreamQuality::Custom:
            if (quality_percent < chast::kMinCustomQualityPercent ||
                quality_percent > chast::kMaxCustomQualityPercent) {
                return false;
            }
            break;
        default:
            return false;
    }
    chast::StreamOptions options;
    options.codec = codec;
    options.chroma = chroma;
    options.max_fps = max_fps;
    options.quality = quality;
    options.quality_percent = quality_percent;
    std::lock_guard<std::mutex> lock(stream_option_mutex_);
    pending_stream_options_ = options;
    return true;
}

void ViewerEngine::SetClipboardSync(bool enabled) {
    if (clipboard_sync_.exchange(enabled, std::memory_order_acq_rel) != enabled) {
        clipboard_sync_version_.fetch_add(1, std::memory_order_release);
    }
}

void ViewerEngine::SetClipboardSource(
    std::function<bool(chast::ClipboardContent&)> source) {
    std::lock_guard<std::mutex> lock(clipboard_sink_mutex_);
    clipboard_source_ = std::move(source);
}

void ViewerEngine::SetClipboardSink(
    std::function<bool(const chast::ClipboardContent&)> sink) {
    std::lock_guard<std::mutex> lock(clipboard_sink_mutex_);
    clipboard_sink_ = std::move(sink);
}

void ViewerEngine::SetClipboardFileSink(
    std::function<bool(const std::vector<std::string>&)> sink) {
    std::lock_guard<std::mutex> lock(clipboard_sink_mutex_);
    clipboard_file_sink_ = std::move(sink);
}

void ViewerEngine::SetClipboardFileSource(
    std::function<bool(std::vector<std::string>&)> source) {
    std::lock_guard<std::mutex> lock(clipboard_sink_mutex_);
    clipboard_file_source_ = std::move(source);
}

void ViewerEngine::SetClipboardVirtualSink(
    std::function<bool(std::shared_ptr<chast::ClipboardVirtualFiles>)> sink) {
    std::lock_guard lock(clipboard_sink_mutex_);
    clipboard_virtual_sink_ = std::move(sink);
}

bool ViewerEngine::SendFiles(const std::vector<std::string>& paths,
                             std::string relative_prefix) {
    if (!running_.load(std::memory_order_acquire) || paths.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionFileTransfer) == 0) {
            return false;
        }
    }
    size_t added = 0;
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        for (const std::string& path : paths) {
            if (path.empty()) {
                continue;
            }
            if (pending_upload_roots_.size() + uploads_.size() >= kMaxQueuedUploads) {
                break;
            }
            pending_upload_roots_.push_back(PendingUploadRoot{path, relative_prefix});
            ++added;
        }
    }
    if (added == 0) {
        return false;
    }
    file_work_cv_.notify_all();
    return true;
}

namespace {

constexpr size_t kMaxPendingBrowse = 8;
// A batch download walks its entries in one session-thread state machine; the
// cap keeps a select-all over a huge share from turning into a multi-minute
// queue the operator cannot see the end of.
constexpr size_t kMaxBatchDownloadRoots = 512;
// A host controls every listing, so the walk itself is bounded: a tree that
// keeps answering with the maximum number of entries must not grow the
// directory and file queues without limit.
constexpr uint64_t kMaxFolderWalkEntries = 200000;
constexpr size_t kMaxFolderWalkDepth = 64;

}  // namespace

bool ViewerEngine::RequestFileList(std::string directory, std::string cursor,
                                   bool show_hidden) {
    if (!running_.load(std::memory_order_acquire) ||
        !chast::valid_file_browser_path(directory) || !chast::valid_file_list_cursor(cursor)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionFileTransfer) == 0) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (pending_browse_.size() >= kMaxPendingBrowse) {
        return false;
    }
    BrowseCommand command;
    command.request_id = next_browse_id_++;
    command.path = std::move(directory);
    command.cursor = std::move(cursor);
    command.show_hidden = show_hidden;
    latest_list_request_ = chast::FileListRequest{command.request_id, command.path,
                                                  command.cursor, command.show_hidden};
    newest_file_list_.reset();
    pending_browse_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::RequestFileDownload(std::string path) {
    if (!running_.load(std::memory_order_acquire) || path.empty() ||
        !chast::valid_file_browser_path(path)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionFileTransfer) == 0) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (pending_browse_.size() >= kMaxPendingBrowse) {
        return false;
    }
    BrowseCommand command;
    command.kind = BrowseCommand::Kind::kDownload;
    command.request_id = next_browse_id_++;
    command.path = std::move(path);
    browse_paths_[command.request_id] = command.path;
    pending_browse_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::RequestFileOperation(chast::FileOperation op, std::string path,
                                        std::string new_name) {
    if (!running_.load(std::memory_order_acquire) ||
        !chast::valid_file_browser_path(path)) {
        return false;
    }
    if (op == chast::FileOperation::Rename) {
        if (!chast::valid_file_browser_name(new_name)) {
            return false;
        }
    } else if (op != chast::FileOperation::MakeDirectory &&
               op != chast::FileOperation::Remove &&
               op != chast::FileOperation::RemoveTree) {
        return false;
    } else if (!new_name.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionFileTransfer) == 0) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (pending_browse_.size() >= kMaxPendingBrowse) {
        return false;
    }
    BrowseCommand command;
    command.kind = BrowseCommand::Kind::kOperation;
    command.request_id = next_browse_id_++;
    command.operation = op;
    command.path = std::move(path);
    command.new_name = std::move(new_name);
    browse_paths_[command.request_id] = command.path;
    pending_browse_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::RequestBatchDownload(std::vector<DownloadRoot> roots, bool show_hidden) {
    return QueueDownload(std::move(roots), true, show_hidden);
}

bool ViewerEngine::RequestDirectoryDownload(std::string path, bool show_hidden) {
    std::vector<DownloadRoot> roots;
    DownloadRoot root;
    root.path = std::move(path);
    root.directory = true;
    roots.push_back(std::move(root));
    return QueueDownload(std::move(roots), false, show_hidden);
}

bool ViewerEngine::QueueDownload(std::vector<DownloadRoot> roots, bool batch,
                                 bool show_hidden) {
    if (!running_.load(std::memory_order_acquire) || roots.empty() ||
        roots.size() > kMaxBatchDownloadRoots) {
        return false;
    }
    for (const DownloadRoot& root : roots) {
        if (root.path.empty() || !chast::valid_file_browser_path(root.path)) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionFileTransfer) == 0) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (folder_request_ || folder_.stage == FolderDownload::Stage::kListing ||
        folder_.stage == FolderDownload::Stage::kDownloading) {
        return false;
    }
    folder_request_ = FolderRequest{std::move(roots), batch, show_hidden};
    return true;
}

void ViewerEngine::CancelDirectoryDownload() {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (folder_.stage == FolderDownload::Stage::kListing ||
        folder_.stage == FolderDownload::Stage::kDownloading) {
        folder_.cancel = true;
    }
}

void ViewerEngine::CancelUploads() {
    upload_cancel_.store(true);
}

void ViewerEngine::CancelDownload() {
    download_cancel_.store(true);
}

bool ViewerEngine::AnswerFileConflict(chast::FileConflictDecision decision,
                                      bool remember) {
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (conflicts_.empty()) {
            return false;
        }
        ConflictAnswer answer;
        answer.transfer_id = conflicts_.front().transfer_id;
        answer.decision = decision;
        // Cancel ends the queue, so there is nothing left to remember.
        answer.remember = remember && decision != chast::FileConflictDecision::Cancel;
        conflict_answers_.push_back(answer);
    }
    // The file worker wakes for the same condition; the session loop polls the
    // answer queue on its next pass.
    file_work_cv_.notify_all();
    return true;
}

bool ViewerEngine::TakeFileList(FileListAnswer& answer) {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (!newest_file_list_) {
        return false;
    }
    answer = std::move(*newest_file_list_);
    newest_file_list_.reset();
    return true;
}

bool ViewerEngine::TakeCompletedFiles(std::vector<CompletedFile>& files) {
    files.clear();
    std::lock_guard<std::mutex> lock(completed_mutex_);
    if (completed_files_.empty()) {
        return false;
    }
    files.reserve(completed_files_.size());
    for (CompletedFile& file : completed_files_) {
        files.push_back(std::move(file));
    }
    completed_files_.clear();
    return true;
}

bool ViewerEngine::HasHashableUploadLocked() const {
    for (const FileUpload& upload : uploads_) {
        if (upload.stage == FileUpload::Stage::kQueued) {
            return true;
        }
    }
    return false;
}

void ViewerEngine::FileWorker() {
    std::unique_lock<std::mutex> lock(file_mutex_);
    for (;;) {
        file_work_cv_.wait(lock, [&] {
            return file_worker_stop_ || !pending_upload_roots_.empty() ||
                   HasHashableUploadLocked();
        });
        if (file_worker_stop_) {
            return;
        }
        const uint64_t epoch = upload_epoch_;
        PendingUploadRoot root;
        FileUpload job;
        bool hash_job = false;
        if (!pending_upload_roots_.empty()) {
            root = std::move(pending_upload_roots_.front());
            pending_upload_roots_.pop_front();
        } else {
            for (FileUpload& upload : uploads_) {
                if (upload.stage == FileUpload::Stage::kQueued) {
                    upload.stage = FileUpload::Stage::kHashing;
                    job = upload;
                    hash_job = true;
                    break;
                }
            }
        }
        lock.unlock();
        if (!root.path.empty()) {
            ExpandUploadRoot(root, epoch);
            lock.lock();
            continue;
        }
        if (hash_job) {
            uint64_t size = 0;
            std::array<uint8_t, 32> digest{};
            std::string error;
            bool ok = chast::sha256_file(job.path, size, digest, &error);
            if (ok && size > chast::kMaxFileTransferBytes) {
                ok = false;
                error = "the file exceeds the protocol's 16 GiB limit";
            }
            // The host compares this with the destination's stamp to tell an
            // identical file from a stale one, so it must be read after the
            // hash proves the bytes.
            uint64_t modified_time = 0;
            if (ok) {
                std::error_code stamp_error;
                const auto stamp =
                    std::filesystem::last_write_time(path_from_utf8(job.path), stamp_error);
                if (!stamp_error) {
                    const auto seconds =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::clock_cast<std::chrono::system_clock>(stamp)
                                .time_since_epoch());
                    if (seconds.count() > 0) {
                        modified_time = static_cast<uint64_t>(seconds.count());
                    }
                }
            }
            lock.lock();
            if (upload_epoch_ != epoch) {
                // A purge cleared the ordinary entries this result belonged to,
                // but concurrent clipboard uploads survive one (the two are
                // gated by different permissions): put a surviving hash job
                // back in the queue instead of leaving it stuck in kHashing.
                for (FileUpload& upload : uploads_) {
                    if (upload.transfer_id == job.transfer_id &&
                        upload.stage == FileUpload::Stage::kHashing &&
                        upload.clipboard) {
                        upload.stage = FileUpload::Stage::kQueued;
                        break;
                    }
                }
                continue;
            }
            for (FileUpload& upload : uploads_) {
                if (upload.transfer_id != job.transfer_id) {
                    continue;
                }
                if (upload.stage != FileUpload::Stage::kHashing) {
                    break;
                }
                if (ok) {
                    upload.size = size;
                    upload.modified_time = modified_time;
                    upload.digest = digest;
                    upload.stage = FileUpload::Stage::kReady;
                    std::fprintf(stderr, "viewer: %s is %llu bytes\n",
                                 job.path.c_str(),
                                 static_cast<unsigned long long>(size));
                } else {
                    upload.stage = FileUpload::Stage::kFailed;
                    upload.error = error.empty() ? "the file could not be hashed" : error;
                    std::fprintf(stderr, "viewer: %s: %s\n", job.path.c_str(),
                                 upload.error.c_str());
                }
                break;
            }
            continue;
        }
        lock.lock();
    }
}

void ViewerEngine::ExpandUploadRoot(const PendingUploadRoot& root, uint64_t epoch) {
    const std::filesystem::path source = path_from_utf8(root.path);
    std::error_code ec;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(source, ec);
    const std::string display_name =
        source.filename().empty() ? root.path : utf8_from_path(source.filename());
    if (ec) {
        EnqueueUploadFailure(display_name, root.prefix,
                             "the path cannot be read: " + ec.message(), epoch);
        return;
    }
    if (!std::filesystem::exists(status)) {
        EnqueueUploadFailure(display_name, root.prefix, "the path no longer exists", epoch);
        return;
    }
    if (std::filesystem::is_regular_file(status)) {
        EnqueueUpload(root.path, root.prefix, epoch);
        return;
    }
    if (!std::filesystem::is_directory(status)) {
        EnqueueUploadFailure(display_name, root.prefix,
                             "only regular files and folders can be sent", epoch);
        return;
    }
    const std::string top = chast::sanitize_received_name(display_name);
    // A folder dropped into a browsed subdirectory keeps its own name below
    // that directory, so the host creates prefix/top/... instead of prefix/...
    const std::string base =
        root.prefix.empty() ? top : root.prefix + "/" + top;
    bool added = false;
    size_t seen = 0;
    std::filesystem::recursive_directory_iterator iterator(
        source, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iterator != end) {
        // A purged queue or a stopped engine must not keep walking a huge tree.
        if ((++seen & 0xFF) == 0) {
            {
                std::lock_guard<std::mutex> lock(file_mutex_);
                if (upload_epoch_ != epoch || file_worker_stop_) {
                    return;
                }
            }
        }
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code status_error;
        const std::filesystem::file_status entry_status =
            entry.symlink_status(status_error);
        // Links are skipped on purpose: following them can leave the dropped
        // tree, loop back and send the same bytes twice.
        if (!status_error && std::filesystem::is_regular_file(entry_status)) {
            const std::filesystem::path relative_dir =
                entry.path().parent_path().lexically_relative(source);
            // lexically_relative uses native separators; the wire wants '/'.
            const std::string relative = chast::make_offer_relative_path(
                base, utf8_from_path(relative_dir));
            if (relative.size() <= chast::kMaxFileRelativePathBytes) {
                if (!EnqueueUpload(utf8_from_path(entry.path()), std::move(relative),
                                   epoch)) {
                    break;
                }
                added = true;
            }
        }
        iterator.increment(ec);
        if (ec) {
            std::fprintf(stderr, "viewer: folder %s: %s\n", root.path.c_str(),
                         ec.message().c_str());
            ec.clear();
        }
    }
    if (!added) {
        EnqueueUploadFailure(top, base,
                             ec ? "the folder cannot be read"
                                : "the folder contains no sendable files",
                             epoch);
    }
}

bool ViewerEngine::EnqueueUpload(const std::string& path, std::string relative_path,
                                 uint64_t epoch, bool clipboard) {
    const std::filesystem::path source = path_from_utf8(path);
    const std::string name = utf8_from_path(source.filename());
    if (name.empty() || name.size() > chast::kMaxFileNameBytes) {
        EnqueueUploadFailure(name.empty() ? "file" : name, std::move(relative_path),
                             "the file name cannot be carried by an offer", epoch);
        return true;
    }
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (upload_epoch_ != epoch) {
        return true;
    }
    if (uploads_.size() >= kMaxQueuedUploads) {
        std::fprintf(stderr, "viewer: upload queue is full; %s was dropped\n",
                     path.c_str());
        return false;
    }
    FileUpload upload;
    upload.stage = FileUpload::Stage::kQueued;
    upload.path = path;
    upload.name = name;
    upload.relative_path = std::move(relative_path);
    upload.transfer_id = next_file_id_++;
    upload.clipboard = clipboard;
    uploads_.push_back(std::move(upload));
    return true;
}

void ViewerEngine::EnqueueUploadFailure(std::string name, std::string relative_path,
                                        std::string error, uint64_t epoch) {
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (upload_epoch_ != epoch || uploads_.size() >= kMaxQueuedUploads) {
        return;
    }
    FileUpload upload;
    upload.stage = FileUpload::Stage::kFailed;
    upload.name = name.empty() ? "file" : std::move(name);
    upload.relative_path = std::move(relative_path);
    upload.error = std::move(error);
    upload.transfer_id = next_file_id_++;
    uploads_.push_back(std::move(upload));
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

bool ViewerEngine::RequestCursorShape(uint64_t shape_id, uint32_t stream_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming) {
            return false;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(session_mutex_);
    // The caller retries on every pointer poll; one request per second for the
    // same missing shape is enough for the host to answer.
    if (cursor_request_valid_ && cursor_request_stream_ == stream_id &&
        cursor_request_shape_ == shape_id &&
        now - cursor_request_at_ < std::chrono::seconds(1)) {
        return false;
    }
    cursor_request_valid_ = true;
    cursor_request_stream_ = stream_id;
    cursor_request_shape_ = shape_id;
    cursor_request_at_ = now;
    pending_cursor_shape_ = std::make_pair(stream_id, shape_id);
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

bool ViewerEngine::SetRecording(bool enabled, std::string directory) {
    std::optional<uint32_t> target;
    if (enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        // The host grants recording explicitly; the stream itself is always
        // recordable locally, so this check is the consent gate, not a
        // technical one. A recording already in progress keeps running if the
        // host later revokes the permission, as in RustDesk.
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionRecord) == 0) {
            return false;
        }
        target = status_.focused_display;
        if (status_.multiple_displays && !target) return false;
    }
    if (!record_wanted_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_stream_ = target;
        if (!directory.empty()) record_dir_ = std::move(directory);
    }
    const bool was_wanted = record_wanted_.exchange(enabled, std::memory_order_relaxed);
    if (enabled && !was_wanted) {
        if (target) {
            std::lock_guard lock(record_mutex_);
            pending_record_keyframe_ = target;
        } else keyframe_wanted_.store(true, std::memory_order_relaxed);
    }
    return true;
}

bool ViewerEngine::SaveScreenshot(std::string directory) {
    std::optional<uint32_t> target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || status_.control_only) {
            return false;
        }
        target = status_.focused_display;
        if (status_.multiple_displays && !target) return false;
    }
    std::lock_guard<std::mutex> lock(screenshot_mutex_);
    pending_screenshot_ = std::move(directory);
    pending_screenshot_stream_ = target;
    if (pending_screenshot_sent_) {
        pending_screenshot_restart_ = true;
    }
    return true;
}

void ViewerEngine::ProcessScreenshot(
    chast::QuicSession* session,
    const std::function<void(const std::string&)>& fallback) {
    std::string directory;
    chast::ScreenshotResult result;
    bool have_result = false;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        if (!pending_screenshot_) {
            return;
        }
        if (pending_screenshot_restart_) {
            // The newer tap replaced the target while this answer was in
            // flight; its slices cannot be attributed to the new request.
            if (session != nullptr) {
                session->cancel_screenshot();
            }
            pending_screenshot_sent_ = false;
            pending_screenshot_restart_ = false;
        }
        if (!pending_screenshot_sent_) {
            if (session != nullptr) {
                const uint32_t stream = pending_screenshot_stream_.value_or(chast::kScreenshotCurrentStream);
                const chast::Error sent = session->request_screenshot(stream);
                if (sent.ok()) {
                    pending_screenshot_sent_ = true;
                    pending_screenshot_deadline_ =
                        std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    return;
                }
                std::fprintf(stderr,
                             "viewer: host screenshot unavailable (%s); saving the rendered picture\n",
                             sent.to_string().c_str());
            }
        } else if (session != nullptr && session->take_screenshot(result)) {
            have_result = true;
        } else if (std::chrono::steady_clock::now() < pending_screenshot_deadline_) {
            return;
        } else {
            if (session != nullptr) {
                session->cancel_screenshot();
            }
            std::fprintf(stderr,
                         "viewer: host screenshot timed out; saving the rendered picture\n");
        }
        directory = std::move(*pending_screenshot_);
        pending_screenshot_.reset();
        pending_screenshot_sent_ = false;
    }

    if (have_result && result.code == chast::ScreenshotCode::Ok && !result.png.empty()) {
        const std::string folder =
            directory.empty() ? default_screenshot_directory() : directory;
        std::string path = screenshot_file_path(folder);
        std::string error;
        if (!chast::write_png_file(path, result.png.data(), result.png.size(), &error)) {
            path.clear();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!path.empty()) {
                status_.screenshot_path = path;
            }
            status_.screenshot_error = error;
            ++status_.screenshot_updates;
        }
        if (error.empty()) {
            std::fprintf(stderr, "viewer: screenshot saved to %s (%ux%u from host)\n",
                         path.c_str(), result.width, result.height);
        } else {
            std::fprintf(stderr, "viewer: screenshot failed: %s\n", error.c_str());
        }
        return;
    }
    if (have_result) {
        std::fprintf(stderr,
                     "viewer: host screenshot refused (%s); saving the rendered picture\n",
                     result.reason.empty() ? "no reason" : result.reason.c_str());
    }
    fallback(directory);
}

void ViewerEngine::SaveScreenshotFromSink(FrameSink* sink, const std::string& directory) {
    std::string error;
    std::string path;
    std::vector<uint8_t> bgra;
    uint32_t width = 0;
    uint32_t height = 0;
    if (sink == nullptr) {
        error = "the session has no picture sink";
    } else if (!sink->CaptureBgra(bgra, width, height, &error)) {
        if (error.empty()) {
            error = "the picture could not be read back";
        }
    } else if (width == 0 || height == 0 || bgra.empty()) {
        error = "the remote picture is empty";
    } else {
        const std::string folder =
            directory.empty() ? default_screenshot_directory() : directory;
        path = screenshot_file_path(folder);
        if (!chast::write_png_bgra(path, bgra.data(), width, height, width * 4, &error)) {
            path.clear();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!path.empty()) {
            status_.screenshot_path = path;
        }
        status_.screenshot_error = error;
        ++status_.screenshot_updates;
    }
    if (error.empty()) {
        std::fprintf(stderr, "viewer: screenshot saved to %s\n", path.c_str());
    } else {
        std::fprintf(stderr, "viewer: screenshot failed: %s\n", error.c_str());
    }
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

bool ViewerEngine::SetVoiceCall(bool enabled) {
    if (enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status_.voice_authorized) {
            return false;
        }
    }
    voice_wanted_.store(enabled, std::memory_order_relaxed);
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

bool ViewerEngine::SelectAudioOutput(std::string device_id) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(audio_output_mutex_);
    audio_output_device_id_ = std::move(device_id);
    return true;
}

bool ViewerEngine::SetAudioProcessing(bool echo_cancellation, bool noise_suppression) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    microphone_aec_.store(echo_cancellation, std::memory_order_relaxed);
    microphone_noise_.store(noise_suppression, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.audio_aec = echo_cancellation;
        status_.audio_noise_suppression = noise_suppression;
    }
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

bool ViewerEngine::ControlDisplays(chast::DisplayControlAction action, uint8_t value,
                                   uint32_t target) {
    const chast::DisplayControlRequest request{action, value, target};
    if (chast::make_display_control(request).empty() ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint16_t need =
            (action == chast::DisplayControlAction::SetCount ||
             action == chast::DisplayControlAction::ToggleOutput)
                ? chast::kPermissionPrivacyMode | chast::kPermissionInput
                : chast::kPermissionPrivacyMode;
        if (status_.state != ViewerState::kStreaming || status_.files_only ||
            status_.display_control_busy ||
            (action != chast::DisplayControlAction::Query &&
             action != chast::DisplayControlAction::Restore &&
             (status_.permissions & need) != need)) {
            return false;
        }
        status_.display_control_busy = true;
    }
    std::lock_guard<std::mutex> lock(privacy_mutex_);
    pending_display_control_ = request;
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

bool ViewerEngine::RequestHostAction(chast::HostAction action, bool block_input) {
    if (action != chast::HostAction::Restart && action != chast::HostAction::LockScreen &&
        action != chast::HostAction::BlockInput) {
        return false;
    }
    if (action != chast::HostAction::BlockInput && block_input) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming) {
            return false;
        }
        if (action == chast::HostAction::Restart && !status_.restart_supported) {
            return false;
        }
        if (action == chast::HostAction::LockScreen && !status_.input_enabled) {
            return false;
        }
        if (action == chast::HostAction::BlockInput &&
            (!status_.input_enabled || !status_.block_input_supported)) {
            return false;
        }
    }
    chast::HostControlRequest request;
    request.action = action;
    request.block_input = block_input;
    std::lock_guard<std::mutex> lock(host_action_mutex_);
    pending_host_action_ = request;
    return true;
}

bool ViewerEngine::RequestOsLogin(const std::string& password) {
    if (password.empty() || chast::make_os_login_request(password).empty()) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.os_login_supported) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(os_login_mutex_);
    if (pending_os_login_) {
        std::fill(pending_os_login_->begin(), pending_os_login_->end(), '\0');
    }
    pending_os_login_ = password;
    return true;
}

bool ViewerEngine::RequestSessionList() {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.session_select_supported) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(session_mutex_);
    pending_session_list_ = true;
    return true;
}

bool ViewerEngine::RequestElevation() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto code = status_.elevation.code;
    if (!running_.load() || status_.state != ViewerState::kStreaming || !status_.input_enabled ||
        (code != chast::ElevationCode::Available && code != chast::ElevationCode::Cancelled &&
         code != chast::ElevationCode::Denied && code != chast::ElevationCode::Failed && code != chast::ElevationCode::Busy)) return false;
    if (++next_elevation_id_ == 0) ++next_elevation_id_;
    status_.elevation = {next_elevation_id_, chast::ElevationCode::Pending, {}};
    ++status_.elevation_updates;
    pending_elevation_.store(next_elevation_id_);
    return true;
}

bool ViewerEngine::RequestSessionSelect(uint32_t session_id) {
    if (chast::make_session_select_request(session_id).empty()) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.session_select_supported) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(session_mutex_);
    pending_session_select_ = session_id;
    return true;
}

bool ViewerEngine::RequestRemoteConfig(uint32_t keys, uint32_t values) {
    if ((keys & ~chast::kRemoteConfigAll) != 0 || (values & ~keys) != 0) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            !status_.remote_config_supported) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(remote_config_mutex_);
    pending_remote_config_ = std::make_pair(keys, values);
    return true;
}

bool ViewerEngine::SendChat(const std::string& text) {
    if (!chast::valid_chat_text(text)) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming) {
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

bool ViewerEngine::SendText(const std::string& text) {
    if (!chast::valid_text_input(text)) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming || !status_.input_enabled) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (!input_enabled_ || pending_input_.size() >= kMaxPendingInput ||
        std::count_if(pending_input_.begin(), pending_input_.end(),
            [](const auto& pending) { return !pending.text.empty(); }) >= 16) {
        return false;
    }
    PendingInput pending;
    pending.text = text;
    pending_input_.push_back(std::move(pending));
    return true;
}

bool ViewerEngine::TakeChat(std::string& text) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    if (received_chat_.empty()) {
        return false;
    }
    text = std::move(received_chat_.front());
    received_chat_.pop_front();
    return true;
}

bool ViewerEngine::OpenTerminal(uint16_t columns, uint16_t rows,
                                uint32_t terminal_id, uint32_t service_id) {
    if (terminal_id == 0 || terminal_id > chast::kMaxTerminals ||
        !chast::valid_terminal_size(columns, rows)) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    uint8_t flags = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionTerminal) == 0) {
            return false;
        }
        // An attach names a console only a persistent host can have kept.
        if (service_id != 0 && !status_.terminal_persist_supported) {
            return false;
        }
        if (service_id != 0) {
            flags = chast::kTerminalRequestAttach;
        } else if (status_.terminal_persist_supported) {
            flags = chast::kTerminalRequestPersist;
        }
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    TerminalChannel& channel = terminals_[terminal_id - 1];
    // The previous console's bytes must be on the panel before its phase can
    // mean anything for a new one. Checked before the phase moves so a
    // rejected open cannot leave kRequesting behind and lock out retries.
    if (!channel.ended || channel.status.phase == TerminalPhase::kRequesting ||
        channel.status.phase == TerminalPhase::kOpen) {
        return false;
    }
    // A fresh generation retires every command and reply that belonged to the
    // previous console in this slot.
    const uint64_t generation = channel.generation + 1;
    channel = {};
    channel.generation = generation;
    channel.ended = false;
    channel.status.phase = TerminalPhase::kRequesting;
    channel.status.columns = columns;
    channel.status.rows = rows;
    ++channel.status.updates;
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kOpen;
    command.slot = terminal_id - 1;
    command.generation = generation;
    command.columns = columns;
    command.rows = rows;
    command.flags = flags;
    command.service_id = service_id;
    pending_terminal_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::SendTerminalInput(const std::string& bytes,
                                     uint32_t terminal_id) {
    if (terminal_id == 0 || terminal_id > chast::kMaxTerminals || bytes.empty() ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    TerminalChannel& channel = terminals_[terminal_id - 1];
    if (channel.status.phase != TerminalPhase::kOpen ||
        bytes.size() > chast::kTerminalInputHighWaterBytes - channel.pending_bytes) {
        return false;
    }
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kInput;
    command.slot = terminal_id - 1;
    command.generation = channel.generation;
    command.bytes = bytes;
    channel.pending_bytes += bytes.size();
    pending_terminal_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::ResizeTerminal(uint16_t columns, uint16_t rows,
                                  uint32_t terminal_id) {
    if (terminal_id == 0 || terminal_id > chast::kMaxTerminals ||
        !chast::valid_terminal_size(columns, rows) ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    const TerminalPhase phase = terminals_[terminal_id - 1].status.phase;
    if (phase != TerminalPhase::kOpen && phase != TerminalPhase::kRequesting) {
        return false;
    }
    // A resize that has not been sent yet is replaced: the console only needs
    // where the panel is now. Only this slot's queued resize is affected.
    for (TerminalCommand& command : pending_terminal_) {
        if (command.slot == terminal_id - 1 &&
            command.generation == terminals_[terminal_id - 1].generation &&
            command.kind == TerminalCommand::Kind::kResize) {
            command.columns = columns;
            command.rows = rows;
            return true;
        }
    }
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kResize;
    command.slot = terminal_id - 1;
    command.generation = terminals_[terminal_id - 1].generation;
    command.columns = columns;
    command.rows = rows;
    pending_terminal_.push_back(std::move(command));
    return true;
}

void ViewerEngine::CloseTerminal(uint32_t terminal_id) {
    if (terminal_id == 0 || terminal_id > chast::kMaxTerminals) {
        return;
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    TerminalChannel& channel = terminals_[terminal_id - 1];
    channel.status.phase = TerminalPhase::kClosed;
    channel.status.code = 0;
    channel.status.reason.clear();
    channel.status.error.clear();
    // The stop kills a persistent service too, so the id must not invite a
    // later attach.
    channel.status.service_id = 0;
    ++channel.status.updates;
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    // Only this tab's queued work is obsolete. A close for one console must
    // not discard input or a pending open belonging to another console.
    for (auto it = pending_terminal_.begin(); it != pending_terminal_.end();) {
        if (it->slot == terminal_id - 1 &&
            it->kind != TerminalCommand::Kind::kOpen) {
            if (it->kind == TerminalCommand::Kind::kInput) {
                channel.pending_bytes -=
                    std::min(channel.pending_bytes, it->bytes.size());
            }
            it = pending_terminal_.erase(it);
        } else {
            ++it;
        }
    }
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kClose;
    command.slot = terminal_id - 1;
    command.generation = channel.generation;
    pending_terminal_.push_back(std::move(command));
}

bool ViewerEngine::TakeTerminalOutput(std::vector<uint8_t>& data, bool& ended,
                                      uint32_t terminal_id) {
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    if (terminal_id == 0 || terminal_id > chast::kMaxTerminals) {
        ended = true;
        return false;
    }
    TerminalChannel& channel = terminals_[terminal_id - 1];
    if (channel.output.empty()) {
        ended = channel.ended;
        return false;
    }
    data = std::move(channel.output.front());
    channel.output.pop_front();
    channel.output_bytes -= std::min(channel.output_bytes, data.size());
    ended = channel.ended && channel.output.empty();
    return true;
}

bool ViewerEngine::RequestCameraList() {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionCamera) == 0) {
            return false;
        }
        if (status_.camera_phase == CameraPhase::kStarting ||
            status_.camera_phase == CameraPhase::kStreaming) {
            return false;
        }
        status_.camera_phase = CameraPhase::kListing;
        status_.camera_code = 0;
        status_.camera_reason.clear();
        status_.camera_error.clear();
        ++status_.camera_updates;
    }
    CameraCommand command;
    command.kind = CameraCommand::Kind::kList;
    std::lock_guard<std::mutex> lock(camera_mutex_);
    pending_camera_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::StartCamera(uint16_t camera_id) {
    if (camera_id == 0 || !running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionCamera) == 0) {
            return false;
        }
        if (status_.camera_phase == CameraPhase::kListing ||
            status_.camera_phase == CameraPhase::kStarting ||
            status_.camera_phase == CameraPhase::kStreaming) {
            return false;
        }
        // The ids in the newest list are the only ones the host accepts; a
        // start for anything else was chosen from a list that is gone.
        bool listed = false;
        for (const chast::CameraDescription& camera : status_.cameras) {
            if (camera.id == camera_id) {
                listed = true;
                break;
            }
        }
        if (!listed) {
            return false;
        }
        status_.camera_phase = CameraPhase::kStarting;
        status_.camera_id = camera_id;
        status_.camera_code = 0;
        status_.camera_reason.clear();
        status_.camera_error.clear();
        ++status_.camera_updates;
    }
    CameraCommand command;
    command.kind = CameraCommand::Kind::kStart;
    command.camera_id = camera_id;
    std::lock_guard<std::mutex> lock(camera_mutex_);
    pending_camera_.push_back(std::move(command));
    return true;
}

void ViewerEngine::StopCamera() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.camera_phase == CameraPhase::kStreaming ||
            status_.camera_phase == CameraPhase::kStarting) {
            status_.camera_phase = CameraPhase::kStopped;
            status_.camera_code = 0;
            status_.camera_reason.clear();
            status_.camera_error.clear();
            ++status_.camera_updates;
        }
    }
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(camera_mutex_);
    // Requests that were never sent would reopen the camera the panel just
    // closed; only the stop itself matters now.
    for (auto it = pending_camera_.begin(); it != pending_camera_.end();) {
        if (it->kind == CameraCommand::Kind::kStop) {
            return;
        }
        it = pending_camera_.erase(it);
    }
    CameraCommand command;
    command.kind = CameraCommand::Kind::kStop;
    pending_camera_.push_back(std::move(command));
}

uint32_t ViewerEngine::StartPortForward(uint16_t local_port,
                                        const std::string& remote_host,
                                        uint16_t remote_port) {
    if (!running_.load(std::memory_order_acquire) || remote_host.empty() ||
        remote_port == 0) {
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionPortForward) == 0) {
            return 0;
        }
    }
    PortForwardCommand command;
    command.kind = PortForwardCommand::Kind::Start;
    command.local_port = local_port;
    command.remote_host = remote_host;
    command.remote_port = remote_port;
    std::lock_guard<std::mutex> lock(port_forward_mutex_);
    command.rule_id = next_port_forward_id_++;
    if (next_port_forward_id_ == 0) {
        next_port_forward_id_ = 1;
    }
    const uint32_t rule_id = command.rule_id;
    pending_port_forward_.push_back(std::move(command));
    return rule_id;
}

void ViewerEngine::StopPortForward(uint32_t rule_id) {
    if (rule_id == 0) {
        return;
    }
    PortForwardCommand command;
    command.kind = PortForwardCommand::Kind::Stop;
    command.rule_id = rule_id;
    std::lock_guard<std::mutex> lock(port_forward_mutex_);
    pending_port_forward_.push_back(std::move(command));
}

ViewerStatus ViewerEngine::Snapshot() const {
    std::scoped_lock lock(mutex_, terminal_mutex_);
    ViewerStatus status = status_;
    for (size_t i = 0; i < terminals_.size(); ++i) {
        status.terminals[i] = terminals_[i].status;
    }
    // The legacy scalars always describe slot 1, so a UI that predates the
    // tabs keeps seeing its console.
    const ViewerTerminalStatus& first = status.terminals.front();
    status.terminal_phase = first.phase;
    status.terminal_code = first.code;
    status.terminal_reason = first.reason;
    status.terminal_error = first.error;
    status.terminal_columns = first.columns;
    status.terminal_rows = first.rows;
    status.terminal_bytes = first.bytes;
    status.terminal_updates = first.updates;
    status.terminal_service_id = first.service_id;
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
                       std::string totp, bool trust_device, ViewerTransport transport,
                       std::string carrier_host, uint16_t carrier_port,
                       std::string carrier_host_id, std::string carrier_token,
                       std::string audit_ref, bool file_transfer_only,
                       chast::SessionMode session_mode,
                       std::vector<uint8_t> access_ticket,
                       std::vector<uint8_t> access_ticket_seed) {
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
    chast::ClipboardDirection clipboard_direction = chast::ClipboardDirection::Both;
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_failed = 0;
    uint64_t chat_sent = 0;
    uint64_t chat_received = 0;
    uint64_t chat_failures = 0;
    uint32_t clipboard_sent_formats = 0;
    uint32_t clipboard_received_formats = 0;
    uint64_t files_sent = 0;
    uint64_t file_failures = 0;
    uint64_t file_cancels = 0;
    uint64_t files_skipped = 0;
    uint64_t files_received = 0;
    uint64_t file_receive_failures = 0;
    uint64_t download_cancels = 0;
    uint64_t downloads_skipped = 0;
    uint64_t file_conflicts = 0;
    // A finished transfer is queued here so the page reports it even if its
    // poll missed the moment. Clipboard staging is excluded: the host does not
    // audit it either, and the two sides must agree on the same file set.
    auto record_completed_file = [&](bool upload, const std::string& name, uint64_t size,
                                     bool success, const std::string& detail) {
        CompletedFile file;
        file.upload = upload;
        file.name = name;
        file.size = size;
        file.success = success;
        file.detail = detail;
        file.occurred_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_files_.push_back(std::move(file));
        while (completed_files_.size() > kMaxCompletedFiles) {
            completed_files_.pop_front();
        }
    };
    uint64_t cursor_updates = 0;
    bool audio_announced = false;
    bool audio_finished = false;
    bool audio_broken = false;
    bool audio_enabled = false;
    bool audio_muted_applied = false;
    chast::PermissionMask permissions = 0;
    uint64_t permission_updates = 0;
    uint32_t audio_sample_rate = 0;
    uint32_t audio_channels = 0;
    uint32_t audio_bitrate = 0;
    uint64_t audio_received = 0;
    uint64_t audio_decoded = 0;
    uint64_t audio_errors = 0;
    uint64_t audio_underruns = 0;
    uint64_t audio_starved_ms = 0;
    uint64_t audio_dropped = 0;
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
    std::unique_ptr<chast::IAudioDecoder> voice_decoder;
    std::unique_ptr<chast::IAudioPlayer> voice_player;
    chast::AudioInfo voice_info;
    bool voice_authorized = false;
    bool voice_active = false;
    bool voice_pending = false;
    uint64_t voice_packets = 0;
    uint64_t voice_dropped = 0;
    uint64_t voice_errors = 0;
    std::string voice_error;
    bool privacy_supported = false;
    bool privacy_enabled = false;
    int privacy_code = 0;
    std::string privacy_error;
    uint64_t privacy_updates = 0;
    chast::DisplayControlState display_control;
    uint64_t display_control_updates = 0;
    chast::DesktopKind desktop = chast::DesktopKind::Default;
    uint64_t desktop_updates = 0;
    int sas_code = 0;
    std::string sas_error;
    uint64_t sas_updates = 0;
    bool restart_supported = false;
    bool block_input_supported = false;
    bool block_input_active = false;
    int host_action_action = 0;
    int host_action_code = 0;
    std::string host_action_error;
    uint64_t host_action_updates = 0;
    bool os_login_supported = false;
    int os_login_code = 0;
    std::string os_login_error;
    uint64_t os_login_updates = 0;
    bool session_select_supported = false;
    std::vector<chast::RemoteSession> sessions;
    int session_list_code = 0;
    std::string session_list_error;
    uint64_t session_list_updates = 0;
    uint32_t session_select_id = 0;
    int session_select_code = 0;
    std::string session_select_error;
    uint64_t session_select_updates = 0;
    uint64_t display_switches = 0;
    uint64_t display_switch_failures = 0;
    std::string display_switch_error;
    uint64_t display_mode_lists = 0;
    uint64_t display_mode_changes = 0;
    uint64_t display_mode_failures = 0;
    std::string display_mode_error;
    std::string display_modes_error;
    uint8_t display_follow_flags = 0;
    int display_follow_code = 0;
    std::string display_follow_error;
    uint64_t display_follow_results = 0;
    int stream_quality = 0;
    int stream_quality_percent = 0;
    int stream_option_code = 0;
    std::string stream_option_error;
    uint64_t stream_option_results = 0;
    bool remote_config_supported = false;
    uint32_t remote_config_flags = 0;
    int remote_config_code = 0;
    std::string remote_config_error;
    uint64_t remote_config_updates = 0;
    double rtt_ms = 0.0;
    double latency_ms = 0.0;
    bool frame_age_valid = false;
    double audio_latency_ms = -1.0;
    double av_skew_ms = -1.0;
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
    // Set from HelloAck: the host serves no screen stream, so the session
    // runs without decoder, sink or media waits. `files_only` is the file
    // browser case; `control_only` covers the v41 terminal, camera and
    // port-forward modes too.
    bool files_only = false;
    bool control_only = false;
    bool logged_frame = false;
    bool logged_drop = false;
    // Frames the display sink refused since the last success, and how many
    // times the sink device has been rebuilt in this session.
    uint64_t sink_failures = 0;
    int display_rebuilds = 0;
    uint64_t init_errors = 0;
    std::string failure;
    // Scratch buffer for native constructors that report through a string.
    std::string error;
    // The path the connect loop settled on, empty until it starts trying, and
    // the reasons earlier paths were abandoned. A successful fallback keeps
    // them so the UI can explain why the session is not on UDP.
    std::string active_transport;
    std::string transport_error;
    std::unique_ptr<chast::IAudioDecoder> audio_decoder;
    std::unique_ptr<chast::IAudioPlayer> audio_player;
    // Datagram audio needs its own ordering: a forward jump is a loss to
    // conceal, a packet behind the highest sequence is too late to play.
    uint64_t expected_audio_seq = 0;
    bool have_audio_seq = false;
    // One reference per session: system-audio PCM feeds it while the player is
    // audible, and the microphone capture cancels against the same ring. It
    // stays inactive until a player actually starts.
    std::shared_ptr<chast::AudioReference> audio_reference =
        std::make_shared<chast::AudioReference>();
    // The endpoint the player was opened with (empty = default), so the menu
    // knows what is actually rendering, plus the selection whose switch was
    // last attempted. They differ after a fallback, which must not rebuild
    // the player on every poll.
    std::string audio_device;
    std::string audio_output_requested;
    {
        std::lock_guard<std::mutex> lock(audio_output_mutex_);
        audio_output_requested = audio_output_device_id_;
    }
    audio_device = audio_output_requested;
    std::string audio_device_name;
    // What the running microphone capture was opened with, plus the processed
    // frame totals of captures that have already stopped this session.
    bool microphone_processing_aec = microphone_aec_.load(std::memory_order_relaxed);
    bool microphone_processing_ns = microphone_noise_.load(std::memory_order_relaxed);
    uint64_t microphone_aec_base = 0;
    uint64_t microphone_ns_base = 0;
    uint64_t microphone_aec_frames = 0;
    uint64_t microphone_ns_frames = 0;

    // Folds the worker's counters into the status the UI polls. Called from
    // this thread only; the window counters are reset once per second.
    // Viewer-side recording. The muxer lives on this thread; the UI only
    // crosses the wanted flag and the folder. The folder is read when a file
    // starts, so a choice made mid-session applies to the next segment.
    std::unique_ptr<chast_tools::Mp4Recorder> recorder;
    std::string record_error;
    const auto close_recording = [&] {
        if (!recorder) return;
        recorder->close();
        if (recorder->failed()) record_error = recorder->error();
        // close() flushes the final sample; retain its counters before the
        // recorder is released so the UI sees the completed file's totals.
        {
            std::lock_guard lock(mutex_);
            status_.record_samples = recorder->samples();
            status_.record_keyframes = recorder->keyframes();
            status_.record_skipped = recorder->skipped();
            status_.record_segments = static_cast<uint32_t>(recorder->segments().size());
            status_.record_path = recorder->segments().empty() ? std::string() : recorder->segments().back();
        }
        recorder.reset();
    };

    DisplayVideoReceiver display_receiver(decoder_factory_);
    bool multiple_displays = false;
    uint64_t window_multi_published = 0;

    std::string record_directory;
    auto next_record_space_check = Clock::now();
    auto record_picture = [&](const chast::FrameHeader& header, const std::vector<uint8_t>& payload,
                              const chast::ServerInfo& picture_info) {
        const bool record_wanted =
            record_wanted_.load(std::memory_order_relaxed);
        if (record_wanted && !recorder) {
            std::string directory;
            {
                std::lock_guard<std::mutex> lock(record_mutex_);
                directory = record_dir_;
            }
            if (directory.empty()) {
                directory = default_record_directory();
            }
            std::error_code dir_ec;
            std::filesystem::create_directories(directory, dir_ec);
            if (!recording_space(directory, kRecordStartFreeBytes)) {
                record_error =
                    "not enough free space in the recording folder (512 MiB required)";
                record_wanted_.store(false, std::memory_order_relaxed);
            } else {
                std::string open_error;
                auto fresh = std::make_unique<chast_tools::Mp4Recorder>();
                if (!fresh->open(record_file_path(directory), picture_info.codec,
                                 picture_info.width, picture_info.height, &open_error)) {
                    record_error = open_error;
                    record_wanted_.store(false, std::memory_order_relaxed);
                } else {
                    record_error.clear();
                    record_directory = directory;
                    next_record_space_check = Clock::now() + std::chrono::seconds(2);
                    std::fprintf(stderr, "viewer: recording started\n");
                    recorder = std::move(fresh);
                }
            }
        } else if (!record_wanted && recorder) {
            close_recording();
            std::fprintf(stderr, "viewer: recording stopped\n");
        }
        if (recorder) {
            const auto now = Clock::now();
            if (now >= next_record_space_check) {
                next_record_space_check = now + std::chrono::seconds(2);
                if (!recording_space(record_directory, kRecordStopFreeBytes)) {
                    record_error = "recording stopped: the recording disk is almost full";
                    close_recording();
                    record_wanted_.store(false, std::memory_order_relaxed);
                }
            }
        }
        if (recorder) {
            recorder->write(header, payload.data(), payload.size());
            if (recorder->failed()) {
                record_error = recorder->error();
                close_recording();
                record_wanted_.store(false, std::memory_order_relaxed);
            }
        }
    };

    auto sync_status = [&](bool final) {
        const auto display_streams = display_receiver.Snapshot();
        const auto multi_totals = display_receiver.Totals();
        const auto now = Clock::now();
        const double window_seconds =
            std::chrono::duration<double>(now - window_start).count();
        double fps = 0.0;
        double kbps = 0.0;
        if (audio_player) {
            const chast::AudioPlayerStats audio_stats = audio_player->stats();
            audio_buffer_ms = audio_stats.buffer_ms;
            audio_underruns = audio_stats.underruns;
            audio_starved_ms = audio_stats.starved_ms;
            audio_dropped = audio_stats.dropped;
            audio_recoveries = audio_stats.recoveries;
            audio_recovering = audio_stats.recovering;
            if (clock_synced) {
                audio_latency_ms = chast::audio_playout_latency_ms(
                    audio_stats.playout_capture_ms, audio_stats.playout_scheduled_ms,
                    clock_offset_us);
                av_skew_ms = audio_latency_ms >= 0.0 && frame_age_valid
                                 ? audio_latency_ms - latency_ms
                                 : -1.0;
            }
        } else {
            audio_latency_ms = -1.0;
            av_skew_ms = -1.0;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (window_seconds >= 1.0) {
                fps = static_cast<double>(window_frames + multi_totals.published - window_multi_published) / window_seconds;
                kbps = static_cast<double>(window_bytes) * 8.0 / 1000.0 / window_seconds;
                status_.fps = fps;
                status_.kbps = kbps;
            }
            status_.received = received;
            status_.decoded = decoded + multi_totals.decoded;
            status_.keyframes = keyframes;
            status_.published = published + multi_totals.published;
            status_.multiple_displays = multiple_displays;
            status_.display_streams = display_streams;
            if (multiple_displays && !display_streams.empty()) {
                const auto focused = std::find_if(display_streams.begin(), display_streams.end(), [&](const auto& stream) {
                    return status_.focused_display && stream.stream.stream_id == *status_.focused_display;
                });
                const auto& actual = (focused == display_streams.end() ? display_streams.front() : *focused).stream.info;
                status_.codec = actual.codec;
                status_.chroma = actual.chroma;
                status_.framerate = actual.framerate;
                status_.server_encoder = actual.encoder;
            }
            status_.crc_errors = crc_errors;
            status_.decode_errors = decode_errors + multi_totals.decode_errors;
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
            status_.clipboard_direction = clipboard_direction;
            status_.clipboard_sent = clipboard_sent;
            status_.clipboard_received = clipboard_received;
            status_.clipboard_failed = clipboard_failed;
            status_.clipboard_sent_formats = clipboard_sent_formats;
            status_.clipboard_received_formats = clipboard_received_formats;
            status_.files_sent = files_sent;
            status_.file_failures = file_failures;
            status_.file_cancels = file_cancels;
            status_.files_skipped = files_skipped;
            status_.files_received = files_received;
            status_.file_receive_failures = file_receive_failures;
            status_.download_cancels = download_cancels;
            status_.downloads_skipped = downloads_skipped;
            status_.file_conflicts = file_conflicts;
            {
                std::lock_guard<std::mutex> file_lock(file_mutex_);
                status_.file_conflict_pending = !conflicts_.empty();
                status_.file_conflict_policy = conflict_policy_;
                if (!conflicts_.empty()) {
                    const PendingFileConflict& conflict = conflicts_.front();
                    status_.file_conflict_download = conflict.download;
                    status_.file_conflict_transfer_id = conflict.transfer_id;
                    status_.file_conflict_name = conflict.name;
                    status_.file_conflict_size = conflict.size;
                    status_.file_conflict_identical = conflict.identical;
                    status_.file_conflict_existing_size = conflict.existing_size;
                    status_.file_conflict_existing_time = conflict.existing_modified_time;
                }
            }
            {
                std::lock_guard<std::mutex> file_lock(file_mutex_);
                status_.download_cancellable = download_.cancellable;
                status_.download_name = download_.name;
                status_.download_path = download_.path;
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
                    case FileDownload::Stage::kConflict:
                        status_.download_phase = FilePhase::kConflict;
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
                const FileUpload& current =
                    uploads_.empty() ? last_upload_ : uploads_.front();
                status_.file_name = current.name;
                status_.file_relative_path = current.relative_path;
                status_.file_size = current.size;
                status_.file_sent = current.sent;
                status_.file_error = current.error;
                status_.file_queue = static_cast<uint32_t>(
                    std::min<size_t>(uploads_.size(), UINT32_MAX));
                status_.last_file_name = last_upload_.name;
                status_.last_file_error = last_upload_.error;
                if (uploads_.empty() && current.name.empty()) {
                    status_.file_phase = FilePhase::kIdle;
                } else {
                    switch (current.stage) {
                        case FileUpload::Stage::kQueued:
                        case FileUpload::Stage::kHashing:
                            status_.file_phase = FilePhase::kHashing;
                            break;
                        case FileUpload::Stage::kReady:
                        case FileUpload::Stage::kOffered:
                            status_.file_phase = FilePhase::kOffered;
                            for (const PendingFileConflict& conflict : conflicts_) {
                                if (!conflict.download &&
                                    conflict.transfer_id == current.transfer_id) {
                                    status_.file_phase = FilePhase::kConflict;
                                    break;
                                }
                            }
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
            status_.audio_starved_ms = audio_starved_ms;
            status_.audio_dropped = audio_dropped;
            status_.audio_recoveries = audio_recoveries;
            status_.audio_recovering = audio_recovering;
            status_.audio_buffer_ms = audio_buffer_ms;
            status_.audio_latency_ms = audio_latency_ms;
            status_.av_skew_ms = av_skew_ms;
            status_.audio_error = audio_error;
            status_.audio_device = audio_device;
            status_.audio_device_name = audio_device_name;
            status_.audio_aec = microphone_aec_.load(std::memory_order_relaxed);
            status_.audio_noise_suppression =
                microphone_noise_.load(std::memory_order_relaxed);
            status_.microphone_aec_frames = microphone_aec_frames;
            status_.microphone_ns_frames = microphone_ns_frames;
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
            if (voice_player) {
                voice_dropped = voice_player->stats().dropped;
            }
            status_.voice_enabled = voice_active;
            status_.voice_pending = voice_pending;
            status_.voice_packets = voice_packets;
            status_.voice_dropped = voice_dropped;
            status_.voice_errors = voice_errors;
            status_.voice_error = voice_error;
            status_.recording = recorder != nullptr;
            if (recorder) {
                status_.record_samples = recorder->samples();
                status_.record_keyframes = recorder->keyframes();
                status_.record_skipped = recorder->skipped();
                status_.record_segments =
                    static_cast<uint32_t>(recorder->segments().size());
                status_.record_path =
                    recorder->segments().empty() ? std::string() : recorder->segments().back();
            }
            status_.record_error = record_error;
            status_.privacy_supported = privacy_supported;
            status_.privacy_enabled = privacy_enabled;
            status_.privacy_code = privacy_code;
            status_.privacy_error = privacy_error;
            status_.privacy_updates = privacy_updates;
            status_.display_control = display_control;
            status_.display_control_updates = display_control_updates;
            status_.desktop = desktop;
            status_.desktop_updates = desktop_updates;
            status_.sas_code = sas_code;
            status_.sas_error = sas_error;
            status_.sas_updates = sas_updates;
            status_.restart_supported = restart_supported;
            status_.block_input_supported = block_input_supported;
            status_.block_input_active = block_input_active;
            status_.host_action_action = host_action_action;
            status_.host_action_code = host_action_code;
            status_.host_action_error = host_action_error;
            status_.host_action_updates = host_action_updates;
            status_.os_login_supported = os_login_supported;
            status_.os_login_code = os_login_code;
            status_.os_login_error = os_login_error;
            status_.os_login_updates = os_login_updates;
            status_.session_select_supported = session_select_supported;
            status_.sessions = sessions;
            status_.session_list_code = session_list_code;
            status_.session_list_error = session_list_error;
            status_.session_list_updates = session_list_updates;
            status_.session_select_id = session_select_id;
            status_.session_select_code = session_select_code;
            status_.session_select_error = session_select_error;
            status_.session_select_updates = session_select_updates;
            status_.display_switches = display_switches;
            status_.display_switch_failures = display_switch_failures;
            status_.display_switch_error = display_switch_error;
            status_.display_mode_lists = display_mode_lists;
            status_.display_mode_changes = display_mode_changes;
            status_.display_mode_failures = display_mode_failures;
            status_.display_mode_error = display_mode_error;
            status_.display_modes_error = display_modes_error;
            status_.display_follow_flags = display_follow_flags;
            status_.display_follow_code = display_follow_code;
            status_.display_follow_error = display_follow_error;
            status_.display_follow_results = display_follow_results;
            status_.stream_quality = stream_quality;
            status_.stream_quality_percent = stream_quality_percent;
            status_.stream_option_code = stream_option_code;
            status_.stream_option_error = stream_option_error;
            status_.stream_option_results = stream_option_results;
            status_.remote_config_supported = remote_config_supported;
            status_.remote_config_flags = remote_config_flags;
            status_.remote_config_code = remote_config_code;
            status_.remote_config_error = remote_config_error;
            status_.remote_config_updates = remote_config_updates;
            if (!active_transport.empty()) {
                status_.transport = active_transport;
            }
            status_.transport_error = transport_error;
            status_.elapsed_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
            if (final) {
                status_.video_finished = video_finished;
            }
        }
        if (window_seconds >= 1.0) {
            window_multi_published = multi_totals.published;
            window_frames = 0;
            window_bytes = 0;
            window_start = now;
        }
    };

    auto fail = [&](std::string message) {
        failure = std::move(message);
    };

    // Both are assigned by the connect loop below. The bridge is declared
    // first so it stays alive until after the session that speaks through it
    // has been destroyed.
    std::unique_ptr<chast::CarrierBridge> carrier_bridge;
    std::unique_ptr<chast::QuicSession> session;

    // Stops the uplink: the capture thread first (it owns the endpoint), then
    // the CMIC stream, so the host sees the switch go off. `switch_off`
    // clears the UI's request too, which is what a failure and a revocation
    // want; closing the stream is also how the host stops decoding.
    auto stop_microphone = [&](const std::string& reason, bool switch_off) {
        if (microphone_capture) {
            const chast::AudioCaptureStats stats = microphone_capture->stats();
            microphone_dropped = stats.dropped;
            microphone_recoveries = stats.recoveries;
            // Fold this capture's DSP work into the session totals before the
            // object (and its counters) goes away.
            if (microphone_processing_aec) {
                microphone_aec_base += stats.processed_frames;
            }
            if (microphone_processing_ns) {
                microphone_ns_base += stats.processed_frames;
            }
            microphone_aec_frames = microphone_aec_base;
            microphone_ns_frames = microphone_ns_base;
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

    // Releases the voice-call playback pipeline. The transport-side call is
    // stopped by drain_voice; this only covers the local decoder and player.
    auto stop_voice = [&] {
        if (voice_player) {
            voice_player->stop();
            voice_player.reset();
        }
        voice_decoder.reset();
        voice_info = chast::AudioInfo{};
        voice_active = false;
        voice_pending = false;
    };

    const bool automatic = transport == ViewerTransport::kAuto;
    std::vector<ViewerTransport> attempts;
    if (automatic) {
        // UDP first: when it works it is the best path. The carrier attempts
        // below are short-lived on purpose, because a blocked UDP port is the
        // very case this fallback exists for.
        attempts = {ViewerTransport::kQuic, ViewerTransport::kTcp,
                    ViewerTransport::kWebSocket};
    } else {
        attempts = {transport};
    }
    // The host listens for carriers on QUIC port + 1. A relay or a forwarded
    // port names its own endpoint; port 65535 has no adjacent port, so the
    // caller has to pass one.
    const std::string fallback_host = carrier_host.empty() ? host : carrier_host;
    uint16_t fallback_port = carrier_port;
    if (fallback_port == 0 && port != 65535) {
        fallback_port = static_cast<uint16_t>(port + 1);
    }

    auto note_attempt = [&](const std::string& label, const std::string& reason) {
        if (!transport_error.empty()) {
            transport_error += "; ";
        }
        transport_error += label + ": " + reason;
        std::fprintf(stderr, "viewer: %s path: %s\n", label.c_str(), reason.c_str());
    };

    std::string server_fingerprint;
    bool connected_ok = false;
    for (const ViewerTransport mode : attempts) {
        const bool over_carrier = mode != ViewerTransport::kQuic;
        const std::string label = mode == ViewerTransport::kQuic
                                      ? "udp"
                                      : mode == ViewerTransport::kTcp ? "tcp" : "ws";
        if (over_carrier) {
            if (fallback_port == 0) {
                note_attempt(label, "no fallback port (QUIC port 65535)");
                continue;
            }
            chast::CarrierBridgeConfig bridge_config;
            bridge_config.host = fallback_host;
            bridge_config.port = fallback_port;
            bridge_config.transport = mode == ViewerTransport::kTcp
                                          ? chast::CarrierTransport::kTcp
                                          : chast::CarrierTransport::kWebSocket;
            bridge_config.connect_timeout = std::chrono::milliseconds(
                automatic ? 3000 : 8000);
            bridge_config.host_id = carrier_host_id;
            bridge_config.token = carrier_token;
            std::string bridge_error;
            carrier_bridge = chast::CarrierBridge::open(bridge_config, &bridge_error);
            if (!carrier_bridge) {
                note_attempt(label, "carrier: " +
                                        (bridge_error.empty() ? "connect failed"
                                                              : bridge_error));
                continue;
            }
            std::fprintf(stderr, "viewer: %s carrier %s:%u, local UDP %u\n",
                         label.c_str(), fallback_host.c_str(), fallback_port,
                         static_cast<unsigned>(carrier_bridge->local_port()));
        }
        chast::QuicClientConfig client_config;
        client_config.host = over_carrier ? "127.0.0.1" : host;
        client_config.port = over_carrier ? carrier_bridge->local_port() : port;
        client_config.pinned_fingerprint = pinned_fingerprint;
        client_config.allow_any_certificate = allow_unverified;
        client_config.access_token = access_token;
        client_config.access_ticket = access_ticket;
        client_config.access_ticket_key = access_ticket_seed;
        client_config.password = password;
        client_config.totp = totp;
        client_config.device_identity = device_identity_.get();
        client_config.trust_device = trust_device;
        client_config.audit_ref = audit_ref;
        {
            std::lock_guard<std::mutex> lock(stream_option_mutex_);
            if (pending_stream_options_) {
                client_config.codec_preference = pending_stream_options_->codec;
                client_config.prefer_chroma = pending_stream_options_->chroma;
            }
        }
        client_config.decoding_444 = sink_ && sink_->device() ? sink_->Decoding444Codecs() : 0;
        client_config.decoding_codecs = sink_ && sink_->device()
            ? sink_->DecodingCodecs() : 0;
        client_config.session_mode =
            session_mode != chast::SessionMode::Desktop
                ? session_mode
                : file_transfer_only ? chast::SessionMode::FileTransfer
                                     : chast::SessionMode::Desktop;
        std::string create_error;
        session = chast::QuicSession::create_client(client_config, &create_error);
        if (!session) {
            note_attempt(label, "QUIC client: " + create_error);
            carrier_bridge.reset();
            continue;
        }
        const std::chrono::seconds connect_timeout =
            !over_carrier ? (automatic ? std::chrono::seconds(4)
                                       : std::chrono::seconds(15))
                          : std::chrono::seconds(10);
        const chast::Error connected = session->connect(connect_timeout);
        // The certificate callback records the presented fingerprint before
        // it decides, so even a rejected identity is visible to the UI.
        server_fingerprint = session->peer_fingerprint();
        if (!server_fingerprint.empty()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.server_fingerprint = server_fingerprint;
            }
            std::fprintf(stderr, "viewer: server identity SHA256 %s\n",
                         chast::format_fingerprint(server_fingerprint).c_str());
        }
        if (connected.ok()) {
            active_transport = label;
            connected_ok = true;
            std::fprintf(stderr, "viewer: connected over %s\n", label.c_str());
            break;
        }
        note_attempt(label, connected.to_string());
        session.reset();
        carrier_bridge.reset();
        // A presented certificate means this path reached the server's TLS
        // stack. Every other carrier would present the same identity, so a
        // refusal is final instead of a reason to try again.
        if (!server_fingerprint.empty()) {
            break;
        }
    }
    if (!connected_ok) {
        fail("connect: " + transport_error);
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
            // The host decides: it answers a control-only request with an
            // empty screen description and the flag, and the UI leaves the
            // picture view for the matching panel.
            files_only = info.files_only;
            control_only = info.files_only || info.control_only;
            display_width = info.width;
            display_height = info.height;
            display_source_width = info.source_width;
            display_source_height = info.source_height;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.files_only = files_only;
                status_.control_only = control_only;
                status_.session_mode = session_mode_name(info.mode);
                status_.codec = info.codec;
                status_.chroma = info.chroma;
                status_.width = info.width;
                status_.height = info.height;
                status_.source_width = info.source_width;
                status_.source_height = info.source_height;
                status_.framerate = info.framerate;
                status_.server_bitrate = info.bitrate;
                status_.server_encoder = info.encoder;
                status_.input_enabled = info.input_enabled;
                status_.clipboard_enabled = info.clipboard_enabled;
                status_.clipboard_direction = info.clipboard_direction;
                status_.origin_x = info.origin_x;
                status_.origin_y = info.origin_y;
                status_.displays = info.displays;
                status_.merged_layout = info.merged;
                status_.terminal_persist_supported = info.terminal_persist;
            }
            {
                std::lock_guard<std::mutex> lock(input_mutex_);
                input_enabled_ = info.input_enabled;
            }
            clipboard_enabled = info.clipboard_enabled;
            clipboard_direction = info.clipboard_direction;

            // Audio is optional end to end: a missing render endpoint or a
            // decoder that cannot start leaves video untouched and reports
            // why through audio_error.
            auto stop_audio_pipeline = [&] {
                if (audio_player) {
                    const chast::AudioPlayerStats stats = audio_player->stats();
                    audio_buffer_ms = stats.buffer_ms;
                    audio_underruns = stats.underruns;
                    audio_starved_ms = stats.starved_ms;
                    audio_dropped = stats.dropped;
                    audio_recoveries = stats.recoveries;
                    audio_recovering = stats.recovering;
                    audio_player->stop();
                    audio_player.reset();
                }
                audio_reference->set_active(false);
                audio_decoder.reset();
                audio_enabled = false;
            };
            // The menu labels the current endpoint with the name WASAPI
            // reported; an unavailable pinned id stays unlabeled and the
            // error text says what happened.
            auto audio_output_name = [](const std::string& id) -> std::string {
                std::string list_error;
                for (const chast::AudioOutputDevice& device :
                     chast::list_audio_outputs(&list_error)) {
                    if (id.empty() ? device.is_default : device.id == id) {
                        return device.name;
                    }
                }
                return {};
            };
            // Opens and starts a player for `wanted`. A pinned endpoint that
            // fails falls back to the default output so a stale pick never
            // silences the session; `note` explains the fallback. Returns
            // false with `note` set when no endpoint could be started.
            auto open_audio_player = [&](const std::string& wanted,
                                         std::string* note) -> bool {
                std::string setup_error;
                chast::AudioPlayerConfig player_config;
                player_config.device_id = wanted;
                // System audio plays as it arrives: no fixed lead and no
                // host-timeline scheduling, so it cannot trail the video by a
                // jitter buffer it does not need. Capture stamps still space
                // blocks inside a burst; the stale drop and queue cap bound
                // the delay after a stall.
                player_config.arrival_anchored = true;
                player_config.lead_ms = 0;
                // The first blocks include session setup; anchor once on the
                // fastest of six so that delay does not stay for the session.
                player_config.anchor_gather_blocks = 6;
                std::unique_ptr<chast::IAudioPlayer> player =
                    chast::create_audio_player(player_config, &setup_error);
                std::string used = wanted;
                if (!player && !wanted.empty()) {
                    player_config.device_id.clear();
                    std::string default_error;
                    player = chast::create_audio_player(player_config, &default_error);
                    if (player) {
                        used.clear();
                        *note = "the selected output could not be opened: " +
                                setup_error + "; using the default output";
                    } else {
                        setup_error += "; default output: " + default_error;
                    }
                }
                if (!player) {
                    *note = setup_error;
                    return false;
                }
                const bool muted = audio_muted_.load(std::memory_order_relaxed);
                player->set_mute(muted);
                chast::Error started = player->start();
                if (!started.ok()) {
                    *note = started.to_string();
                    return false;
                }
                audio_player = std::move(player);
                audio_muted_applied = muted;
                audio_device = used;
                audio_device_name = audio_output_name(used);
                // A muted player renders silence, so it must not seed the
                // echo reference.
                audio_reference->set_active(!muted);
                return true;
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
                std::string wanted_device;
                {
                    std::lock_guard<std::mutex> lock(audio_output_mutex_);
                    wanted_device = audio_output_device_id_;
                }
                std::string note;
                if (!open_audio_player(wanted_device, &note)) {
                    audio_decoder.reset();
                    audio_device = wanted_device;
                    audio_device_name.clear();
                    audio_error = "playback: " + note;
                    std::fprintf(stderr, "viewer: audio %s\n", audio_error.c_str());
                    return;
                }
                audio_enabled = true;
                audio_error = note;
                if (!note.empty()) {
                    std::fprintf(stderr, "viewer: audio %s\n", note.c_str());
                }
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
            auto apply_permissions = [&](chast::PermissionMask mask) {
                if (permission_updates != 0 && mask == permissions) {
                    return;
                }
                permissions = mask;
                ++permission_updates;
                const bool input = (mask & chast::kPermissionInput) != 0;
                const bool clipboard = (mask & chast::kPermissionClipboard) != 0;
                const bool microphone = (mask & chast::kPermissionMicrophone) != 0;
                const bool privacy = (mask & chast::kPermissionPrivacyMode) != 0;
                const bool forward_allowed =
                    (mask & chast::kPermissionPortForward) != 0;
                const bool terminal_allowed =
                    (mask & chast::kPermissionTerminal) != 0;
                const bool camera_allowed = (mask & chast::kPermissionCamera) != 0;
                const bool restart = (mask & chast::kPermissionRestart) != 0;
                // Blocking the operator's own input needs both the input gate
                // and the explicit block-input permission.
                const bool block_input =
                    (mask & (chast::kPermissionInput | chast::kPermissionBlockInput)) ==
                    (chast::kPermissionInput | chast::kPermissionBlockInput);
                // Signing in means typing on the secure desktop, so OS login
                // also requires the input gate.
                const bool os_login =
                    (mask & (chast::kPermissionInput | chast::kPermissionOsLogin)) ==
                    (chast::kPermissionInput | chast::kPermissionOsLogin);
                // The session bit is mask-only: the host itself decides when
                // the service backend can honour a switch.
                const bool session_select =
                    (mask & chast::kPermissionSessionSelect) != 0;
                const bool remote_config =
                    (mask & chast::kPermissionRemoteConfig) != 0;
                restart_supported = restart;
                block_input_supported = block_input;
                os_login_supported = os_login;
                session_select_supported = session_select;
                remote_config_supported = remote_config;
                if (!block_input) {
                    block_input_active = false;
                }
                {
                    std::lock_guard<std::mutex> lock(input_mutex_);
                    input_enabled_ = input;
                    if (!input) {
                        pending_input_.clear();
                    }
                }
                clipboard_enabled = clipboard;
                microphone_authorized = microphone;
                voice_authorized = microphone;
                privacy_supported = privacy;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.permissions = mask;
                    status_.permission_updates = permission_updates;
                    status_.input_enabled = input;
                    status_.clipboard_enabled = clipboard;
                    status_.microphone_authorized = microphone;
                    status_.voice_authorized = microphone;
                    status_.privacy_supported = privacy;
                    status_.port_forward_enabled = forward_allowed;
                    status_.terminal_supported = terminal_allowed;
                    status_.camera_supported = camera_allowed;
                    status_.restart_supported = restart;
                    status_.block_input_supported = block_input;
                    status_.os_login_supported = os_login;
                    status_.session_select_supported = session_select;
                    status_.remote_config_supported = remote_config;
                    if (!block_input) {
                        status_.block_input_active = false;
                    }
                }
                // The transport aborts the stream on revocation; stopping here
                // releases the microphone at once and tells the UI why.
                if (!microphone && (microphone_capture || microphone_stream_open)) {
                    stop_microphone("the host revoked the microphone", true);
                }
                if (!microphone && (voice_active || voice_pending)) {
                    stop_voice();
                    voice_wanted_.store(false, std::memory_order_relaxed);
                    voice_error = "the host revoked the microphone";
                }
            };
            apply_permissions(session->permissions());

            std::fprintf(stderr,
                         "viewer: display request %s; %zu announced, stream origin %d,%d\n",
                         display == chast::kDisplayAuto ? "auto"
                                                       : std::to_string(display).c_str(),
                         info.displays.size(), info.origin_x, info.origin_y);

            // Kept outside the decoder gate below: apply_stream_updates
            // rebuilds the decoder from this config when the host changes
            // its picture size or codec.
            chast::DecoderConfig decoder_config;
            std::unique_ptr<chast::IVideoDecoder> decoder;
            if (!control_only) {
                decoder_config.codec = info.codec;
                decoder_config.chroma = info.chroma;
                const bool picture_fits =
                    info.width <= kMaxFrameSide && info.height <= kMaxFrameSide &&
                    static_cast<uint64_t>(info.width) * info.height <= kMaxFramePixels;
                if (!picture_fits) {
                    fail("display: the host announced a picture larger than this viewer "
                         "accepts");
                } else if (sink_ == nullptr || sink_->device() == nullptr) {
                    fail("display: " +
                         (sink_ != nullptr ? sink_->error()
                                           : std::string("the frame sink is not available")));
                } else {
                    // Decode straight into a D3D11 texture and hand each
                    // picture to the sink while the decoder still owns the
                    // surface: the image never reaches system memory.
                    decoder_config.d3d11_device = sink_->device();
                }
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
                        sink_failures = 0;
                        if (!logged_frame) {
                            logged_frame = true;
                            std::fprintf(stderr, "viewer: first GPU frame %ux%u\n",
                                         frame.width, frame.height);
                        }
                        return chast::Error::success();
                    }
                    if (sink_ == nullptr) {
                        return chast::Error(chast::Status::Unsupported,
                                            "display: the frame sink is not available");
                    }
                    const std::string sink_error = sink_->error();
                    if (sink_->fatal()) {
                        return chast::Error(chast::Status::DeviceRemoved,
                                            "display: " + sink_error);
                    }
                    if (sink_error.empty()) {
                        // No Flutter texture is attached (for example while
                        // the UI is tearing the page down); nothing was
                        // rendered, so this is not a sink failure.
                        if (!logged_drop) {
                            logged_drop = true;
                            std::fprintf(stderr,
                                         "viewer: the display has no GPU output; dropping "
                                         "frames\n");
                        }
                        return chast::Error::success();
                    }
                    // A transient driver failure (mode change, TDR recovery,
                    // a stalled blit) drops this frame; the sink clears the
                    // reason at the start of the next one.
                    ++sink_failures;
                    if (sink_failures >= kMaxSinkFailureStreak) {
                        sink_failures = 0;
                        return chast::Error(chast::Status::CaptureFailed,
                                            "display: " + sink_error);
                    }
                    if (!logged_drop) {
                        logged_drop = true;
                        std::fprintf(stderr,
                                     "viewer: the display sink dropped a frame (%s); "
                                     "retrying\n",
                                     sink_error.c_str());
                    }
                    return chast::Error::success();
                };
                decoder = decoder_config.d3d11_device != nullptr
                              ? decoder_factory_(decoder_config, &error)
                              : nullptr;
                if (!decoder && failure.empty()) {
                    fail("decoder (" + codec_name(info.codec) + "): " + error);
                }
            }
            if (control_only || decoder) {
                if (decoder) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.decoder = decoder->implementation();
                    }
                    std::fprintf(stderr, "viewer: decoder %s\n",
                                 decoder->implementation().c_str());
                } else {
                    std::fprintf(stderr,
                                 "viewer: control-only session; no screen stream follows\n");
                }
                SetState(ViewerState::kStreaming);

                auto request_keyframe = [&] {
                    const auto now = Clock::now();
                    if (now - last_keyframe_request >= kKeyframeRetry) {
                        last_keyframe_request = now;
                        if (multiple_displays) {
                            for (const auto& entry : display_receiver.Snapshot())
                                display_receiver.Recover(entry.stream.stream_id);
                        } else session->request_keyframe();
                    }
                };

                auto poll_displays = [&]() -> bool {
                    chast::CaptureDisplaysResult result;
                    while (session->take_capture_displays_result(result)) {
                        if (!display_receiver.Apply(result, &error)) { fail(error); return false; }
                        if (!multiple_displays && record_wanted_.load()) {
                            std::lock_guard lock(record_mutex_);
                            record_stream_ = 0; // the original decoder's stream identity
                        }
                        multiple_displays = true;
                        decoder.reset();
                        {
                            std::lock_guard lock(cursor_mutex_);
                            pending_cursor_.reset();
                            for (auto it = pending_display_cursors_.begin(); it != pending_display_cursors_.end();)
                                if (!display_receiver.Contains(it->first)) it = pending_display_cursors_.erase(it);
                                else ++it;
                        }
                        {
                            std::lock_guard lock(mutex_);
                            ++status_.display_subscription_results;
                            // Topology notifications must not hide a completed UI request.
                            if (result.request_id != 0) {
                                status_.display_subscription_request = result.request_id;
                                status_.display_subscription_error = result.code == chast::DisplaySwitchCode::Ok ? "" : result.reason;
                            }
                            if (!status_.focused_display || !display_receiver.Contains(*status_.focused_display))
                                status_.focused_display = result.streams.empty() ? std::optional<uint32_t>{} : result.streams.front().stream_id;
                        }
                    }
                    chast::DisplayStreamInfo update;
                    while (session->take_display_stream_update(update)) {
                        if (!display_receiver.Update(update, &error)) { fail(error); return false; }
                    }
                    std::deque<chast::CaptureDisplaysRequest> requests;
                    std::map<uint32_t, std::shared_ptr<FrameSink>> outputs;
                    {
                        std::lock_guard lock(displays_mutex_);
                        requests.swap(pending_displays_); outputs.swap(pending_display_sinks_);
                    }
                    for (const auto& request : requests) {
                        const auto sent = session->request_capture_displays(request);
                        if (!sent.ok()) { fail("display subscriptions: " + sent.to_string()); return false; }
                    }
                    for (auto& [id, output] : outputs) {
                        if (!display_receiver.Bind(id, std::move(output)) && display_receiver.Contains(id)) {
                            std::lock_guard lock(mutex_);
                            status_.display_subscription_error = "a display output is already used by another stream";
                        }
                    }
                    chast::DisplayCursorUpdate cursor;
                    while (session->take_display_cursor_update(cursor)) {
                        if (!display_receiver.Contains(cursor.stream_id)) continue;
                        std::lock_guard lock(cursor_mutex_);
                        chast::coalesce_cursor_update(pending_display_cursors_[cursor.stream_id], std::move(cursor.cursor));
                    }
                    {
                        std::lock_guard lock(record_mutex_);
                        if (pending_record_keyframe_) {
                            display_receiver.Recover(*pending_record_keyframe_);
                            pending_record_keyframe_.reset();
                        }
                    }
                    for (auto id : display_receiver.TakeKeyframeRequests()) session->request_display_keyframe(id);
                    if (multiple_displays) {
                        std::optional<uint32_t> record_target;
                        { std::lock_guard lock(record_mutex_); record_target = record_stream_; }
                        if (record_wanted_.load() && record_target && !display_receiver.Contains(*record_target)) {
                            record_wanted_.store(false);
                            record_error = "the recorded display was unsubscribed";
                        }
                        std::optional<uint32_t> target;
                        bool screenshot_pending = false;
                        {
                            std::lock_guard lock(screenshot_mutex_);
                            target = pending_screenshot_stream_;
                            screenshot_pending = pending_screenshot_.has_value();
                        }
                        if (screenshot_pending) {
                            ProcessScreenshot(
                                session.get(),
                                [this, target, &display_receiver](const std::string& directory) {
                                    auto save = [this, path = directory](FrameSink* sink) {
                                        SaveScreenshotFromSink(sink, path);
                                    };
                                    if (!target) {
                                        save(nullptr);
                                    } else if (!display_receiver.WithFrame(*target, save)) {
                                        save(nullptr);
                                    }
                                });
                        }
                    }
                    if (!record_wanted_.load() && recorder) {
                        close_recording();
                    }
                    return true;
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
                                                : !pending.text.empty()
                                                    ? session->send_text_input(pending.text)
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
                bool clipboard_baseline_pending = true;
                // A failed first observation still records a later baseline,
                // but that baseline is pre-session history and must not feed
                // the one-shot opening send below.
                bool clipboard_baseline_failed = false;
                // One-shot opening sync: the baseline read is sent once so the
                // host starts with the viewer's current item, then later polls
                // keep the change-only semantics.
                bool clipboard_initial_send = false;
                bool clipboard_initial_files = false;
                uint64_t clipboard_sync_version = clipboard_sync_version_.load();
                uint64_t clipboard_epoch = 0;
                uint64_t clipboard_hash = 0;
                uint64_t clipboard_peer_file_list = 0;
                uint64_t clipboard_out_identity = 0;
                uint64_t clipboard_source_version = 0;
                uint64_t last_peer_clipboard_seq = 0;
                bool clipboard_has_files = false;
                auto next_clipboard_poll = Clock::now();
                uint64_t clipboard_seen_generation = chast::ClipboardListener::Generation();
                std::optional<chast::ClipboardSnapshot> clipboard_snapshot;
                chast::ClipboardFileChannel clipboard_channel;
                bool virtual_clipboard_files = true;
                struct PendingClipboard {
                    bool active = false;
                    bool inflight = false;
                    bool files = false;
                    unsigned attempts = 0;
                    chast::ClipboardContent content;
                    std::vector<std::string> paths;
                    std::shared_ptr<chast::ClipboardVirtualFiles> virtual_files;
                    Clock::time_point retry{};
                } pending_clipboard;
                bool clipboard_native = false;
                {
                    chast::ClipboardBackend backend;
                    std::lock_guard lock(clipboard_sink_mutex_);
                    clipboard_native = !clipboard_source_ && !clipboard_file_source_;
                    if (clipboard_source_) {
                        backend.read = [source = clipboard_source_](chast::ClipboardContent& content) {
                            if (!source(content)) return chast::ClipboardReadState::Failed;
                            return content.empty() ? chast::ClipboardReadState::Empty
                                                   : chast::ClipboardReadState::Content;
                        };
                    }
                    if (clipboard_file_source_) {
                        backend.read_files = [source = clipboard_file_source_](std::vector<std::string>& paths) {
                            return source(paths) && !paths.empty() ? chast::ClipboardReadState::Content
                                                                  : chast::ClipboardReadState::Empty;
                        };
                    }
                    backend.write = clipboard_sink_;
                    backend.write_files = clipboard_file_sink_;
                    backend.write_virtual_files = clipboard_virtual_sink_;
                    virtual_clipboard_files = !clipboard_file_sink_ || static_cast<bool>(clipboard_virtual_sink_);
                    if (!clipboard_worker_) {
                        clipboard_worker_ = std::make_unique<chast::ClipboardWorker>(std::move(backend));
                    } else {
                        clipboard_worker_->SetBackend(std::move(backend));
                    }
                }
                // The native clipboard is change-driven: one shared listener
                // serves every window, and the timer below is only a fallback
                // for missed notifications. A synthetic backend cannot emit
                // WM_CLIPBOARDUPDATE, so it keeps the old cadence.
                struct ClipboardSubscription {
                    uint64_t id = 0;
                    ~ClipboardSubscription() {
                        if (id != 0) chast::ClipboardListener::Unsubscribe(id);
                    }
                } clipboard_listener;
                if (clipboard_native) clipboard_listener.id = chast::ClipboardListener::Subscribe();
                const auto clipboard_poll_interval =
                    clipboard_listener.id != 0 ? std::chrono::seconds(5) : kClipboardPollInterval;
                struct ClipboardCancel {
                    chast::ClipboardWorker* worker;
                    ~ClipboardCancel() { worker->Cancel(); }
                } clipboard_cancel{clipboard_worker_.get()};
                auto clipboard_allowed = [&] {
                    return clipboard_enabled &&
                           (permissions & chast::kPermissionClipboard) != 0 &&
                           clipboard_sync_.load(std::memory_order_relaxed);
                };
                auto clipboard_send_allowed = [&] {
                    return clipboard_allowed() && chast::clipboard_to_host(clipboard_direction);
                };
                auto clipboard_receive_allowed = [&] {
                    return clipboard_allowed() && chast::clipboard_from_host(clipboard_direction);
                };
                auto cancel_clipboard_work = [&] {
                    ++clipboard_epoch;
                    clipboard_worker_->Cancel();
                    clipboard_channel.CancelRemote();
                    pending_clipboard = {};
                    clipboard_snapshot.reset();
                };

                // Clipboard file copy (protocol v33). The host announces the
                // files on its clipboard as a FileList; the viewer pulls each
                // entry through the ordinary download channel into a staging
                // directory and puts the finished top-level items on the local
                // clipboard as CF_HDROP. Copied folders arrive as entries
                // sharing a first rel_path component. v1 runs host -> viewer
                // only.
                struct ClipboardFiles {
                    enum class Stage {
                        kIdle,
                        kRequesting,
                        kOffering,
                        kReceiving,
                    };
                    Stage stage = Stage::kIdle;
                    std::vector<chast::ClipboardFileEntry> entries;
                    // Top-level paths for CF_HDROP: flat files, and one
                    // staging folder per copied tree.
                    std::vector<std::string> paths;
                    size_t next = 0;
                    uint32_t request_id = 0;
                    uint32_t transfer_id = 0;
                    std::string directory;
                    uint64_t hash = 0;
                    std::chrono::steady_clock::time_point deadline;
                    std::chrono::steady_clock::time_point retry_after;
                } clipboard_files;
                uint32_t next_clipboard_request = 0x40000000u;
                uint32_t clipboard_staging_seq = 0;

                auto discard_clipboard_files = [&] {
                    std::error_code ec;
                    if (!clipboard_files.directory.empty()) {
                        std::filesystem::remove_all(path_from_utf8(clipboard_files.directory),
                                                    ec);
                    }
                    clipboard_files = ClipboardFiles{};
                };
                auto clip_files_fail = [&](const std::string& message) {
                    if (clipboard_files.stage == ClipboardFiles::Stage::kIdle) {
                        return;
                    }
                    ++clipboard_failed;
                    std::fprintf(stderr, "viewer: clipboard files failed: %s\n",
                                 message.c_str());
                    discard_clipboard_files();
                };
                auto start_clipboard_files =
                    [&](std::vector<chast::ClipboardFileEntry> entries, uint64_t hash) {
                        if (entries.empty() || !clipboard_receive_allowed()) {
                            return;
                        }
                        if (clipboard_files.stage != ClipboardFiles::Stage::kIdle) {
                            std::fprintf(stderr,
                                         "viewer: clipboard files: a transfer is running; "
                                         "the new list was ignored\n");
                            return;
                        }
                        std::error_code ec;
                        const std::filesystem::path base =
                            std::filesystem::temp_directory_path(ec);
                        if (ec || base.empty()) {
                            ++clipboard_failed;
                            std::fprintf(stderr,
                                         "viewer: clipboard files: no temporary directory\n");
                            return;
                        }
                        std::filesystem::path directory;
                        bool created = false;
                        for (int attempt = 0; attempt < 100 && !created; ++attempt) {
                            directory = base / ("chast-clipboard-" +
                                std::to_string(static_cast<unsigned long long>(std::time(nullptr))) +
                                "-" + std::to_string(++clipboard_staging_seq));
                            created = std::filesystem::create_directory(directory, ec);
                            if (ec) break;
                        }
                        if (!created) {
                            ++clipboard_failed;
                            std::fprintf(stderr, "viewer: clipboard files: cannot create the staging folder: %s\n",
                                         ec ? ec.message().c_str() : "no unused staging name");
                            return;
                        }
                        clipboard_files.entries = std::move(entries);
                        clipboard_files.hash = hash;
                        clipboard_files.directory = utf8_from_path(directory);
                        clipboard_files.stage = ClipboardFiles::Stage::kRequesting;
                        std::fprintf(stderr, "viewer: clipboard carries %zu file(s)\n",
                                     clipboard_files.entries.size());
                    };
                auto step_clipboard_files = [&] {
                    if (clipboard_files.stage == ClipboardFiles::Stage::kIdle) {
                        return;
                    }
                    if (!clipboard_receive_allowed()) {
                        clip_files_fail("clipboard sync is disabled");
                        return;
                    }
                    const auto now = Clock::now();
                    if (clipboard_files.stage == ClipboardFiles::Stage::kRequesting &&
                        clipboard_files.request_id != 0) {
                        if (now > clipboard_files.deadline) {
                            clip_files_fail(
                                "the host did not answer the download of '" +
                                clipboard_files.entries[clipboard_files.next].name + "'");
                        }
                        return;
                    }
                    if (clipboard_files.stage == ClipboardFiles::Stage::kOffering ||
                        clipboard_files.stage == ClipboardFiles::Stage::kReceiving) {
                        return;
                    }
                    if (now < clipboard_files.retry_after) {
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (download_.stage == FileDownload::Stage::kReceiving) {
                            clipboard_files.retry_after =
                                now + std::chrono::milliseconds(500);
                            return;
                        }
                    }
                    // Empty directory entries only carry metadata. Create
                    // them locally, retaining their original list indices so
                    // subsequent .chast-clipboard/<index> requests stay valid.
                    while (clipboard_files.next < clipboard_files.entries.size() &&
                           clipboard_files.entries[clipboard_files.next].directory) {
                        const auto& entry = clipboard_files.entries[clipboard_files.next];
                        const auto relative = entry.rel_path.empty() ? entry.name : entry.rel_path;
                        std::string error;
                        if (!chast::create_receive_directory(clipboard_files.directory, relative, &error)) {
                            clip_files_fail(error);
                            return;
                        }
                        const auto slash = relative.find('/');
                        const auto top = utf8_from_path(path_from_utf8(clipboard_files.directory) /
                            path_from_utf8(relative.substr(0, slash)));
                        if (std::find(clipboard_files.paths.begin(), clipboard_files.paths.end(), top) == clipboard_files.paths.end())
                            clipboard_files.paths.push_back(top);
                        ++clipboard_files.next;
                    }
                    if (clipboard_files.next >= clipboard_files.entries.size()) {
                        if (clipboard_peer_file_list != clipboard_files.hash) {
                            // A newer clipboard item replaced this list while
                            // it downloaded; stale files must not take over.
                            std::fprintf(stderr,
                                         "viewer: clipboard files: a newer item replaced "
                                         "the list\n");
                            discard_clipboard_files();
                            return;
                        }
                        if (!pending_clipboard.active) {
                            cancel_clipboard_work();
                            pending_clipboard.active = true;
                            pending_clipboard.files = true;
                            pending_clipboard.paths = clipboard_files.paths;
                            pending_clipboard.retry = now;
                        }
                        return;
                    }
                    const size_t index = clipboard_files.next;
                    const std::string path = kClipboardFilePathPrefix + std::to_string(index);
                    const uint32_t request_id = next_clipboard_request++;
                    chast::Error sent = session->request_file_download(request_id, path);
                    if (!sent.ok()) {
                        clip_files_fail("request '" + path + "': " + sent.to_string());
                        return;
                    }
                    clipboard_files.request_id = request_id;
                    clipboard_files.deadline = now + kClipboardAnswerTimeout;
                };

                auto poll_clipboard = [&] {
                    if (!clipboard_send_allowed()) return;
                    if (!clipboard_snapshot || clipboard_snapshot->has_files ||
                        clipboard_snapshot->state != chast::ClipboardReadState::Content) return;
                    chast::ClipboardContent current = std::move(clipboard_snapshot->content);
                    clipboard_snapshot.reset();
                    const uint64_t hash = chast::clipboard_content_hash(current);
                    if (hash == clipboard_hash && !clipboard_initial_send) {
                        return;
                    }
                    clipboard_initial_send = false;
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
                    if (!clipboard_receive_allowed()) return;
                    if (content.seq && content.seq <= last_peer_clipboard_seq) return;
                    if (content.seq) last_peer_clipboard_seq = content.seq;
                    const uint64_t hash = chast::clipboard_content_hash(content);
                    if (hash == clipboard_hash && !pending_clipboard.active && !content.find(chast::ClipboardFormat::FileList)) {
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
                    const chast::ClipboardFormatData* file_list = nullptr;
                    for (const chast::ClipboardFormatData& format : content.formats) {
                        if (format.format == chast::ClipboardFormat::FileList) {
                            file_list = &format;
                            break;
                        }
                    }
                    if (file_list != nullptr) {
                        // The files themselves arrive through the download
                        // channel; only the list crosses the clipboard stream.
                        std::vector<chast::ClipboardFileEntry> entries;
                        std::string file_error;
                        if (!chast::decode_clipboard_file_list(file_list->data, entries,
                                                               &file_error)) {
                            ++clipboard_failed;
                            std::fprintf(stderr,
                                         "viewer: clipboard: the host sent an unusable "
                                         "file list: %s\n",
                                         file_error.c_str());
                            return;
                        }
                        cancel_clipboard_work();
                        clipboard_hash = hash;
                        clipboard_peer_file_list = hash;
                        clipboard_channel.ClearLocal();
                        if (virtual_clipboard_files) {
                            pending_clipboard.virtual_files = clipboard_channel.Receive(content.seq, std::move(entries));
                            if (!pending_clipboard.virtual_files) { ++clipboard_failed; return; }
                            pending_clipboard.active = true;
                            pending_clipboard.files = true;
                            pending_clipboard.retry = Clock::now();
                            return;
                        }
                        start_clipboard_files(std::move(entries), hash);
                        return;
                    }
                    cancel_clipboard_work();
                    clipboard_peer_file_list = 0;
                    clipboard_channel.ClearLocal();
                    pending_clipboard.active = true;
                    pending_clipboard.content = std::move(content);
                    pending_clipboard.retry = Clock::now();
                };

                auto receive_clipboard = [&] {
                    chast::ClipboardContent content;
                    while (clipboard_receive_allowed() && session->take_clipboard_content(content)) {
                        apply_peer_clipboard(std::move(content));
                    }
                    // A v15 host may still send the text message; treat it as
                    // a text-only item rather than dropping it.
                    chast::ClipboardUpdate legacy;
                    while (clipboard_receive_allowed() && session->take_clipboard_update(legacy)) {
                        chast::ClipboardContent text_only;
                        text_only.seq = legacy.seq;
                        chast::ClipboardFormatData text;
                        text.format = chast::ClipboardFormat::Text;
                        text.data.assign(legacy.text.begin(), legacy.text.end());
                        text_only.formats.push_back(std::move(text));
                        apply_peer_clipboard(std::move(text_only));
                    }
                };

                // One upload at a time: the head of the queue is offered,
                // opened at the host's resume offset and streamed in
                // time-budgeted chunks so video, audio and input keep
                // flowing. The worker fills the queue off this thread.
                std::ifstream file_stream;
                double file_tokens = kFileUploadBurst;
                Clock::time_point file_token_time = Clock::now();

                // Drops queued clipboard uploads without touching ordinary
                // ones: the two kinds are gated by different permissions, and
                // a clipboard change must not cancel a file the operator sent
                // from the browser. `keep_front` leaves a head that is already
                // marked failed for the accounting loop.
                auto drop_clipboard_uploads = [&](const std::string& message,
                                                  bool keep_front = false) {
                    uint32_t active_id = 0;
                    size_t dropped = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        bool first = true;
                        for (auto it = uploads_.begin(); it != uploads_.end();) {
                            const bool skip = keep_front && first;
                            first = false;
                            if (skip || !it->clipboard) {
                                ++it;
                                continue;
                            }
                            if (it->stage == FileUpload::Stage::kOffered ||
                                it->stage == FileUpload::Stage::kSending ||
                                it->stage == FileUpload::Stage::kFinishing) {
                                active_id = it->transfer_id;
                            }
                            it = uploads_.erase(it);
                            ++dropped;
                        }
                    }
                    if (active_id != 0) {
                        file_stream.close();
                        session->cancel_file_upload(active_id);
                    }
                    if (dropped > 0) {
                        std::fprintf(stderr,
                                     "viewer: clipboard files: %zu upload(s) dropped (%s)\n",
                                     dropped, message.c_str());
                    }
                };

                // Marks the head failed. The failure counter advances when a
                // failed entry leaves the queue, so every failure is counted
                // exactly once no matter which path reported it.
                auto file_fail = [&](const std::string& message) {
                    uint32_t transfer_id = 0;
                    std::string name;
                    bool clipboard = false;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (uploads_.empty()) {
                            return;
                        }
                        FileUpload& head = uploads_.front();
                        if (head.stage == FileUpload::Stage::kDone ||
                            head.stage == FileUpload::Stage::kFailed) {
                            return;
                        }
                        transfer_id = head.transfer_id;
                        name = head.name;
                        clipboard = head.clipboard;
                        head.stage = FileUpload::Stage::kFailed;
                        head.error = message;
                    }
                    file_stream.close();
                    // Frees the host's receive slot and its .part; harmless
                    // when the host already ended the transfer itself.
                    session->cancel_file_upload(transfer_id);
                    std::fprintf(stderr, "viewer: file %s failed: %s\n", name.c_str(),
                                 message.c_str());
                    if (clipboard) {
                        // The host aborted the list after this entry; offering
                        // the rest would only draw one refusal per file.
                        drop_clipboard_uploads(message, true);
                    }
                };

                // A revoked grant drops the ordinary queue; the user should
                // not receive one rejection per entry for a permission that is
                // already gone. Clipboard uploads belong to the clipboard
                // permission and stay queued.
                auto purge_uploads = [&](const std::string& message,
                                         bool cancelled = false) {
                    uint32_t active_id = 0;
                    uint32_t held_offer = 0;
                    std::string name;
                    size_t dropped = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        // A folder walk or hash still in flight must not
                        // repopulate the queue after the purge.
                        ++upload_epoch_;
                        // A held upload prompt leaves with the queue it
                        // belongs to; the host has to learn the offer ended.
                        for (auto it = conflicts_.begin(); it != conflicts_.end();) {
                            if (it->download) {
                                ++it;
                                continue;
                            }
                            if (held_offer == 0) {
                                held_offer = it->transfer_id;
                            }
                            it = conflicts_.erase(it);
                        }
                        bool reported = false;
                        for (auto it = uploads_.begin(); it != uploads_.end();) {
                            if (it->clipboard) {
                                ++it;
                                continue;
                            }
                            if (it->stage == FileUpload::Stage::kOffered ||
                                it->stage == FileUpload::Stage::kSending ||
                                it->stage == FileUpload::Stage::kFinishing) {
                                active_id = it->transfer_id;
                            }
                            // An entry that already reached a terminal stage
                            // keeps its real outcome in the audit; only the
                            // entries the purge actually aborts are failures.
                            const bool terminal = it->stage == FileUpload::Stage::kDone ||
                                                  it->stage == FileUpload::Stage::kFailed;
                            if (terminal) {
                                record_completed_file(true, it->name, it->size,
                                                      it->stage == FileUpload::Stage::kDone,
                                                      it->error);
                            } else {
                                record_completed_file(true, it->name, it->size, false,
                                                      message);
                            }
                            if (!reported) {
                                reported = true;
                                name = it->name;
                                FileUpload finished = std::move(*it);
                                finished.stage = FileUpload::Stage::kFailed;
                                finished.error = message;
                                last_upload_ = std::move(finished);
                                if (cancelled) {
                                    ++file_cancels;
                                } else {
                                    ++file_failures;
                                }
                            }
                            it = uploads_.erase(it);
                            ++dropped;
                        }
                        pending_upload_roots_.clear();
                    }
                    if (active_id != 0) {
                        file_stream.close();
                        session->cancel_file_upload(active_id);
                    }
                    if (held_offer != 0) {
                        chast::FileConflictAnswer release;
                        release.transfer_id = held_offer;
                        release.decision = chast::FileConflictDecision::Cancel;
                        (void)session->send_file_conflict_answer(release);
                    }
                    if (!name.empty()) {
                        std::fprintf(stderr,
                                     "viewer: file %s failed: %s (%zu queued entries "
                                     "dropped)\n",
                                     name.c_str(), message.c_str(), dropped);
                    }
                };

                // Viewer -> host clipboard files (protocol v34). The local
                // CF_HDROP list is announced as a FileList and every entry then
                // rides the ordinary upload channel marked clipboard, so the
                // host stages it for its own clipboard instead of its receive
                // directory. A baseline is taken when sync opens: whatever was
                // on the clipboard before the session is never flushed.
                auto pump_clipboard = [&] {
                    const auto now = Clock::now();
                    clipboard_channel.Poll(*session, clipboard_send_allowed(), clipboard_receive_allowed());
                    clipboard_snapshot.reset();
                    const uint64_t version = clipboard_sync_version_.load(std::memory_order_acquire);
                    if (version != clipboard_sync_version) {
                        cancel_clipboard_work();
                        clipboard_was_allowed = false;
                        clipboard_sync_version = version;
                    }
                    if (!clipboard_allowed()) {
                        if (clipboard_was_allowed) cancel_clipboard_work();
                        clipboard_was_allowed = false;
                        clipboard_has_files = false;
                        return;
                    }
                    if (!clipboard_was_allowed) {
                        cancel_clipboard_work();
                        clipboard_was_allowed = true;
                        clipboard_baseline_pending = true;
                        clipboard_baseline_failed = false;
                        clipboard_initial_send = false;
                        clipboard_initial_files = false;
                        clipboard_hash = 0;
                        clipboard_out_identity = 0;
                        next_clipboard_poll = now;
                    }
                    chast::ClipboardWorker::Result result;
                    if (clipboard_worker_->Take(result) && result.epoch == clipboard_epoch) {
                        if (result.operation == chast::ClipboardWorker::Operation::Read) {
                            if (!pending_clipboard.active &&
                                result.snapshot.state == chast::ClipboardReadState::Failed &&
                                clipboard_baseline_pending) {
                                clipboard_baseline_failed = true;
                            }
                            if (!pending_clipboard.active && result.snapshot.state != chast::ClipboardReadState::Failed) {
                                clipboard_has_files = result.snapshot.has_files;
                                if (clipboard_baseline_pending) {
                                    clipboard_baseline_pending = false;
                                    clipboard_hash = result.snapshot.content.empty() ? 0
                                        : chast::clipboard_content_hash(result.snapshot.content);
                                    clipboard_out_identity = clipboard_has_files
                                        ? chast::clipboard_file_identity_hash(result.snapshot.entries) : 0;
                                    clipboard_source_version = result.snapshot.local_files ? result.snapshot.local_files->SourceVersion() : 0;
                                    clipboard_initial_files = !clipboard_baseline_failed &&
                                        clipboard_has_files && !result.snapshot.entries.empty();
                                    clipboard_initial_send = !clipboard_baseline_failed &&
                                        !clipboard_has_files && !result.snapshot.content.empty();
                                    clipboard_baseline_failed = false;
                                }
                                clipboard_snapshot = std::move(result.snapshot);
                            }
                            next_clipboard_poll = now + clipboard_poll_interval;
                        } else if (pending_clipboard.active) {
                            pending_clipboard.inflight = false;
                            if (result.written) {
                                clipboard_baseline_pending = false;
                                clipboard_baseline_failed = false;
                                clipboard_initial_send = false;
                                clipboard_initial_files = false;
                                ++clipboard_received;
                                if (pending_clipboard.files) {
                                    clipboard_received_formats = 1u << static_cast<uint32_t>(chast::ClipboardFormat::FileList);
                                    clipboard_out_identity = chast::clipboard_file_identity_hash(pending_clipboard.virtual_files
                                        ? pending_clipboard.virtual_files->entries() : clipboard_files.entries);
                                    clipboard_source_version = 0;
                                    clipboard_files = ClipboardFiles{};
                                } else {
                                    clipboard_hash = chast::clipboard_content_hash(pending_clipboard.content);
                                    clipboard_out_identity = 0;
                                    clipboard_received_formats = formats_mask(pending_clipboard.content);
                                }
                                pending_clipboard = {};
                            } else {
                                ++clipboard_failed;
                                if (++pending_clipboard.attempts >= 10) {
                                    if (pending_clipboard.virtual_files) clipboard_channel.CancelRemote();
                                    if (pending_clipboard.files) discard_clipboard_files();
                                    pending_clipboard = {};
                                } else {
                                    pending_clipboard.retry = now + std::chrono::milliseconds(250);
                                }
                            }
                        }
                    }
                    if (pending_clipboard.active) {
                        if (!pending_clipboard.inflight && now >= pending_clipboard.retry) {
                            pending_clipboard.inflight = pending_clipboard.virtual_files
                                ? clipboard_worker_->WriteVirtualFiles(clipboard_epoch, pending_clipboard.virtual_files)
                                : pending_clipboard.files
                                ? clipboard_worker_->WriteFiles(clipboard_epoch, pending_clipboard.paths)
                                : clipboard_worker_->Write(clipboard_epoch, pending_clipboard.content);
                        }
                    } else if (clipboard_send_allowed()) {
                        const uint64_t change = chast::ClipboardListener::Generation();
                        if (now >= next_clipboard_poll ||
                            (clipboard_listener.id != 0 && change != clipboard_seen_generation)) {
                            if (clipboard_worker_->Read(clipboard_epoch)) {
                                clipboard_seen_generation = change;
                            }
                        }
                    }
                };
                auto poll_local_clipboard_files = [&] {
                    if (!clipboard_send_allowed()) return;
                    if (!clipboard_snapshot) return;
                    if (!clipboard_snapshot->has_files) {
                        clipboard_out_identity = 0;
                        clipboard_channel.ClearLocal();
                        return;
                    }
                    auto entries = std::move(clipboard_snapshot->entries);
                    auto local_files = std::move(clipboard_snapshot->local_files);
                    clipboard_snapshot.reset();
                    const uint64_t identity = chast::clipboard_file_identity_hash(entries);
                    const auto source_version = local_files ? local_files->SourceVersion() : 0;
                    const bool initial_files = clipboard_initial_files;
                    clipboard_initial_files = false;
                    if (identity == 0 || (!initial_files && identity == clipboard_out_identity &&
                        (!clipboard_source_version || source_version == clipboard_source_version))) {
                        return;
                    }
                    chast::ClipboardFormatData format;
                    format.format = chast::ClipboardFormat::FileList;
                    format.data = chast::encode_clipboard_file_list(entries);
                    if (format.data.empty()) {
                        clipboard_out_identity = identity;
                        std::fprintf(stderr,
                                     "viewer: clipboard files: the local list is not "
                                     "sendable\n");
                        return;
                    }
                    // The local copy wins over a host list still arriving: the
                    // staging download is aborted and its slot released.
                    if (clipboard_files.stage != ClipboardFiles::Stage::kIdle) {
                        uint32_t replaced = 0;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (download_.stage == FileDownload::Stage::kReceiving) {
                                replaced = download_.transfer_id;
                                if (download_.receiver) {
                                    download_.receiver->abort(true);
                                    download_.receiver.reset();
                                }
                                download_.stage = FileDownload::Stage::kFailed;
                                download_.error =
                                    "the local clipboard replaced the host's list";
                            }
                        }
                        if (replaced != 0) {
                            (void)session->send_file_result(
                                replaced, chast::FileCode::IoError, 0,
                                "the local clipboard replaced the host's list");
                        }
                        std::fprintf(stderr,
                                     "viewer: clipboard files: the local copy replaced "
                                     "the host's list\n");
                        discard_clipboard_files();
                    }
                    // Publish metadata only. The host's paste target requests
                    // bounded ranges through the per-session clipboard channel.
                    drop_clipboard_uploads("the local clipboard changed");
                    const uint64_t sent_identity = identity;
                    chast::ClipboardContent content;
                    content.seq = clipboard_seq;
                    content.formats.push_back(std::move(format));
                    chast::Error sent = session->send_clipboard_content(std::move(content));
                    if (!sent.ok()) {
                        ++clipboard_failed;
                        std::fprintf(stderr, "viewer: clipboard files send failed: %s\n",
                                     sent.to_string().c_str());
                        drop_clipboard_uploads("the file list could not be sent");
                        return;
                    }
                    clipboard_out_identity = sent_identity;
                    clipboard_channel.CancelRemote();
                    clipboard_channel.SetLocal(clipboard_seq, std::move(local_files));
                    clipboard_source_version = source_version;
                    // Files own the clipboard now; the next rich item counts as
                    // a fresh change even if it matches the old text.
                    clipboard_hash = 0;
                    clipboard_sent_formats =
                        1u << static_cast<uint32_t>(chast::ClipboardFormat::FileList);
                    ++clipboard_seq;
                    ++clipboard_sent;
                    std::fprintf(stderr, "viewer: clipboard files announced (%zu item(s))\n",
                                 entries.size());
                };

                auto drive_file_upload = [&] {
                    // A finished head hands over to last_upload_, which keeps
                    // the last outcome visible after the queue drains.
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        // A prompt whose offer was purged (cancel, revocation)
                        // has nobody left to answer it.
                        std::erase_if(conflicts_, [&](const PendingFileConflict& conflict) {
                            return !conflict.download &&
                                   (uploads_.empty() ||
                                    uploads_.front().transfer_id != conflict.transfer_id);
                        });
                        while (!uploads_.empty()) {
                            const FileUpload::Stage stage = uploads_.front().stage;
                            if (stage != FileUpload::Stage::kDone &&
                                stage != FileUpload::Stage::kFailed) {
                                break;
                            }
                            const bool success = stage == FileUpload::Stage::kDone;
                            const bool skipped = success && uploads_.front().skipped;
                            if (skipped) {
                                ++files_skipped;
                            } else if (!success) {
                                ++file_failures;
                            } else {
                                ++files_sent;
                            }
                            if (!uploads_.front().clipboard && !skipped) {
                                // The host writes no audit record for a kept
                                // file either, so the two sets stay aligned.
                                record_completed_file(true, uploads_.front().name,
                                                      uploads_.front().size, success,
                                                      uploads_.front().error);
                            }
                            last_upload_ = std::move(uploads_.front());
                            uploads_.pop_front();
                        }
                    }
                    // The clipboard half of the queue has its own gate, and an
                    // incoming host list supersedes whatever the local poll
                    // queued: the host's clipboard is the current one.
                    if (!clipboard_send_allowed()) {
                        drop_clipboard_uploads("clipboard sync was disabled");
                    } else if (clipboard_files.stage != ClipboardFiles::Stage::kIdle) {
                        drop_clipboard_uploads("the host replaced the clipboard");
                    }
                    FileUpload snapshot;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (uploads_.empty()) {
                            return;
                        }
                        snapshot = uploads_.front();
                    }
                    // Every result ends the attempt it belongs to, including
                    // a host-side cancellation of a stream we still own.
                    chast::FileResult result;
                    while (session->take_file_result(result)) {
                        if (result.transfer_id != snapshot.transfer_id) {
                            continue;
                        }
                        if (snapshot.stage != FileUpload::Stage::kOffered &&
                            snapshot.stage != FileUpload::Stage::kSending &&
                            snapshot.stage != FileUpload::Stage::kFinishing) {
                            continue;
                        }
                        file_stream.close();
                        if (result.code == chast::FileCode::Ok) {
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                if (!uploads_.empty() &&
                                    uploads_.front().transfer_id ==
                                        snapshot.transfer_id) {
                                    uploads_.front().stage = FileUpload::Stage::kDone;
                                    uploads_.front().error.clear();
                                }
                            }
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
                                if (!uploads_.empty() &&
                                    uploads_.front().transfer_id ==
                                        snapshot.transfer_id) {
                                    uploads_.front().stage = FileUpload::Stage::kFailed;
                                    uploads_.front().error = reason;
                                }
                            }
                            std::fprintf(stderr, "viewer: file %s failed: %s\n",
                                         snapshot.name.c_str(), reason.c_str());
                            if (snapshot.clipboard) {
                                // A refused entry means the host already
                                // abandoned the list; the rest would only draw
                                // one refusal per file.
                                drop_clipboard_uploads(reason, true);
                            }
                        }
                        return;
                    }
                    // Clipboard uploads are gated by the clipboard permission,
                    // already applied above; only ordinary entries answer to a
                    // revoked file transfer.
                    if (!snapshot.clipboard &&
                        (permissions & chast::kPermissionFileTransfer) == 0) {
                        purge_uploads("the host revoked file transfer");
                        return;
                    }
                    if (snapshot.stage == FileUpload::Stage::kQueued ||
                        snapshot.stage == FileUpload::Stage::kHashing) {
                        return;
                    }
                    if (snapshot.stage == FileUpload::Stage::kReady) {
                        chast::FileOffer offer;
                        offer.transfer_id = snapshot.transfer_id;
                        offer.size = snapshot.size;
                        offer.modified_time = snapshot.modified_time;
                        offer.resume = true;
                        offer.sha256 = snapshot.digest;
                        offer.name = snapshot.name;
                        offer.relative_path = snapshot.relative_path;
                        offer.clipboard = snapshot.clipboard;
                        chast::Error sent = session->offer_file(offer);
                        if (!sent.ok()) {
                            file_fail("offer: " + sent.to_string());
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (!uploads_.empty() &&
                                uploads_.front().transfer_id == snapshot.transfer_id &&
                                uploads_.front().stage == FileUpload::Stage::kReady) {
                                uploads_.front().stage = FileUpload::Stage::kOffered;
                            }
                        }
                        std::fprintf(stderr, "viewer: file %s offered (%llu bytes)\n",
                                     snapshot.name.c_str(),
                                     static_cast<unsigned long long>(snapshot.size));
                        return;
                    }
                    if (snapshot.stage == FileUpload::Stage::kOffered) {
                        chast::FileConflict conflict;
                        while (session->take_file_conflict(conflict)) {
                            if (conflict.transfer_id != snapshot.transfer_id) {
                                continue;
                            }
                            // The host found the destination name already in
                            // use and holds the offer for a decision. A
                            // remembered choice answers at once; otherwise the
                            // prompt waits for AnswerFileConflict().
                            int policy = 0;
                            bool already_asked = false;
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                policy = conflict_policy_;
                                for (const PendingFileConflict& pending : conflicts_) {
                                    if (!pending.download &&
                                        pending.transfer_id == conflict.transfer_id) {
                                        already_asked = true;
                                        break;
                                    }
                                }
                            }
                            if (policy == 0 && !already_asked) {
                                PendingFileConflict pending;
                                pending.download = false;
                                pending.transfer_id = conflict.transfer_id;
                                pending.name = snapshot.name;
                                pending.size = snapshot.size;
                                pending.identical = conflict.identical;
                                pending.existing_size = conflict.existing_size;
                                pending.existing_modified_time =
                                    conflict.existing_modified_time;
                                {
                                    std::lock_guard<std::mutex> lock(file_mutex_);
                                    conflicts_.push_back(std::move(pending));
                                }
                                ++file_conflicts;
                                std::fprintf(stderr,
                                             "viewer: file %s already exists on the host%s; "
                                             "waiting for the operator\n",
                                             snapshot.name.c_str(),
                                             conflict.identical ? " (identical)" : "");
                                return;
                            }
                            chast::FileConflictAnswer reply;
                            reply.transfer_id = conflict.transfer_id;
                            reply.decision = policy == 1
                                                 ? chast::FileConflictDecision::Overwrite
                                                 : chast::FileConflictDecision::Skip;
                            (void)session->send_file_conflict_answer(reply);
                            std::fprintf(stderr,
                                         "viewer: file %s already exists; %s (remembered)\n",
                                         snapshot.name.c_str(),
                                         policy == 1 ? "overwriting"
                                                     : "keeping the existing file");
                            return;
                        }
                        chast::FileAnswer answer;
                        while (session->take_file_answer(answer)) {
                            if (answer.transfer_id != snapshot.transfer_id) {
                                continue;
                            }
                            if (answer.code == chast::FileCode::Skipped) {
                                {
                                    std::lock_guard<std::mutex> lock(file_mutex_);
                                    if (!uploads_.empty() &&
                                        uploads_.front().transfer_id ==
                                            snapshot.transfer_id) {
                                        uploads_.front().stage = FileUpload::Stage::kDone;
                                        uploads_.front().skipped = true;
                                        uploads_.front().error =
                                            "the existing file was kept";
                                    }
                                }
                                std::fprintf(stderr,
                                             "viewer: file %s skipped; the host kept its file\n",
                                             snapshot.name.c_str());
                                return;
                            }
                            if (answer.code == chast::FileCode::Cancelled) {
                                purge_uploads(answer.reason.empty()
                                                  ? "the transfer was cancelled"
                                                  : answer.reason,
                                              true);
                                return;
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
                                if (uploads_.empty() ||
                                    uploads_.front().transfer_id !=
                                        snapshot.transfer_id ||
                                    uploads_.front().stage != FileUpload::Stage::kOffered) {
                                    return;
                                }
                                uploads_.front().stage = FileUpload::Stage::kSending;
                                uploads_.front().resume_offset = answer.resume_offset;
                                uploads_.front().sent = answer.resume_offset;
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
                        if (!uploads_.empty() &&
                            uploads_.front().transfer_id == snapshot.transfer_id &&
                            uploads_.front().stage == FileUpload::Stage::kSending) {
                            uploads_.front().sent = sent;
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
                            if (!uploads_.empty() &&
                                uploads_.front().transfer_id == snapshot.transfer_id &&
                                uploads_.front().stage == FileUpload::Stage::kSending) {
                                uploads_.front().stage = FileUpload::Stage::kFinishing;
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
                    uint64_t size = 0;
                    bool clipboard_transfer = false;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (download_.stage != FileDownload::Stage::kReceiving) {
                            return;
                        }
                        transfer_id = download_.transfer_id;
                        name = download_.name;
                        size = download_.size;
                        if (download_.receiver) {
                            download_.receiver->abort(true);
                            download_.receiver.reset();
                        }
                        download_.stage = FileDownload::Stage::kFailed;
                        download_.error = message;
                        clipboard_transfer =
                            clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                            clipboard_files.transfer_id == transfer_id;
                    }
                    if (!clipboard_transfer) {
                        record_completed_file(false, name, size, false, message);
                    }
                    if (report) {
                        (void)session->send_file_result(transfer_id, chast::FileCode::IoError, 0,
                                                        message);
                    }
                    ++file_receive_failures;
                    std::fprintf(stderr, "viewer: download %s failed: %s\n", name.c_str(),
                                 message.c_str());
                    if (clipboard_transfer) {
                        clip_files_fail("the download of '" + name + "' failed: " + message);
                    }
                };

                // CancelDownload() lands here. Only an ordinary single-file
                // download is stopped: a walk has CancelDirectoryDownload()
                // and clipboard transfers follow the clipboard permission.
                auto cancel_download = [&] {
                    uint32_t transfer_id = 0;
                    uint64_t received = 0;
                    std::string name;
                    uint64_t size = 0;
                    bool held = false;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        held = download_.stage == FileDownload::Stage::kConflict;
                        if (download_.stage != FileDownload::Stage::kReceiving && !held) {
                            return;
                        }
                        transfer_id = download_.transfer_id;
                        received = download_.received;
                        name = download_.name;
                        if (clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                            clipboard_files.transfer_id == transfer_id) {
                            return;
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(browse_mutex_);
                        if (folder_transfers_.find(transfer_id) != folder_transfers_.end()) {
                            return;
                        }
                    }
                    if (held) {
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (download_.stage != FileDownload::Stage::kConflict ||
                                download_.transfer_id != transfer_id) {
                                return;
                            }
                            size = download_.size;
                            download_.stage = FileDownload::Stage::kFailed;
                            download_.error = "the viewer cancelled the download";
                            // The prompt leaves with the transfer it names.
                            std::erase_if(conflicts_,
                                          [&](const PendingFileConflict& conflict) {
                                              return conflict.download &&
                                                     conflict.transfer_id == transfer_id;
                                          });
                        }
                        chast::FileConflictAnswer answer;
                        answer.transfer_id = transfer_id;
                        answer.decision = chast::FileConflictDecision::Cancel;
                        (void)session->send_file_conflict_answer(answer);
                        record_completed_file(false, name, size, false,
                                              "the viewer cancelled the download");
                        ++download_cancels;
                        std::fprintf(stderr, "viewer: download %s cancelled by the viewer\n",
                                     name.c_str());
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (download_.stage != FileDownload::Stage::kReceiving ||
                            download_.transfer_id != transfer_id) {
                            return;
                        }
                        if (download_.receiver) {
                            // A cancel throws the partial file away; only a
                            // failure keeps it for a resume.
                            download_.receiver->abort(false);
                            download_.receiver.reset();
                        }
                        download_.stage = FileDownload::Stage::kFailed;
                        download_.error = "the viewer cancelled the download";
                        size = download_.size;
                    }
                    record_completed_file(false, name, size, false,
                                          "the viewer cancelled the download");
                    ++download_cancels;
                    (void)session->send_file_result(transfer_id, chast::FileCode::Cancelled,
                                                    received,
                                                    "the viewer cancelled the download");
                    std::fprintf(stderr, "viewer: download %s cancelled by the viewer\n",
                                 name.c_str());
                };

                // The folder walk's shared state lives under browse_mutex_; the
                // page polls these copies through ViewerStatus.
                auto publish_folder_progress = [&] {
                    bool active = false;
                    std::string name;
                    std::string destination;
                    std::string current;
                    uint64_t total = 0;
                    uint64_t done = 0;
                    uint32_t roots = 0;
                    bool batch = false;
                    {
                        std::lock_guard<std::mutex> lock(browse_mutex_);
                        active = folder_.stage == FolderDownload::Stage::kListing ||
                                 folder_.stage == FolderDownload::Stage::kDownloading;
                        name = folder_.name;
                        destination = folder_.destination;
                        current = folder_.current;
                        total = folder_.files_total;
                        done = folder_.files_done;
                        roots = folder_.batch_total;
                        batch = folder_.batch;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.folder_download_active = active;
                    status_.folder_download_name = name;
                    status_.folder_download_destination = destination;
                    status_.folder_download_current = current;
                    status_.folder_download_files_total = total;
                    status_.folder_download_files_done = done;
                    status_.folder_download_roots = roots;
                    status_.folder_download_batch = batch;
                };

                // Ends the walk and publishes the outcome once. `code` is 0 for
                // a completed download, 1 for a failure and 2 for a cancel.
                auto finish_folder_download = [&](int code, const std::string& reason) {
                    bool first = false;
                    std::string name;
                    std::string destination;
                    uint64_t total = 0;
                    uint64_t done = 0;
                    uint32_t roots = 0;
                    bool batch = false;
                    std::string error;
                    {
                        std::lock_guard<std::mutex> lock(browse_mutex_);
                        if (folder_.stage != FolderDownload::Stage::kListing &&
                            folder_.stage != FolderDownload::Stage::kDownloading) {
                            return;
                        }
                        first = true;
                        folder_.stage = code == 0   ? FolderDownload::Stage::kDone
                                        : code == 2 ? FolderDownload::Stage::kCancelled
                                                    : FolderDownload::Stage::kFailed;
                        folder_.finished = true;
                        folder_.error = code == 0 ? std::string() : reason;
                        folder_.current.clear();
                        folder_.roots.clear();
                        folder_.directories.clear();
                        folder_.files.clear();
                        folder_.listing_id = 0;
                        folder_.download_id = 0;
                        folder_.transfer_id = 0;
                        folder_transfers_.clear();
                        name = folder_.name;
                        destination = folder_.destination;
                        total = folder_.files_total;
                        done = folder_.files_done;
                        roots = folder_.batch_total;
                        batch = folder_.batch;
                        error = folder_.error;
                    }
                    if (!first) {
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.folder_download_active = false;
                        status_.folder_download_name = name;
                        status_.folder_download_destination = destination;
                        status_.folder_download_current.clear();
                        status_.folder_download_files_total = total;
                        status_.folder_download_files_done = done;
                        status_.folder_download_roots = roots;
                        status_.folder_download_batch = batch;
                        ++status_.folder_download_answers;
                        status_.folder_download_code = code;
                        status_.folder_download_error = error;
                    }
                    std::fprintf(stderr, "viewer: folder download '%s' %s%s\n", name.c_str(),
                                 code == 0 ? "done" : code == 2 ? "cancelled" : "failed: ",
                                 code == 1 ? error.c_str() : "");
                };

                // Ends a download the host is holding at the same-name prompt.
                // `report` answers the host with Cancel so it stops holding the
                // offer; the walk that owns the transfer ends with it.
                auto fail_conflict_download = [&](const std::string& message, bool report,
                                                  bool cancelled = false) {
                    uint32_t transfer_id = 0;
                    std::string name;
                    uint64_t size = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (download_.stage != FileDownload::Stage::kConflict) {
                            return;
                        }
                        transfer_id = download_.transfer_id;
                        name = download_.name;
                        size = download_.size;
                        download_.stage = FileDownload::Stage::kFailed;
                        download_.error = message;
                    }
                    if (report) {
                        chast::FileConflictAnswer answer;
                        answer.transfer_id = transfer_id;
                        answer.decision = chast::FileConflictDecision::Cancel;
                        (void)session->send_file_conflict_answer(answer);
                    }
                    record_completed_file(false, name, size, false, message);
                    if (cancelled) {
                        ++download_cancels;
                    } else {
                        ++file_receive_failures;
                    }
                    std::fprintf(stderr, "viewer: download %s failed: %s\n", name.c_str(),
                                 message.c_str());
                    bool walk = false;
                    {
                        std::lock_guard<std::mutex> lock(browse_mutex_);
                        walk = folder_transfers_.erase(transfer_id) > 0;
                        if (walk && folder_.transfer_id == transfer_id) {
                            folder_.transfer_id = 0;
                            folder_.current.clear();
                        }
                    }
                    if (walk) {
                        finish_folder_download(cancelled ? 2 : 1, message);
                    }
                };

                // Applies the answers AnswerFileConflict() queued. A remembered
                // choice covers the rest of the queue; Cancel drops the upload
                // queue or ends the download it belongs to.
                auto drain_file_conflicts = [&] {
                    for (;;) {
                        ConflictAnswer answer;
                        PendingFileConflict pending;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (conflict_answers_.empty()) {
                                return;
                            }
                            answer = conflict_answers_.front();
                            conflict_answers_.pop_front();
                            auto it = std::find_if(conflicts_.begin(), conflicts_.end(),
                                                   [&](const PendingFileConflict& entry) {
                                                       return entry.transfer_id ==
                                                              answer.transfer_id;
                                                   });
                            if (it == conflicts_.end()) {
                                // The prompt left with its transfer.
                                continue;
                            }
                            pending = *it;
                            conflicts_.erase(it);
                            if (answer.remember &&
                                answer.decision != chast::FileConflictDecision::Cancel) {
                                conflict_policy_ =
                                    answer.decision == chast::FileConflictDecision::Overwrite
                                        ? 1
                                        : 2;
                            }
                        }
                        chast::FileConflictAnswer reply;
                        reply.transfer_id = pending.transfer_id;
                        reply.decision = answer.decision;
                        if (!pending.download) {
                            (void)session->send_file_conflict_answer(reply);
                            if (answer.decision == chast::FileConflictDecision::Cancel) {
                                // The head still waits in kOffered; dropping it
                                // also ends the rest of the queue.
                                purge_uploads("the operator cancelled the upload", true);
                            }
                            // Skip and Overwrite finish through the host's
                            // FileAnswer: Skipped ends the entry, Ok starts it.
                            continue;
                        }
                        if (answer.decision == chast::FileConflictDecision::Cancel) {
                            (void)session->send_file_conflict_answer(reply);
                            fail_conflict_download("the viewer cancelled the download", false,
                                                   true);
                            continue;
                        }
                        if (answer.decision == chast::FileConflictDecision::Skip) {
                            (void)session->send_file_conflict_answer(reply);
                            bool current = false;
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                if (download_.stage == FileDownload::Stage::kConflict &&
                                    download_.transfer_id == pending.transfer_id) {
                                    download_.stage = FileDownload::Stage::kDone;
                                    download_.error = "the existing file was kept";
                                    current = true;
                                }
                            }
                            if (current) {
                                // No audit record: the host writes none for a
                                // kept file either.
                                ++downloads_skipped;
                                std::fprintf(stderr,
                                             "viewer: download %s skipped; the existing "
                                             "file is kept\n",
                                             pending.name.c_str());
                            }
                            continue;
                        }
                        // Overwrite: reopen the destination over the existing
                        // file; the host restarts the stream from zero.
                        chast::FileReceiveOptions options = pending.options;
                        options.overwrite = true;
                        uint64_t resume_offset = 0;
                        std::string receive_error;
                        std::unique_ptr<chast::FileReceiver> receiver =
                            chast::FileReceiver::create(options, &resume_offset, &receive_error);
                        if (!receiver) {
                            reply.decision = chast::FileConflictDecision::Cancel;
                            (void)session->send_file_conflict_answer(reply);
                            fail_conflict_download(
                                "cannot replace '" + pending.name + "': " + receive_error,
                                false);
                            continue;
                        }
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            if (download_.stage != FileDownload::Stage::kConflict ||
                                download_.transfer_id != pending.transfer_id) {
                                receiver->abort(false);
                                continue;
                            }
                            download_.stage = FileDownload::Stage::kReceiving;
                            download_.receiver = std::move(receiver);
                            download_.received = resume_offset;
                            download_.answer_time = Clock::now();
                        }
                        (void)session->send_file_conflict_answer(reply);
                        std::fprintf(stderr,
                                     "viewer: download %s accepted over the existing file\n",
                                     pending.name.c_str());
                    }
                };

                auto drain_downloads = [&] {
                    bool transfer_receiving = false;
                    bool transfer_held = false;
                    bool clipboard_receiving = false;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        transfer_receiving =
                            download_.stage == FileDownload::Stage::kReceiving;
                        transfer_held = download_.stage == FileDownload::Stage::kConflict;
                        clipboard_receiving =
                            transfer_receiving &&
                            clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                            clipboard_files.transfer_id == download_.transfer_id;
                    }
                    if ((transfer_receiving || transfer_held) && !clipboard_receiving &&
                        (permissions & chast::kPermissionFileTransfer) == 0) {
                        if (transfer_held) {
                            // The host dropped the held offer with the grant.
                            fail_conflict_download("the host revoked file transfer", false);
                        } else {
                            download_fail("the host revoked file transfer", false);
                        }
                    }
                    if (transfer_receiving && clipboard_receiving && !clipboard_receive_allowed()) {
                        download_fail("clipboard sync is disabled", false);
                    }
                    chast::FileOffer offer;
                    while (session->take_file_offer(offer)) {
                        const bool clipboard_transfer =
                            clipboard_files.stage == ClipboardFiles::Stage::kOffering &&
                            clipboard_files.transfer_id == offer.transfer_id;
                        if (offer.clipboard != clipboard_transfer) {
                            (void)session->send_file_answer(offer.transfer_id, chast::FileCode::Refused, 0,
                                "clipboard offer does not match a requested clipboard file");
                            continue;
                        }
                        if ((permissions & chast::kPermissionFileTransfer) == 0 &&
                            !clipboard_transfer) {
                            continue;
                        }
                        if (clipboard_transfer && !clipboard_receive_allowed()) {
                            (void)session->send_file_answer(
                                offer.transfer_id, chast::FileCode::Busy, 0,
                                "clipboard sync is disabled");
                            clip_files_fail("clipboard sync was disabled before the transfer "
                                            "started");
                            continue;
                        }
                        bool ignored = false;
                        {
                            // A walk that was cancelled just as the host sent
                            // this offer already turned the transfer away.
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            ignored = ignored_transfers_.erase(offer.transfer_id) > 0;
                        }
                        if (ignored) {
                            (void)session->send_file_answer(
                                offer.transfer_id, chast::FileCode::Busy, 0,
                                "the folder download was cancelled");
                            continue;
                        }
                        bool busy = false;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            busy = download_.stage == FileDownload::Stage::kReceiving ||
                                   download_.stage == FileDownload::Stage::kConflict;
                        }
                        const bool no_directory = clipboard_transfer
                                                      ? clipboard_files.directory.empty()
                                                      : download_directory.empty();
                        if (busy || no_directory) {
                            const char* reason =
                                busy ? "another download is running"
                                     : "the viewer has no download directory";
                            (void)session->send_file_answer(offer.transfer_id,
                                                            chast::FileCode::Busy, 0, reason);
                            if (clipboard_transfer) {
                                if (no_directory) {
                                    clip_files_fail("the viewer has no staging directory");
                                } else {
                                    // Let the running download finish, then ask
                                    // the host for this entry again.
                                    clipboard_files.stage =
                                        ClipboardFiles::Stage::kRequesting;
                                    clipboard_files.request_id = 0;
                                    clipboard_files.transfer_id = 0;
                                    clipboard_files.retry_after =
                                        Clock::now() + std::chrono::milliseconds(500);
                                }
                                continue;
                            }
                            ++file_receive_failures;
                            // The walk's file was turned away; queue it again
                            // instead of ending the whole walk.
                            bool walk_transfer = false;
                            bool retry = false;
                            {
                                std::lock_guard<std::mutex> lock(browse_mutex_);
                                walk_transfer = folder_transfers_.erase(offer.transfer_id) > 0;
                                if (walk_transfer &&
                                    folder_.stage == FolderDownload::Stage::kDownloading) {
                                    folder_.transfer_id = 0;
                                    folder_.current.clear();
                                    if (busy) {
                                        folder_.files.push_front(std::make_pair(
                                            folder_.download_path, folder_.download_directory));
                                        folder_.retry_after =
                                            Clock::now() + std::chrono::milliseconds(500);
                                        folder_.download_path.clear();
                                        folder_.download_directory.clear();
                                        retry = true;
                                    }
                                }
                            }
                            if (retry) {
                                publish_folder_progress();
                            } else if (walk_transfer) {
                                finish_folder_download(1, reason);
                            }
                            if (!retry) {
                                record_completed_file(false, offer.name, offer.size, false,
                                                      reason);
                            }
                            continue;
                        }
                        chast::FileReceiveOptions options;
                        options.directory = clipboard_transfer ? clipboard_files.directory
                                                               : download_directory;
                        options.name = offer.name;
                        options.relative_path =
                            clipboard_transfer ? std::string() : offer.relative_path;
                        bool walk_offer = false;
                        if (clipboard_transfer) {
                            // The entry's rel_path holds the copied folder
                            // layout, so the file lands under the same
                            // subdirectories in the staging root.
                            if (clipboard_files.next < clipboard_files.entries.size()) {
                                const std::string& rel =
                                    clipboard_files.entries[clipboard_files.next].rel_path;
                                const size_t slash = rel.rfind('/');
                                if (slash != std::string::npos) {
                                    options.relative_path = rel.substr(0, slash);
                                }
                            }
                        } else {
                            // A walk's file lands in the mirrored tree, not
                            // in the receive root.
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            auto it = folder_transfers_.find(offer.transfer_id);
                            if (it != folder_transfers_.end()) {
                                walk_offer = true;
                                options.relative_path = it->second;
                            }
                        }
                        options.size = offer.size;
                        options.sha256 = offer.sha256;
                        options.allow_resume = offer.resume;
                        // Clipboard staging is a fresh directory per list, so
                        // a name there is never the operator's own file.
                        options.overwrite = clipboard_transfer;
                        auto refuse_offer = [&](const std::string& reason) {
                            ++file_receive_failures;
                            (void)session->send_file_answer(offer.transfer_id,
                                                            chast::FileCode::Refused, 0,
                                                            reason);
                            std::fprintf(stderr, "viewer: download %s refused: %s\n",
                                         offer.name.c_str(), reason.c_str());
                            if (clipboard_transfer) {
                                clip_files_fail("cannot receive '" + offer.name +
                                                "': " + reason);
                                return;
                            }
                            record_completed_file(false, offer.name, offer.size, false, reason);
                            bool walk_transfer = false;
                            {
                                std::lock_guard<std::mutex> lock(browse_mutex_);
                                walk_transfer = folder_transfers_.erase(offer.transfer_id) > 0;
                            }
                            if (walk_transfer) {
                                finish_folder_download(1, reason);
                            }
                        };
                        if (!clipboard_transfer) {
                            chast::FileReceiveTarget target;
                            std::string target_error;
                            if (!chast::inspect_receive_target(options, target, &target_error)) {
                                refuse_offer(target_error);
                                continue;
                            }
                            if (target.exists) {
                                if (target.directory) {
                                    refuse_offer("the destination is an existing directory");
                                    continue;
                                }
                                // A remembered choice answers without a
                                // prompt, the same way the upload direction
                                // does.
                                int policy = 0;
                                {
                                    std::lock_guard<std::mutex> lock(file_mutex_);
                                    policy = conflict_policy_;
                                }
                                if (policy != 0) {
                                    const chast::Error held = session->send_file_answer(
                                        offer.transfer_id, chast::FileCode::Conflict, 0);
                                    if (!held.ok()) {
                                        refuse_offer("cannot hold the offer: " +
                                                     held.to_string());
                                        continue;
                                    }
                                    chast::FileConflictAnswer answer;
                                    answer.transfer_id = offer.transfer_id;
                                    if (policy == 2) {
                                        answer.decision = chast::FileConflictDecision::Skip;
                                        (void)session->send_file_conflict_answer(answer);
                                        {
                                            std::lock_guard<std::mutex> lock(file_mutex_);
                                            download_ = FileDownload{};
                                            download_.stage = FileDownload::Stage::kDone;
                                            download_.transfer_id = offer.transfer_id;
                                            download_.name = offer.name;
                                            download_.size = offer.size;
                                            download_.error = "the existing file was kept";
                                            download_.answer_time = Clock::now();
                                        }
                                        ++downloads_skipped;
                                        std::fprintf(stderr,
                                                     "viewer: download %s skipped "
                                                     "(remembered)\n",
                                                     offer.name.c_str());
                                        continue;
                                    }
                                    // Overwrite: reopen the destination over
                                    // the existing file; the host restarts the
                                    // stream from zero.
                                    chast::FileReceiveOptions overwrite_options = options;
                                    overwrite_options.overwrite = true;
                                    uint64_t resume_offset = 0;
                                    std::string overwrite_error;
                                    std::unique_ptr<chast::FileReceiver> overwrite_receiver =
                                        chast::FileReceiver::create(overwrite_options,
                                                                    &resume_offset,
                                                                    &overwrite_error);
                                    if (!overwrite_receiver) {
                                        answer.decision = chast::FileConflictDecision::Cancel;
                                        (void)session->send_file_conflict_answer(answer);
                                        refuse_offer("cannot replace '" + offer.name +
                                                     "': " + overwrite_error);
                                        continue;
                                    }
                                    {
                                        std::lock_guard<std::mutex> lock(file_mutex_);
                                        download_ = FileDownload{};
                                        download_.stage = FileDownload::Stage::kReceiving;
                                        download_.transfer_id = offer.transfer_id;
                                        download_.name = overwrite_receiver->name();
                                        download_.size = offer.size;
                                        download_.received = resume_offset;
                                        download_.answer_time = Clock::now();
                                        download_.receiver = std::move(overwrite_receiver);
                                        download_.cancellable = !walk_offer;
                                    }
                                    answer.decision = chast::FileConflictDecision::Overwrite;
                                    (void)session->send_file_conflict_answer(answer);
                                    std::fprintf(stderr,
                                                 "viewer: download %s accepted over the "
                                                 "existing file (remembered)\n",
                                                 offer.name.c_str());
                                    continue;
                                }
                                // Hold the offer at the host and let the
                                // operator decide; the receiver is created
                                // only once an overwrite is chosen.
                                PendingFileConflict pending;
                                pending.download = true;
                                pending.transfer_id = offer.transfer_id;
                                pending.name = offer.name;
                                pending.size = offer.size;
                                // RustDesk calls a same-size, same-second file
                                // identical and still asks, with a hint.
                                const bool identical =
                                    offer.modified_time != 0 &&
                                    target.modified_time == offer.modified_time &&
                                    target.size == offer.size;
                                pending.identical = identical;
                                pending.existing_size = target.size;
                                pending.existing_modified_time = target.modified_time;
                                pending.options = options;
                                // An overwrite restarts at zero, so a stale
                                // .part must not be resumed into it.
                                pending.options.allow_resume = false;
                                pending.walk = walk_offer;
                                {
                                    std::lock_guard<std::mutex> lock(file_mutex_);
                                    download_ = FileDownload{};
                                    download_.stage = FileDownload::Stage::kConflict;
                                    download_.transfer_id = offer.transfer_id;
                                    download_.name = offer.name;
                                    download_.size = offer.size;
                                    download_.answer_time = Clock::now();
                                    download_.cancellable = !walk_offer;
                                    conflicts_.push_back(std::move(pending));
                                }
                                ++file_conflicts;
                                const chast::Error held = session->send_file_answer(
                                    offer.transfer_id, chast::FileCode::Conflict, 0);
                                if (!held.ok()) {
                                    fail_conflict_download(
                                        "cannot hold the offer: " + held.to_string(), false);
                                    continue;
                                }
                                std::fprintf(stderr,
                                             "viewer: download %s already exists locally%s; "
                                             "waiting for the operator\n",
                                             offer.name.c_str(),
                                             identical ? " (identical)" : "");
                                continue;
                            }
                        }
                        uint64_t resume_offset = 0;
                        std::string receive_error;
                        std::unique_ptr<chast::FileReceiver> receiver = chast::FileReceiver::create(
                            options, &resume_offset, &receive_error);
                        if (!receiver) {
                            refuse_offer(receive_error);
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
                            download_.cancellable = !clipboard_transfer && !walk_offer;
                            download_.clipboard = clipboard_transfer;
                        }
                        if (clipboard_transfer) {
                            clipboard_files.stage = ClipboardFiles::Stage::kReceiving;
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
                    const bool clipboard_transfer =
                        clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                        clipboard_files.transfer_id == transfer_id;
                    std::string completed_name;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        completed_name = download_.name;
                        download_.receiver.reset();
                        download_.stage = FileDownload::Stage::kDone;
                        download_.path = final_path;
                        download_.error.clear();
                    }
                    if (!clipboard_transfer) {
                        record_completed_file(false, completed_name, bytes, true, "");
                    }
                    if (clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                        clipboard_files.transfer_id == transfer_id &&
                        clipboard_files.next < clipboard_files.entries.size()) {
                        // CF_HDROP lists what the operator sees: a flat file
                        // by itself, a copied folder once for all of its files.
                        // The folder comes from where the file actually landed
                        // inside the staging root, not from the host's
                        // rel_path; the spelling only decides flat vs. tree.
                        const std::string& rel =
                            clipboard_files.entries[clipboard_files.next].rel_path;
                        const std::filesystem::path staging =
                            path_from_utf8(clipboard_files.directory);
                        const std::filesystem::path relative =
                            path_from_utf8(final_path).lexically_relative(staging);
                        bool inside = !relative.empty();
                        for (const std::filesystem::path& part : relative) {
                            if (part == "..") {
                                inside = false;
                                break;
                            }
                        }
                        if (!inside) {
                            clip_files_fail(
                                "the received file landed outside the clipboard staging "
                                "folder");
                        } else if (rel.find('/') == std::string::npos) {
                            clipboard_files.paths.push_back(final_path);
                        } else {
                            const std::string top_path =
                                utf8_from_path(staging / *relative.begin());
                            if (std::find(clipboard_files.paths.begin(),
                                          clipboard_files.paths.end(),
                                          top_path) == clipboard_files.paths.end()) {
                                clipboard_files.paths.push_back(top_path);
                            }
                        }
                        if (clipboard_files.stage == ClipboardFiles::Stage::kReceiving) {
                            ++clipboard_files.next;
                            clipboard_files.stage = ClipboardFiles::Stage::kRequesting;
                            clipboard_files.request_id = 0;
                            clipboard_files.transfer_id = 0;
                            clipboard_files.retry_after = Clock::now();
                        }
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
                        bool held = false;
                        {
                            std::lock_guard<std::mutex> lock(file_mutex_);
                            held = download_.stage == FileDownload::Stage::kConflict;
                            matches = (download_.stage == FileDownload::Stage::kReceiving ||
                                       held) &&
                                      download_.transfer_id == cancel.transfer_id;
                        }
                        if (!matches) {
                            continue;
                        }
                        const std::string reason = cancel.reason.empty()
                                                       ? "the host cancelled the download"
                                                       : cancel.reason;
                        if (held) {
                            fail_conflict_download(reason, false);
                        } else {
                            download_fail(reason, false);
                        }
                    }
                };

                // Sends queued browse requests in the order the operator made
                // them. A revoked permission drops them: the host would refuse
                // and the page is about to hide the browser anyway.
                auto drain_file_browse = [&] {
                    for (;;) {
                        if ((permissions & chast::kPermissionFileTransfer) == 0) {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            pending_browse_.clear();
                            browse_paths_.clear();
                            latest_list_request_.reset();
                            newest_file_list_.reset();
                            return;
                        }
                        BrowseCommand command;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            if (pending_browse_.empty()) {
                                return;
                            }
                            command = std::move(pending_browse_.front());
                            pending_browse_.pop_front();
                            if (command.kind == BrowseCommand::Kind::kList &&
                                (!latest_list_request_ || latest_list_request_->request_id != command.request_id)) continue;
                        }
                        chast::Error sent =
                            command.kind == BrowseCommand::Kind::kDownload
                                ? session->request_file_download(command.request_id, command.path)
                            : command.kind == BrowseCommand::Kind::kOperation
                                ? session->request_file_op(command.request_id, command.operation,
                                                           command.path, command.new_name)
                                : session->request_file_list(command.request_id, command.path,
                                                             command.cursor, command.show_hidden);
                        if (sent.ok()) {
                            continue;
                        }
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            browse_paths_.erase(command.request_id);
                            if (command.kind == BrowseCommand::Kind::kList) {
                                if (!latest_list_request_ || latest_list_request_->request_id != command.request_id) continue;
                                newest_file_list_ = FileListAnswer{};
                                newest_file_list_->directory = command.path;
                                newest_file_list_->cursor = command.cursor;
                                newest_file_list_->show_hidden = command.show_hidden;
                                newest_file_list_->result.code = chast::FileCode::IoError;
                                newest_file_list_->result.reason = "request: " + sent.to_string();
                                latest_list_request_.reset();
                            }
                        }
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (command.kind == BrowseCommand::Kind::kDownload) {
                            ++status_.file_download_answers;
                            status_.file_download_code = static_cast<int>(chast::FileCode::IoError);
                            status_.file_download_path = command.path;
                            status_.file_download_error = "request: " + sent.to_string();
                        } else if (command.kind == BrowseCommand::Kind::kOperation) {
                            ++status_.file_op_answers;
                            status_.file_op_code = static_cast<int>(chast::FileCode::IoError);
                            status_.file_op_path = command.path;
                            status_.file_op_error = "request: " + sent.to_string();
                        } else {
                            ++status_.file_list_answers;
                            status_.file_list_directory = command.path;
                            status_.file_list_entries = 0;
                            status_.file_list_truncated = false;
                            status_.file_list_error = "request: " + sent.to_string();
                        }
                        std::fprintf(stderr, "viewer: file browse request: %s\n",
                                     sent.to_string().c_str());
                    }
                };

                // Adopts every browse answer. The newest listing is kept for
                // TakeFileList(); a refusal replaces it too, because the page
                // asked about this directory and must not see an older one.
                auto receive_file_browse = [&] {
                    chast::FileListResult list;
                    while (session->take_file_list_result(list)) {
                        bool walk = false;
                        std::string walk_error;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            if (list.request_id != 0 && list.request_id == folder_.listing_id &&
                                folder_.stage == FolderDownload::Stage::kListing) {
                                walk = true;
                                const std::string listed = folder_.listing_path;
                                const std::string cursor = folder_.listing_cursor;
                                folder_.listing_id = 0;
                                folder_.listing_path.clear();
                                folder_.listing_cursor.clear();
                                if (list.code != chast::FileCode::Ok) {
                                    walk_error = list.reason.empty() ? file_code_name(list.code)
                                                                     : list.reason;
                                } else if (!cursor.empty() && !list.entries.empty() &&
                                    chast::file_list_cursor(list.entries.front()) <= cursor) {
                                    walk_error = "directory continuation did not advance";
                                } else {
                                    std::string local_sub;
                                    if (listed.size() > folder_.root.size() + 1) {
                                        local_sub = listed.substr(folder_.root.size() + 1);
                                    }
                                    // Mirror the folder itself, so folders
                                    // that hold no files still appear.
                                    std::string local_dir = folder_.name;
                                    if (!local_sub.empty()) {
                                        local_dir += "/" + local_sub;
                                    }
                                    std::string sanitized;
                                    if (!chast::sanitize_received_directory(local_dir,
                                                                             sanitized)) {
                                        walk_error =
                                            "the folder path is too deep to save locally";
                                    } else {
                                        std::error_code ec;
                                        std::filesystem::create_directories(
                                            path_from_utf8(download_directory) /
                                                path_from_utf8(sanitized),
                                            ec);
                                        if (ec) {
                                            walk_error = "cannot create '" + sanitized +
                                                         "': " + ec.message();
                                        }
                                    }
                                    if (walk_error.empty()) {
                                        for (const chast::FileListEntry& entry : list.entries) {
                                            const std::string child =
                                                listed + "/" + entry.name;
                                            if (++folder_.walk_entries >
                                                kMaxFolderWalkEntries) {
                                                walk_error =
                                                    "the folder has too many entries to "
                                                    "download";
                                                break;
                                            }
                                            if (entry.directory) {
                                                const size_t depth = static_cast<size_t>(
                                                    std::count(child.begin(), child.end(), '/'));
                                                const size_t root_depth = static_cast<size_t>(
                                                    std::count(folder_.root.begin(),
                                                               folder_.root.end(), '/'));
                                                if (depth - root_depth > kMaxFolderWalkDepth) {
                                                    walk_error =
                                                        "the folder is nested too deeply to "
                                                        "download";
                                                    break;
                                                }
                                                folder_.directories.emplace_back(child, std::string{});
                                            } else {
                                                std::string local = folder_.name;
                                                if (!local_sub.empty()) {
                                                    local += "/" + local_sub;
                                                }
                                                folder_.files.emplace_back(child, local);
                                                ++folder_.files_total;
                                            }
                                        }
                                        if (walk_error.empty() && list.truncated)
                                            folder_.directories.emplace_front(listed, list.next_cursor);
                                    }
                                }
                            }
                        }
                        if (walk) {
                            if (!walk_error.empty()) {
                                finish_folder_download(1, walk_error);
                            } else {
                                publish_folder_progress();
                            }
                            list = chast::FileListResult{};
                            continue;
                        }
                        std::string error;
                        uint32_t entries = 0;
                        bool truncated = false;
                        std::string directory;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            // Late replies from a previous folder, refresh or
                            // cancelled walk must never replace the current page.
                            if (!latest_list_request_ || list.request_id != latest_list_request_->request_id) continue;
                            directory = latest_list_request_->directory;
                            newest_file_list_ = FileListAnswer{};
                            newest_file_list_->directory = directory;
                            newest_file_list_->cursor = latest_list_request_->cursor;
                            newest_file_list_->show_hidden = latest_list_request_->show_hidden;
                            if (!latest_list_request_->cursor.empty() && !list.entries.empty() &&
                                chast::file_list_cursor(list.entries.front()) <= latest_list_request_->cursor) {
                                list.entries.clear(); list.truncated = false; list.next_cursor.clear();
                                list.code = chast::FileCode::IoError;
                                list.reason = "directory continuation did not advance";
                            }
                            latest_list_request_.reset();
                            error = list.code == chast::FileCode::Ok ? std::string() :
                                (list.reason.empty() ? file_code_name(list.code) : list.reason);
                            entries = static_cast<uint32_t>(list.entries.size());
                            truncated = list.truncated;
                            newest_file_list_->result = std::move(list);
                            list = chast::FileListResult{};
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            ++status_.file_list_answers;
                            status_.file_list_directory = directory;
                            status_.file_list_entries = entries;
                            status_.file_list_truncated = truncated;
                            status_.file_list_error = error;
                        }
                        std::fprintf(stderr, "viewer: listing '%s' has %u entries%s%s\n",
                                     directory.c_str(), entries,
                                     truncated ? " (truncated)" : "",
                                     error.empty() ? "" : error.c_str());
                    }
                    chast::FileDownloadResult answer;
                    while (session->take_file_download_result(answer)) {
                        if (answer.request_id != 0 &&
                            clipboard_files.stage == ClipboardFiles::Stage::kRequesting &&
                            answer.request_id == clipboard_files.request_id) {
                            clipboard_files.request_id = 0;
                            if (answer.code == chast::FileCode::Ok &&
                                answer.transfer_id != 0) {
                                clipboard_files.transfer_id = answer.transfer_id;
                                clipboard_files.stage = ClipboardFiles::Stage::kOffering;
                            } else if (answer.code == chast::FileCode::Busy) {
                                // The host is sending something else; ask again
                                // shortly through step_clipboard_files.
                                clipboard_files.retry_after =
                                    Clock::now() + std::chrono::milliseconds(500);
                            } else {
                                const std::string reason =
                                    answer.reason.empty() ? file_code_name(answer.code)
                                                          : answer.reason;
                                clip_files_fail(
                                    "the host refused the download of '" +
                                    clipboard_files.entries[clipboard_files.next].name +
                                    "': " + reason);
                            }
                            continue;
                        }
                        bool walk = false;
                        bool walk_cancelled = false;
                        std::string walk_error;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            if (answer.request_id != 0 &&
                                answer.request_id == folder_.download_id &&
                                folder_.stage == FolderDownload::Stage::kDownloading) {
                                walk = true;
                                walk_cancelled = folder_.cancel;
                                folder_.download_id = 0;
                                if (answer.code == chast::FileCode::Ok) {
                                    folder_.transfer_id = answer.transfer_id;
                                    folder_transfers_[answer.transfer_id] =
                                        folder_.download_directory;
                                    folder_.transfer_deadline =
                                        Clock::now() + std::chrono::seconds(10);
                                } else if (answer.code == chast::FileCode::Busy) {
                                    folder_.files.push_front(std::make_pair(
                                        folder_.download_path, folder_.download_directory));
                                    folder_.download_path.clear();
                                    folder_.download_directory.clear();
                                    folder_.current.clear();
                                    folder_.retry_after =
                                        Clock::now() + std::chrono::milliseconds(500);
                                } else {
                                    walk_error = answer.reason.empty()
                                                     ? file_code_name(answer.code)
                                                     : answer.reason;
                                }
                            }
                        }
                        if (walk) {
                            if (!walk_error.empty()) {
                                finish_folder_download(walk_cancelled ? 2 : 1, walk_error);
                            } else {
                                publish_folder_progress();
                            }
                            continue;
                        }
                        std::string path;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            auto it = browse_paths_.find(answer.request_id);
                            if (it != browse_paths_.end()) {
                                path = it->second;
                                browse_paths_.erase(it);
                            }
                        }
                        const std::string error =
                            answer.code == chast::FileCode::Ok
                                ? std::string()
                                : (answer.reason.empty() ? file_code_name(answer.code)
                                                         : answer.reason);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            ++status_.file_download_answers;
                            status_.file_download_code = static_cast<int>(answer.code);
                            status_.file_download_transfer_id = answer.transfer_id;
                            status_.file_download_path = path;
                            status_.file_download_error = error;
                        }
                        std::fprintf(stderr, "viewer: download '%s' %s%s\n", path.c_str(),
                                     error.empty() ? "accepted" : "refused: ", error.c_str());
                    }
                    chast::FileOpResult op_answer;
                    while (session->take_file_op_result(op_answer)) {
                        std::string path;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            auto it = browse_paths_.find(op_answer.request_id);
                            if (it != browse_paths_.end()) {
                                path = it->second;
                                browse_paths_.erase(it);
                            }
                        }
                        const std::string error =
                            op_answer.code == chast::FileCode::Ok
                                ? std::string()
                                : (op_answer.reason.empty() ? file_code_name(op_answer.code)
                                                           : op_answer.reason);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            ++status_.file_op_answers;
                            status_.file_op_code = static_cast<int>(op_answer.code);
                            status_.file_op_path = path;
                            status_.file_op_error = error;
                        }
                        std::fprintf(stderr, "viewer: file operation '%s' %s%s\n", path.c_str(),
                                     error.empty() ? "applied" : "refused: ", error.c_str());
                    }
                };

                // Starts the next selected entry under browse_mutex_. A folder
                // begins by listing itself; a file is queued for its download
                // with an empty local folder, so it lands at the top of the
                // receive directory. Returns false when no roots are left.
                auto begin_next_root = [](FolderDownload& walk, Clock::time_point now) {
                    if (walk.roots.empty()) {
                        return false;
                    }
                    DownloadRoot root = std::move(walk.roots.front());
                    walk.roots.pop_front();
                    walk.root = std::move(root.path);
                    const size_t slash = walk.root.find_last_of('/');
                    walk.name =
                        slash == std::string::npos ? walk.root : walk.root.substr(slash + 1);
                    walk.listing_id = 0;
                    walk.listing_path.clear();
                    walk.listing_cursor.clear();
                    walk.download_id = 0;
                    walk.download_path.clear();
                    walk.download_directory.clear();
                    walk.transfer_id = 0;
                    walk.current.clear();
                    walk.walk_entries = 0;
                    walk.retry_after = now;
                    walk.transfer_deadline = now;
                    if (root.directory) {
                        walk.stage = FolderDownload::Stage::kListing;
                        walk.directories.emplace_back(walk.root, std::string{});
                    } else {
                        walk.stage = FolderDownload::Stage::kDownloading;
                        walk.files.emplace_back(walk.root, std::string());
                        ++walk.files_total;
                    }
                    return true;
                };

                // Advances the folder walk once per poll: starts a queued
                // download, lists the next folder, sends the next file and
                // observes the transfer in flight. Everything the walk sends
                // uses its own request ids, so the page's listing is untouched.
                auto step_folder_download = [&] {
                    const auto now = Clock::now();
                    {
                        bool active = false;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            active = folder_.stage == FolderDownload::Stage::kListing ||
                                     folder_.stage == FolderDownload::Stage::kDownloading;
                        }
                        if (active && (permissions & chast::kPermissionFileTransfer) == 0) {
                            finish_folder_download(1, "the host revoked file transfer");
                            return;
                        }
                    }

                    {
                        std::vector<DownloadRoot> roots;
                        bool batch = false;
                        bool show_hidden = false;
                        bool start = false;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            if (folder_request_ &&
                                folder_.stage != FolderDownload::Stage::kListing &&
                                folder_.stage != FolderDownload::Stage::kDownloading) {
                                roots = std::move(folder_request_->roots);
                                batch = folder_request_->batch;
                                show_hidden = folder_request_->show_hidden;
                                folder_request_.reset();
                                start = true;
                            }
                        }
                        if (start) {
                            bool no_directory = false;
                            bool begun = false;
                            std::string destination;
                            std::string started;
                            uint32_t selected = 0;
                            {
                                std::lock_guard<std::mutex> lock(browse_mutex_);
                                folder_ = FolderDownload{};
                                folder_.batch_total = static_cast<uint32_t>(roots.size());
                                folder_.batch = batch;
                                folder_.show_hidden = show_hidden;
                                folder_.roots.assign(std::make_move_iterator(roots.begin()),
                                                     std::make_move_iterator(roots.end()));
                                folder_.retry_after = now;
                                folder_.transfer_deadline = now;
                                no_directory = download_directory.empty();
                                begun = begin_next_root(folder_, now);
                                selected = folder_.batch_total;
                                started = folder_.root;
                                if (!no_directory && begun) {
                                    // One folder keeps the "below the folder's
                                    // name" destination; a batch and a lone
                                    // file land in the receive root.
                                    folder_.destination = download_directory;
                                    if (folder_.batch_total == 1 &&
                                        folder_.stage == FolderDownload::Stage::kListing) {
                                        folder_.destination += "\\" + folder_.name;
                                    }
                                    destination = folder_.destination;
                                }
                            }
                            if (no_directory) {
                                finish_folder_download(1, "the viewer has no download directory");
                            } else if (!begun) {
                                {
                                    std::lock_guard<std::mutex> lock(browse_mutex_);
                                    folder_.stage = FolderDownload::Stage::kDownloading;
                                }
                                finish_folder_download(1, "the download has no entries");
                            } else {
                                publish_folder_progress();
                                if (batch) {
                                    std::fprintf(
                                        stderr,
                                        "viewer: downloading %u selected %s to '%s'\n", selected,
                                        selected == 1 ? "entry" : "entries", destination.c_str());
                                } else {
                                    std::fprintf(stderr,
                                                 "viewer: downloading folder '%s' to '%s'\n",
                                                 started.c_str(), destination.c_str());
                                }
                            }
                        }
                    }

                    // The file the walk sent is complete once the transfer
                    // it allocated reports done.
                    {
                        uint32_t transfer = 0;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            transfer = folder_.transfer_id;
                        }
                        if (transfer != 0) {
                            uint32_t active = 0;
                            FileDownload::Stage stage = FileDownload::Stage::kNone;
                            std::string error;
                            {
                                std::lock_guard<std::mutex> lock(file_mutex_);
                                active = download_.transfer_id;
                                stage = download_.stage;
                                error = download_.error;
                            }
                            if (active == transfer && stage == FileDownload::Stage::kDone) {
                                {
                                    std::lock_guard<std::mutex> lock(browse_mutex_);
                                    if (folder_.transfer_id == transfer) {
                                        folder_.transfer_id = 0;
                                        folder_.current.clear();
                                        folder_transfers_.erase(transfer);
                                        ++folder_.files_done;
                                    }
                                }
                                publish_folder_progress();
                            } else if (active == transfer &&
                                       stage == FileDownload::Stage::kFailed) {
                                finish_folder_download(
                                    1, error.empty() ? "the download failed" : error);
                                return;
                            } else {
                                bool expired = false;
                                {
                                    std::lock_guard<std::mutex> lock(browse_mutex_);
                                    if (folder_.transfer_id == transfer) {
                                        if (active == transfer &&
                                            (stage == FileDownload::Stage::kReceiving ||
                                             stage == FileDownload::Stage::kConflict)) {
                                            // The prompt may stay open as long
                                            // as the operator takes.
                                            folder_.transfer_deadline =
                                                now + std::chrono::seconds(60);
                                        } else if (now > folder_.transfer_deadline) {
                                            expired = true;
                                        }
                                    }
                                }
                                if (expired) {
                                    finish_folder_download(
                                        1, "the host never opened the file stream");
                                    return;
                                }
                            }
                        }
                    }

                    // A cancel aborts the file in flight and ends the walk;
                    // answers for requests still in the queues are ignored
                    // because the stage is terminal. A download request the
                    // host has not answered yet is awaited first, so the
                    // transfer it names can be turned away before the offer
                    // opens a stream.
                    {
                        bool cancel = false;
                        uint32_t transfer = 0;
                        uint32_t outstanding = 0;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            cancel = folder_.cancel &&
                                     (folder_.stage == FolderDownload::Stage::kListing ||
                                      folder_.stage == FolderDownload::Stage::kDownloading);
                            transfer = folder_.transfer_id;
                            outstanding = folder_.download_id;
                        }
                        if (cancel && outstanding != 0) {
                            return;
                        }
                        if (cancel) {
                            if (transfer != 0) {
                                bool active = false;
                                bool held = false;
                                std::string cancelled_name;
                                uint64_t cancelled_size = 0;
                                {
                                    std::lock_guard<std::mutex> lock(file_mutex_);
                                    held = download_.stage == FileDownload::Stage::kConflict &&
                                           download_.transfer_id == transfer;
                                    active = download_.stage == FileDownload::Stage::kReceiving &&
                                             download_.transfer_id == transfer;
                                    if (held) {
                                        cancelled_name = download_.name;
                                        cancelled_size = download_.size;
                                    }
                                    if (active) {
                                        cancelled_name = download_.name;
                                        cancelled_size = download_.size;
                                        if (download_.receiver) {
                                            download_.receiver->abort(false);
                                            download_.receiver.reset();
                                        }
                                        download_.stage = FileDownload::Stage::kFailed;
                                        download_.error = "cancelled by the operator";
                                    }
                                }
                                if (held) {
                                    // The host holds this offer; only the
                                    // conflict answer releases it.
                                    fail_conflict_download("cancelled by the operator", true,
                                                           true);
                                    return;
                                }
                                if (active) {
                                    record_completed_file(false, cancelled_name, cancelled_size,
                                                          false, "cancelled by the operator");
                                    (void)session->send_file_result(
                                        transfer, chast::FileCode::IoError, 0,
                                        "cancelled by the operator");
                                } else {
                                    // The answer named the transfer but the
                                    // offer has not opened it yet; refuse it
                                    // now, and refuse the offer as well should
                                    // it already be in flight.
                                    {
                                        std::lock_guard<std::mutex> lock(browse_mutex_);
                                        ignored_transfers_.insert(transfer);
                                    }
                                    (void)session->send_file_answer(
                                        transfer, chast::FileCode::Busy, 0,
                                        "the folder download was cancelled");
                                }
                            }
                            finish_folder_download(2, "cancelled by the operator");
                            return;
                        }
                    }

                    FolderDownload::Stage stage = FolderDownload::Stage::kIdle;
                    {
                        std::lock_guard<std::mutex> lock(browse_mutex_);
                        stage = folder_.stage;
                    }
                    if (stage == FolderDownload::Stage::kListing) {
                        uint32_t request_id = 0;
                        std::string path;
                        std::string cursor;
                        bool show_hidden = false;
                        bool listed_all = false;
                        bool advanced = false;
                        std::string timed_out;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            show_hidden = folder_.show_hidden;
                            if (folder_.listing_id != 0) {
                                if (now > folder_.transfer_deadline) {
                                    timed_out = folder_.listing_path;
                                } else {
                                    return;
                                }
                            } else if (folder_.directories.empty()) {
                                if (folder_.files.empty() && begin_next_root(folder_, now)) {
                                    advanced = true;
                                } else {
                                    folder_.stage = FolderDownload::Stage::kDownloading;
                                    listed_all = true;
                                }
                            } else {
                                path = folder_.directories.front().first;
                                cursor = folder_.directories.front().second;
                                folder_.directories.pop_front();
                                request_id = next_browse_id_++;
                                folder_.listing_id = request_id;
                                folder_.listing_path = path;
                                folder_.listing_cursor = cursor;
                                folder_.transfer_deadline = now + std::chrono::seconds(10);
                            }
                        }
                        if (!timed_out.empty()) {
                            finish_folder_download(1, "listing '" + timed_out + "' timed out");
                            return;
                        }
                        if (advanced) {
                            publish_folder_progress();
                            return;
                        }
                        if (listed_all) {
                            publish_folder_progress();
                            return;
                        }
                        chast::Error sent =
                            session->request_file_list(request_id, path, cursor, show_hidden);
                        if (!sent.ok()) {
                            finish_folder_download(1, "listing '" + path + "': " + sent.to_string());
                        }
                        return;
                    }
                    if (stage != FolderDownload::Stage::kDownloading) {
                        return;
                    }
                    uint32_t request_id = 0;
                    std::string path;
                    bool start_file = false;
                    bool finished = false;
                    bool advanced = false;
                    std::string timed_out;
                    bool timed_out_cancelled = false;
                    {
                        std::lock_guard<std::mutex> lock(browse_mutex_);
                        if (folder_.stage != FolderDownload::Stage::kDownloading) {
                            return;
                        }
                        if (folder_.download_id != 0) {
                            if (now > folder_.transfer_deadline) {
                                timed_out = folder_.download_path;
                                timed_out_cancelled = folder_.cancel;
                            } else {
                                return;
                            }
                        } else if (folder_.transfer_id != 0) {
                            return;
                        } else if (folder_.files.empty()) {
                            if (begin_next_root(folder_, now)) {
                                advanced = true;
                            } else {
                                finished = true;
                            }
                        } else if (now >= folder_.retry_after) {
                            bool busy = false;
                            {
                                std::lock_guard<std::mutex> file_lock(file_mutex_);
                                busy = download_.stage == FileDownload::Stage::kReceiving ||
                                       download_.stage == FileDownload::Stage::kConflict;
                            }
                            if (!busy) {
                                path = folder_.files.front().first;
                                const std::string local = folder_.files.front().second;
                                folder_.files.pop_front();
                                request_id = next_browse_id_++;
                                folder_.download_id = request_id;
                                folder_.download_path = path;
                                folder_.download_directory = local;
                                folder_.current = path;
                                folder_.transfer_deadline =
                                    now + std::chrono::seconds(10);
                                start_file = true;
                            }
                        }
                    }
                    if (!timed_out.empty()) {
                        finish_folder_download(
                            timed_out_cancelled ? 2 : 1,
                            "the host did not answer the download of '" + timed_out + "'");
                        return;
                    }
                    if (advanced) {
                        publish_folder_progress();
                        return;
                    }
                    if (finished) {
                        finish_folder_download(0, {});
                        return;
                    }
                    if (!start_file) {
                        return;
                    }
                    chast::Error sent = session->request_file_download(request_id, path);
                    if (!sent.ok()) {
                        finish_folder_download(1, "download '" + path + "': " + sent.to_string());
                        return;
                    }
                    publish_folder_progress();
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
                        if (pending_cursor_) chast::coalesce_cursor_update(*pending_cursor_, std::move(update));
                        else pending_cursor_ = std::move(update);
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
                        if (!have_audio_seq) {
                            have_audio_seq = true;
                            expected_audio_seq = audio_header.seq;
                        }
                        if (audio_header.seq < expected_audio_seq) {
                            // Too late to play; a newer packet already
                            // covered this slot, with concealment if needed.
                            continue;
                        }
                        if (audio_header.seq > expected_audio_seq && audio_player &&
                            audio_decoder) {
                            const uint32_t missing = static_cast<uint32_t>(
                                audio_header.seq - expected_audio_seq);
                            const uint32_t fill = std::min<uint32_t>(missing, 5);
                            for (uint32_t back = fill; back > 0; --back) {
                                std::vector<chast::DecodedAudio> concealed_blocks;
                                chast::Error concealed = audio_decoder->conceal(
                                    audio_header, back, concealed_blocks);
                                if (!concealed.ok()) {
                                    break;
                                }
                                for (const chast::DecodedAudio& block : concealed_blocks) {
                                    chast::Error pushed = audio_player->push(block);
                                    if (!pushed.ok()) {
                                        ++audio_errors;
                                        break;
                                    }
                                    if (!audio_muted_applied && block.channels >= 1 &&
                                        block.channels <= 2 && !block.samples.empty()) {
                                        audio_reference->push(
                                            block.samples.data(),
                                            block.samples.size() / block.channels,
                                            block.channels);
                                    }
                                }
                            }
                        }
                        expected_audio_seq = audio_header.seq + 1;
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
                            // Feed the same PCM the player renders to the echo
                            // reference. Muted output is silence and must not
                            // train the canceller.
                            if (!audio_muted_applied && block.channels >= 1 &&
                                block.channels <= 2 && !block.samples.empty()) {
                                audio_reference->push(
                                    block.samples.data(),
                                    block.samples.size() / block.channels,
                                    block.channels);
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
                            audio_reference->set_active(!muted);
                        }
                        // The picker may have moved the speaker. Rebuild only
                        // the player: the decoder, the timeline and the last
                        // clock offset stay, and a failed id falls back to the
                        // default instead of ending audio.
                        std::string wanted_output;
                        {
                            std::lock_guard<std::mutex> lock(audio_output_mutex_);
                            wanted_output = audio_output_device_id_;
                        }
                        if (wanted_output != audio_output_requested) {
                            audio_output_requested = wanted_output;
                            audio_player->stop();
                            audio_player.reset();
                            audio_reference->set_active(false);
                            std::string note;
                            if (open_audio_player(wanted_output, &note)) {
                                audio_enabled = true;
                                audio_error = note;
                                std::fprintf(stderr,
                                             "viewer: audio output is now %s\n",
                                             audio_device_name.empty()
                                                 ? "the system default"
                                                 : audio_device_name.c_str());
                            } else {
                                audio_enabled = false;
                                audio_device = wanted_output;
                                audio_device_name.clear();
                                audio_error = "playback: " + note;
                            }
                            if (!note.empty()) {
                                std::fprintf(stderr, "viewer: audio %s\n", note.c_str());
                            }
                        } else {
                            const std::string player_error = audio_player->error();
                            if (!player_error.empty()) {
                                audio_error = "playback: " + player_error;
                                std::fprintf(stderr, "viewer: audio stopped: %s\n",
                                             player_error.c_str());
                                stop_audio_pipeline();
                            }
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
                    const bool wanted_aec =
                        microphone_aec_.load(std::memory_order_relaxed);
                    const bool wanted_ns =
                        microphone_noise_.load(std::memory_order_relaxed);
                    if (microphone_capture &&
                        (wanted_device != microphone_device ||
                         wanted_aec != microphone_processing_aec ||
                         wanted_ns != microphone_processing_ns)) {
                        // The picker chose another endpoint or the processing
                        // switches moved: close this stream before opening
                        // the next one (the CMIC format stays the same).
                        stop_microphone({}, false);
                    }
                    if (!microphone_capture) {
                        chast::MicrophoneCaptureConfig config;
                        config.device_id = wanted_device;
                        config.reference = wanted_aec ? audio_reference : nullptr;
                        config.echo_cancellation = wanted_aec;
                        config.noise_suppression = wanted_ns;
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
                        microphone_processing_aec = wanted_aec;
                        microphone_processing_ns = wanted_ns;
                        microphone_capture->start();
                        microphone_enabled = true;
                        microphone_error.clear();
                        std::fprintf(stderr,
                                     "viewer: microphone %s %u Hz %u ch %u kbps, "
                                     "sending%s%s\n",
                                     audio_codec_name(microphone_info.codec),
                                     microphone_info.sample_rate, microphone_info.channels,
                                     microphone_info.bitrate / 1000u,
                                     microphone_processing_aec ? ", echo cancellation on" : "",
                                     microphone_processing_ns ? ", noise suppression on" : "");
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
                    microphone_aec_frames =
                        microphone_aec_base +
                        (microphone_processing_aec ? stats.processed_frames : 0);
                    microphone_ns_frames =
                        microphone_ns_base +
                        (microphone_processing_ns ? stats.processed_frames : 0);
                };

                // Adopts every PermissionUpdate the host sent. The transport
                // enforces the same mask on its side; this keeps the UI's
                // gates and the queues consistent with it immediately.
                auto receive_permissions = [&] {
                    chast::PermissionMask updated = 0;
                    while (session->take_permission_update(updated)) {
                        apply_permissions(updated);
                        std::fprintf(stderr, "viewer: permissions are now 0x%08x\n",
                                     updated);
                    }
                };

                // Sends the state the operator picked last. A request that
                // fails locally (permission revoked between the queue and the
                // send) is reported like a refusal, so the switch returns to
                // what the host actually has.
                // Sends the newest display-management request and adopts every
                // answer. A local send failure is reported like a host refusal,
                // so the toolbar's busy state can never stick.
                auto poll_display_control = [&] {
                    std::optional<chast::DisplayControlRequest> requested;
                    {
                        std::lock_guard<std::mutex> lock(privacy_mutex_);
                        requested.swap(pending_display_control_);
                    }
                    if (files_only) {
                        return;
                    }
                    bool updated = false;
                    if (requested) {
                        const chast::Error sent =
                            session->request_display_control(*requested);
                        if (!sent.ok()) {
                            display_control.code = chast::DisplayControlCode::Failed;
                            display_control.reason = sent.to_string();
                            ++display_control_updates;
                            updated = true;
                            std::fprintf(stderr, "viewer: display control: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                    chast::DisplayControlState answer;
                    while (session->take_display_control_state(answer)) {
                        display_control = std::move(answer);
                        ++display_control_updates;
                        updated = true;
                        std::fprintf(stderr, "viewer: display control %s%s%s\n",
                                     display_control.code == chast::DisplayControlCode::Ok
                                         ? "ok"
                                         : "refused",
                                     display_control.reason.empty() ? "" : ": ",
                                     display_control.reason.c_str());
                    }
                    if (updated) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.display_control = display_control;
                        status_.display_control_updates = display_control_updates;
                        status_.display_control_busy = false;
                    }
                };

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

                // Sends the newest queued host-control action. A local failure
                // (the permission was revoked between the queue and the send)
                // is reported like a HostActionResult, so the UI always gets an
                // answer.
                auto drain_host_action = [&] {
                    std::optional<chast::HostControlRequest> requested;
                    {
                        std::lock_guard<std::mutex> lock(host_action_mutex_);
                        requested.swap(pending_host_action_);
                    }
                    if (!requested) {
                        return;
                    }
                    const chast::Error sent =
                        session->request_host_action(requested->action,
                                                     requested->block_input);
                    if (sent.ok()) {
                        return;
                    }
                    host_action_action = static_cast<int>(requested->action);
                    host_action_code = static_cast<int>(chast::HostActionCode::Failed);
                    host_action_error = "request: " + sent.to_string();
                    ++host_action_updates;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.host_action_action = host_action_action;
                    status_.host_action_code = host_action_code;
                    status_.host_action_error = host_action_error;
                    status_.host_action_updates = host_action_updates;
                    std::fprintf(stderr, "viewer: %s: %s\n",
                                 host_action_name(requested->action),
                                 sent.to_string().c_str());
                };

                // Adopts every answer. The host also sends an unsolicited
                // BlockInput denial when it releases the block itself, so the
                // toggle always follows what the host actually did.
                auto receive_host_action = [&] {
                    chast::HostControlResult result;
                    while (session->take_host_action_result(result)) {
                        host_action_action = static_cast<int>(result.action);
                        host_action_code = static_cast<int>(result.code);
                        host_action_error =
                            result.code == chast::HostActionCode::Ok
                                ? std::string()
                                : (result.reason.empty()
                                       ? host_action_code_name(result.code)
                                       : result.reason);
                        if (result.action == chast::HostAction::BlockInput) {
                            block_input_active = result.block_input;
                        }
                        ++host_action_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.host_action_action = host_action_action;
                            status_.host_action_code = host_action_code;
                            status_.host_action_error = host_action_error;
                            status_.host_action_updates = host_action_updates;
                            status_.block_input_active = block_input_active;
                        }
                        std::fprintf(stderr, "viewer: %s %s%s%s\n",
                                     host_action_name(result.action),
                                     result.code == chast::HostActionCode::Ok
                                         ? "done"
                                         : "refused",
                                     host_action_error.empty() ? "" : ": ",
                                     host_action_error.c_str());
                    }
                };

                // Sends the newest queued remote-config request. A local
                // failure (the permission was revoked between the queue and
                // the send) is reported like a RemoteConfigState, so the
                // dialog always gets an answer.
                auto drain_remote_config = [&] {
                    std::optional<std::pair<uint32_t, uint32_t>> requested;
                    {
                        std::lock_guard<std::mutex> lock(remote_config_mutex_);
                        requested.swap(pending_remote_config_);
                    }
                    if (!requested) {
                        return;
                    }
                    const chast::Error sent =
                        session->request_remote_config(requested->first, requested->second);
                    if (sent.ok()) {
                        return;
                    }
                    remote_config_code =
                        static_cast<int>(chast::RemoteConfigCode::Failed);
                    remote_config_error = "request: " + sent.to_string();
                    ++remote_config_updates;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.remote_config_code = remote_config_code;
                    status_.remote_config_error = remote_config_error;
                    status_.remote_config_updates = remote_config_updates;
                    std::fprintf(stderr, "viewer: remote configuration: %s\n",
                                 sent.to_string().c_str());
                };

                // Adopts the newest answer. The flags snapshot is what the
                // host stored, so the dialog closes on the state that actually
                // took effect.
                auto receive_remote_config = [&] {
                    chast::RemoteConfigState state;
                    while (session->take_remote_config_state(state)) {
                        remote_config_flags = state.flags;
                        remote_config_code = static_cast<int>(state.code);
                        remote_config_error =
                            state.code == chast::RemoteConfigCode::Ok
                                ? std::string()
                                : (state.reason.empty()
                                       ? remote_config_code_name(state.code)
                                       : state.reason);
                        ++remote_config_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.remote_config_flags = remote_config_flags;
                            status_.remote_config_code = remote_config_code;
                            status_.remote_config_error = remote_config_error;
                            status_.remote_config_updates = remote_config_updates;
                        }
                        std::fprintf(stderr,
                                     "viewer: remote configuration %s%s%s (flags 0x%08x)\n",
                                     state.code == chast::RemoteConfigCode::Ok
                                         ? "applied"
                                         : "refused",
                                     remote_config_error.empty() ? "" : ": ",
                                     remote_config_error.c_str(), state.flags);
                    }
                };

                // Sends the newest queued OS password. The slot and the local
                // copy are wiped as soon as the transport has the payload; a
                // local refusal is reported like an OsLoginResult.
                auto drain_os_login = [&] {
                    std::optional<std::string> password;
                    {
                        std::lock_guard<std::mutex> lock(os_login_mutex_);
                        password.swap(pending_os_login_);
                    }
                    if (!password) {
                        return;
                    }
                    const chast::Error sent = session->request_os_login(*password);
                    std::fill(password->begin(), password->end(), '\0');
                    password.reset();
                    if (sent.ok()) {
                        return;
                    }
                    os_login_code = static_cast<int>(chast::OsLoginCode::Failed);
                    os_login_error = "request: " + sent.to_string();
                    ++os_login_updates;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.os_login_code = os_login_code;
                    status_.os_login_error = os_login_error;
                    status_.os_login_updates = os_login_updates;
                    std::fprintf(stderr, "viewer: OS login request failed: %s\n",
                                 sent.to_string().c_str());
                };

                // Adopts every answer and keeps only the reason, never the
                // password that was sent.
                auto receive_os_login = [&] {
                    chast::OsLoginCode code = chast::OsLoginCode::Ok;
                    std::string reason;
                    while (session->take_os_login_result(code, reason)) {
                        os_login_code = static_cast<int>(code);
                        os_login_error =
                            code == chast::OsLoginCode::Ok
                                ? std::string()
                                : (reason.empty() ? os_login_code_name(code) : reason);
                        ++os_login_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.os_login_code = os_login_code;
                            status_.os_login_error = os_login_error;
                            status_.os_login_updates = os_login_updates;
                        }
                        std::fprintf(stderr, "viewer: OS login %s%s%s\n",
                                     code == chast::OsLoginCode::Ok ? "done" : "refused",
                                     os_login_error.empty() ? "" : ": ",
                                     os_login_error.c_str());
                    }
                };

                // Sends the queued list refresh and switch target. A local
                // failure (the permission was revoked between the queue and
                // the send) is reported like the host's answer, so the UI
                // always gets one.
                auto drain_elevation = [&] {
                    const auto request = pending_elevation_.exchange(0);
                    if (request) {
                        const auto sent = session->request_elevation(request);
                        if (!sent.ok()) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.elevation = {request, chast::ElevationCode::Failed, sent.to_string()};
                            ++status_.elevation_updates;
                        }
                    }
                    chast::ElevationState state;
                    while (session->take_elevation_state(state)) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (state.request_id == 0 && status_.elevation.request_id != 0) continue;
                        if (state.request_id != 0 && state.request_id != status_.elevation.request_id) continue;
                        status_.elevation = state;
                        ++status_.elevation_updates;
                    }
                };
                auto drain_session_requests = [&] {
                    bool list = false;
                    std::optional<uint32_t> select;
                    std::optional<std::pair<uint32_t, uint64_t>> cursor_shape;
                    {
                        std::lock_guard<std::mutex> lock(session_mutex_);
                        list = pending_session_list_;
                        pending_session_list_ = false;
                        select.swap(pending_session_select_);
                        cursor_shape.swap(pending_cursor_shape_);
                    }
                    if (cursor_shape) {
                        const chast::Error sent = session->request_cursor_shape(
                            cursor_shape->first, cursor_shape->second);
                        if (!sent.ok()) {
                            std::fprintf(stderr, "viewer: cursor shape request failed: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                    if (list) {
                        const chast::Error sent = session->request_session_list();
                        if (!sent.ok()) {
                            session_list_code =
                                static_cast<int>(chast::SessionListCode::Failed);
                            session_list_error = "request: " + sent.to_string();
                            ++session_list_updates;
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.session_list_code = session_list_code;
                            status_.session_list_error = session_list_error;
                            status_.session_list_updates = session_list_updates;
                            std::fprintf(stderr, "viewer: session list request failed: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                    if (select) {
                        const chast::Error sent = session->request_session_select(*select);
                        if (!sent.ok()) {
                            session_select_id = *select;
                            session_select_code =
                                static_cast<int>(chast::SessionSelectCode::Failed);
                            session_select_error = "request: " + sent.to_string();
                            ++session_select_updates;
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.session_select_id = session_select_id;
                            status_.session_select_code = session_select_code;
                            status_.session_select_error = session_select_error;
                            status_.session_select_updates = session_select_updates;
                            std::fprintf(stderr, "viewer: session select request failed: %s\n",
                                         sent.to_string().c_str());
                        }
                    }
                };

                // Adopts every answer. An accepted switch ends this
                // connection; the caller's reconnect then lands in the target
                // session.
                auto receive_session_answers = [&] {
                    chast::SessionListResult list_result;
                    while (session->take_session_list_result(list_result)) {
                        session_list_code = static_cast<int>(list_result.code);
                        session_list_error =
                            list_result.code == chast::SessionListCode::Ok
                                ? std::string()
                                : (list_result.reason.empty()
                                       ? session_list_code_name(list_result.code)
                                       : list_result.reason);
                        sessions = std::move(list_result.sessions);
                        ++session_list_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.sessions = sessions;
                            status_.session_list_code = session_list_code;
                            status_.session_list_error = session_list_error;
                            status_.session_list_updates = session_list_updates;
                        }
                        std::fprintf(stderr, "viewer: session list %s%s%s (%zu session(s))\n",
                                     list_result.code == chast::SessionListCode::Ok
                                         ? "read"
                                         : "refused",
                                     session_list_error.empty() ? "" : ": ",
                                     session_list_error.c_str(), sessions.size());
                    }
                    chast::SessionSelectResult select_result;
                    while (session->take_session_select_result(select_result)) {
                        session_select_id = select_result.session_id;
                        session_select_code = static_cast<int>(select_result.code);
                        session_select_error =
                            select_result.code == chast::SessionSelectCode::Ok
                                ? std::string()
                                : (select_result.reason.empty()
                                       ? session_select_code_name(select_result.code)
                                       : select_result.reason);
                        ++session_select_updates;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.session_select_id = session_select_id;
                            status_.session_select_code = session_select_code;
                            status_.session_select_error = session_select_error;
                            status_.session_select_updates = session_select_updates;
                        }
                        std::fprintf(stderr, "viewer: session select %s%s%s\n",
                                     select_result.code == chast::SessionSelectCode::Ok
                                         ? "accepted"
                                         : "refused",
                                     session_select_error.empty() ? "" : ": ",
                                     session_select_error.c_str());
                    }
                };

                // Chat is ungated: an admitted session is the authorization.
                // Text goes into the UI queue; the status only carries counts,
                // so a 4 KiB line never inflates the poll.
                auto receive_chat = [&] {
                    std::string text;
                    while (session->take_chat(text)) {
                        ++chat_received;
                        {
                            std::lock_guard<std::mutex> lock(chat_mutex_);
                            if (received_chat_.size() < 64) {
                                received_chat_.push_back(text);
                            }
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.chat_received = chat_received;
                        }
                        std::fprintf(stderr, "viewer: chat received (%zu bytes)\n",
                                     text.size());
                    }
                };

                // Sends the lines the UI queued. A failure is reported in the
                // status and ends the drain: the link is no longer usable.
                auto drain_chat = [&] {
                    std::deque<std::string> outbox;
                    {
                        std::lock_guard<std::mutex> lock(chat_mutex_);
                        outbox.swap(pending_chat_);
                    }
                    for (const std::string& text : outbox) {
                        chast::Error sent = session->send_chat(text);
                        if (!sent.ok()) {
                            ++chat_failures;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                status_.chat_failures = chat_failures;
                                status_.chat_error = "send: " + sent.to_string();
                            }
                            std::fprintf(stderr, "viewer: send chat: %s\n",
                                         sent.to_string().c_str());
                            break;
                        }
                        ++chat_sent;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.chat_sent = chat_sent;
                        }
                        std::fprintf(stderr, "viewer: chat sent (%zu bytes)\n", text.size());
                    }
                };

                // Voice call. The request round trip and the host's answer are
                // taken here; a host-initiated close (permission revocation or
                // capture failure) clears the switch and explains itself.
                auto drain_voice = [&] {
                    const bool wanted = voice_wanted_.load(std::memory_order_relaxed);
                    if (!wanted && (voice_active || voice_pending)) {
                        session->request_voice_call_stop();
                        stop_voice();
                        voice_error.clear();
                        std::fprintf(stderr, "viewer: voice call stopped\n");
                        return;
                    }
                    if (!wanted || voice_active || voice_pending) {
                        return;
                    }
                    if (!voice_authorized) {
                        voice_error = "the host did not grant the microphone";
                        voice_wanted_.store(false, std::memory_order_relaxed);
                        return;
                    }
                    chast::Error requested = session->request_voice_call();
                    if (!requested.ok()) {
                        ++voice_errors;
                        voice_error = "request: " + requested.to_string();
                        voice_wanted_.store(false, std::memory_order_relaxed);
                        std::fprintf(stderr, "viewer: voice call request: %s\n",
                                     requested.to_string().c_str());
                        return;
                    }
                    voice_pending = true;
                    voice_error.clear();
                };
                auto receive_voice = [&] {
                    chast::VoiceCallResponse response;
                    while (session->take_voice_call_response(response)) {
                        voice_pending = false;
                        if (response.code != chast::VoiceCallCode::Ok) {
                            stop_voice();
                            voice_wanted_.store(false, std::memory_order_relaxed);
                            voice_error = response.reason.empty()
                                              ? std::string("the host refused the voice call")
                                              : response.reason;
                            ++voice_errors;
                            std::fprintf(stderr, "viewer: voice call refused: %s\n",
                                         voice_error.c_str());
                            continue;
                        }
                        std::string setup_error;
                        voice_decoder =
                            chast::create_audio_decoder(response.audio, &setup_error);
                        if (!voice_decoder) {
                            ++voice_errors;
                            voice_error = "decoder: " + setup_error;
                            voice_wanted_.store(false, std::memory_order_relaxed);
                            voice_pending = false;
                            session->request_voice_call_stop();
                            std::fprintf(stderr, "viewer: voice decoder: %s\n",
                                         setup_error.c_str());
                            continue;
                        }
                        chast::AudioPlayerConfig player_config;
                        // The host's capture clock is unrelated to this
                        // machine's; anchoring playback on arrival keeps the
                        // 80 ms jitter buffer and recovers after a stall.
                        player_config.arrival_anchored = true;
                        player_config.device_id = audio_device;
                        voice_player = chast::create_audio_player(player_config, &setup_error);
                        if (!voice_player) {
                            ++voice_errors;
                            voice_error = "playback: " + setup_error;
                            voice_wanted_.store(false, std::memory_order_relaxed);
                            voice_decoder.reset();
                            session->request_voice_call_stop();
                            std::fprintf(stderr, "viewer: voice playback: %s\n",
                                         setup_error.c_str());
                            continue;
                        }
                        if (chast::Error started = voice_player->start(); !started.ok()) {
                            ++voice_errors;
                            voice_error = "playback: " + started.to_string();
                            voice_wanted_.store(false, std::memory_order_relaxed);
                            voice_player.reset();
                            voice_decoder.reset();
                            session->request_voice_call_stop();
                            std::fprintf(stderr, "viewer: voice playback: %s\n",
                                         started.to_string().c_str());
                            continue;
                        }
                        voice_info = response.audio;
                        voice_active = true;
                        voice_error.clear();
                        std::fprintf(stderr, "viewer: voice call up, Opus %u Hz %u ch\n",
                                     voice_info.sample_rate, voice_info.channels);
                    }
                    if (session->take_voice_call_closed() &&
                        (voice_active || voice_pending)) {
                        stop_voice();
                        voice_wanted_.store(false, std::memory_order_relaxed);
                        voice_error = "the host ended the voice call";
                        std::fprintf(stderr, "viewer: voice call closed by the host\n");
                    }
                    chast::AudioHeader header;
                    std::vector<uint8_t> payload;
                    while (voice_active && session->take_voice_audio(header, payload)) {
                        if (!voice_decoder || !voice_player) {
                            break;
                        }
                        std::vector<chast::DecodedAudio> blocks;
                        chast::Error decoded = voice_decoder->decode(
                            header, payload.data(), payload.size(), blocks);
                        if (!decoded.ok()) {
                            ++voice_errors;
                            if (voice_errors <= 3) {
                                std::fprintf(stderr, "viewer: voice decode: %s\n",
                                             decoded.to_string().c_str());
                            }
                            continue;
                        }
                        bool push_failed = false;
                        for (const chast::DecodedAudio& block : blocks) {
                            chast::Error pushed = voice_player->push(block);
                            if (!pushed.ok()) {
                                ++voice_errors;
                                push_failed = true;
                                break;
                            }
                        }
                        if (push_failed) {
                            break;
                        }
                        ++voice_packets;
                        const std::string render_error = voice_player->error();
                        if (!render_error.empty()) {
                            ++voice_errors;
                            voice_error = "playback: " + render_error;
                            stop_voice();
                            voice_wanted_.store(false, std::memory_order_relaxed);
                            std::fprintf(stderr, "viewer: voice playback lost the "
                                                 "endpoint: %s\n",
                                         render_error.c_str());
                            break;
                        }
                    }
                };

                // Remote terminals. The panel drives up to kMaxTerminals
                // consoles through the command queue; this thread owns the
                // session, so request, input, resize and the stop all leave
                // from here in order. Each slot gets a fresh wire id on every
                // open, so an attached service starts under a new
                // session-scoped id and stale answers for the previous console
                // can no longer match. `requesting`/`closing` describe the
                // wire console behind one slot; the slot's command generation
                // retires everything that belonged to its previous console.
                struct ActiveTerminal {
                    uint32_t wire_id = 0;
                    bool requesting = false;
                    bool closing = false;
                };
                std::array<ActiveTerminal, chast::kMaxTerminals> active_terminals{};
                uint32_t terminal_next_id = 1;
                auto terminal_slot = [&](uint32_t wire_id) -> size_t {
                    for (size_t i = 0; i < active_terminals.size(); ++i) {
                        if (active_terminals[i].wire_id == wire_id) return i;
                    }
                    return active_terminals.size();
                };
                auto terminals_active = [&] {
                    return std::any_of(active_terminals.begin(), active_terminals.end(),
                                       [](const ActiveTerminal& terminal) {
                                           return terminal.wire_id != 0;
                                       });
                };
                auto set_terminal_phase = [&](size_t slot, TerminalPhase phase, int code,
                                              std::string reason, std::string error) {
                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                    ViewerTerminalStatus& status = terminals_[slot].status;
                    // A host reply arriving after the panel closes must not
                    // revive that panel or accept new keystrokes for it.
                    if (status.phase == TerminalPhase::kClosed) {
                        return;
                    }
                    status.phase = phase;
                    status.code = code;
                    status.reason = std::move(reason);
                    status.error = std::move(error);
                    if (phase == TerminalPhase::kEnded ||
                        phase == TerminalPhase::kFailed) {
                        // Nothing is left to attach to once the console is
                        // over or its open was refused.
                        status.service_id = 0;
                    }
                    ++status.updates;
                };
                auto terminal_phase = [&](size_t slot) {
                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                    return terminals_[slot].status.phase;
                };

                // Forgets the session-side entry once its output is done. The
                // UI keeps whatever was already handed over, and the phase it
                // sees next poll is the caller's. The service id goes with the
                // entry: either the host removed the service or the next open
                // names a different one.
                auto finish_terminal = [&](size_t slot) {
                    session->close_terminal(active_terminals[slot].wire_id);
                    active_terminals[slot] = {};
                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                    terminals_[slot].ended = true;
                    terminals_[slot].status.service_id = 0;
                };

                auto drain_terminal = [&] {
                    // Work on a bounded snapshot. Backpressure for one tab
                    // defers only that tab; the other tab can still resize,
                    // type and close during the same frame.
                    std::deque<TerminalCommand> commands;
                    {
                        std::lock_guard<std::mutex> lock(terminal_mutex_);
                        commands.swap(pending_terminal_);
                    }
                    std::deque<TerminalCommand> deferred;
                    std::array<bool, chast::kMaxTerminals> stalled{};
                    for (TerminalCommand& command : commands) {
                        const size_t slot = command.slot;
                        {
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            if (command.generation != terminals_[slot].generation) {
                                continue;
                            }
                        }
                        ActiveTerminal& active = active_terminals[slot];
                        if (stalled[slot]) {
                            deferred.push_back(std::move(command));
                            continue;
                        }
                        switch (command.kind) {
                            case TerminalCommand::Kind::kOpen: {
                                if (terminal_phase(slot) == TerminalPhase::kClosed) {
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    terminals_[slot].ended = true;
                                    break;
                                }
                                if ((session->permissions() &
                                     chast::kPermissionTerminal) == 0) {
                                    set_terminal_phase(
                                        slot, TerminalPhase::kFailed, 0, {},
                                        "the host did not grant remote terminals");
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    terminals_[slot].ended = true;
                                    break;
                                }
                                if (active.wire_id != 0) {
                                    // The previous console is still draining;
                                    // opening now would mix its bytes into the
                                    // new panel.
                                    deferred.push_back(std::move(command));
                                    stalled[slot] = true;
                                    break;
                                }
                                // A fresh id per console, wrapping only past
                                // ids no session could still be waiting on.
                                const uint32_t wire_id = terminal_next_id++;
                                if (terminal_next_id == 0) {
                                    terminal_next_id = 1;
                                }
                                chast::Error requested = session->request_terminal(
                                    wire_id, command.columns, command.rows,
                                    command.flags, command.service_id);
                                if (!requested.ok()) {
                                    set_terminal_phase(slot, TerminalPhase::kFailed, 0, {},
                                                       "request: " +
                                                           requested.to_string());
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    terminals_[slot].ended = true;
                                    break;
                                }
                                active.wire_id = wire_id;
                                active.requesting = true;
                                active.closing = false;
                                std::fprintf(stderr,
                                             "viewer: terminal %zu requested %ux%u "
                                             "(id %u, flags 0x%02x, service %u)\n",
                                             slot + 1, command.columns, command.rows,
                                             wire_id, command.flags, command.service_id);
                                break;
                            }
                            case TerminalCommand::Kind::kInput: {
                                const size_t original_bytes = command.bytes.size();
                                bool retry = false;
                                if (active.wire_id != 0 && !active.closing &&
                                    terminal_phase(slot) == TerminalPhase::kOpen) {
                                    while (!command.bytes.empty()) {
                                        const size_t chunk = std::min(
                                            command.bytes.size(),
                                            chast::kMaxTerminalChunkBytes);
                                        chast::Error sent = session->send_terminal_input(
                                            active.wire_id,
                                            reinterpret_cast<const uint8_t*>(
                                                command.bytes.data()),
                                            chunk, std::chrono::milliseconds(0));
                                        if (sent.status() == chast::Status::WaitTimeout) {
                                            retry = true;
                                            break;
                                        }
                                        if (!sent.ok()) {
                                            set_terminal_phase(slot, TerminalPhase::kFailed,
                                                               0, {},
                                                               "send input: " +
                                                                   sent.to_string());
                                            session->send_terminal_stop(
                                                active.wire_id,
                                                chast::TerminalCode::Failed,
                                                "input stream failed");
                                            finish_terminal(slot);
                                            break;
                                        }
                                        command.bytes.erase(0, chunk);
                                    }
                                }
                                {
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    TerminalChannel& channel = terminals_[slot];
                                    if (channel.generation == command.generation) {
                                        channel.pending_bytes -= std::min(
                                            channel.pending_bytes,
                                            original_bytes -
                                                (retry ? command.bytes.size() : 0));
                                    }
                                }
                                if (retry) {
                                    deferred.push_back(std::move(command));
                                    stalled[slot] = true;
                                }
                                break;
                            }
                            case TerminalCommand::Kind::kResize: {
                                if (active.wire_id == 0 || active.closing ||
                                    terminal_phase(slot) == TerminalPhase::kClosed) {
                                    break;
                                }
                                if (active.requesting) {
                                    deferred.push_back(std::move(command));
                                    stalled[slot] = true;
                                    break;
                                }
                                chast::Error sent = session->send_terminal_resize(
                                    active.wire_id, command.columns, command.rows);
                                if (sent.ok()) {
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    terminals_[slot].status.columns = command.columns;
                                    terminals_[slot].status.rows = command.rows;
                                } else {
                                    std::fprintf(stderr,
                                                 "viewer: terminal %zu resize %ux%u: %s\n",
                                                 slot + 1, command.columns,
                                                 command.rows,
                                                 sent.to_string().c_str());
                                }
                                break;
                            }
                            case TerminalCommand::Kind::kClose: {
                                if (active.wire_id != 0) {
                                    chast::Error stopped = session->send_terminal_stop(
                                        active.wire_id, chast::TerminalCode::Cancelled,
                                        "closed by the viewer");
                                    if (!stopped.ok()) {
                                        std::lock_guard<std::mutex> lock(terminal_mutex_);
                                        terminals_[slot].status.error =
                                            "close: " + stopped.to_string();
                                    }
                                    finish_terminal(slot);
                                }
                                break;
                            }
                        }
                    }
                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                    // A close may have arrived while this batch was running.
                    // Drop stalled input for closed tabs before restoring
                    // order, so a stale byte can never precede the close.
                    for (auto it = deferred.rbegin(); it != deferred.rend(); ++it) {
                        if (it->generation != terminals_[it->slot].generation) continue;
                        if (terminals_[it->slot].status.phase == TerminalPhase::kClosed) {
                            if (it->kind == TerminalCommand::Kind::kInput) {
                                size_t& pending = terminals_[it->slot].pending_bytes;
                                pending -= std::min(pending, it->bytes.size());
                            }
                        } else {
                            pending_terminal_.push_front(std::move(*it));
                        }
                    }
                };

                auto poll_terminal = [&] {
                    chast::TerminalResult result;
                    while (session->take_terminal_result(result)) {
                        const size_t slot = terminal_slot(result.terminal_id);
                        if (slot == active_terminals.size()) {
                            continue;
                        }
                        ActiveTerminal& active = active_terminals[slot];
                        active.requesting = false;
                        if (terminal_phase(slot) == TerminalPhase::kClosed) {
                            // The panel closed while the answer was in
                            // flight; the host's console must not outlive it.
                            if (result.code == chast::TerminalCode::Ok) {
                                session->send_terminal_stop(
                                    active.wire_id, chast::TerminalCode::Cancelled,
                                    "closed by the viewer");
                            }
                            finish_terminal(slot);
                            continue;
                        }
                        if (result.code != chast::TerminalCode::Ok) {
                            set_terminal_phase(slot, TerminalPhase::kFailed,
                                               static_cast<int>(result.code),
                                               result.reason, {});
                            std::fprintf(stderr, "viewer: terminal refused: %s\n",
                                         result.reason.empty() ? "no reason given"
                                                               : result.reason.c_str());
                            finish_terminal(slot);
                            continue;
                        }
                        // Persistent consoles hand back the id a later session
                        // attaches with. A legacy console leaves it at 0.
                        {
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            terminals_[slot].status.service_id = result.service_id;
                        }
                        chast::Error opened =
                            session->open_terminal_input(active.wire_id);
                        if (!opened.ok()) {
                            set_terminal_phase(
                                slot, TerminalPhase::kFailed,
                                static_cast<int>(chast::TerminalCode::Failed), {},
                                "input stream: " + opened.to_string());
                            session->send_terminal_stop(
                                active.wire_id, chast::TerminalCode::Failed,
                                "input stream failed");
                            finish_terminal(slot);
                            continue;
                        }
                        set_terminal_phase(slot, TerminalPhase::kOpen, 0, {}, {});
                        if (result.service_id != 0) {
                            std::fprintf(stderr,
                                         "viewer: terminal %zu open (service #%u)\n",
                                         slot + 1, result.service_id);
                        } else {
                            std::fprintf(stderr, "viewer: terminal %zu open\n",
                                         slot + 1);
                        }
                    }

                    chast::TerminalStop stop;
                    while (session->take_terminal_stop(stop)) {
                        const size_t slot = terminal_slot(stop.terminal_id);
                        if (slot == active_terminals.size()) {
                            continue;
                        }
                        active_terminals[slot].closing = true;
                        // A local close already told the panel why it ended;
                        // a host stop is the answer for everything else.
                        if (terminal_phase(slot) != TerminalPhase::kClosed &&
                            terminal_phase(slot) != TerminalPhase::kFailed) {
                            set_terminal_phase(slot, TerminalPhase::kEnded,
                                               static_cast<int>(stop.code),
                                               stop.reason, {});
                        }
                        std::fprintf(stderr, "viewer: terminal %zu stopped: %s\n",
                                     slot + 1,
                                     stop.reason.empty() ? "no reason given"
                                                         : stop.reason.c_str());
                    }

                    for (size_t slot = 0; slot < active_terminals.size(); ++slot) {
                        const ActiveTerminal& active = active_terminals[slot];
                        if (active.wire_id == 0) {
                            continue;
                        }
                        if ((session->permissions() &
                             chast::kPermissionTerminal) == 0) {
                            // The transport aborted the streams and the
                            // session forgot the entry; anything received
                            // before that stays queued for the panel.
                            finish_terminal(slot);
                            set_terminal_phase(slot, TerminalPhase::kFailed, 0, {},
                                               "the host revoked terminal access");
                            continue;
                        }
                        if (active.requesting) {
                            // The host has not answered yet, so the session has
                            // no entry to read. Asking now would report the
                            // console as ended and cancel the open that is
                            // still in flight.
                            continue;
                        }
                        for (;;) {
                            {
                                std::lock_guard<std::mutex> lock(terminal_mutex_);
                                if (terminals_[slot].output_bytes >=
                                    chast::kTerminalReceiveHighWaterBytes) {
                                    // The panel has not caught up. Stop pulling
                                    // so the session's receive window pauses
                                    // the host instead of truncating the
                                    // picture.
                                    break;
                                }
                            }
                            std::vector<uint8_t> data;
                            bool ended = false;
                            if (!session->take_terminal_output(active.wire_id, data,
                                                               ended)) {
                                if (ended) {
                                    // A host-initiated ending carries its own
                                    // stop; a lost one is still an ending.
                                    if (terminal_phase(slot) ==
                                            TerminalPhase::kOpen ||
                                        terminal_phase(slot) ==
                                            TerminalPhase::kRequesting) {
                                        set_terminal_phase(slot,
                                                           TerminalPhase::kEnded, 0,
                                                           {}, {});
                                    }
                                    finish_terminal(slot);
                                }
                                break;
                            }
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            TerminalChannel& channel = terminals_[slot];
                            channel.status.bytes += data.size();
                            channel.output_bytes += data.size();
                            channel.output.push_back(std::move(data));
                        }
                    }
                };

                // Camera sharing (protocol v29). The panel lists the host's
                // cameras and watches at most one. The CCAM stream runs beside
                // the desktop picture and has its own decoder: pictures decode
                // straight into the camera sink's texture, so the camera never
                // touches the CPU any more than the desktop does.
                // `camera_pending` is the id StartCamera asked for and has no
                // answer yet; `camera_active` means the stream is open;
                // `camera_stopping` means a stop went out, so the stream's end
                // is expected rather than a failure.
                uint16_t camera_pending = 0;
                bool camera_active = false;
                bool camera_stopping = false;
                std::unique_ptr<chast::IVideoDecoder> camera_decoder;
                chast::VideoCodec camera_codec = chast::VideoCodec::H264;
                chast::VideoChroma camera_chroma = chast::VideoChroma::Yuv420;
                uint32_t camera_decode_width = 0;
                uint32_t camera_decode_height = 0;
                uint64_t camera_frames = 0;
                uint64_t camera_gaps = 0;
                uint64_t camera_decode_errors = 0;
                uint64_t camera_sink_failures = 0;
                uint64_t camera_next_seq = 0;
                bool camera_seq_valid = false;
                std::vector<chast::DecodedFrame> camera_decoded;

                auto set_camera_phase = [&](CameraPhase phase, int code,
                                            std::string reason, std::string error) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.camera_phase = phase;
                    status_.camera_code = code;
                    status_.camera_reason = std::move(reason);
                    status_.camera_error = std::move(error);
                    ++status_.camera_updates;
                };
                auto camera_phase_is = [&](CameraPhase phase) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    return status_.camera_phase == phase;
                };
                // Forgets the session-side camera once its stream is over. The
                // phase the panel sees is the caller's to set.
                auto reset_camera = [&] {
                    camera_active = false;
                    camera_pending = 0;
                    camera_stopping = false;
                    camera_decoder.reset();
                    camera_seq_valid = false;
                    camera_sink_failures = 0;
                    camera_decoded.clear();
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.camera_decoder.clear();
                };

                auto drain_camera = [&] {
                    while (true) {
                        CameraCommand command;
                        {
                            std::lock_guard<std::mutex> lock(camera_mutex_);
                            if (pending_camera_.empty()) {
                                return;
                            }
                            command = std::move(pending_camera_.front());
                            pending_camera_.pop_front();
                        }
                        switch (command.kind) {
                            case CameraCommand::Kind::kList: {
                                if ((session->permissions() & chast::kPermissionCamera) ==
                                    0) {
                                    set_camera_phase(
                                        CameraPhase::kFailed,
                                        static_cast<int>(chast::CameraCode::Refused), {},
                                        "the host did not grant camera access");
                                    continue;
                                }
                                chast::Error requested = session->request_camera_list();
                                if (!requested.ok()) {
                                    set_camera_phase(CameraPhase::kFailed, 0, {},
                                                     "request: " + requested.to_string());
                                    continue;
                                }
                                std::fprintf(stderr, "viewer: camera list requested\n");
                                continue;
                            }
                            case CameraCommand::Kind::kStart: {
                                if ((session->permissions() & chast::kPermissionCamera) ==
                                    0) {
                                    set_camera_phase(
                                        CameraPhase::kFailed,
                                        static_cast<int>(chast::CameraCode::Refused), {},
                                        "the host did not grant camera access");
                                    continue;
                                }
                                if (camera_sink_ == nullptr ||
                                    camera_sink_->device() == nullptr) {
                                    set_camera_phase(CameraPhase::kFailed, 0, {},
                                                     "the camera output is not available");
                                    continue;
                                }
                                if (camera_active || camera_pending != 0) {
                                    // A new start while the old camera is open
                                    // replaces it; the host has one camera
                                    // slot, so the old stream must close first.
                                    session->request_camera_stop();
                                    reset_camera();
                                }
                                chast::Error requested =
                                    session->request_camera_start(command.camera_id);
                                if (!requested.ok()) {
                                    set_camera_phase(CameraPhase::kFailed, 0, {},
                                                     "request: " + requested.to_string());
                                    continue;
                                }
                                camera_pending = command.camera_id;
                                camera_stopping = false;
                                std::fprintf(stderr, "viewer: camera %u requested\n",
                                             static_cast<unsigned>(command.camera_id));
                                continue;
                            }
                            case CameraCommand::Kind::kStop: {
                                if (camera_active || camera_pending != 0) {
                                    chast::Error stopped = session->request_camera_stop();
                                    if (!stopped.ok()) {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        status_.camera_error =
                                            "stop: " + stopped.to_string();
                                    }
                                }
                                camera_stopping = true;
                                camera_active = false;
                                camera_pending = 0;
                                camera_decoder.reset();
                                camera_seq_valid = false;
                                std::lock_guard<std::mutex> lock(mutex_);
                                status_.camera_decoder.clear();
                                continue;
                            }
                        }
                    }
                };

                auto poll_camera = [&] {
                    chast::CameraList list;
                    while (session->take_camera_list(list)) {
                        if (!camera_phase_is(CameraPhase::kListing)) {
                            // The panel stopped waiting for this answer.
                            continue;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.cameras = list.cameras;
                            ++status_.camera_list_updates;
                        }
                        if (list.code != chast::CameraCode::Ok) {
                            set_camera_phase(CameraPhase::kFailed,
                                             static_cast<int>(list.code), list.reason, {});
                            std::fprintf(stderr, "viewer: camera list refused: %s\n",
                                         list.reason.empty() ? "no reason given"
                                                             : list.reason.c_str());
                            continue;
                        }
                        set_camera_phase(CameraPhase::kListed, 0, {}, {});
                        std::fprintf(stderr, "viewer: camera list: %zu\n",
                                     list.cameras.size());
                    }

                    chast::CameraResult result;
                    while (session->take_camera_result(result)) {
                        if (camera_pending == 0 || result.camera_id != camera_pending) {
                            continue;
                        }
                        camera_pending = 0;
                        if (!camera_phase_is(CameraPhase::kStarting)) {
                            // The panel stopped while the answer was in
                            // flight; an accepted camera must not outlive it.
                            if (result.code == chast::CameraCode::Ok) {
                                session->request_camera_stop();
                            }
                            continue;
                        }
                        if (result.code != chast::CameraCode::Ok) {
                            set_camera_phase(CameraPhase::kFailed,
                                             static_cast<int>(result.code), result.reason,
                                             {});
                            std::fprintf(stderr, "viewer: camera refused: %s\n",
                                         result.reason.empty() ? "no reason given"
                                                               : result.reason.c_str());
                            continue;
                        }
                        camera_active = true;
                        camera_stopping = false;
                        set_camera_phase(CameraPhase::kStreaming, 0, {}, {});
                        std::fprintf(stderr, "viewer: camera open\n");
                    }

                    if (!camera_active) {
                        return;
                    }
                    if ((session->permissions() & chast::kPermissionCamera) == 0) {
                        // The transport aborted the stream; nothing is left to
                        // drain.
                        reset_camera();
                        set_camera_phase(CameraPhase::kFailed,
                                         static_cast<int>(chast::CameraCode::Refused), {},
                                         "the host revoked camera access");
                        return;
                    }
                    for (;;) {
                        chast::FrameHeader header;
                        std::vector<uint8_t> payload;
                        const chast::Error received = session->receive_camera_frame(
                            header, payload, std::chrono::milliseconds(0));
                        if (received.status() == chast::Status::WaitTimeout) {
                            break;
                        }
                        if (!received.ok()) {
                            const bool already_stopped =
                                camera_phase_is(CameraPhase::kStopped);
                            reset_camera();
                            if (!already_stopped) {
                                if (received.status() == chast::Status::Unavailable) {
                                    set_camera_phase(
                                        CameraPhase::kStopped, 0,
                                        "the host ended the camera stream", {});
                                } else {
                                    set_camera_phase(CameraPhase::kFailed, 0, {},
                                                     received.to_string());
                                }
                            }
                            std::fprintf(stderr, "viewer: camera ended: %s\n",
                                         received.to_string().c_str());
                            break;
                        }
                        if (payload.size() != header.payload_size ||
                            chast::crc32(payload.data(), payload.size()) !=
                                header.payload_crc32) {
                            ++camera_decode_errors;
                            if (camera_decode_errors >= 30) {
                                session->request_camera_stop();
                                reset_camera();
                                set_camera_phase(CameraPhase::kFailed, 0, {},
                                                 "too many damaged camera frames");
                                break;
                            }
                            continue;
                        }
                        if (camera_seq_valid && header.seq != camera_next_seq) {
                            ++camera_gaps;
                        }
                        camera_next_seq = header.seq + 1;
                        camera_seq_valid = true;
                        if (!camera_decoder || header.codec != camera_codec || header.chroma != camera_chroma ||
                            header.width != camera_decode_width ||
                            header.height != camera_decode_height) {
                            chast::DecoderConfig camera_config;
                            camera_config.codec = header.codec;
                            camera_config.chroma = header.chroma;
                            camera_config.d3d11_device =
                                camera_sink_ != nullptr ? camera_sink_->device()
                                                        : nullptr;
                            camera_config.gpu_handler =
                                [&](const chast::GpuFrame& frame) -> chast::Error {
                                ++camera_frames;
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    status_.camera_frames = camera_frames;
                                    status_.camera_width = frame.width;
                                    status_.camera_height = frame.height;
                                }
                                if (camera_sink_ != nullptr &&
                                    camera_sink_->OnGpuFrame(
                                        frame.texture, frame.subresource, frame.width,
                                        frame.height, frame.crop_x, frame.crop_y,
                                        frame.pts_ms)) {
                                    camera_sink_failures = 0;
                                    return chast::Error::success();
                                }
                                if (camera_sink_ == nullptr) {
                                    return chast::Error(
                                        chast::Status::Unsupported,
                                        "camera: the frame sink is not available");
                                }
                                const std::string sink_error = camera_sink_->error();
                                if (camera_sink_->fatal()) {
                                    return chast::Error(chast::Status::DeviceRemoved,
                                                        "camera: " + sink_error);
                                }
                                if (sink_error.empty()) {
                                    // No texture is attached yet; dropping
                                    // keeps the decoder running.
                                    return chast::Error::success();
                                }
                                ++camera_sink_failures;
                                if (camera_sink_failures >= kMaxSinkFailureStreak) {
                                    camera_sink_failures = 0;
                                    return chast::Error(chast::Status::CaptureFailed,
                                                        "camera: " + sink_error);
                                }
                                return chast::Error::success();
                            };
                            std::string decoder_error;
                            camera_decoder =
                                camera_config.d3d11_device != nullptr
                                    ? chast::create_decoder(camera_config, &decoder_error)
                                    : nullptr;
                            if (!camera_decoder) {
                                if (decoder_error.empty()) {
                                    decoder_error = "the camera output is not available";
                                }
                                session->request_camera_stop();
                                reset_camera();
                                set_camera_phase(
                                    CameraPhase::kFailed,
                                    static_cast<int>(chast::CameraCode::Failed), {},
                                    "decoder (" + codec_name(header.codec) + "): " +
                                        decoder_error);
                                break;
                            }
                            camera_codec = header.codec;
                            camera_chroma = header.chroma;
                            camera_decode_width = header.width;
                            camera_decode_height = header.height;
                            camera_decode_errors = 0;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                status_.camera_decoder =
                                    camera_decoder->implementation();
                            }
                            std::fprintf(stderr, "viewer: camera decoder %s\n",
                                         camera_decoder->implementation().c_str());
                        }
                        camera_decoded.clear();
                        const chast::Error decode_error = camera_decoder->decode(
                            payload.data(), payload.size(), header.pts_ms,
                            header.keyframe, camera_decoded);
                        if (!decode_error.ok()) {
                            if (camera_sink_ != nullptr &&
                                decode_error.message().rfind("camera: ", 0) == 0) {
                                session->request_camera_stop();
                                reset_camera();
                                set_camera_phase(CameraPhase::kFailed, 0, {},
                                                 decode_error.message());
                                break;
                            }
                            if (camera_decode_errors < 3) {
                                std::fprintf(stderr, "viewer: camera decode: %s\n",
                                             decode_error.to_string().c_str());
                            }
                            ++camera_decode_errors;
                            if (camera_decode_errors >= 30) {
                                session->request_camera_stop();
                                reset_camera();
                                set_camera_phase(CameraPhase::kFailed, 0, {},
                                                 "too many camera decode failures");
                                break;
                            }
                            continue;
                        }
                    }
                };

                // Port forwarding. Every rule owns one listener and one host
                // target; commands cross the mutex in order and the rules are
                // driven here by pump(). Up to kMaxPortForwardTargets run at
                // once, and a rule that failed to start stays listed with its
                // error until the operator removes it.
                struct ActiveForward {
                    uint32_t rule_id = 0;
                    uint16_t local_port = 0;
                    std::string remote_host;
                    uint16_t remote_port = 0;
                    std::unique_ptr<chast::PortForwardClient> client;
                    bool active = false;
                    uint64_t connections = 0;
                    uint64_t failures = 0;
                    uint64_t bytes_up = 0;
                    uint64_t bytes_down = 0;
                    std::string error;
                };
                std::vector<ActiveForward> forwards;

                auto publish_forwards = [&] {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.port_forward_rules.clear();
                    status_.port_forward_rules.reserve(forwards.size());
                    for (const ActiveForward& forward : forwards) {
                        ViewerPortForwardRule rule;
                        rule.rule_id = forward.rule_id;
                        rule.active = forward.active;
                        rule.local_port = forward.local_port;
                        rule.remote_host = forward.remote_host;
                        rule.remote_port = forward.remote_port;
                        rule.connections = forward.connections;
                        rule.failures = forward.failures;
                        rule.bytes_up = forward.bytes_up;
                        rule.bytes_down = forward.bytes_down;
                        rule.error = forward.error;
                        status_.port_forward_rules.push_back(std::move(rule));
                    }
                };

                auto stop_forward = [&](size_t index) {
                    ActiveForward& forward = forwards[index];
                    if (forward.client) {
                        forward.client->stop();
                        forward.client.reset();
                    }
                    std::fprintf(stderr, "viewer: port forward rule %u stopped\n",
                                 static_cast<unsigned>(forward.rule_id));
                    forwards.erase(forwards.begin() +
                                   static_cast<std::ptrdiff_t>(index));
                };

                auto drain_port_forward = [&] {
                    std::deque<PortForwardCommand> commands;
                    {
                        std::lock_guard<std::mutex> lock(port_forward_mutex_);
                        commands.swap(pending_port_forward_);
                    }
                    if (commands.empty()) {
                        return;
                    }
                    for (const PortForwardCommand& command : commands) {
                        if (command.kind == PortForwardCommand::Kind::Stop) {
                            for (size_t i = 0; i < forwards.size(); ++i) {
                                if (forwards[i].rule_id == command.rule_id) {
                                    stop_forward(i);
                                    break;
                                }
                            }
                            continue;
                        }
                        ActiveForward forward;
                        forward.rule_id = command.rule_id;
                        forward.local_port = command.local_port;
                        forward.remote_host = command.remote_host;
                        forward.remote_port = command.remote_port;
                        const auto live_forwards = std::count_if(
                            forwards.begin(), forwards.end(),
                            [](const ActiveForward& entry) {
                                return entry.active ||
                                       (entry.client != nullptr &&
                                        entry.client->running());
                            });
                        if ((session->permissions() & chast::kPermissionPortForward) == 0) {
                            forward.error = "the host did not grant port forwarding";
                            forwards.push_back(std::move(forward));
                            continue;
                        }
                        if (live_forwards >=
                            static_cast<std::ptrdiff_t>(
                                chast::kMaxPortForwardTargets)) {
                            forward.error = "at most " +
                                            std::to_string(chast::kMaxPortForwardTargets) +
                                            " port forwards run at once";
                            forwards.push_back(std::move(forward));
                            continue;
                        }
                        forward.client = std::make_unique<chast::PortForwardClient>(*session);
                        uint16_t bound = 0;
                        const chast::Error started = forward.client->start(
                            forward.rule_id, command.remote_host, command.remote_port,
                            command.local_port, &bound);
                        if (!started.ok()) {
                            forward.client.reset();
                            forward.local_port = 0;
                            forward.error = "listener: " + started.to_string();
                            std::fprintf(stderr, "viewer: port forward: %s\n",
                                         started.to_string().c_str());
                            forwards.push_back(std::move(forward));
                            continue;
                        }
                        forward.local_port = bound;
                        std::fprintf(stderr,
                                     "viewer: port forward rule %u listening on "
                                     "127.0.0.1:%u -> %s:%u\n",
                                     static_cast<unsigned>(forward.rule_id),
                                     static_cast<unsigned>(bound),
                                     command.remote_host.c_str(),
                                     static_cast<unsigned>(command.remote_port));
                        forwards.push_back(std::move(forward));
                    }
                    publish_forwards();
                };

                auto poll_port_forward = [&] {
                    if (forwards.empty()) {
                        return;
                    }
                    const bool allowed =
                        (session->permissions() & chast::kPermissionPortForward) != 0;
                    for (ActiveForward& forward : forwards) {
                        if (!forward.client) {
                            continue;
                        }
                        if (!allowed) {
                            // A revocation closes every local port at once; the
                            // transport aborts whatever was still in flight.
                            forward.client->stop();
                            forward.client.reset();
                            forward.active = false;
                            forward.error = "the host revoked port forwarding";
                            continue;
                        }
                        for (const std::string& event : forward.client->pump()) {
                            std::fprintf(stderr, "viewer: %s\n", event.c_str());
                        }
                        forward.active = forward.client->running();
                        forward.connections = forward.client->connections();
                        forward.failures = forward.client->failures();
                        forward.bytes_up = forward.client->bytes_up();
                        forward.bytes_down = forward.client->bytes_down();
                        const std::string error = forward.client->error();
                        if (error != forward.error) {
                            forward.error = error;
                            if (!error.empty()) {
                                std::fprintf(stderr,
                                             "viewer: port forward rule %u: %s\n",
                                             static_cast<unsigned>(forward.rule_id),
                                             error.c_str());
                            }
                        }
                    }
                    publish_forwards();
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
                        // UAC/lock-screen capture pauses are announced by the
                        // host. Resume gets a fresh frame deadline instead of
                        // inheriting the time spent on the secure desktop.
                        last_frame = Clock::now();
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

                // Sends the newest follow toggles. One slot, like the display
                // pickers: the menu's last state is what matters.
                auto drain_display_follow = [&] {
                    std::optional<uint8_t> requested;
                    {
                        std::lock_guard<std::mutex> lock(follow_mutex_);
                        requested.swap(pending_follow_);
                    }
                    if (!requested) {
                        return;
                    }
                    chast::Error sent = session->request_display_follow(*requested);
                    if (sent.ok()) {
                        return;
                    }
                    ++display_follow_results;
                    display_follow_code = static_cast<int>(chast::DisplayFollowCode::Failed);
                    display_follow_error = "request: " + sent.to_string();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.display_follow_code = display_follow_code;
                        status_.display_follow_error = display_follow_error;
                        status_.display_follow_results = display_follow_results;
                    }
                    std::fprintf(stderr, "viewer: set display follow: %s\n",
                                 sent.to_string().c_str());
                };

                // Sends the newest pointer hint. Position updates are frequent,
                // so only the latest one is worth sending.
                auto drain_show_cursor = [&] {
                    std::optional<chast::ShowCursorHint> hint;
                    {
                        std::lock_guard<std::mutex> lock(cursor_hint_mutex_);
                        hint.swap(pending_show_cursor_);
                    }
                    if (!hint) {
                        return;
                    }
                    chast::Error sent = session->request_show_cursor(*hint);
                    if (!sent.ok()) {
                        std::fprintf(stderr, "viewer: show cursor hint: %s\n",
                                     sent.to_string().c_str());
                    }
                };

                // Adopts every follow state. The host reports the modes in
                // effect, not the modes requested, so a refusal or a
                // host-initiated disable returns the menu to reality.
                auto receive_display_follow = [&] {
                    chast::DisplayFollowState state;
                    while (session->take_display_follow_state(state)) {
                        ++display_follow_results;
                        display_follow_flags = state.flags;
                        display_follow_code = static_cast<int>(state.code);
                        display_follow_error =
                            state.code == chast::DisplayFollowCode::Ok
                                ? std::string()
                                : (state.reason.empty() ? follow_code_name(state.code)
                                                        : state.reason);
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.display_follow_flags = display_follow_flags;
                            status_.display_follow_code = display_follow_code;
                            status_.display_follow_error = display_follow_error;
                            status_.display_follow_results = display_follow_results;
                        }
                        std::fprintf(stderr, "viewer: display follow 0x%02x: %s\n",
                                     static_cast<unsigned>(display_follow_flags),
                                     display_follow_error.empty()
                                         ? "ok"
                                         : display_follow_error.c_str());
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
                        stream_quality_percent = result.quality_percent;
                        ++stream_option_results;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.stream_option_code = stream_option_code;
                            status_.stream_option_error = stream_option_error;
                            status_.stream_option_results = stream_option_results;
                            status_.stream_quality = stream_quality;
                            status_.stream_quality_percent = stream_quality_percent;
                            status_.codec = result.codec;
                            status_.chroma = result.chroma;
                            status_.framerate = result.fps;
                        }
                        std::string quality_text = stream_quality_name(result.quality);
                        if (result.quality == chast::StreamQuality::Custom) {
                            quality_text +=
                                " " + std::to_string(result.quality_percent) + "%";
                        }
                        std::fprintf(stderr,
                                     "viewer: stream options %s: %s, %u fps, quality %s%s%s\n",
                                     result.code == chast::StreamOptionsCode::Ok ? "accepted"
                                                                                 : "refused",
                                     codec_name(result.codec).c_str(), result.fps,
                                     quality_text.c_str(),
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
                        if (!update.control_only && !update.files_only &&
                            (update.width > kMaxFrameSide ||
                             update.height > kMaxFrameSide ||
                             static_cast<uint64_t>(update.width) * update.height >
                                 kMaxFramePixels)) {
                            fail("display: the host announced a picture larger than this "
                                 "viewer accepts");
                            return false;
                        }
                        const bool needs_decoder = update.codec != info.codec || update.chroma != info.chroma ||
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
                            status_.chroma = info.chroma;
                            status_.framerate = info.framerate;
                            status_.server_bitrate = info.bitrate;
                            status_.server_encoder = info.encoder;
                            status_.input_enabled = info.input_enabled;
                            status_.clipboard_enabled = info.clipboard_enabled;
                            status_.clipboard_direction = info.clipboard_direction;
                            status_.origin_x = info.origin_x;
                            status_.origin_y = info.origin_y;
                            status_.displays = info.displays;
                            status_.merged_layout = info.merged;
                            status_.terminal_persist_supported =
                                info.terminal_persist;
                        }
                        {
                            std::lock_guard<std::mutex> lock(input_mutex_);
                            input_enabled_ = info.input_enabled;
                        }
                        clipboard_enabled = info.clipboard_enabled;
                        clipboard_direction = info.clipboard_direction;
                        // StreamInfo carries the permission mask too (for
                        // example after the host rebuilt its capture under a
                        // reduced grant), so adopt it before announcing.
                        apply_permissions(session->permissions());
                        std::fprintf(stderr,
                                     "viewer: stream is now %s %ux%u at %d,%d\n",
                                     codec_name(info.codec).c_str(), info.width, info.height,
                                     info.origin_x, info.origin_y);
                        if (!needs_decoder || multiple_displays) {
                            continue;
                        }
                        decoder.reset();
                        decoder_config.codec = info.codec;
                        decoder_config.chroma = info.chroma;
                        decoder = decoder_factory_(decoder_config, &error);
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

                // The UI fetches tickets, but only this thread may talk to the
                // QUIC client. A refusal is final at the host, which closes the
                // session; a transport failure is left to the lease deadline.
                auto drain_ticket_renew = [&] {
                    std::optional<std::vector<uint8_t>> pending;
                    {
                        std::lock_guard<std::mutex> lock(ticket_mutex_);
                        pending.swap(pending_ticket_renew_);
                    }
                    if (!pending || pending->empty()) {
                        return;
                    }
                    const chast::Error renewed = session->renew_access_ticket(
                        *pending, std::chrono::seconds(3));
                    if (renewed.ok()) {
                        std::fprintf(stderr, "viewer: access ticket renewed\n");
                    } else {
                        std::fprintf(stderr, "viewer: access ticket renewal failed: %s\n",
                                     renewed.to_string().c_str());
                    }
                };

                while (!stop_requested_.load(std::memory_order_acquire)) {
                    drain_ticket_renew();
                    if (!poll_displays()) break;


                    if (!apply_stream_updates()) {
                        break;
                    }
                    receive_permissions();
                    poll_display_control();
                    drain_privacy();
                    receive_privacy();
                    drain_sas();
                    receive_sas();
                    drain_host_action();
                    receive_host_action();
                    drain_remote_config();
                    receive_remote_config();
                    drain_os_login();
                    receive_os_login();
                    drain_elevation();
                    drain_session_requests();
                    receive_session_answers();
                    receive_chat();
                    drain_chat();
                    poll_terminal();
                    drain_terminal();
                    poll_camera();
                    drain_camera();
                    drain_port_forward();
                    poll_port_forward();
                    receive_desktop();
                    receive_display_switch();
                    receive_display_modes();
                    receive_display_mode_results();
                    receive_display_follow();
                    receive_stream_options();
                    drain_display_switch();
                    drain_display_modes();
                    drain_display_follow();
                    drain_show_cursor();
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
                            }
                        }
                    }
                    drain_input();
                    pump_clipboard();
                    poll_local_clipboard_files();
                    poll_clipboard();
                    receive_clipboard();
                    if (upload_cancel_.exchange(false)) {
                        purge_uploads("the viewer cancelled the upload", true);
                    }
                    drive_file_upload();
                    drain_file_conflicts();
                    // Browse answers are adopted before file offers: a walk's
                    // download answer names the transfer whose offer follows,
                    // and the offer must already know its mirrored directory.
                    drain_file_browse();
                    receive_file_browse();
                    drain_downloads();
                    step_clipboard_files();
                    if (download_cancel_.exchange(false)) {
                        cancel_download();
                    }
                    drain_download_cancels();
                    step_folder_download();
                    receive_cursor();
                    drain_audio();
                    drain_microphone();
                    drain_voice();
                    receive_voice();
                    chast::FrameHeader header;
                    std::vector<uint8_t> payload;
                    FileUpload::Stage upload_stage = FileUpload::Stage::kDone;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        upload_stage = uploads_.empty() ? FileUpload::Stage::kDone
                                                        : uploads_.front().stage;
                    }
                    const bool file_active = upload_stage == FileUpload::Stage::kOffered ||
                                             upload_stage == FileUpload::Stage::kSending ||
                                             upload_stage == FileUpload::Stage::kFinishing;
                    const bool fast_media_poll =
                        (permissions & (chast::kPermissionInput |
                                        chast::kPermissionClipboard)) != 0 ||
                        file_active || audio_announced || microphone_enabled ||
                        microphone_wanted_.load(std::memory_order_relaxed) ||
                        voice_wanted_.load(std::memory_order_relaxed) || voice_active ||
                        voice_pending || !forwards.empty() || terminals_active() ||
                        camera_active || multiple_displays;
                    if (keyframe_wanted_.exchange(false, std::memory_order_relaxed)) {
                        request_keyframe();
                    }
                    chast::Error received_frame = chast::Error::success();
                    if (control_only) {
                        // No media stream to wait on: the loop polls on short
                        // sleeps so file requests keep flowing, and the
                        // transport tells when the viewer left.
                        std::this_thread::sleep_for(kMediaPoll);
                        if (session->video_finished() || !session->peer_alive()) {
                            video_finished = true;
                            break;
                        }
                        received_frame =
                            chast::Error(chast::Status::WaitTimeout, "no media stream");
                    } else {
                        received_frame = session->receive_frame(
                            header, payload, fast_media_poll ? kMediaPoll : kReceiveTimeout);
                    }
                    // Audio and microphone packets arrive on their own streams
                    // and never wake receive_frame(), so drain them as soon as
                    // the wait ends.
                    drain_audio();
                    drain_microphone();
                    drain_voice();
                    receive_voice();
                    receive_permissions();
                    poll_display_control();
                    drain_privacy();
                    receive_privacy();
                    drain_sas();
                    receive_sas();
                    drain_host_action();
                    receive_host_action();
                    drain_remote_config();
                    receive_remote_config();
                    drain_os_login();
                    receive_os_login();
                    drain_elevation();
                    drain_session_requests();
                    receive_session_answers();
                    receive_chat();
                    drain_chat();
                    poll_terminal();
                    drain_terminal();
                    poll_camera();
                    drain_camera();
                    drain_port_forward();
                    poll_port_forward();
                    receive_desktop();
                    receive_display_switch();
                    receive_display_modes();
                    receive_display_mode_results();
                    receive_display_follow();
                    receive_stream_options();
                    const auto now = Clock::now();

                    if (!poll_displays()) break;
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
                        if (!control_only && !multiple_displays && desktop == chast::DesktopKind::Default &&
                            now - last_frame >= kFrameStall) {
                            fail("no frames arrived for 20 s");
                            break;
                        }
                        if (!control_only && !multiple_displays && sink_ != nullptr && !sink_->error().empty()) {
                            fail("display: " + sink_->error());
                            break;
                        }
                        sync_status(false);
                        if (!multiple_displays) {
                            ProcessScreenshot(session.get(), [this](const std::string& directory) {
                                SaveScreenshotFromSink(sink_, directory);
                            });
                        }
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
                            frame_age_valid = true;
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

                    if (multiple_displays) {
                        std::optional<uint32_t> record_target;
                        { std::lock_guard lock(record_mutex_); record_target = record_stream_; }
                        if (record_target && *record_target == header.stream_id &&
                            chast::crc32(payload.data(), payload.size()) == header.payload_crc32) {
                            for (const auto& stream : display_receiver.Snapshot()) {
                                const auto& described = stream.stream.info;
                                if (stream.stream.stream_id == header.stream_id && header.codec == described.codec && header.chroma == described.chroma &&
                                    header.width == described.width && header.height == described.height) {
                                    record_picture(header, payload, described);
                                    break;
                                }
                            }
                        }
                        display_receiver.Push(header, std::move(payload));
                        sync_status(false);
                        continue;
                    }

                    if (header.codec != info.codec || header.chroma != info.chroma || header.width != info.width ||
                        header.height != info.height) {
                        // The description and the first picture of a rebuilt
                        // stream travel on different QUIC streams, so the
                        // picture can win the race. Look for the update before
                        // treating the header as a violation.
                        if (!apply_stream_updates()) {
                            break;
                        }
                    }
                    if (header.codec != info.codec || header.chroma != info.chroma || header.width != info.width ||
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

                    record_picture(header, payload, info);

                    frames.clear();
                    chast::Error decode_error = decoder->decode(
                        payload.data(), payload.size(), header.pts_ms, header.keyframe, frames);
                    if (!decode_error.ok()) {
                        const bool display_failure =
                            sink_ != nullptr &&
                            decode_error.message().rfind("display: ", 0) == 0;
                        if (display_failure && sink_->fatal() && display_rebuilds < 1) {
                            // A reset or removed adapter took the device under
                            // the sink. Rebuild both the sink device and the
                            // decoder once; the Flutter texture was registered
                            // on Flutter's own device, so it survives.
                            ++display_rebuilds;
                            std::fprintf(stderr,
                                         "viewer: display device lost (%s); rebuilding\n",
                                         sink_->error().c_str());
                            if (sink_->Reset()) {
                                decoder.reset();
                                decoder_config.d3d11_device = sink_->device();
                                std::string rebuild_error;
                                decoder =
                                    decoder_factory_(decoder_config, &rebuild_error);
                                if (decoder) {
                                    {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        status_.decoder = decoder->implementation();
                                    }
                                    std::fprintf(stderr, "viewer: decoder %s\n",
                                                 decoder->implementation().c_str());
                                    sync_status(false);
                                    request_keyframe();
                                    continue;
                                }
                                fail("display: the decoder could not be rebuilt after "
                                     "the display device was lost: " +
                                     rebuild_error);
                                break;
                            }
                            fail("display: " + sink_->error());
                            break;
                        }
                        if (display_failure) {
                            fail(decode_error.message());
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
                    ProcessScreenshot(session.get(), [this](const std::string& directory) {
                        SaveScreenshotFromSink(sink_, directory);
                    });
                }
                // A clipboard file list cannot finish on a closed session;
                // files that never landed are removed from the staging folder.
                if (clipboard_files.stage != ClipboardFiles::Stage::kIdle) {
                    if (clipboard_files.stage == ClipboardFiles::Stage::kReceiving) {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (download_.receiver) {
                            download_.receiver->abort(true);
                            download_.receiver.reset();
                        }
                        download_.stage = FileDownload::Stage::kFailed;
                        download_.error =
                            "the session ended before the clipboard files finished";
                    }
                    discard_clipboard_files();
                }
            }
        }
    }

    // Finalise any recording before the session teardown; a file that fails
    // here still reports why through ViewerStatus::record_error.
    close_recording();

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
    // Bumping the epoch also voids any folder walk or hash still in flight.
    uint32_t cancelled_upload = 0;
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        ++upload_epoch_;
        // A result that arrived just before the stream ended still counts as
        // finished; only the entries that cannot continue are failed.
        while (!uploads_.empty()) {
            const FileUpload::Stage stage = uploads_.front().stage;
            if (stage != FileUpload::Stage::kDone &&
                stage != FileUpload::Stage::kFailed) {
                break;
            }
            const bool success = stage == FileUpload::Stage::kDone;
            if (!success) {
                ++file_failures;
            } else {
                ++files_sent;
            }
            if (!uploads_.front().clipboard) {
                record_completed_file(true, uploads_.front().name, uploads_.front().size,
                                      success, uploads_.front().error);
            }
            last_upload_ = std::move(uploads_.front());
            uploads_.pop_front();
        }
        if (!uploads_.empty()) {
            for (const FileUpload& upload : uploads_) {
                if (upload.stage == FileUpload::Stage::kOffered ||
                    upload.stage == FileUpload::Stage::kSending ||
                    upload.stage == FileUpload::Stage::kFinishing) {
                    cancelled_upload = upload.transfer_id;
                    break;
                }
            }
            // Every entry that cannot continue is one failed file in the
            // audit, not a single aggregate failure.
            for (const FileUpload& upload : uploads_) {
                if (!upload.clipboard) {
                    record_completed_file(true, upload.name, upload.size, false,
                                          "the session ended before the file finished");
                }
            }
            last_upload_ = std::move(uploads_.front());
            last_upload_.stage = FileUpload::Stage::kFailed;
            last_upload_.error = "the session ended before the file finished";
            uploads_.clear();
        }
        pending_upload_roots_.clear();
        // A prompt cannot outlive the connection that carries its offer.
        conflicts_.clear();
        conflict_answers_.clear();
        conflict_policy_ = 0;
    }
    if (cancelled_upload != 0) {
        session->cancel_file_upload(cancelled_upload);
    }
    // A download that outlived the stream cannot finish on this connection.
    // Clipboard staging is handled with the rest of the clipboard state below.
    {
        bool failed_download = false;
        std::string download_name;
        uint64_t download_size = 0;
        {
            std::lock_guard<std::mutex> lock(file_mutex_);
            if (download_.stage == FileDownload::Stage::kReceiving ||
                download_.stage == FileDownload::Stage::kConflict) {
                if (!download_.clipboard) {
                    failed_download = true;
                    download_name = download_.name;
                    download_size = download_.size;
                    if (download_.receiver) {
                        download_.receiver->abort(true);
                        download_.receiver.reset();
                    }
                    download_.stage = FileDownload::Stage::kFailed;
                    download_.error = "the session ended before the download finished";
                }
            }
        }
        if (failed_download) {
            record_completed_file(false, download_name, download_size, false,
                                  "the session ended before the download finished");
        }
    }
    // A folder walk cannot continue on a closed session; nothing of it may
    // leak into the next connection either.
    {
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(browse_mutex_);
            active = folder_.stage == FolderDownload::Stage::kListing ||
                     folder_.stage == FolderDownload::Stage::kDownloading;
            folder_ = FolderDownload{};
            folder_request_.reset();
            folder_transfers_.clear();
            ignored_transfers_.clear();
        }
        if (active) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.folder_download_active = false;
            ++status_.folder_download_answers;
            status_.folder_download_code = 1;
            status_.folder_download_error =
                "the session ended before the folder download finished";
        }
    }
    // The uplink cannot outlive the session: capture stops first, then the
    // stream ends so the host tears down its decoder and player. The echo
    // reference goes inactive with the playback it described.
    audio_reference->set_active(false);
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

    display_receiver.Reset();
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
