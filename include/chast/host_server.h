#pragma once

// The host half of a chast session: listener, capture, encode and the
// per-session handshake, packaged so the command-line tool and the tray
// application run the same code.
//
// One HostServer owns one listener and serves viewers in sequence. A viewer
// picks a display in Hello; the capturer and encoder are created after the
// handshake so the announcement describes the output that is actually read.
// When the viewer leaves, the connection is closed and the listener stays up
// for the next one. In one-time token mode the token is rotated after every
// accepted session, so a spent token never guards the next connection.
//
// Everything runs on one host thread; callbacks are invoked from it and must
// return promptly. status() may be read from any thread.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chast/clipboard.h"
#include "chast/encoder.h"
#include "chast/host_security.h"
#include "chast/host_settings.h"
#include "chast/input.h"
#include "chast/protocol.h"
#include "chast/quic.h"

namespace chast {

class ILoopbackCapture;
class IAudioPlayer;

enum class HostState {
    Stopped,    // not started, or the run ended
    Listening,  // bound and waiting for a viewer
    Streaming,  // a viewer is connected and frames are flowing
    Failed,     // the run stopped on an error; status().error says why
};

struct HostServerStats {
    uint64_t sessions = 0;  // viewers that passed the handshake
    uint64_t frames = 0;
    uint64_t encoded = 0;
    uint64_t keyframes = 0;
    uint64_t bytes = 0;
    uint64_t pointer_only = 0;
    uint64_t repeats = 0;
    uint64_t send_errors = 0;
    uint64_t stream_updates = 0;
    // Adaptive bitrate and link telemetry. target_bitrate starts at the
    // configured bitrate minus the audio reserve and only moves when the
    // controller sees loss, RTT inflation or a filling send queue.
    // framerate is the capture rate the ladder picked for that target, and
    // resolution_percent is the encoded picture size as a percentage of the
    // captured frame (100, 75 or 50).
    uint32_t target_bitrate = 0;  // bits per second
    uint16_t framerate = 0;
    uint16_t resolution_percent = 100;
    uint64_t abr_changes = 0;
    uint64_t resolution_changes = 0;
    // Viewer-requested display switches handled this session: completed
    // rebuilds plus no-ops (the requested output was already streaming), and
    // refused requests (not found, busy or the rebuild failed).
    uint64_t display_switches = 0;
    uint64_t display_switch_failures = 0;
    // Viewer-requested display mode changes for the streamed output: answered
    // Ok (a completed change or the mode was already running) and refused.
    uint64_t display_mode_changes = 0;
    uint64_t display_mode_failures = 0;
    double rtt_ms = 0.0;
    double min_rtt_ms = 0.0;
    double send_kbps = 0.0;  // encoded payload rate over the last ABR interval
    bool client_connected = false;  // at least one viewer got through
    bool peer_left = false;         // the last session ended with the peer gone
    bool stats_received = false;
    ClientStats peer;
    InputStats input_stats;
    uint64_t input_events = 0;
    uint64_t input_releases = 0;
    // Clipboard updates applied locally (received from the viewer) and sent
    // out (local changes). Both stay zero when clipboard sync is disabled.
    uint64_t clipboard_sent = 0;
    uint64_t clipboard_received = 0;
    // Viewer uploads that were verified and renamed into place, their total
    // size, and offers that were refused or failed before that.
    uint64_t files_received = 0;
    uint64_t file_bytes_received = 0;
    uint64_t file_failures = 0;
    // Operator downloads the viewer accepted and verified, their total size,
    // and attempts that failed before the viewer confirmed the digest.
    uint64_t files_sent = 0;
    uint64_t file_bytes_sent = 0;
    uint64_t file_send_failures = 0;
    // System sound. audio_active says whether the current session announced
    // and is still sending audio; the counters cover packets handed to the
    // transport, encoded bytes, packets dropped by the capture or send queues,
    // and failures that turned audio off.
    bool audio_active = false;
    uint64_t audio_packets = 0;
    uint64_t audio_bytes = 0;
    uint64_t audio_dropped = 0;
    uint64_t audio_errors = 0;
    uint64_t audio_capture_recoveries = 0;
    uint64_t audio_gate_transitions = 0;
    bool audio_recovering = false;
    // Viewer microphone. microphone_active says whether the viewer currently
    // has a CMIC stream open; the counters cover decoded blocks played on the
    // host, packets and bytes that failed to decode, and the player's
    // underruns (a gap the queue could not cover).
    bool microphone_active = false;
    uint64_t microphone_packets = 0;
    uint64_t microphone_bytes = 0;
    uint64_t microphone_frames = 0;
    uint64_t microphone_errors = 0;
    uint64_t microphone_underruns = 0;
    // Streams opened over the session, including reconnects after the viewer
    // toggled the microphone off and on.
    uint64_t microphone_streams = 0;
    // Pointer traffic. cursor_sent counts the CursorUpdates handed to the
    // transport; cursor_shapes is how many of them carried a sprite and
    // cursor_bytes their total sprite size. cursor_dropped counts updates the
    // full control queue refused (the next frame sends the same state again).
    uint64_t cursor_sent = 0;
    uint64_t cursor_shapes = 0;
    uint64_t cursor_bytes = 0;
    uint64_t cursor_dropped = 0;
    uint64_t cursor_errors = 0;
};

struct HostStatus {
    HostState state = HostState::Stopped;
    std::string fingerprint;  // SHA-256 of the certificate this host presents
    std::string token;        // the token CLI/automation has to send
    // The live authentication policy. The tray shows the temporary password
    // and the security page reads the rest; a running server reflects edits
    // only after it is recreated, except for the temporary password.
    HostAuthMode auth_mode = HostAuthMode::Both;
    bool password_configured = false;
    bool two_factor_enabled = false;
    std::string temporary_password;  // empty in click and service modes
    // Permissions granted to the current (or last) viewer after approval and
    // any live reduction. 0 when no viewer was admitted yet.
    uint16_t session_permissions = 0;
    std::string peer_address;  // "ip:port" of the current or last viewer
    uint16_t requested_display = kDisplayAuto;
    size_t display_index = 0;
    std::string display_name;
    // The operator's most recent download to the viewer. `download_active`
    // stays true while the offer is pending or bytes are flowing; the tray
    // shows the name and progress while it does.
    bool download_active = false;
    std::string download_name;
    uint64_t download_size = 0;
    uint64_t download_sent = 0;
    std::string download_error;
    ServerInfo stream;  // description from the last announce
    HostServerStats stats;
    std::string error;  // empty unless the run stopped on a failure
};

struct HostServerConfig {
    HostSettings settings;
    // The tray keeps its identity in the user's store; the Windows service
    // worker uses the machine store so the fingerprint survives logins.
    IdentityScope identity_scope = IdentityScope::CurrentUser;
    // No operator is present: click approval and the temporary password are
    // unavailable and a permanent password is mandatory. A host without one
    // still listens, but refuses every viewer and says why in the log.
    bool service_mode = false;
    // Walk the injection path without calling SendInput. Only for tests.
    bool dry_run_input = false;
    bool want_hdr = true;
    bool capture_cursor = true;
    // End the run after the first session, however it ends. The command-line
    // server uses this; the tray keeps listening.
    bool serve_once = false;
    // Stop this many seconds after the first stream was announced; 0 = run
    // until stop().
    double run_seconds = 0.0;
    // Re-announce the stream once, two seconds in. Exercises the viewer's
    // update path; the loopback check turns it on.
    bool reannounce = false;
    // Scale the encoded picture once, 2.5 s in, through the adaptive-
    // resolution path (including the encoder recreation and the second IDR).
    // Exercises a live resize for the loopback check; 0 leaves it off.
    uint16_t resolution_probe_percent = 0;
    // Force an IDR this often; 0 leaves key frames to viewer requests.
    uint32_t keyframe_interval_ms = 0;
    // Let the host lower the encoder bitrate and frame rate when the transport
    // reports loss, RTT inflation or a full send queue. The configured bitrate
    // (less the audio reserve) is the ceiling; the target recovers when the
    // path is clean again.
    bool adaptive_bitrate = true;
    // How long the first wait_for_client() may take; 0 = wait forever. Later
    // waits are unlimited.
    uint32_t first_client_timeout_ms = 0;
    uint32_t finish_timeout_ms = 15000;
    // Consecutive open/session failures before the run gives up. 0 retries
    // forever: the service worker must survive a locked desktop, where
    // capture fails until the user unlocks.
    uint32_t max_consecutive_failures = 5;

