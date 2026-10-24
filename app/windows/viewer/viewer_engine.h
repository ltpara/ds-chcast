#pragma once

// Client-side viewer engine for the Flutter shell:
//
//   QUIC client -> VPL decoder (D3D11 NV12) -> GPU surface texture
//
// The engine owns one worker thread and is deliberately independent of
// Flutter. The UI polls Snapshot() for state and counters; decoded pictures are
// pushed to the sink from the worker thread as D3D11 textures while the
// decoder still owns the surface.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "chast/encoder.h"
#include "chast/protocol.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace chast {
class DeviceIdentity;
class FileReceiver;
}

namespace chast_viewer {

class FrameSink {
public:
    virtual ~FrameSink() = default;
    // The D3D11 device shared with the decoder and the renderer.
    virtual ID3D11Device* device() = 0;
    // Called on the session thread with the decoder's NV12 texture and the
    // array slice that holds the picture. Returns true when the frame was
    // published to Flutter, false when it was dropped. Fatal display failures
    // are reported through error().
    virtual bool OnGpuFrame(ID3D11Texture2D* nv12, uint32_t subresource, uint32_t width,
                            uint32_t height, uint32_t crop_x, uint32_t crop_y,
                            uint64_t pts_ms) = 0;
    virtual std::string error() const = 0;
};

enum class ViewerState {
    kIdle,
    kConnecting,
    kHandshaking,
    kStreaming,
    kEnded,
    kFailed,
};

// The viewer's upload queue, as the UI shows it. kHashing covers both waiting
// for a folder to expand and the local SHA-256 pass that precedes the offer;
// the last two are terminal states of the most recent attempt (the local file
// is never modified).
enum class FilePhase {
    kIdle = 0,
    kHashing = 1,
    kOffered = 2,
    kSending = 3,
    kDone = 4,
    kFailed = 5,
};

struct ViewerStatus {
    ViewerState state = ViewerState::kIdle;
    std::string error;
    std::string host;
    uint16_t port = 0;
    chast::VideoCodec codec = chast::VideoCodec::H264;
    // The encoded picture size, which is what the decoder produces and the
    // texture holds. `source_width`/`source_height` are the captured desktop
    // size; pointer positions and input events travel in that space, so the
    // UI maps them with these while it draws the smaller picture.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t framerate = 0;
    uint32_t server_bitrate = 0;
    std::string decoder;
    std::string server_encoder;
    // SHA-256 of the certificate the server presented, uppercase hex without
    // separators. Empty until a certificate arrived; also set when the
    // connection was rejected for not matching the pinned one, so the UI can
    // offer the new identity.
    std::string server_fingerprint;
    // The server's AccessDenied code when it refused the Hello: one of
    // chast::AccessDenial, or 0 when it never did. The UI turns this into its
    // own wording; `error` carries the server's reason.
    int denial = 0;
    uint64_t received = 0;
    uint64_t decoded = 0;
    uint64_t keyframes = 0;
    uint64_t published = 0;
    uint64_t crc_errors = 0;
    uint64_t decode_errors = 0;
    uint64_t protocol_errors = 0;
    uint64_t gaps = 0;
    uint64_t bytes = 0;
    double fps = 0.0;
    double kbps = 0.0;
    // Link quality from LatencyProbe/Reply. Both stay 0 until the first reply
    // (and, for latency_ms, the first frame after it); latency_ms is the age
    // of the most recent frame from host capture to local receipt, after
    // correcting for the measured clock offset.
    double rtt_ms = 0.0;
    double latency_ms = 0.0;
    uint64_t elapsed_ms = 0;
    bool video_finished = false;
    // The permission mask the host last announced (chast::PermissionBit) and
    // how many masks have been applied, including the one from HelloAck. The
    // UI compares the mask to explain a live revocation.
    uint16_t permissions = 0;
    uint64_t permission_updates = 0;
    // Input gate and coordinate origin from HelloAck. `origin_x`/`origin_y`
    // are the captured output's top-left in virtual-desktop pixels; the UI
    // adds them to the source-space pointer position.
    bool input_enabled = false;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    uint64_t input_sent = 0;
    uint64_t input_failed = 0;
    // Clipboard gate from HelloAck. Changes are read from and written to the
    // local Win32 clipboard by the session thread; the counters cover every
    // rich item exchanged this session. `clipboard_*_formats` is a bitmask of
    // chast::ClipboardFormat (1 << id) for the most recent item each way.
    bool clipboard_enabled = false;
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_failed = 0;
    uint32_t clipboard_sent_formats = 0;
    uint32_t clipboard_received_formats = 0;
    // The viewer's current or most recent upload. `file_phase` is a FilePhase
    // value; `file_sent` counts bytes streamed in this attempt (resume offset
    // included). A failure keeps the local file in place and explains itself
    // in `file_error`. `file_relative_path` is the folder below the host's
    // receive root and `file_queue` counts the entries not yet finished,
    // including the current one. The counters cover the whole session.
    FilePhase file_phase = FilePhase::kIdle;
    std::string file_name;
    std::string file_relative_path;
    uint32_t file_queue = 0;
    uint64_t file_size = 0;
    uint64_t file_sent = 0;
    std::string file_error;
    // The last entry that left the queue. The page pairs this name and reason
    // with the counters below, because the active entry may already be the
    // next file by the time it polls.
    std::string last_file_name;
    std::string last_file_error;
    uint64_t files_sent = 0;
    uint64_t file_failures = 0;
    // The host's most recent download. The bytes are written to the receive
    // directory as they arrive, so `download_received` counts bytes on disk;
    // a resume starts at the partial length already on disk.
    FilePhase download_phase = FilePhase::kIdle;
    std::string download_name;
    uint64_t download_size = 0;
    uint64_t download_received = 0;
    std::string download_error;
    uint64_t files_received = 0;
    uint64_t file_receive_failures = 0;
    // Pointer: updates received, and whether one is waiting for the UI. The
    // sprite is deliberately not part of ViewerStatus: it can be a quarter of
    // a megabyte and the UI polls the status four times a second, so it is
    // taken separately through TakeCursor().
    uint64_t cursor_updates = 0;
    bool cursor_pending = false;
    // System sound from the host. `audio_announced` mirrors the server's gate;
    // `audio_enabled` means the local decoder and renderer are also running.
    // A local pipeline that failed while video kept going explains itself in
    // `audio_error`. All counters cover the current session.
    bool audio_announced = false;
    bool audio_enabled = false;
    bool audio_muted = false;
    uint32_t audio_sample_rate = 0;
    uint32_t audio_channels = 0;
    uint32_t audio_bitrate = 0;
    uint64_t audio_received = 0;
    uint64_t audio_decoded = 0;
    uint64_t audio_errors = 0;
    uint64_t audio_underruns = 0;
    // Endpoint rebuilds the renderer performed after a loss or a default
    // output change. `audio_recovering` means it is between rebuilds now;
    // the audio stream itself stays up, so this is not an error.
    uint64_t audio_recoveries = 0;
    bool audio_recovering = false;
    uint32_t audio_buffer_ms = 0;
    std::string audio_error;
    // The playback endpoint the player is using: empty follows the default
    // output. Selecting another one rebuilds the player but keeps the decoder
    // and the last clock offset. `audio_device_name` is the endpoint's
    // friendly name, or empty when the default (or an unavailable id) is in
    // use; a fallback to the default after a failed open is explained in
    // `audio_error`.
    std::string audio_device;
    std::string audio_device_name;
    // Microphone processing switches, both on by default. AEC uses the system
    // audio this viewer plays as its reference; changing either switch
    // reopens the capture within one poll. The counters below cover the
    // session and count near-end frames processed with each switch enabled.
    bool audio_aec = true;
    bool audio_noise_suppression = true;
    uint64_t microphone_aec_frames = 0;
    uint64_t microphone_ns_frames = 0;
    // Microphone uplink to the host's speakers. `microphone_authorized`
    // mirrors the host permission; `microphone_enabled` means the local
    // capture thread and the CMIC stream are running. `microphone_device` is
    // the endpoint id the capture is pinned to (empty selects the default
    // communications device) and `microphone_name` is what the picker shows.
    // Failures never touch video or system sound and explain themselves in
    // `microphone_error`.
    bool microphone_authorized = false;
    bool microphone_enabled = false;
    std::string microphone_device;
    std::string microphone_name;
    uint64_t microphone_packets = 0;
    uint64_t microphone_dropped = 0;
    uint64_t microphone_errors = 0;
    uint64_t microphone_recoveries = 0;
    bool microphone_recovering = false;
    std::string microphone_error;
    // Privacy mode hides the host's own screen while the stream continues.
    // `privacy_supported` mirrors the host's permission; `privacy_enabled` is
    // the overlay state from the most recent PrivacyModeState, and every
    // session starts off. A refusal or a host-initiated disable explains itself
    // in `privacy_error`; `privacy_updates` lets the UI pair an answer with the
    // poll that saw it.
    bool privacy_supported = false;
    bool privacy_enabled = false;
    int privacy_code = 0;
    std::string privacy_error;
    uint64_t privacy_updates = 0;
    // Which desktop the host's video is capturing. Secure means the lock
    // screen, UAC or a credential prompt is up; the picture pauses while the
    // host cannot follow it and resumes when the user desktop returns.
    chast::DesktopKind desktop = chast::DesktopKind::Default;
    uint64_t desktop_updates = 0;
    // Secure Attention Sequence (Ctrl+Alt+Del). The host answers every request
    // with one SasResult; `sas_code` is that answer (0 = Ok), `sas_error` its
    // explanation, and `sas_updates` lets the UI pair one answer with a poll.
    int sas_code = 0;
    std::string sas_error;
    uint64_t sas_updates = 0;
    // What the server announced in HelloAck. `displays` is the list the UI's
    // picker shows; a single-display stream's entry is the one whose rect
    // starts at origin_x/origin_y and matches width x height. `merged_layout`
    // means the picture is the union of every listed display, so no entry
    // describes the streamed surface and per-display mode changes are off.
    std::vector<chast::DisplayDescription> displays;
    bool merged_layout = false;
    // Display switches the host accepted (including a request for the output
    // already streaming) and the refusals this engine learned about, plus the
    // host's reason for the most recent refusal. A success clears the error.
    // The UI compares both counters with the previous poll, so it can tell a
    // new refusal from the stale text of an old one; the error alone cannot
    // distinguish two identical reasons.
    uint64_t display_switches = 0;
    uint64_t display_switch_failures = 0;
    std::string display_switch_error;
    // Resolution picker. The engine asks for the streamed output's modes on
    // demand; display_modes_display says which announced index the newest
    // answer describes, display_modes_code is the host's DisplayModeCode
    // (0 = Ok) and display_modes_error its reason. display_mode_changes and
    // display_mode_failures count SetDisplayMode answers, with the host's
    // reason for the most recent refusal in display_mode_error (cleared by an
    // accepted change). The UI compares the counters so two identical
    // refusals still read as two answers.
    std::vector<chast::DisplayModeInfo> display_modes;
    uint16_t display_modes_display = 0;
    int display_modes_code = 0;
    std::string display_modes_error;
    uint64_t display_mode_lists = 0;
    uint64_t display_mode_changes = 0;
    uint64_t display_mode_failures = 0;
    std::string display_mode_error;
    // Stream preferences. `stream_quality` is the host's effective
    // StreamQuality (0 = High); the effective codec and frame rate are the
    // existing `codec`/`framerate`, which the host's StreamInfo also updates.
    // `stream_option_code` is the StreamOptionsCode of the newest answer
    // (0 = Ok) and `stream_option_error` its refusal reason; a success clears
    // it. `stream_option_results` counts answers (including local send
    // failures), so the UI can pair one answer with a poll.
    int stream_quality = 0;
    int stream_option_code = 0;
    std::string stream_option_error;
    uint64_t stream_option_results = 0;
};

class ViewerEngine {
public:
    explicit ViewerEngine(FrameSink* sink);
    ~ViewerEngine();

