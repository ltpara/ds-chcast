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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
class PortForwardClient;
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

// How the viewer reaches the host. Auto tries QUIC briefly, then the TCP
// carrier and finally the WebSocket carrier; the forced modes skip the paths
// before them, which lets a test or an operator choose one deliberately.
// Every mode runs the same QUIC session, TLS and pinning end to end.
enum class ViewerTransport {
    kAuto,
    kQuic,
    kTcp,
    kWebSocket,
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

// One remote console's life on the viewer. Requesting covers the round trip
// from OpenTerminal() to the host's answer; Ended means the host stopped the
// console (its code and reason are in the status) and every byte it printed
// was already queued for TakeTerminalOutput().
enum class TerminalPhase {
    kClosed = 0,
    kRequesting = 1,
    kOpen = 2,
    kEnded = 3,
    kFailed = 4,
};

// One camera the viewer is watching. Listing covers the round trip of a list
// request, Starting covers StartCamera until the host's answer, and Streaming
// means the CCAM stream is open. Stopped is terminal: the viewer asked to
// stop or the host ended the stream (a device failure has no wire reason, so
// camera_reason explains a host-initiated ending). Failed carries the host's
// CameraCode and wording for a refusal, or a local error.
enum class CameraPhase {
    kIdle = 0,
    kListing = 1,
    kListed = 2,
    kStarting = 3,
    kStreaming = 4,
    kStopped = 5,
    kFailed = 6,
};

struct ViewerStatus {
    ViewerState state = ViewerState::kIdle;
    std::string error;
    std::string host;
    uint16_t port = 0;
    // The path the session actually uses: "quic" (UDP), "tcp" or "ws". It is
    // the requested mode while connecting and the settled path afterwards.
    // transport_error carries the paths that were abandoned before this one,
    // so the UI can say why it fell back.
    std::string transport = "quic";
    std::string transport_error;
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
    // Lowercase hex reference generated for this session and carried in Hello.
    // The host copies it into its inbound audit record; the UI reports it with
    // the session and file events, so the service merges both sides into one
    // row. Empty only when the CSPRNG failed.
    std::string audit_ref;
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
    // The host answered with a file-transfer-only session: no video, audio,
    // input or pointer stream exists, and the file browser is the session.
    // `state` still reaches kStreaming once the host accepted the Hello.
    bool files_only = false;
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
    // The finished file's path on this viewer; empty until the download is
    // renamed into place.
    std::string download_path;
    uint64_t download_size = 0;
    uint64_t download_received = 0;
    std::string download_error;
    uint64_t files_received = 0;
    uint64_t file_receive_failures = 0;
    // Shared-folder browsing. The entries themselves are taken through
    // TakeFileList(); these fields describe the newest answer so a page can
    // update without copying the list. `file_list_directory` echoes the
    // request and `file_list_error` explains a refusal or send failure.
    uint64_t file_list_answers = 0;
    std::string file_list_directory;
    uint32_t file_list_entries = 0;
    bool file_list_truncated = false;
    std::string file_list_error;
    // The newest viewer-initiated download request. Ok means the host queued
    // the transfer and its offer will show up in download_*; a refusal keeps
    // the host's reason here.
    uint64_t file_download_answers = 0;
    int file_download_code = 0;
    uint32_t file_download_transfer_id = 0;
    std::string file_download_path;
    std::string file_download_error;
    // The newest file-operation answer (new folder, rename or remove). Ok means
    // the host applied it; `file_op_path` echoes the entry it applied to and
    // `file_op_error` explains a refusal. The page reloads its listing when
    // the counter moves.
    uint64_t file_op_answers = 0;
    int file_op_code = 0;
    std::string file_op_path;
    std::string file_op_error;
    // A folder download walks the browsed tree with ordinary list and download
    // requests. Files land below `folder_download_destination`, mirroring the
    // remote folders under the folder's own name; `folder_download_files_total`
    // grows as folders are listed. When the walk ends,
    // `folder_download_answers` moves and `folder_download_code` carries the
    // outcome (0 done, 1 failed, 2 cancelled) with the reason in
    // `folder_download_error`.
    bool folder_download_active = false;
    std::string folder_download_name;
    std::string folder_download_destination;
    uint64_t folder_download_files_total = 0;
    uint64_t folder_download_files_done = 0;
    std::string folder_download_current;
    // Entries the running walk was asked for. 1 is one entry; higher counts
    // are a batch of selected files and folders. `folder_download_batch`
    // separates a multi-select batch (even of one file) from the folder row's
    // recursive download, so the page words its progress correctly.
    uint32_t folder_download_roots = 0;
    bool folder_download_batch = false;
    uint64_t folder_download_answers = 0;
    int folder_download_code = 0;
    std::string folder_download_error;
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
    // Session chat. Both directions are ungated once the host admits the
    // viewer. `chat_error` explains a queued line that the transport then
    // failed to send; inbound text is taken through TakeChat() so the status
    // poll never carries conversation content.
    uint64_t chat_sent = 0;
    uint64_t chat_received = 0;
    uint64_t chat_failures = 0;
    std::string chat_error;
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
    // Viewer-side port forwarding. `port_forward_enabled` mirrors the host's
    // announced permission; `port_forward_active` means the local listener is
    // up and the host accepted the target. While a request is in flight the
    // listener is already bound (so the UI can show the port) but the flag
    // stays false; a refusal closes the listener and explains itself in
    // `port_forward_error`. The byte counters cover the whole session and all
    // relayed connections.
    bool port_forward_enabled = false;
    bool port_forward_active = false;
    uint16_t port_forward_local_port = 0;
    std::string port_forward_remote_host;
    uint16_t port_forward_remote_port = 0;
    uint64_t port_forward_connections = 0;
    uint64_t port_forward_failures = 0;
    uint64_t port_forward_bytes_up = 0;
    uint64_t port_forward_bytes_down = 0;
    std::string port_forward_error;
    // Remote terminal (protocol v27). The viewer drives at most one console.
    // `terminal_supported` mirrors the host's announced permission;
    // `terminal_phase` is a TerminalPhase value; `terminal_code` and
    // `terminal_reason` are the host's TerminalCode and wording for a refusal
    // or an ending, while `terminal_error` explains a local failure such as a
    // send error or a revocation. Output bytes never travel in this struct:
    // TakeTerminalOutput() hands them over, and `terminal_bytes` counts what
    // was received. `terminal_columns`/`terminal_rows` are the size the
    // console was opened with and that the host acknowledged through the most
    // recent resize. `terminal_updates` counts phase transitions so the panel
    // can pair an ending with the poll that saw it.
    bool terminal_supported = false;
    TerminalPhase terminal_phase = TerminalPhase::kClosed;
    int terminal_code = 0;
    std::string terminal_reason;
    std::string terminal_error;
    uint16_t terminal_columns = 0;
    uint16_t terminal_rows = 0;
    uint64_t terminal_bytes = 0;
    uint64_t terminal_updates = 0;