    // Human-readable progress, one line per event. May be empty.
    std::function<void(const std::string&)> log;
    // Asked when the security policy wants the operator to decide. Returning
    // allow=false refuses the viewer and keeps listening. `request.permissions`
    // is the ceiling approval starts from; the returned mask may only reduce
    // it. Runs on the host thread while the viewer waits.
    std::function<ClientApproval(const ClientRequest&)> approve_client;
    // Called when the initial token is generated or a spent one is replaced.
    // The tray saves it so the next connection uses the shown token.
    std::function<void(const std::string&)> token_changed;
    // The trust list changed: a viewer asked to be remembered, a trusted
    // device reconnected, or the operator revoked one. Called on the host
    // thread; the receiver saves the list so it survives a restart.
    std::function<void(const std::vector<TrustedDevice>&)> trusted_devices_changed;
    // Called after HelloAck with the description that went out.
    std::function<void(const ServerInfo&)> stream_announced;
    // Called for every encoded access unit that was handed to the transport,
    // in wire order. `data` is only valid for the duration of the call.
    std::function<void(const FrameHeader&, const uint8_t* data, size_t size)> packet;
    // Called for every InputEvent the session drained, in order.
    std::function<void(const InputEvent&)> input_event;
    // Rich clipboard access. Empty means the real Win32 backend (text, HTML,
    // RTF and DIB); the loopback check substitutes an in-memory pair so it
    // never touches the operator's clipboard.
    std::function<bool(ClipboardContent&)> clipboard_read;
    std::function<bool(const ClipboardContent&)> clipboard_write;
    // Called after an upload passed its digest and was renamed into place.
    std::function<void(const std::string& path, uint64_t bytes)> file_received;
    // Creates the system-audio source. Empty opens the default render
    // endpoint's loopback; the loopback check substitutes a synthetic tone so
    // it covers encode -> wire -> decode without a sound card. Called only
    // when settings.allow_audio is set; returning nullptr (with `error`) means
    // this session streams video only.
    std::function<std::unique_ptr<ILoopbackCapture>(std::string*)> audio_capture;
    // Creates the microphone player. Empty opens the default render endpoint;
    // the loopback check substitutes a sink that only counts frames so it does
    // not make noise. Created lazily, the first time a viewer opens CMIC.
    std::function<std::unique_ptr<IAudioPlayer>(std::string*)> microphone_player;
};

class HostServer {
public:
    // Opens the listener synchronously, so a taken port or an unusable
    // certificate is reported before start(). Generates an access token when
    // the settings carry none.
    static std::unique_ptr<HostServer> create(HostServerConfig config, std::string* error);