    ViewerEngine(const ViewerEngine&) = delete;
    ViewerEngine& operator=(const ViewerEngine&) = delete;

    // Starts a session thread, asking the server for `display` (an index into
    // its announced list, or kDisplayAuto). The server must present the
    // certificate whose SHA-256 is `pinned_fingerprint`; `allow_unverified`
    // skips that check and is only for the first contact, whose fingerprint
    // the caller then stores. `access_token` is sent in Hello and is empty for
    // servers that do not gate access. `password` and `totp` answer an
    // interactive host's challenges; `device_identity` lets the host recognize
    // this viewer and skip two-factor, and `trust_device` asks it to remember
    // the key after a successful sign-in. Fails when one is already running;
    // call Stop() first to switch targets or displays.
    bool Start(const std::string& host, uint16_t port, uint16_t display,
               std::string pinned_fingerprint = {},
               bool allow_unverified = false,
               std::string access_token = {}, std::string password = {},
               std::string totp = {},
               std::shared_ptr<chast::DeviceIdentity> device_identity = {},
               bool trust_device = false);
    // Requests the session to end and waits for the worker to tear down the
    // QUIC connection. Safe to call when no session is running.
    void Stop();

    // Queues one input event for the session thread, which owns the QUIC
    // session and sends events in order. Returns false when input is not
    // enabled by the server yet, no session is running, or the queue is full.
    bool SendInput(const chast::InputEvent& event);
    // Queues a request to lift everything the host still holds. Ordered with
    // the events above, so releases the UI already sent land first.
    bool ReleaseInput();
    // Queues a mid-session switch to another announced display. Returns false
    // when no session is streaming, the host did not grant input (a view-only
    // session cannot move the host's picture), or no such display is in the
    // announced list. The answer arrives through ViewerStatus: display_switches
    // on success and display_switch_error on refusal. The new geometry travels
    // in the StreamInfo the host sends after an accepted switch.
    bool SwitchDisplay(uint16_t display);
    // Asks the host for the modes of one announced display. Returns false
    // when no session is streaming or the display is not in the announced
    // list. The answer arrives through ViewerStatus::display_modes, which
    // pairs it with display_modes_display/display_modes_code; listing needs
    // no input permission.
    bool RequestDisplayModes(uint16_t display);
    // Queues a mode change for the streamed output. Returns false when no
    // session is streaming, the host did not grant input, or the display is
    // not in the announced list. The answer arrives through
    // display_mode_changes/display_mode_failures; an accepted change is
    // followed by a StreamInfo with the new source size. One request is kept:
    // a newer one replaces an unsent one.
    bool SetDisplayMode(uint16_t display, uint16_t width, uint16_t height,
                        uint16_t refresh_hz);
    // Queues stream preferences: encoder choice, frame-rate cap and quality
    // preset. Returns false when no session is running or a value is outside
    // its wire range. Accepted as soon as the session starts, before the first
    // frame, so a page can re-apply its choices right after Start(). One
    // request is kept: a newer one replaces an unsent one. The host answers
    // through ViewerStatus::stream_option_code, and reports the values it
    // actually runs in codec/framerate/stream_quality (so a refusal also
    // returns the menus to reality). No input permission is required.
    bool SetStreamOptions(chast::StreamCodecPreference codec, uint16_t max_fps,
                          chast::StreamQuality quality);
    // Pauses or resumes the native rich-clipboard bridge. While paused neither
    // direction touches the local clipboard; resuming starts from a fresh
    // baseline, so whatever changed in between is not flushed to the host.
    // The host's own clipboard permission is applied on top of this.
    void SetClipboardSync(bool enabled);
    // Queues local paths for upload (viewer -> host). Folders are expanded
    // into their regular files, preserving the folder as the first relative
    // directory component. Returns false when the session is not streaming,
    // the host did not grant file transfer, or no path was usable. Hashing
    // and folder walks run on the file worker; progress arrives through
    // ViewerStatus::file_phase and the queue counters.
    bool SendFiles(const std::vector<std::string>& paths);

