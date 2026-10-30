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
#include <utility>
#include <vector>

#include "mp4_recorder.h"

#include "chast/audio.h"
#include "chast/carrier.h"
#include "chast/clipboard.h"
#include "chast/decoder.h"
#include "chast/file_transfer.h"
#include "chast/identity.h"
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
constexpr auto kClipboardPollInterval = std::chrono::milliseconds(500);
// Virtual browse path that the host serves announced clipboard files from;
// the digits after the prefix index the FileList the host sent.
constexpr char kClipboardFilePathPrefix[] = ".chast-clipboard/";
// How long the host may take to answer a clipboard download request. The
// offer itself is not bounded: the host hashes the file first.
constexpr auto kClipboardAnswerTimeout = std::chrono::seconds(10);

// A recording file name that is unlikely to collide: the local wall clock.
// Mp4Recorder appends "-N" to the stem when a size change rolls over.
std::string record_file_path(const std::string& directory) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return (std::filesystem::path(directory) / ("chast-" + std::string(stamp) + ".mp4"))
        .string();
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
// A bulk upload is paced far below the loopback ceiling: unpaced, a 1 GiB file
// fills the 32 MiB send queue and the viewer loop stalls on it, which costs the
// video stream almost all of its frames. The burst lets one pass move a few
// chunks without being reduced to a trickle by the poll cadence.
constexpr double kFileUploadBytesPerSecond = 32.0 * 1024.0 * 1024.0;
constexpr double kFileUploadBurst = 256.0 * 1024.0;
constexpr size_t kMaxPendingInput = 256;
// The viewer runs one console per session; the id only has to be stable
// within that session.
constexpr uint32_t kViewerTerminalId = 1;
// Safety valve for a folder that expands into a huge tree; the UI would
// otherwise build an unbounded queue on the session thread.
constexpr size_t kMaxQueuedUploads = 10000;

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

std::string utf8_from_path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace

ViewerEngine::ViewerEngine(FrameSink* sink, FrameSink* camera_sink)
    : sink_(sink), camera_sink_(camera_sink) {}

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
                         bool file_transfer_only) {
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
        status_.host = host;
        status_.port = port;
        status_.state = ViewerState::kConnecting;
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
    // Browse answers belong to the session that was asked; a listing from the
    // previous host must not appear in the next one's browser.
    {
        std::lock_guard<std::mutex> lock(browse_mutex_);
        pending_browse_.clear();
        browse_paths_.clear();
        newest_file_list_.reset();
        next_browse_id_ = 1;
    }
    {
        std::lock_guard<std::mutex> lock(privacy_mutex_);
        pending_privacy_.reset();
    }
    // A SAS request queued for a session that ended must not fire into the
    // next one.
    pending_sas_.store(false, std::memory_order_release);
    // Port forwarding belongs to one session too; the listener lives on the
    // session thread and goes away with it.
    {
        std::lock_guard<std::mutex> lock(port_forward_mutex_);
        pending_port_forward_.reset();
    }
    // Chat belongs to one viewer session in both directions.
    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        pending_chat_.clear();
        received_chat_.clear();
    }
    // A console belongs to one session too; neither its commands nor its
    // unread output may leak into the next connection.
    {
        std::lock_guard<std::mutex> lock(terminal_mutex_);
        pending_terminal_.clear();
        pending_terminal_bytes_ = 0;
        terminal_output_.clear();
        terminal_output_bytes_ = 0;
        terminal_output_ended_ = true;
    }
    // A camera request belongs to one session; the host's ids are only valid
    // for its own list.
    {
        std::lock_guard<std::mutex> lock(camera_mutex_);
        pending_camera_.clear();
    }
    // Microphone capture is a per-session privacy decision: every session
    // starts with the uplink off, while the chosen device is remembered.
    microphone_wanted_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ViewerEngine::Run, this, host, port, display,
                          std::move(pinned_fingerprint), allow_unverified,
                          std::move(access_token), std::move(password),
                          std::move(totp), trust_device, transport,
                          std::move(carrier_host), carrier_port,
                          std::move(carrier_host_id), std::move(carrier_token),
                          std::move(audit_ref), file_transfer_only);
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

}  // namespace