    ~HostServer();
    HostServer(const HostServer&) = delete;
    HostServer& operator=(const HostServer&) = delete;

    // Starts the host thread. Call once.
    void start();
    // Asks the run to stop and waits for the thread. Safe to call twice.
    void stop();
    // Waits for the run to end on its own (serve_once or run_seconds).
    void wait();
    // False once the thread has finished.
    bool running() const;

    HostStatus status() const;
    // The token CLI/automation needs right now. Rotates after a one-time
    // session.
    std::string token() const;

    // Generates a new temporary password and returns it (empty in click and
    // service modes). Safe to call from the UI thread.
    std::string refresh_temporary_password();

    // Unexpired trusted devices, most recently used first.
    std::vector<TrustedDevice> trusted_devices() const;
    // Revoking and clearing also publish the new list through
    // trusted_devices_changed. Safe to call from the UI thread.
    bool revoke_trusted_device(const std::vector<uint8_t>& public_key);
    void clear_trusted_devices();

    // Asks the host thread to reduce or restore the current session's
    // permissions. The mask is clamped to what was granted at approval time
    // and to the settings gates; the viewer is told through PermissionUpdate.
    // Ignored while no session is streaming.
    void set_permissions(uint16_t permissions);

    // Queues one file to send to the current viewer. Only valid while a viewer
    // is streaming and file transfer is allowed; the host thread hashes the
    // file off the frame loop, offers it and streams it in budgeted chunks.
    // Returns false with `error` for an unusable path, an active transfer or
    // a missing session.
    bool start_file_download(const std::string& utf8_path, std::string* error);

private:
    explicit HostServer(HostServerConfig config)
        : config_(std::move(config)), settings_(config_.settings) {}