    // Copies the newest pointer state the host sent, once per received update.
    // The sprite is only attached to the update that introduced a shape, so a
    // caller caches it by `shape_id`. Returns false when nothing new arrived.
    bool TakeCursor(chast::CursorUpdate& update);

    // Mutes or unmutes system sound. Safe from the UI thread; the session
    // thread applies it to the render session. Returns false when no audio
    // pipeline is running, so the caller knows the tap had no effect.
    bool SetAudioMuted(bool mute);

    // Turns the microphone uplink on or off. Opening the capture device and
    // the CMIC stream happens on the session thread; switching off stops them
    // and ends the stream immediately. Returns false when no session is
    // running or the host did not grant kPermissionMicrophone (a revocation
    // also clears the switch and reports why in microphone_error).
    bool SetMicrophone(bool enabled);
    // Pins the capture endpoint. Empty follows the default communications
    // device. An open capture is reopened on the new id within one poll; the
    // choice is kept for later switches. Returns false when no session is
    // running.
    bool SelectMicrophone(std::string device_id);

    // Pins the playback endpoint. Empty follows the default output. A live
    // player is rebuilt on the next poll with the same decoder and clock
    // offset; an id that cannot be opened falls back to the default and
    // explains itself in audio_error. Returns false when no session is
    // running.
    bool SelectAudioOutput(std::string device_id);
    // Switches microphone echo cancellation and noise suppression. The
    // capture reopens on the next poll (only when it is running), and the
    // choices are kept for later sessions. Returns false when no session is
    // running.
    bool SetAudioProcessing(bool echo_cancellation, bool noise_suppression);

