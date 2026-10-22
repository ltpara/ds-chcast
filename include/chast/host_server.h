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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "chast/encoder.h"
#include "chast/host_settings.h"
#include "chast/input.h"
#include "chast/protocol.h"
#include "chast/quic.h"

namespace chast {

class ILoopbackCapture;

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
    // configured (or hardware auto) bitrate and only moves when the
    // controller sees loss, RTT inflation or a filling send queue.
    uint32_t target_bitrate = 0;  // bits per second
    uint64_t abr_changes = 0;
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
    std::string token;        // the token the next viewer has to send
    std::string peer_address;  // "ip:port" of the current or last viewer
    uint16_t requested_display = kDisplayAuto;
    size_t display_index = 0;
    std::string display_name;
    ServerInfo stream;  // description from the last announce
    HostServerStats stats;
    std::string error;  // empty unless the run stopped on a failure
};

struct HostServerConfig {
    HostSettings settings;
    // The tray keeps its identity in the user's store; the Windows service
    // worker uses the machine store so the fingerprint survives logins.
    IdentityScope identity_scope = IdentityScope::CurrentUser;
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
    // Force an IDR this often; 0 leaves key frames to viewer requests.
    uint32_t keyframe_interval_ms = 0;
    // Let the host lower the encoder bitrate when the transport reports loss,
    // RTT inflation or a full send queue. The configured bitrate is the
    // ceiling; the target recovers when the path is clean again.
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
    // Asked after the token check passed; returning false refuses the viewer
    // and keeps listening. Runs on the host thread while the viewer waits.
    std::function<bool(const ClientRequest&)> approve_client;
    // Called when the initial token is generated or a spent one is replaced.
    // The tray saves it so the next connection uses the shown token.
    std::function<void(const std::string&)> token_changed;
    // Called after HelloAck with the description that went out.
    std::function<void(const ServerInfo&)> stream_announced;
    // Called for every encoded access unit that was handed to the transport,
    // in wire order. `data` is only valid for the duration of the call.
    std::function<void(const FrameHeader&, const uint8_t* data, size_t size)> packet;
    // Called for every InputEvent the session drained, in order.
    std::function<void(const InputEvent&)> input_event;
    // Text clipboard access. Empty means the real Win32 CF_UNICODETEXT
    // backend; the loopback check substitutes an in-memory pair so it never
    // touches the operator's clipboard.
    std::function<bool(std::string&)> clipboard_read;
    std::function<bool(const std::string&)> clipboard_write;
    // Creates the system-audio source. Empty opens the default render
    // endpoint's loopback; the loopback check substitutes a synthetic tone so
    // it covers encode -> wire -> decode without a sound card. Called only
    // when settings.allow_audio is set; returning nullptr (with `error`) means
    // this session streams video only.
    std::function<std::unique_ptr<ILoopbackCapture>(std::string*)> audio_capture;
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
    // The token a viewer needs right now. Rotates after a one-time session.
    std::string token() const;

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

    HostServerConfig config_;
    HostSettings settings_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    bool started_ = false;

    // Owned by the host thread after start(); created in create().
    std::unique_ptr<QuicSession> session_;
    bool run_clock_started_ = false;
    std::chrono::steady_clock::time_point run_start_;

    mutable std::mutex status_mutex_;
    HostStatus status_;
};

}  // namespace chast