    void run();
    bool open_session(std::string* error);
    // Serves the connected viewer until the peer leaves, the run ends or an
    // error stops it. Returns false only for a failure, not for a normal
    // disconnect. `peer_left` distinguishes a vanished viewer.
    bool stream_session(HostServerStats& stats, std::string* error, bool* peer_left);
    bool sleep_unless_stopping(std::chrono::milliseconds duration);
    void log(const std::string& message);
    void publish_state(HostState state, const std::string& error = {});
    void publish_stats(const HostServerStats& stats);
    void publish_stream(const ServerInfo& info, size_t display_index,
                        const std::string& display_name);
    void publish_security_status();

    // Policy hooks handed to the transport. Evaluate one Hello; the transport
    // calls the approval wrapper only when the policy asks the operator.
    ClientAuthResult authenticate_client(const ClientRequest& request);
    ClientApproval approve_client(const ClientRequest& request);
    void commit_security();
    HostSecurity::Config security_config(const std::string& fingerprint);
    void refresh_security_config();
    void publish_trusted_devices();

    HostServerConfig config_;
    HostSettings settings_;
    // The policy outlives listener reopens so lockouts and the temporary
    // password survive a reconnect; only a new HostServer replaces it.
    std::unique_ptr<HostSecurity> security_;
    // The Result of the Hello currently being admitted. authenticate_client
    // and approve_client run on the same host thread, so committing after the
    // operator's decision cannot race another handshake.
    HostSecurity::Result pending_security_;
    // A UI request is queued instead of touching the session from that
    // thread; the frame loop applies it.
    std::atomic<bool> permission_update_pending_{false};
    std::atomic<uint16_t> pending_permissions_{0};
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    bool started_ = false;

    // Owned by the host thread after start(); created in create().
    std::unique_ptr<QuicSession> session_;
    bool run_clock_started_ = false;
    std::chrono::steady_clock::time_point run_start_;

    // The operator download. Queued by the UI, hashed off the frame loop,
    // then offered and streamed from the host thread.
    struct FileSend {
        enum class Stage {
            kNone,
            kHashing,
            kReady,
            kOffered,
            kSending,
            kFinishing,
            kDone,
            kFailed,
        };
        Stage stage = Stage::kNone;
        uint32_t transfer_id = 0;
        std::string path;
        std::string name;
        std::string error;
        uint64_t size = 0;
        uint64_t sent = 0;
        uint64_t resume_offset = 0;
        std::array<uint8_t, 32> digest{};
        std::chrono::steady_clock::time_point answer_time;
        // Set once the frame loop has counted this failure, so a terminal
        // state is not tallied again on every pass.
        bool counted = false;
    };
    void publish_download();

    mutable std::mutex download_mutex_;
    FileSend send_;
    std::thread send_hasher_;
    uint32_t next_send_id_ = 1;
    std::string pending_download_path_;

    mutable std::mutex status_mutex_;
    HostStatus status_;
};

}  // namespace chast