    // Camera sharing (protocol v29). `camera_supported` mirrors the host's
    // announced permission; the viewer watches at most one camera at a time.
    // `camera_phase` is a CameraPhase value, `camera_code`/`camera_reason`
    // carry the host's CameraCode and wording for a refusal or an ending, and
    // `camera_error` explains a local failure. `camera_id` names the camera
    // the phase describes. `cameras` is the newest list; the ids are only
    // valid until the next answer, which is why `camera_list_updates` counts
    // answers. `camera_width`/`camera_height` are the encoded surface size
    // (16-aligned, so a camera whose height is not a multiple of 16 shows a
    // thin black band) and `camera_frames` counts decoded pictures.
    bool camera_supported = false;
    CameraPhase camera_phase = CameraPhase::kIdle;
    int camera_code = 0;
    std::string camera_reason;
    std::string camera_error;
    uint16_t camera_id = 0;
    std::vector<chast::CameraDescription> cameras;
    uint64_t camera_list_updates = 0;
    uint32_t camera_width = 0;
    uint32_t camera_height = 0;
    uint64_t camera_frames = 0;
    std::string camera_decoder;
    uint64_t camera_updates = 0;

    // Viewer-side recording. Nothing here crosses the wire: the viewer muxes
    // the host's already-encoded stream to a local MP4. `recording` is true
    // while a muxer is writing; the counters mirror the command-line tool,
    // `record_segments` counts every file opened (a picture-size change rolls
    // into a new one), `record_path` is the newest file and `record_error`
    // explains why recording stopped or never started.
    bool recording = false;
    uint64_t record_samples = 0;
    uint64_t record_keyframes = 0;
    uint64_t record_skipped = 0;
    uint32_t record_segments = 0;
    std::string record_path;
    std::string record_error;
};

class ViewerEngine {
public:
    // `sink` publishes the desktop picture; `camera_sink`, when present,
    // publishes the shared camera. The camera decodes on its own sink because
    // it is an independent stream with its own size and timeline, and both
    // sinks may render at the same time.
    explicit ViewerEngine(FrameSink* sink, FrameSink* camera_sink = nullptr);
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
    // the key after a successful sign-in. `transport` selects UDP or the
    // TCP/WebSocket fallback; `carrier_host`/`carrier_port` name the fallback
    // endpoint (empty host means `host`, port 0 means `port + 1`).
    // `carrier_host_id`/`carrier_token` select a host on a shared carrier; they
    // stay empty for a per-host relay port. Fails when one is already running;
    // call Stop() first to switch targets or displays.
    //
    // `file_transfer_only` asks the host for a session without a screen
    // stream (protocol v32). The host answers through
    // ViewerStatus::files_only; a host that lacks file transfer refuses the
    // Hello, so `display` and the sink are not used.
    bool Start(const std::string& host, uint16_t port, uint16_t display,
               std::string pinned_fingerprint = {},
               bool allow_unverified = false,
               std::string access_token = {}, std::string password = {},
               std::string totp = {},
               std::shared_ptr<chast::DeviceIdentity> device_identity = {},
               bool trust_device = false,
               ViewerTransport transport = ViewerTransport::kAuto,
               std::string carrier_host = {}, uint16_t carrier_port = 0,
               std::string carrier_host_id = {}, std::string carrier_token = {},
               bool file_transfer_only = false);
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
    // Test hook: when set, a clipboard file list lands in this callback with
    // the staged absolute paths instead of the Windows clipboard. The loopback
    // probe uses it so an automated run never disturbs the operator's
    // clipboard; returning false reports a failed write.
    void SetClipboardFileSink(
        std::function<bool(const std::vector<std::string>&)> sink);
    // Test hook for the reverse direction: when set, this callback supplies the
    // local file list (the CF_HDROP items) the viewer announces to the host.
    // Unset reads the Windows clipboard; the loopback probe uses it so an
    // automated run never touches the operator's clipboard.
    void SetClipboardFileSource(
        std::function<bool(std::vector<std::string>&)> source);
    // Queues local paths for upload (viewer -> host). Folders are expanded
    // into their regular files, preserving the folder as the first relative
    // directory component. `relative_prefix` names the directory below the
    // host's receive root the files should land in (the folder being browsed);
    // it is empty for the root. Returns false when the session is not
    // streaming, the host did not grant file transfer, or no path was usable.
    // Hashing and folder walks run on the file worker; progress arrives through
    // ViewerStatus::file_phase and the queue counters.
    bool SendFiles(const std::vector<std::string>& paths,
                   std::string relative_prefix = {});