bool ViewerEngine::RequestFileList(std::string directory) {
    if (!running_.load(std::memory_order_acquire)) {
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
    browse_paths_[command.request_id] = command.path;
    pending_browse_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::RequestFileDownload(std::string path) {
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
               op != chast::FileOperation::Remove) {
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

bool ViewerEngine::RequestBatchDownload(std::vector<DownloadRoot> roots) {
    return QueueDownload(std::move(roots), true);
}

bool ViewerEngine::RequestDirectoryDownload(std::string path) {
    std::vector<DownloadRoot> roots;
    DownloadRoot root;
    root.path = std::move(path);
    root.directory = true;
    roots.push_back(std::move(root));
    return QueueDownload(std::move(roots), false);
}

bool ViewerEngine::QueueDownload(std::vector<DownloadRoot> roots, bool batch) {
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
    folder_request_ = FolderRequest{std::move(roots), batch};
    return true;
}

void ViewerEngine::CancelDirectoryDownload() {
    std::lock_guard<std::mutex> lock(browse_mutex_);
    if (folder_.stage == FolderDownload::Stage::kListing ||
        folder_.stage == FolderDownload::Stage::kDownloading) {
        folder_.cancel = true;
    }
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
    }
    if (!directory.empty()) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_dir_ = std::move(directory);
    }
    record_wanted_.store(enabled, std::memory_order_relaxed);
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

bool ViewerEngine::TakeChat(std::string& text) {
    std::lock_guard<std::mutex> lock(chat_mutex_);
    if (received_chat_.empty()) {
        return false;
    }
    text = std::move(received_chat_.front());
    received_chat_.pop_front();
    return true;
}

bool ViewerEngine::OpenTerminal(uint16_t columns, uint16_t rows) {
    if (!chast::valid_terminal_size(columns, rows)) {
        return false;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionTerminal) == 0) {
            return false;
        }
        if (status_.terminal_phase == TerminalPhase::kRequesting ||
            status_.terminal_phase == TerminalPhase::kOpen) {
            return false;
        }
        status_.terminal_phase = TerminalPhase::kRequesting;
        status_.terminal_code = 0;
        status_.terminal_reason.clear();
        status_.terminal_error.clear();
        status_.terminal_columns = columns;
        status_.terminal_rows = rows;
        ++status_.terminal_updates;
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    // The previous console's bytes must be on the panel before its phase can
    // mean anything for a new one.
    if (!terminal_output_ended_) {
        return false;
    }
    // Leftovers from the previous console would be printed as if they
    // belonged to this one.
    terminal_output_.clear();
    terminal_output_bytes_ = 0;
    terminal_output_ended_ = false;
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kOpen;
    command.columns = columns;
    command.rows = rows;
    pending_terminal_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::SendTerminalInput(const std::string& bytes) {
    if (bytes.empty() || !running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.terminal_phase != TerminalPhase::kOpen) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    if (pending_terminal_bytes_ + bytes.size() >
        chast::kTerminalInputHighWaterBytes) {
        return false;
    }
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kInput;
    command.bytes = bytes;
    pending_terminal_bytes_ += command.bytes.size();
    pending_terminal_.push_back(std::move(command));
    return true;
}

bool ViewerEngine::ResizeTerminal(uint16_t columns, uint16_t rows) {
    if (!chast::valid_terminal_size(columns, rows) ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.terminal_phase != TerminalPhase::kOpen &&
            status_.terminal_phase != TerminalPhase::kRequesting) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    // A resize that has not been sent yet is replaced: the console only needs
    // where the panel is now.
    for (TerminalCommand& command : pending_terminal_) {
        if (command.kind == TerminalCommand::Kind::kResize) {
            command.columns = columns;
            command.rows = rows;
            return true;
        }
    }
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kResize;
    command.columns = columns;
    command.rows = rows;
    pending_terminal_.push_back(std::move(command));
    return true;
}

void ViewerEngine::CloseTerminal() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.terminal_phase == TerminalPhase::kRequesting ||
            status_.terminal_phase == TerminalPhase::kOpen) {
            status_.terminal_phase = TerminalPhase::kClosed;
            status_.terminal_code = 0;
            status_.terminal_reason.clear();
            status_.terminal_error.clear();
            ++status_.terminal_updates;
        }
    }
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    // Input queued after this close is pointless; the close itself stays.
    for (auto it = pending_terminal_.begin(); it != pending_terminal_.end();) {
        if (it->kind == TerminalCommand::Kind::kInput) {
            pending_terminal_bytes_ -= std::min(pending_terminal_bytes_, it->bytes.size());
            it = pending_terminal_.erase(it);
        } else {
            ++it;
        }
    }
    TerminalCommand command;
    command.kind = TerminalCommand::Kind::kClose;
    pending_terminal_.push_back(std::move(command));
}

bool ViewerEngine::TakeTerminalOutput(std::vector<uint8_t>& data, bool& ended) {
    std::lock_guard<std::mutex> lock(terminal_mutex_);
    if (terminal_output_.empty()) {
        ended = terminal_output_ended_;
        return false;
    }
    data = std::move(terminal_output_.front());
    terminal_output_.pop_front();
    terminal_output_bytes_ -=
        std::min(terminal_output_bytes_, data.size());
    ended = terminal_output_ended_ && terminal_output_.empty();
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

bool ViewerEngine::StartPortForward(uint16_t local_port,
                                    const std::string& remote_host,
                                    uint16_t remote_port) {
    if (!running_.load(std::memory_order_acquire) || remote_host.empty() ||
        remote_port == 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.state != ViewerState::kStreaming ||
            (status_.permissions & chast::kPermissionPortForward) == 0) {
            return false;
        }
    }
    PortForwardCommand command;
    command.local_port = local_port;
    command.remote_host = remote_host;
    command.remote_port = remote_port;
    std::lock_guard<std::mutex> lock(port_forward_mutex_);
    pending_port_forward_ = std::move(command);
    return true;
}