    // Queues a request to hide (true) or restore (false) the host's physical
    // display. Returns false when no session is streaming or the host did not
    // grant kPermissionPrivacyMode. Only the newest request is kept; the answer
    // arrives through ViewerStatus::privacy_enabled/privacy_code/privacy_error.
    bool SetPrivacyMode(bool enabled);
    // Queues one Ctrl+Alt+Del request. Returns false when no session is
    // streaming or the host did not grant input control. The answer arrives
    // through ViewerStatus::sas_code/sas_error/sas_updates; a newer request
    // replaces one that has not been sent yet.
    bool SendSas();

    ViewerStatus Snapshot() const;

private:
    void Run(std::string host, uint16_t port, uint16_t display,
             std::string pinned_fingerprint, bool allow_unverified,
             std::string access_token, std::string password, std::string totp,
             bool trust_device);
    void SetState(ViewerState state, std::string error = {});
    // Expands dropped folders and hashes queued uploads on its own thread, so
    // a multi-gigabyte read never stalls the session's receive loop.
    void FileWorker();
    void ExpandUploadRoot(const std::string& root, uint64_t epoch);
    bool EnqueueUpload(const std::string& path, std::string relative_path, uint64_t epoch);
    void EnqueueUploadFailure(std::string name, std::string relative_path, std::string error,
                              uint64_t epoch);
    bool HasHashableUploadLocked() const;