    // Asks the host to list one directory below its shared receive root.
    // `directory` is a '/'-separated relative path; empty lists the root.
    // Returns false when no session is streaming, the host did not grant file
    // transfer, or the request queue is full. The newest answer is taken
    // through TakeFileList(); counters and the last refusal arrive in
    // ViewerStatus::file_list_*.
    bool RequestFileList(std::string directory);
    // Asks the host to send one regular file from the shared root. The host
    // answers through ViewerStatus::file_download_*; on Ok the offer then
    // arrives like any host download and lands in the viewer's receive
    // directory, with progress in download_*.
    bool RequestFileDownload(std::string path);
    // One listing the host sent, tagged with the directory it was requested
    // for (the host echoes no path, so the engine remembers the request).
    struct FileListAnswer {
        std::string directory;
        chast::FileListResult result;
    };
    // Copies the newest listing the host sent, once per answer. Returns false
    // when nothing new arrived.
    bool TakeFileList(FileListAnswer& answer);
    // Asks the host to create a folder, rename one entry or remove one below
    // its shared receive root. `path` is the '/'-separated relative path of
    // the entry (for MakeDirectory, the folder to create); `new_name` is one
    // path component and is only used by Rename. Returns false when no session
    // is streaming, the host did not grant file transfer, an argument is
    // invalid, or the request queue is full. The answer arrives through
    // ViewerStatus::file_op_*; the page reloads the listing after it.
    bool RequestFileOperation(chast::FileOperation op, std::string path,
                              std::string new_name = {});
    // One entry of a batch download: `path` is the '/'-separated relative
    // path and `directory` says whether the walk must list it recursively.
    struct DownloadRoot {
        std::string path;
        bool directory = false;
    };
    // Downloads several browsed entries into the viewer's receive directory in
    // one walk: folders mirror below their own names, files land at the top of
    // the receive directory. Roots are walked in order with ordinary list and
    // download requests, one transfer at a time, and the page's newest listing
    // stays untouched. Returns false when no session is streaming, the host
    // did not grant file transfer, the list is empty or holds an invalid path,
    // or a walk is already running. Progress arrives in
    // ViewerStatus::folder_download_* with folder_download_roots saying how
    // many entries were selected.
    bool RequestBatchDownload(std::vector<DownloadRoot> roots);
    // Downloads one browsed folder and everything below it into the viewer's
    // receive directory, mirroring the remote tree below the folder's name.
    // The walk sends ordinary list and download requests in browse order, one
    // transfer at a time, and keeps the page's newest listing untouched.
    // Returns false when no session is streaming, the host did not grant file
    // transfer, `path` is empty or invalid, or a walk is already running.
    // Progress arrives in ViewerStatus::folder_download_*.
    bool RequestDirectoryDownload(std::string path);
    // Stops the running walk; the transfer in flight is aborted and its partial
    // file removed. The outcome is published as cancelled.
    void CancelDirectoryDownload();

