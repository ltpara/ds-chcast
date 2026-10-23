#pragma once

// Client-side viewer engine for the Flutter shell:
//
//   QUIC client -> VPL decoder (D3D11 NV12) -> GPU surface texture
//
// The engine owns one worker thread and is deliberately independent of
// Flutter. The UI polls Snapshot() for state and counters; decoded pictures are
// pushed to the sink from the worker thread as D3D11 textures while the
// decoder still owns the surface.

#include <atomic>
#include <chrono>
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
    // Clipboard gate from HelloAck, and updates exchanged this session. A
    // received text stays queued until the UI takes it; counting happens when
    // it is queued, so a UI that stops polling does not lose the count.
    bool clipboard_enabled = false;
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    uint64_t clipboard_failed = 0;
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
    // What the server announced in HelloAck. `displays` is the list the UI's
    // picker shows; the streamed entry is the one whose rect starts at
    // origin_x/origin_y and matches width x height.
    std::vector<chast::DisplayDescription> displays;
    // Display switches the host accepted (including a request for the output
    // already streaming) and the refusals this engine learned about, plus the
    // host's reason for the most recent refusal. A success clears the error.
    // The UI compares both counters with the previous poll, so it can tell a
    // new refusal from the stale text of an old one; the error alone cannot
    // distinguish two identical reasons.
    uint64_t display_switches = 0;
    uint64_t display_switch_failures = 0;
    std::string display_switch_error;
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
    // Queues one text for the host. Returns false when the server did not
    // offer clipboard sync, no session is running, the text is invalid, or
    // the queue already holds the newest text. Only changes should be sent;
    // echo suppression belongs to the UI.
    bool SendClipboard(const std::string& text);
    // Pops the oldest text the host sent. Returns false when none is pending.
    bool TakeClipboard(std::string& text);

    // Copies the newest pointer state the host sent, once per received update.
    // The sprite is only attached to the update that introduced a shape, so a
    // caller caches it by `shape_id`. Returns false when nothing new arrived.
    bool TakeCursor(chast::CursorUpdate& update);

    // Mutes or unmutes system sound. Safe from the UI thread; the session
    // thread applies it to the render session. Returns false when no audio
    // pipeline is running, so the caller knows the tap had no effect.
    bool SetAudioMuted(bool mute);

    ViewerStatus Snapshot() const;

private:
    void Run(std::string host, uint16_t port, uint16_t display,
             std::string pinned_fingerprint, bool allow_unverified,
             std::string access_token, std::string password, std::string totp,
             bool trust_device);
    void SetState(ViewerState state, std::string error = {});

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

    // Clipboard crosses between the UI and the session thread the same way:
    // bounded queues, newest wins for outgoing text because the UI only sends
    // changes.
    mutable std::mutex clipboard_mutex_;
    std::deque<std::string> pending_clipboard_;
    std::deque<std::string> received_clipboard_;
    bool clipboard_enabled_ = false;

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
};

}  // namespace chast_viewer