    FrameSink* sink_;
    // Kept alive for the whole session because QuicClientConfig borrows it.
    std::shared_ptr<chast::DeviceIdentity> device_identity_;
    mutable std::mutex mutex_;
    ViewerStatus status_;
    std::chrono::steady_clock::time_point started_at_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    // Survives reconnects: the speaker stays off until the user turns it on.
    std::atomic<bool> audio_muted_{false};
    // The microphone switch only crosses as a flag; the session thread owns
    // the capture thread and the CMIC stream. Every session starts off, and a
    // missing permission clears the flag rather than leaving it queued.
    std::atomic<bool> microphone_wanted_{false};
    mutable std::mutex microphone_mutex_;
    std::string microphone_device_id_;
    // The playback choice survives reconnects, like the microphone device.
    mutable std::mutex audio_output_mutex_;
    std::string audio_output_device_id_;
    std::atomic<bool> microphone_aec_{true};
    std::atomic<bool> microphone_noise_{true};

    // Newest privacy request, for the same reason as the display switch: the
    // operator wants the state they picked last, not a replay of every toggle.
    mutable std::mutex privacy_mutex_;
    std::optional<bool> pending_privacy_;
    // SAS is an action, not a state: one pending request is enough, and a
    // second tap replaces an unsent one instead of queueing a burst.
    std::atomic<bool> pending_sas_{false};