    // Binds a viewer-side port-forward listener on 127.0.0.1:local_port (0
    // picks a free port) and asks the host to relay it to
    // remote_host:remote_port. Returns false when no session is streaming, the
    // host did not grant kPermissionPortForward, or the arguments are invalid
    // (an empty host or a zero remote port). The listener is applied by the
    // session thread within one poll; the bound port, the host's answer and the
    // counters arrive through ViewerStatus::port_forward_*. One target per
    // viewer: starting again replaces the running listener.
    bool StartPortForward(uint16_t local_port, const std::string& remote_host,
                          uint16_t remote_port);
    // Stops the listener and every relayed connection, and tells the host to
    // drop the target. Safe to call when nothing is running.
    void StopPortForward();

    // Copies the newest pointer state the host sent, once per received update.
    // The sprite is only attached to the update that introduced a shape, so a
    // caller caches it by `shape_id`. Returns false when nothing new arrived.
    bool TakeCursor(chast::CursorUpdate& update);

    // Mutes or unmutes system sound. Safe from the UI thread; the session
    // thread applies it to the render session. Returns false when no audio
    // pipeline is running, so the caller knows the tap had no effect.
    bool SetAudioMuted(bool mute);

    // Starts or stops a local, lossless recording of the received stream. The
    // host is never told and nothing is uploaded. `directory` is used when the
    // recording starts (empty means the user's Videos folder); the file name is
    // chosen by the viewer and rolls to a new segment when the picture size
    // changes. Recording begins on the next key frame, so a switch that finds
    // no key frame yet keeps waiting. Returns false when no session is
    // streaming. Progress and failures arrive through ViewerStatus::recording
    // / record_path / record_error.
    bool SetRecording(bool enabled, std::string directory = {});

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

    // Queues one UTF-8 chat line for the host. Returns false for empty or
    // oversized text, when no session is streaming, or when the outgoing queue
    // is full. Host lines are popped one at a time through TakeChat().
    bool SendChat(const std::string& text);
    // Pops the oldest line the host sent. Returns false when none is waiting.
    bool TakeChat(std::string& text);

    // Opens the session's remote console at the given character size. Returns
    // false when no session is streaming, the host did not grant
    // kPermissionTerminal, the size is outside 1..500 x 1..200, or a console
    // is already open (or being requested). The round trip and the host's
    // answer arrive through terminal_phase/terminal_code/terminal_reason.
    bool OpenTerminal(uint16_t columns, uint16_t rows);
    // Queues raw console input: printable text as UTF-8 and control sequences
    // as bytes (for example "\x03" for Ctrl+C). Returns false when no console
    // is open or the queue's 256 KiB bound is reached.
    bool SendTerminalInput(const std::string& bytes);
    // Queues a console resize. Returns false when no console is open or the
    // size is out of range. The host applies it best effort; the acknowledged
    // size shows up in terminal_columns/terminal_rows.
    bool ResizeTerminal(uint16_t columns, uint16_t rows);
    // Ends the console. The host closes the shell; output that was already in
    // flight is still queued for TakeTerminalOutput() until it reports ended.
    void CloseTerminal();
    // Pops the oldest console output. `ended` reports that the console is over
    // and this was its last buffered piece; the phase in the status stays
    // terminal until the panel takes the bytes. Returns false when nothing is
    // waiting, with `ended` still telling whether the console finished.
    bool TakeTerminalOutput(std::vector<uint8_t>& data, bool& ended);