void ViewerEngine::StopPortForward() {
    PortForwardCommand command;
    command.stop = true;
    std::lock_guard<std::mutex> lock(port_forward_mutex_);
    pending_port_forward_ = std::move(command);
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
                       std::string totp, bool trust_device, ViewerTransport transport,
                       std::string carrier_host, uint16_t carrier_port,
                       std::string carrier_host_id, std::string carrier_token,
                       std::string audit_ref, bool file_transfer_only) {
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
    uint64_t chat_sent = 0;
    uint64_t chat_received = 0;
    uint64_t chat_failures = 0;
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
    // Set from HelloAck: the host serves file transfer only, so the session
    // runs without decoder, sink or media waits.
    bool files_only = false;
    bool logged_frame = false;
    bool logged_drop = false;
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
            status_.audio_recoveries = audio_recoveries;
            status_.audio_recovering = audio_recovering;
            status_.audio_buffer_ms = audio_buffer_ms;
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
    // Declared after the session so its destructor (which tells the host to
    // drop the target) runs while the connection still exists.
    std::unique_ptr<chast::PortForwardClient> port_forward;

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
        client_config.password = password;
        client_config.totp = totp;
        client_config.device_identity = device_identity_.get();
        client_config.trust_device = trust_device;
        client_config.audit_ref = audit_ref;
        client_config.session_mode = file_transfer_only
                                         ? chast::SessionMode::FileTransfer
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
            // The host decides: it answers a files-only request with an empty
            // screen description and the flag, and the UI leaves the picture
            // view for the file browser.
            files_only = info.files_only;
            display_width = info.width;
            display_height = info.height;
            display_source_width = info.source_width;
            display_source_height = info.source_height;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.files_only = files_only;
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
                status_.merged_layout = info.merged;
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
                const bool forward_allowed =
                    (mask & chast::kPermissionPortForward) != 0;
                const bool terminal_allowed =
                    (mask & chast::kPermissionTerminal) != 0;
                const bool camera_allowed = (mask & chast::kPermissionCamera) != 0;
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
                    status_.port_forward_enabled = forward_allowed;
                    status_.terminal_supported = terminal_allowed;
                    status_.camera_supported = camera_allowed;
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

            // Kept outside the decoder gate below: apply_stream_updates
            // rebuilds the decoder from this config when the host changes
            // its picture size or codec.
            chast::DecoderConfig decoder_config;
            std::unique_ptr<chast::IVideoDecoder> decoder;
            if (!files_only) {
                decoder_config.codec = info.codec;
                if (sink_ == nullptr || sink_->device() == nullptr) {
                    fail("display: " +
                         (sink_ != nullptr ? sink_->error()
                                           : std::string("the frame sink is not available")));
                }
                // Decode straight into a D3D11 texture and hand each picture
                // to the sink while the decoder still owns the surface: the
                // image never reaches system memory.
                decoder_config.d3d11_device =
                    sink_ != nullptr ? sink_->device() : nullptr;
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
                            std::fprintf(stderr, "viewer: first GPU frame %ux%u\n",
                                         frame.width, frame.height);
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
                decoder = decoder_config.d3d11_device != nullptr
                              ? chast::create_decoder(decoder_config, &error)
                              : nullptr;
                if (!decoder && failure.empty()) {
                    fail("decoder (" + codec_name(info.codec) + "): " + error);
                }
            }
            if (files_only || decoder) {
                if (decoder) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.decoder = decoder->implementation();
                    }
                    std::fprintf(stderr, "viewer: decoder %s\n",
                                 decoder->implementation().c_str());
                } else {
                    std::fprintf(stderr,
                                 "viewer: file transfer only; no screen stream follows\n");
                }
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
                // Hash of the host's latest FileList; unlike clipboard_hash it
                // is not touched by outbound polls, so a local copy on the
                // viewer cannot make a downloading list look stale.
                uint64_t clipboard_peer_file_list = 0;
                // Viewer -> host clipboard files (protocol v34): identity of the
                // list last announced or baselined, whether the local clipboard
                // currently owns files (the rich poll then stays out of the
                // way), and one log line per distinct collection refusal.
                uint64_t clipboard_out_identity = 0;
                bool clipboard_has_files = false;
                bool clipboard_out_was_allowed = false;
                auto next_clipboard_file_poll = Clock::now();
                std::string last_clipboard_file_error;
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
                        if (entries.empty() || !clipboard_allowed()) {
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
                        const std::filesystem::path directory =
                            base / ("chast-clipboard-" +
                                    std::to_string(
                                        static_cast<unsigned long long>(std::time(nullptr))) +
                                    "-" + std::to_string(++clipboard_staging_seq));
                        std::filesystem::create_directories(directory, ec);
                        if (ec) {
                            ++clipboard_failed;
                            std::fprintf(stderr,
                                         "viewer: clipboard files: cannot create the "
                                         "staging folder: %s\n",
                                         ec.message().c_str());
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
                    if (!clipboard_allowed()) {
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
                        std::function<bool(const std::vector<std::string>&)> sink;
                        {
                            std::lock_guard<std::mutex> lock(clipboard_sink_mutex_);
                            sink = clipboard_file_sink_;
                        }
                        std::string write_error;
                        const bool written =
                            sink ? sink(clipboard_files.paths)
                                 : chast::write_clipboard_files(clipboard_files.paths,
                                                                &write_error);
                        if (!written) {
                            if (write_error.empty()) {
                                write_error = "the clipboard sink rejected the list";
                            }
                            ++clipboard_failed;
                            std::fprintf(stderr,
                                         "viewer: clipboard files: cannot write the "
                                         "clipboard: %s\n",
                                         write_error.c_str());
                            discard_clipboard_files();
                            return;
                        }
                        ++clipboard_received;
                        clipboard_received_formats =
                            1u << static_cast<uint32_t>(chast::ClipboardFormat::FileList);
                        // These files are on the local clipboard now: the
                        // reverse poll must see its own write as the baseline
                        // instead of copying the list back to the host.
                        clipboard_out_identity = chast::clipboard_file_identity_hash(
                            clipboard_files.entries);
                        std::fprintf(stderr,
                                     "viewer: clipboard files written (%zu top-level "
                                     "item(s), formats 0x%x)\n",
                                     clipboard_files.paths.size(),
                                     clipboard_received_formats);
                        clipboard_files = ClipboardFiles{};
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
                    // A file copy owns the clipboard; a stale text hash would
                    // otherwise announce a rich item beside it. The next rich
                    // read after the files are gone is a fresh change again.
                    if (clipboard_has_files) {
                        clipboard_hash = 0;
                        return;
                    }
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
                        clipboard_hash = hash;
                        clipboard_peer_file_list = hash;
                        start_clipboard_files(std::move(entries), hash);
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
                    clipboard_peer_file_list = 0;
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
                auto purge_uploads = [&](const std::string& message) {
                    uint32_t active_id = 0;
                    std::string name;
                    size_t dropped = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        // A folder walk or hash still in flight must not
                        // repopulate the queue after the purge.
                        ++upload_epoch_;
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
                            if (!reported) {
                                reported = true;
                                name = it->name;
                                FileUpload finished = std::move(*it);
                                finished.stage = FileUpload::Stage::kFailed;
                                finished.error = message;
                                last_upload_ = std::move(finished);
                                ++file_failures;
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
                auto read_local_clipboard_files =
                    [&](std::vector<chast::ClipboardFileEntry>& entries,
                        std::vector<std::string>& paths) {
                        entries.clear();
                        paths.clear();
                        std::vector<std::string> found;
                        std::function<bool(std::vector<std::string>&)> source;
                        {
                            std::lock_guard<std::mutex> lock(clipboard_sink_mutex_);
                            source = clipboard_file_source_;
                        }
                        if (source) {
                            if (!source(found)) {
                                return false;
                            }
                        } else {
                            std::string read_error;
                            if (!chast::read_clipboard_files(found, &read_error)) {
                                return false;
                            }
                        }
                        std::string collect_error;
                        if (!chast::collect_clipboard_files(found, entries, paths,
                                                            &collect_error)) {
                            // One line per distinct refusal: the operator can
                            // copy again, but an unchanged unsendable list must
                            // not repaint the log twice a second.
                            if (collect_error != last_clipboard_file_error) {
                                last_clipboard_file_error = collect_error;
                                std::fprintf(stderr, "viewer: clipboard files: %s\n",
                                             collect_error.c_str());
                            }
                            return false;
                        }
                        last_clipboard_file_error.clear();
                        return true;
                    };
                auto poll_local_clipboard_files = [&] {
                    const auto now = Clock::now();
                    if (!clipboard_allowed()) {
                        clipboard_out_was_allowed = false;
                        clipboard_out_identity = 0;
                        clipboard_has_files = false;
                        return;
                    }
                    std::vector<chast::ClipboardFileEntry> entries;
                    std::vector<std::string> paths;
                    if (!clipboard_out_was_allowed) {
                        clipboard_out_was_allowed = true;
                        clipboard_has_files = read_local_clipboard_files(entries, paths);
                        clipboard_out_identity =
                            clipboard_has_files
                                ? chast::clipboard_file_identity_hash(entries)
                                : 0;
                        next_clipboard_file_poll = now + kClipboardPollInterval;
                        return;
                    }
                    if (now < next_clipboard_file_poll) {
                        return;
                    }
                    next_clipboard_file_poll = now + kClipboardPollInterval;
                    clipboard_has_files = read_local_clipboard_files(entries, paths);
                    if (!clipboard_has_files) {
                        clipboard_out_identity = 0;
                        return;
                    }
                    const uint64_t identity = chast::clipboard_file_identity_hash(entries);
                    if (identity == 0 || identity == clipboard_out_identity) {
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
                    // The host expects the offers in list order and refuses the
                    // whole list on the first mismatch, so all entries must fit
                    // in the queue before the list is announced.
                    size_t room = 0;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        size_t ordinary = 0;
                        for (const FileUpload& upload : uploads_) {
                            if (!upload.clipboard) {
                                ++ordinary;
                            }
                        }
                        room = kMaxQueuedUploads > ordinary ? kMaxQueuedUploads - ordinary : 0;
                    }
                    if (entries.size() > room) {
                        clipboard_out_identity = identity;
                        std::fprintf(stderr,
                                     "viewer: clipboard files: the upload queue is full; "
                                     "the list was not announced\n");
                        return;
                    }
                    drop_clipboard_uploads("the local clipboard changed");
                    bool queued = false;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        if (uploads_.size() + entries.size() <= kMaxQueuedUploads) {
                            for (size_t i = 0; i < entries.size(); ++i) {
                                const std::string& rel = entries[i].rel_path;
                                const size_t slash = rel.rfind('/');
                                FileUpload upload;
                                upload.path = paths[i];
                                upload.name = entries[i].name;
                                if (slash != std::string::npos) {
                                    upload.relative_path = rel.substr(0, slash);
                                }
                                upload.transfer_id = next_file_id_++;
                                upload.clipboard = true;
                                uploads_.push_back(std::move(upload));
                            }
                            queued = true;
                        }
                    }
                    if (!queued) {
                        // A folder walk filled the queue between the check and
                        // the enqueue; the current host-side list stays valid.
                        clipboard_out_identity = identity;
                        std::fprintf(stderr,
                                     "viewer: clipboard files: the upload queue is full; "
                                     "the list was not announced\n");
                        return;
                    }
                    file_work_cv_.notify_all();
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
                    // Files own the clipboard now; the next rich item counts as
                    // a fresh change even if it matches the old text.
                    clipboard_hash = 0;
                    clipboard_sent_formats =
                        1u << static_cast<uint32_t>(chast::ClipboardFormat::FileList);
                    ++clipboard_seq;
                    ++clipboard_sent;
                    std::fprintf(stderr, "viewer: clipboard files announced (%zu file(s))\n",
                                 entries.size());
                };

                auto drive_file_upload = [&] {
                    // A finished head hands over to last_upload_, which keeps
                    // the last outcome visible after the queue drains.
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        while (!uploads_.empty()) {
                            const FileUpload::Stage stage = uploads_.front().stage;
                            if (stage != FileUpload::Stage::kDone &&
                                stage != FileUpload::Stage::kFailed) {
                                break;
                            }
                            if (stage == FileUpload::Stage::kFailed) {
                                ++file_failures;
                            } else {
                                ++files_sent;
                            }
                            last_upload_ = std::move(uploads_.front());
                            uploads_.pop_front();
                        }
                    }
                    // The clipboard half of the queue has its own gate, and an
                    // incoming host list supersedes whatever the local poll
                    // queued: the host's clipboard is the current one.
                    if (!clipboard_allowed()) {
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
                    bool clipboard_transfer = false;
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
                        clipboard_transfer =
                            clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                            clipboard_files.transfer_id == transfer_id;
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

                auto drain_downloads = [&] {
                    bool transfer_receiving = false;
                    bool clipboard_receiving = false;
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        transfer_receiving =
                            download_.stage == FileDownload::Stage::kReceiving;
                        clipboard_receiving =
                            transfer_receiving &&
                            clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                            clipboard_files.transfer_id == download_.transfer_id;
                    }
                    if (transfer_receiving && !clipboard_receiving &&
                        (permissions & chast::kPermissionFileTransfer) == 0) {
                        download_fail("the host revoked file transfer", false);
                    }
                    if (transfer_receiving && clipboard_receiving && !clipboard_allowed()) {
                        download_fail("clipboard sync is disabled", false);
                    }
                    chast::FileOffer offer;
                    while (session->take_file_offer(offer)) {
                        const bool clipboard_transfer =
                            clipboard_files.stage == ClipboardFiles::Stage::kOffering &&
                            clipboard_files.transfer_id == offer.transfer_id;
                        if ((permissions & chast::kPermissionFileTransfer) == 0 &&
                            !clipboard_transfer) {
                            continue;
                        }
                        if (clipboard_transfer && !clipboard_allowed()) {
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
                            busy = download_.stage == FileDownload::Stage::kReceiving;
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
                            continue;
                        }
                        chast::FileReceiveOptions options;
                        options.directory = clipboard_transfer ? clipboard_files.directory
                                                               : download_directory;
                        options.name = offer.name;
                        options.relative_path =
                            clipboard_transfer ? std::string() : offer.relative_path;
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
                                options.relative_path = it->second;
                            }
                        }
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
                            if (clipboard_transfer) {
                                clip_files_fail("cannot receive '" + offer.name +
                                                "': " + receive_error);
                                continue;
                            }
                            bool walk_transfer = false;
                            {
                                std::lock_guard<std::mutex> lock(browse_mutex_);
                                walk_transfer = folder_transfers_.erase(offer.transfer_id) > 0;
                            }
                            if (walk_transfer) {
                                finish_folder_download(1, receive_error);
                            }
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
                    {
                        std::lock_guard<std::mutex> lock(file_mutex_);
                        download_.receiver.reset();
                        download_.stage = FileDownload::Stage::kDone;
                        download_.path = final_path;
                        download_.error.clear();
                    }
                    if (clipboard_files.stage == ClipboardFiles::Stage::kReceiving &&
                        clipboard_files.transfer_id == transfer_id &&
                        clipboard_files.next < clipboard_files.entries.size()) {
                        // CF_HDROP lists what the operator sees: a flat file
                        // by itself, a copied folder once for all of its files.
                        const std::string& rel =
                            clipboard_files.entries[clipboard_files.next].rel_path;
                        const size_t slash = rel.find('/');
                        if (slash == std::string::npos) {
                            clipboard_files.paths.push_back(final_path);
                        } else {
                            const std::string top_path = utf8_from_path(
                                path_from_utf8(clipboard_files.directory) /
                                path_from_utf8(rel.substr(0, slash)));
                            if (std::find(clipboard_files.paths.begin(),
                                          clipboard_files.paths.end(),
                                          top_path) == clipboard_files.paths.end()) {
                                clipboard_files.paths.push_back(top_path);
                            }
                        }
                        ++clipboard_files.next;
                        clipboard_files.stage = ClipboardFiles::Stage::kRequesting;
                        clipboard_files.request_id = 0;
                        clipboard_files.transfer_id = 0;
                        clipboard_files.retry_after = Clock::now();
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

                // Sends queued browse requests in the order the operator made
                // them. A revoked permission drops them: the host would refuse
                // and the page is about to hide the browser anyway.
                auto drain_file_browse = [&] {
                    for (;;) {
                        if ((permissions & chast::kPermissionFileTransfer) == 0) {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            pending_browse_.clear();
                            browse_paths_.clear();
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
                        }
                        chast::Error sent =
                            command.kind == BrowseCommand::Kind::kDownload
                                ? session->request_file_download(command.request_id, command.path)
                            : command.kind == BrowseCommand::Kind::kOperation
                                ? session->request_file_op(command.request_id, command.operation,
                                                           command.path, command.new_name)
                                : session->request_file_list(command.request_id, command.path);
                        if (sent.ok()) {
                            continue;
                        }
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            browse_paths_.erase(command.request_id);
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
                                folder_.listing_id = 0;
                                folder_.listing_path.clear();
                                if (list.code != chast::FileCode::Ok) {
                                    walk_error = list.reason.empty() ? file_code_name(list.code)
                                                                     : list.reason;
                                } else if (list.truncated) {
                                    walk_error = "the folder has too many entries to download";
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
                                            if (entry.directory) {
                                                folder_.directories.push_back(child);
                                            } else {
                                                std::string local = folder_.name;
                                                if (!local_sub.empty()) {
                                                    local += "/" + local_sub;
                                                }
                                                folder_.files.emplace_back(child, local);
                                                ++folder_.files_total;
                                            }
                                        }
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
                        const std::string error =
                            list.code == chast::FileCode::Ok
                                ? std::string()
                                : (list.reason.empty() ? file_code_name(list.code) : list.reason);
                        const uint32_t entries = static_cast<uint32_t>(list.entries.size());
                        const bool truncated = list.truncated;
                        std::string directory;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            auto it = browse_paths_.find(list.request_id);
                            if (it != browse_paths_.end()) {
                                directory = it->second;
                                browse_paths_.erase(it);
                            }
                            newest_file_list_ = FileListAnswer{};
                            newest_file_list_->directory = directory;
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
                    walk.download_id = 0;
                    walk.download_path.clear();
                    walk.download_directory.clear();
                    walk.transfer_id = 0;
                    walk.current.clear();
                    walk.retry_after = now;
                    walk.transfer_deadline = now;
                    if (root.directory) {
                        walk.stage = FolderDownload::Stage::kListing;
                        walk.directories.push_back(walk.root);
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
                        bool start = false;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
                            if (folder_request_ &&
                                folder_.stage != FolderDownload::Stage::kListing &&
                                folder_.stage != FolderDownload::Stage::kDownloading) {
                                roots = std::move(folder_request_->roots);
                                batch = folder_request_->batch;
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
                                            stage == FileDownload::Stage::kReceiving) {
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
                                {
                                    std::lock_guard<std::mutex> lock(file_mutex_);
                                    active = download_.stage == FileDownload::Stage::kReceiving &&
                                             download_.transfer_id == transfer;
                                    if (active) {
                                        if (download_.receiver) {
                                            download_.receiver->abort(false);
                                            download_.receiver.reset();
                                        }
                                        download_.stage = FileDownload::Stage::kFailed;
                                        download_.error = "cancelled by the operator";
                                    }
                                }
                                if (active) {
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
                        bool listed_all = false;
                        bool advanced = false;
                        std::string timed_out;
                        {
                            std::lock_guard<std::mutex> lock(browse_mutex_);
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
                                path = folder_.directories.front();
                                folder_.directories.pop_front();
                                request_id = next_browse_id_++;
                                folder_.listing_id = request_id;
                                folder_.listing_path = path;
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
                        chast::Error sent = session->request_file_list(request_id, path);
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
                                busy = download_.stage == FileDownload::Stage::kReceiving;
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

                // Remote terminal. The panel drives one console through the
                // command queue; this thread owns the session, so request,
                // input, resize and the stop all leave from here in order.
                // `terminal_active` means the session holds an entry for
                // kViewerTerminalId; `terminal_closing` means a stop went out
                // or came in and the remaining output is still draining.
                bool terminal_active = false;
                bool terminal_closing = false;
                uint64_t terminal_bytes = 0;

                auto set_terminal_phase = [&](TerminalPhase phase, int code,
                                              std::string reason, std::string error) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.terminal_phase = phase;
                    status_.terminal_code = code;
                    status_.terminal_reason = std::move(reason);
                    status_.terminal_error = std::move(error);
                    ++status_.terminal_updates;
                };
                auto terminal_phase_is = [&](TerminalPhase phase) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    return status_.terminal_phase == phase;
                };

                // Forgets the session-side entry once its output is done. The
                // UI keeps whatever was already handed over, and the phase it
                // sees next poll is the caller's.
                auto finish_terminal = [&] {
                    session->close_terminal(kViewerTerminalId);
                    terminal_active = false;
                    terminal_closing = false;
                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                    terminal_output_ended_ = true;
                };

                auto drain_terminal = [&] {
                    while (true) {
                        TerminalCommand command;
                        {
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            if (pending_terminal_.empty()) {
                                return;
                            }
                            command = std::move(pending_terminal_.front());
                            pending_terminal_.pop_front();
                            if (command.kind == TerminalCommand::Kind::kInput) {
                                pending_terminal_bytes_ -= std::min(
                                    pending_terminal_bytes_, command.bytes.size());
                            }
                        }
                        switch (command.kind) {
                            case TerminalCommand::Kind::kOpen: {
                                if ((session->permissions() & chast::kPermissionTerminal) ==
                                    0) {
                                    set_terminal_phase(
                                        TerminalPhase::kFailed, 0, {},
                                        "the host did not grant remote terminals");
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    terminal_output_ended_ = true;
                                    continue;
                                }
                                if (terminal_active) {
                                    // The previous console is still draining;
                                    // opening now would mix its bytes into the
                                    // new panel.
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    pending_terminal_.push_front(std::move(command));
                                    return;
                                }
                                chast::Error requested = session->request_terminal(
                                    kViewerTerminalId, command.columns, command.rows);
                                if (!requested.ok()) {
                                    set_terminal_phase(TerminalPhase::kFailed, 0, {},
                                                       "request: " +
                                                           requested.to_string());
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    terminal_output_ended_ = true;
                                    continue;
                                }
                                terminal_active = true;
                                terminal_closing = false;
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    status_.terminal_columns = command.columns;
                                    status_.terminal_rows = command.rows;
                                }
                                std::fprintf(stderr,
                                             "viewer: terminal requested %ux%u\n",
                                             command.columns, command.rows);
                                continue;
                            }
                            case TerminalCommand::Kind::kInput: {
                                if (!terminal_active || terminal_closing) {
                                    continue;
                                }
                                bool retry = false;
                                while (!command.bytes.empty()) {
                                    const size_t chunk = std::min(
                                        command.bytes.size(), chast::kMaxTerminalChunkBytes);
                                    chast::Error sent = session->send_terminal_input(
                                        kViewerTerminalId,
                                        reinterpret_cast<const uint8_t*>(
                                            command.bytes.data()),
                                        chunk, std::chrono::milliseconds(0));
                                    if (sent.status() == chast::Status::WaitTimeout) {
                                        retry = true;
                                        break;
                                    }
                                    if (!sent.ok()) {
                                        set_terminal_phase(TerminalPhase::kFailed, 0, {},
                                                           "send input: " +
                                                               sent.to_string());
                                        terminal_closing = true;
                                        break;
                                    }
                                    command.bytes.erase(0, chunk);
                                }
                                if (retry && !command.bytes.empty()) {
                                    std::lock_guard<std::mutex> lock(terminal_mutex_);
                                    pending_terminal_bytes_ += command.bytes.size();
                                    pending_terminal_.push_front(std::move(command));
                                    return;
                                }
                                continue;
                            }
                            case TerminalCommand::Kind::kResize: {
                                if (!terminal_active || terminal_closing) {
                                    continue;
                                }
                                chast::Error sent = session->send_terminal_resize(
                                    kViewerTerminalId, command.columns, command.rows);
                                if (!sent.ok()) {
                                    std::fprintf(stderr,
                                                 "viewer: terminal resize %ux%u: %s\n",
                                                 command.columns, command.rows,
                                                 sent.to_string().c_str());
                                    continue;
                                }
                                std::lock_guard<std::mutex> lock(mutex_);
                                status_.terminal_columns = command.columns;
                                status_.terminal_rows = command.rows;
                                continue;
                            }
                            case TerminalCommand::Kind::kClose: {
                                if (terminal_active && !terminal_closing) {
                                    chast::Error stopped = session->send_terminal_stop(
                                        kViewerTerminalId,
                                        chast::TerminalCode::Cancelled,
                                        "closed by the viewer");
                                    terminal_closing = true;
                                    if (!stopped.ok()) {
                                        std::lock_guard<std::mutex> lock(mutex_);
                                        status_.terminal_error =
                                            "close: " + stopped.to_string();
                                    }
                                }
                                continue;
                            }
                        }
                    }
                };

                auto poll_terminal = [&] {
                    chast::TerminalResult result;
                    while (session->take_terminal_result(result)) {
                        if (result.terminal_id != kViewerTerminalId) {
                            continue;
                        }
                        if (terminal_phase_is(TerminalPhase::kClosed)) {
                            // The panel closed while the answer was in
                            // flight; the host's console must not outlive it.
                            if (result.code == chast::TerminalCode::Ok) {
                                session->close_terminal(kViewerTerminalId);
                                terminal_active = false;
                                terminal_closing = false;
                                std::lock_guard<std::mutex> lock(terminal_mutex_);
                                terminal_output_ended_ = true;
                            }
                            continue;
                        }
                        if (result.code != chast::TerminalCode::Ok) {
                            terminal_active = false;
                            terminal_closing = false;
                            set_terminal_phase(TerminalPhase::kFailed,
                                               static_cast<int>(result.code),
                                               result.reason, {});
                            std::fprintf(stderr, "viewer: terminal refused: %s\n",
                                         result.reason.empty() ? "no reason given"
                                                               : result.reason.c_str());
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            terminal_output_ended_ = true;
                            continue;
                        }
                        chast::Error opened = session->open_terminal_input(kViewerTerminalId);
                        if (!opened.ok()) {
                            session->close_terminal(kViewerTerminalId);
                            terminal_active = false;
                            terminal_closing = false;
                            set_terminal_phase(
                                TerminalPhase::kFailed,
                                static_cast<int>(chast::TerminalCode::Failed), {},
                                "input stream: " + opened.to_string());
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            terminal_output_ended_ = true;
                            continue;
                        }
                        set_terminal_phase(TerminalPhase::kOpen, 0, {}, {});
                        std::fprintf(stderr, "viewer: terminal open\n");
                    }

                    chast::TerminalStop stop;
                    while (session->take_terminal_stop(stop)) {
                        if (stop.terminal_id != kViewerTerminalId || !terminal_active) {
                            continue;
                        }
                        terminal_closing = true;
                        // A local close already told the panel why it ended;
                        // a host stop is the answer for everything else.
                        if (!terminal_phase_is(TerminalPhase::kClosed) &&
                            !terminal_phase_is(TerminalPhase::kFailed)) {
                            set_terminal_phase(TerminalPhase::kEnded,
                                               static_cast<int>(stop.code), stop.reason,
                                               {});
                        }
                        std::fprintf(stderr, "viewer: terminal stopped: %s\n",
                                     stop.reason.empty() ? "no reason given"
                                                         : stop.reason.c_str());
                    }

                    if (!terminal_active) {
                        return;
                    }
                    if ((session->permissions() & chast::kPermissionTerminal) == 0) {
                        // The transport aborted the streams and the session
                        // forgot the entry; anything received before that stays
                        // queued for the panel.
                        finish_terminal();
                        set_terminal_phase(TerminalPhase::kFailed, 0, {},
                                           "the host revoked terminal access");
                        return;
                    }
                    if (terminal_phase_is(TerminalPhase::kRequesting)) {
                        // The host has not answered yet, so the session has no
                        // entry to read. Asking now would report the console as
                        // ended and cancel the open that is still in flight.
                        return;
                    }
                    for (;;) {
                        {
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            if (terminal_output_bytes_ >=
                                chast::kTerminalReceiveHighWaterBytes) {
                                // The panel has not caught up. Stop pulling so
                                // the session's receive window pauses the host
                                // instead of truncating the picture.
                                break;
                            }
                        }
                        std::vector<uint8_t> data;
                        bool ended = false;
                        if (!session->take_terminal_output(kViewerTerminalId, data,
                                                           ended)) {
                            if (ended) {
                                // A host-initiated ending carries its own stop;
                                // a lost one is still an ending.
                                if (terminal_phase_is(TerminalPhase::kOpen) ||
                                    terminal_phase_is(TerminalPhase::kRequesting)) {
                                    set_terminal_phase(TerminalPhase::kEnded, 0, {},
                                                       {});
                                }
                                finish_terminal();
                            }
                            break;
                        }
                        terminal_bytes += data.size();
                        {
                            std::lock_guard<std::mutex> lock(terminal_mutex_);
                            terminal_output_bytes_ += data.size();
                            terminal_output_.push_back(std::move(data));
                        }
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.terminal_bytes = terminal_bytes;
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
                uint32_t camera_decode_width = 0;
                uint32_t camera_decode_height = 0;
                uint64_t camera_frames = 0;
                uint64_t camera_gaps = 0;
                uint64_t camera_decode_errors = 0;
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
                        if (!camera_decoder || header.codec != camera_codec ||
                            header.width != camera_decode_width ||
                            header.height != camera_decode_height) {
                            chast::DecoderConfig camera_config;
                            camera_config.codec = header.codec;
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
                                    return chast::Error::success();
                                }
                                if (camera_sink_ == nullptr) {
                                    return chast::Error(
                                        chast::Status::Unsupported,
                                        "camera: the frame sink is not available");
                                }
                                if (!camera_sink_->error().empty()) {
                                    return chast::Error(
                                        chast::Status::CaptureFailed,
                                        "camera: " + camera_sink_->error());
                                }
                                // No texture is attached yet; dropping keeps
                                // the decoder running.
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
                                !camera_sink_->error().empty()) {
                                const std::string sink_error = camera_sink_->error();
                                session->request_camera_stop();
                                reset_camera();
                                set_camera_phase(CameraPhase::kFailed, 0, {},
                                                 "display: " + sink_error);
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

                // Port forwarding. The operator's target crosses the mutex;
                // the listener and every relay are driven here by pump(). The
                // counters add up across targets, so the status stays
                // session-wide even when a target is replaced.
                uint64_t forward_connections_done = 0;
                uint64_t forward_failures_done = 0;
                uint64_t forward_bytes_up_done = 0;
                uint64_t forward_bytes_down_done = 0;
                std::string last_forward_error;

                auto drop_port_forward = [&] {
                    if (!port_forward) {
                        return;
                    }
                    forward_connections_done += port_forward->connections();
                    forward_failures_done += port_forward->failures();
                    forward_bytes_up_done += port_forward->bytes_up();
                    forward_bytes_down_done += port_forward->bytes_down();
                    port_forward->stop();
                    port_forward.reset();
                };

                auto drain_port_forward = [&] {
                    std::optional<PortForwardCommand> command;
                    {
                        std::lock_guard<std::mutex> lock(port_forward_mutex_);
                        command.swap(pending_port_forward_);
                    }
                    if (!command) {
                        return;
                    }
                    drop_port_forward();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.port_forward_active = false;
                        status_.port_forward_local_port = 0;
                        status_.port_forward_remote_host.clear();
                        status_.port_forward_remote_port = 0;
                        status_.port_forward_error.clear();
                        status_.port_forward_connections = forward_connections_done;
                        status_.port_forward_failures = forward_failures_done;
                        status_.port_forward_bytes_up = forward_bytes_up_done;
                        status_.port_forward_bytes_down = forward_bytes_down_done;
                    }
                    last_forward_error.clear();
                    if (command->stop) {
                        return;
                    }
                    if ((session->permissions() & chast::kPermissionPortForward) == 0) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.port_forward_error =
                            "the host did not grant port forwarding";
                        return;
                    }
                    auto client = std::make_unique<chast::PortForwardClient>(*session);
                    uint16_t bound = 0;
                    const chast::Error started = client->start(
                        command->remote_host, command->remote_port, command->local_port,
                        &bound);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.port_forward_remote_host = command->remote_host;
                        status_.port_forward_remote_port = command->remote_port;
                        if (started.ok()) {
                            status_.port_forward_local_port = bound;
                        } else {
                            status_.port_forward_local_port = 0;
                            status_.port_forward_error =
                                "listener: " + started.to_string();
                        }
                    }
                    if (!started.ok()) {
                        std::fprintf(stderr, "viewer: port forward: %s\n",
                                     started.to_string().c_str());
                        return;
                    }
                    std::fprintf(stderr,
                                 "viewer: port forward listening on 127.0.0.1:%u -> %s:%u\n",
                                 static_cast<unsigned>(bound), command->remote_host.c_str(),
                                 static_cast<unsigned>(command->remote_port));
                    port_forward = std::move(client);
                };

                auto poll_port_forward = [&] {
                    if (!port_forward) {
                        return;
                    }
                    if ((session->permissions() & chast::kPermissionPortForward) == 0) {
                        // A revocation closes the local port immediately; the
                        // transport aborts whatever was still in flight.
                        drop_port_forward();
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.port_forward_active = false;
                        status_.port_forward_error =
                            "the host revoked port forwarding";
                        status_.port_forward_connections = forward_connections_done;
                        status_.port_forward_failures = forward_failures_done;
                        status_.port_forward_bytes_up = forward_bytes_up_done;
                        status_.port_forward_bytes_down = forward_bytes_down_done;
                        return;
                    }
                    for (const std::string& event : port_forward->pump()) {
                        std::fprintf(stderr, "viewer: %s\n", event.c_str());
                    }
                    const bool active = port_forward->running();
                    const std::string error = port_forward->error();
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.port_forward_active = active;
                        status_.port_forward_connections =
                            forward_connections_done + port_forward->connections();
                        status_.port_forward_failures =
                            forward_failures_done + port_forward->failures();
                        status_.port_forward_bytes_up =
                            forward_bytes_up_done + port_forward->bytes_up();
                        status_.port_forward_bytes_down =
                            forward_bytes_down_done + port_forward->bytes_down();
                        status_.port_forward_error = error;
                    }
                    if (error != last_forward_error) {
                        if (!error.empty()) {
                            std::fprintf(stderr, "viewer: port forward: %s\n",
                                         error.c_str());
                        }
                        last_forward_error = error;
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
                            status_.merged_layout = info.merged;
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
                            }
                        }
                    }
                    drain_input();
                    poll_local_clipboard_files();
                    poll_clipboard();
                    receive_clipboard();
                    drive_file_upload();
                    // Browse answers are adopted before file offers: a walk's
                    // download answer names the transfer whose offer follows,
                    // and the offer must already know its mirrored directory.
                    drain_file_browse();
                    receive_file_browse();
                    drain_downloads();
                    step_clipboard_files();
                    drain_download_cancels();
                    step_folder_download();
                    receive_cursor();
                    drain_audio();
                    drain_microphone();
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
                        port_forward != nullptr || terminal_active || camera_active;
                    chast::Error received_frame = chast::Error::success();
                    if (files_only) {
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
                    receive_permissions();
                    drain_privacy();
                    receive_privacy();
                    drain_sas();
                    receive_sas();
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
                        if (!files_only && now - last_frame >= kFrameStall) {
                            fail("no frames arrived for 20 s");
                            break;
                        }
                        if (!files_only && sink_ != nullptr && !sink_->error().empty()) {
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

                    // Viewer-side recording. The switch may arrive mid-session,
                    // so it is applied here, on a picture the decoder also
                    // accepts: the muxer waits for the next key frame before it
                    // writes anything, and a size change rolls into a new file.
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
                        std::string open_error;
                        auto fresh = std::make_unique<chast_tools::Mp4Recorder>();
                        if (!fresh->open(record_file_path(directory), info.codec, info.width,
                                         info.height, &open_error)) {
                            record_error = open_error;
                            record_wanted_.store(false, std::memory_order_relaxed);
                        } else {
                            record_error.clear();
                            std::fprintf(stderr, "viewer: recording started\n");
                            recorder = std::move(fresh);
                        }
                    } else if (!record_wanted && recorder) {
                        recorder->close();
                        recorder.reset();
                        std::fprintf(stderr, "viewer: recording stopped\n");
                    }
                    if (recorder) {
                        recorder->write(header, payload.data(), payload.size());
                        if (recorder->failed()) {
                            record_error = recorder->error();
                            recorder.reset();
                            record_wanted_.store(false, std::memory_order_relaxed);
                        }
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
    if (recorder) {
        recorder->close();
        if (recorder->failed()) {
            record_error = recorder->error();
        }
        recorder.reset();
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
            if (stage == FileUpload::Stage::kFailed) {
                ++file_failures;
            } else {
                ++files_sent;
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
            last_upload_ = std::move(uploads_.front());
            last_upload_.stage = FileUpload::Stage::kFailed;
            last_upload_.error = "the session ended before the file finished";
            uploads_.clear();
        }
        pending_upload_roots_.clear();
    }
    if (cancelled_upload != 0) {
        session->cancel_file_upload(cancelled_upload);
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