    // Input crosses from the UI thread to the session thread here. The queue
    // is bounded: a stalled transport drops the newest events instead of
    // growing, which is the right trade for a pointer stream that is already
    // stale.
    mutable std::mutex input_mutex_;
    struct PendingInput {
        bool release = false;
        chast::InputEvent event;
    };
    std::deque<PendingInput> pending_input_;
    bool input_enabled_ = false;

    // Native clipboard bridge. The session thread reads the local clipboard
    // and writes what the host sends; the toggle only crosses as a flag, and
    // the UI never moves clipboard bytes through Dart.
    std::atomic<bool> clipboard_sync_{true};

    // Uploads wait in one queue. The session thread works on the front while
    // the file worker expands folders and hashes upcoming entries; every
    // field is read or written under file_mutex_. A terminal head is popped by
    // the session thread after its counters have been accounted for.
    struct FileUpload {
        enum class Stage {
            kQueued,
            kHashing,
            kReady,
            kOffered,
            kSending,
            kFinishing,
            kDone,
            kFailed,
        };
        Stage stage = Stage::kQueued;
        std::string path;
        std::string name;
        std::string relative_path;
        uint64_t size = 0;
        uint64_t sent = 0;
        uint64_t resume_offset = 0;
        std::array<uint8_t, 32> digest{};
        uint32_t transfer_id = 0;
        std::string error;
    };
    // The single incoming download. The session thread owns the receiver;
    // the UI reads a snapshot under the same lock.
    struct FileDownload {
        enum class Stage {
            kNone,
            kReceiving,
            kDone,
            kFailed,
        };
        Stage stage = Stage::kNone;
        uint32_t transfer_id = 0;
        std::string name;
        std::string path;
        uint64_t size = 0;
        uint64_t received = 0;
        std::chrono::steady_clock::time_point answer_time;
        std::unique_ptr<chast::FileReceiver> receiver;
        std::string error;
    };
    mutable std::mutex file_mutex_;
    std::condition_variable file_work_cv_;
    std::deque<FileUpload> uploads_;
    std::deque<std::string> pending_upload_roots_;
    FileUpload last_upload_;
    FileDownload download_;
    std::thread file_worker_;
    bool file_worker_stop_ = false;
    // Bumped whenever the queue is purged; a worker that was walking a folder
    // or hashing a file then discards its result instead of re-adding it.
    uint64_t upload_epoch_ = 0;
    uint32_t next_file_id_ = 1;

    // Pointer state crosses the same way, but only the newest update is kept:
    // a viewer that falls behind wants where the pointer is now, not a replay.
    mutable std::mutex cursor_mutex_;
    std::optional<chast::CursorUpdate> pending_cursor_;
    uint64_t cursor_updates_ = 0;

    // A switch is a single newest target, for the same reason: the viewer
    // wants where the picker last pointed, not a tour of the list it passed
    // through.
    mutable std::mutex switch_mutex_;
    std::optional<uint16_t> pending_switch_;

    // Mode requests use the same newest-wins slots: a list answer for a
    // display the picker left is useless, and an unsent mode change is
    // superseded by the one the operator clicked last.
    struct PendingMode {
        uint16_t display = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t refresh_hz = 0;
    };
    mutable std::mutex mode_mutex_;
    std::optional<uint16_t> pending_mode_list_;
    std::optional<PendingMode> pending_mode_set_;

    // Stream preferences are a snapshot, not a queue: the operator wants the
    // menu's last choice applied, not every intermediate one.
    mutable std::mutex stream_option_mutex_;
    std::optional<chast::StreamOptions> pending_stream_options_;
};

}  // namespace chast_viewer