    // Asks the host to enumerate its cameras. Returns false when no session is
    // streaming, the host did not grant kPermissionCamera, or a camera is
    // starting or running. The answer (possibly a refusal) arrives through
    // camera_phase/camera_code/camera_reason and the list in `cameras`.
    bool RequestCameraList();
    // Asks the host to open one listed camera. Returns false when no session
    // is streaming, the host did not grant kPermissionCamera, `camera_id` is
    // not in the newest list, or another camera is still starting or running.
    // The answer arrives through camera_phase/camera_code; a refusal carries
    // the host's reason and a success is followed by decoded pictures on the
    // camera sink.
    bool StartCamera(uint16_t camera_id);
    // Stops the running camera (or cancels one that is still starting). Safe
    // to call when nothing is running. The panel's own close moves the phase
    // immediately; the host stops sending after it sees the stop.
    void StopCamera();

    ViewerStatus Snapshot() const;

private:
    void Run(std::string host, uint16_t port, uint16_t display,
             std::string pinned_fingerprint, bool allow_unverified,
             std::string access_token, std::string password, std::string totp,
             bool trust_device, ViewerTransport transport,
             std::string carrier_host, uint16_t carrier_port,
             std::string carrier_host_id, std::string carrier_token,
             std::string audit_ref, bool file_transfer_only);
    void SetState(ViewerState state, std::string error = {});
    // Expands dropped folders and hashes queued uploads on its own thread, so
    // a multi-gigabyte read never stalls the session's receive loop.
    void FileWorker();
    struct PendingUploadRoot {
        std::string path;
        std::string prefix;
    };
    void ExpandUploadRoot(const PendingUploadRoot& root, uint64_t epoch);
    bool EnqueueUpload(const std::string& path, std::string relative_path, uint64_t epoch,
                       bool clipboard = false);
    void EnqueueUploadFailure(std::string name, std::string relative_path, std::string error,
                              uint64_t epoch);
    bool HasHashableUploadLocked() const;
    // Shared validation and queueing for RequestBatchDownload and
    // RequestDirectoryDownload. `batch` tells the page which wording to use.
    bool QueueDownload(std::vector<DownloadRoot> roots, bool batch);

    FrameSink* sink_;
    // The camera picture goes to its own Flutter texture; null means no
    // camera can be started (the command-line tools only display the desktop).
    FrameSink* camera_sink_;
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

    // Viewer-side recording. The switch crosses to the session thread, which
    // owns the muxer; `record_dir_` is read when a recording starts, so a new
    // folder applies to the next file instead of interrupting one in flight.
    std::atomic<bool> record_wanted_{false};
    mutable std::mutex record_mutex_;
    std::string record_dir_;

    // Newest privacy request, for the same reason as the display switch: the
    // operator wants the state they picked last, not a replay of every toggle.
    mutable std::mutex privacy_mutex_;
    std::optional<bool> pending_privacy_;
    // SAS is an action, not a state: one pending request is enough, and a
    // second tap replaces an unsent one instead of queueing a burst.
    std::atomic<bool> pending_sas_{false};

    // The port-forward target crosses to the session thread here. One slot is
    // enough: the operator picks a target, and a newer pick replaces an
    // unsent one. `stop` is a command of its own so it cannot be dropped by a
    // later start.
    struct PortForwardCommand {
        bool stop = false;
        uint16_t local_port = 0;
        std::string remote_host;
        uint16_t remote_port = 0;
    };
    mutable std::mutex port_forward_mutex_;
    std::optional<PortForwardCommand> pending_port_forward_;

    // Chat crosses in both directions. The outgoing queue is bounded and
    // cleared when a session starts; the session thread appends host lines for
    // the UI to pop, dropping the newest when the UI falls too far behind.
    mutable std::mutex chat_mutex_;
    std::deque<std::string> pending_chat_;
    std::deque<std::string> received_chat_;

    // One remote console crosses in both directions. Commands keep their
    // order: typing, resizes and the close must reach the host in the order
    // they happened. Input is bounded by the same high water the host applies
    // to the uplink; output is handed to the UI in pieces, and the session
    // thread stops pulling when the UI falls behind so the transport's flow
    // control pauses the host instead of dropping bytes that are part of the
    // console picture.
    struct TerminalCommand {
        enum class Kind {
            kOpen,
            kInput,
            kResize,
            kClose,
        };
        Kind kind = Kind::kOpen;
        std::string bytes;
        uint16_t columns = 0;
        uint16_t rows = 0;
    };
    mutable std::mutex terminal_mutex_;
    std::deque<TerminalCommand> pending_terminal_;
    size_t pending_terminal_bytes_ = 0;
    std::deque<std::vector<uint8_t>> terminal_output_;
    size_t terminal_output_bytes_ = 0;
    bool terminal_output_ended_ = true;

    // Camera requests cross here in order: a stop that follows a start must
    // reach the host after it, and a list answer is only useful to the panel
    // that asked for it. A stop drops commands that were never sent, so a
    // closed panel cannot leave a camera opening behind.
    struct CameraCommand {
        enum class Kind {
            kList,
            kStart,
            kStop,
        };
        Kind kind = Kind::kList;
        uint16_t camera_id = 0;
    };
    mutable std::mutex camera_mutex_;
    std::deque<CameraCommand> pending_camera_;

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
    mutable std::mutex clipboard_sink_mutex_;
    std::function<bool(const std::vector<std::string>&)> clipboard_file_sink_;
    std::function<bool(std::vector<std::string>&)> clipboard_file_source_;

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
        // The upload belongs to the local clipboard (protocol v34): the host
        // stages it for CF_HDROP and the clipboard permission gates it.
        bool clipboard = false;
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
    // Shared-folder browse requests cross in order: a listing, a download or
    // an operation the operator clicked after it must reach the host in that
    // order. The request ids map back to the paths they were made for so an
    // answer can name its entry. The newest listing waits here for
    // TakeFileList().
    struct BrowseCommand {
        enum class Kind {
            kList,
            kDownload,
            kOperation,
        };
        Kind kind = Kind::kList;
        uint32_t request_id = 0;
        chast::FileOperation operation = chast::FileOperation::MakeDirectory;
        std::string path;
        std::string new_name;
    };
    // One walk of a remote folder. The session thread owns every field; the UI
    // threads only queue a request and set `cancel`, all under browse_mutex_.
    // `directories` still have to be listed, `files` pair a remote path with
    // the local folder (relative to the receive directory) it must land in.
    struct FolderDownload {
        enum class Stage {
            kIdle,
            kListing,
            kDownloading,
            kDone,
            kFailed,
            kCancelled,
        };
        Stage stage = Stage::kIdle;
        // Selected entries not started yet; `root`/`name` describe the one in
        // progress. `directories` and `files` only ever hold its work.
        std::deque<DownloadRoot> roots;
        uint32_t batch_total = 0;
        // True when the request came from the multi-select bar, including a
        // batch that holds one file.
        bool batch = false;
        std::string root;
        std::string name;
        std::string destination;
        std::deque<std::string> directories;
        std::deque<std::pair<std::string, std::string>> files;
        uint32_t listing_id = 0;
        std::string listing_path;
        uint32_t download_id = 0;
        std::string download_path;
        std::string download_directory;
        uint32_t transfer_id = 0;
        uint64_t files_total = 0;
        uint64_t files_done = 0;
        std::string current;
        std::string error;
        std::chrono::steady_clock::time_point retry_after;
        std::chrono::steady_clock::time_point transfer_deadline;
        bool cancel = false;
        bool finished = false;
    };
    mutable std::mutex browse_mutex_;
    std::deque<BrowseCommand> pending_browse_;
    std::map<uint32_t, std::string> browse_paths_;
    std::optional<FileListAnswer> newest_file_list_;
    uint32_t next_browse_id_ = 1;
    // Set by RequestBatchDownload/RequestDirectoryDownload, consumed by the
    // session thread.
    struct FolderRequest {
        std::vector<DownloadRoot> roots;
        bool batch = false;
    };
    std::optional<FolderRequest> folder_request_;
    FolderDownload folder_;
    // Transfer id -> local folder for the walk's transfers, so an offer lands
    // in the mirrored tree instead of the receive root.
    std::map<uint32_t, std::string> folder_transfers_;
    // Transfers the walk turned away while cancelling. An offer that was
    // already in flight is refused too instead of landing in the receive root.
    std::set<uint32_t> ignored_transfers_;

    mutable std::mutex file_mutex_;
    std::condition_variable file_work_cv_;
    std::deque<FileUpload> uploads_;
    std::deque<PendingUploadRoot> pending_upload_roots_;
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
