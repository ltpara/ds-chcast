#pragma once

// The host half of a chast session: listener, capture, encode and the
// per-session handshake, packaged so the command-line tool and the tray
// application run the same code.
//
// One HostServer owns one listener. With max_viewers == 1 it serves viewers in
// sequence; above 1 the listener hands every accepted connection to a
// per-viewer HostServer worker, so a slow, stalled or vanishing viewer cannot
// hold up the others. Each viewer gets its own handshake, approval,
// permissions, capturer, encoder, streams and audit record; the policy, the
// listener, the certificate and the TCP carrier are shared. Only one desktop
// viewer at a time holds the control lease, so input injection and the
// host-wide privacy overlay have a single owner.
//
// A viewer picks a display in Hello; the capturer and encoder are created after
// the handshake so the announcement describes the output that is actually read.
// When the viewer leaves, its connection is closed and the listener stays up
// for the next one. In one-time token mode the token is rotated once the last
// active viewer has gone.
//
// status() may be read from any thread. A multi-viewer host reports the
// focused (most recently admitted) viewer and the live viewer count; the
// operator actions below target that focused viewer.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chast/carrier.h"
#include "chast/clipboard.h"
#include "chast/encoder.h"
#include "chast/file_transfer.h"
#include "chast/host_security.h"
#include "chast/desktop.h"
#include "chast/host_settings.h"
#include "chast/input.h"
#include "chast/protocol.h"
#include "chast/quic.h"

namespace chast {

class AudioReference;
class IAudioDecoder;
class IAudioPlayer;
class ILoopbackCapture;
class IMicrophoneCapture;
struct ViewerRegistry;

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
    // Lowest target a rate decision applied this session, so a verification
    // run can tell a link that forced a cut from one that never reacted.
    uint32_t lowest_target_bitrate = 0;  // bits per second
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
    // Viewer items the local clipboard refused; the previous content stays.
    uint64_t clipboard_write_failures = 0;
    // Chat lines sent to the viewer and received from it. Both directions are
    // ungated by permissions; an admitted session is the authorization.
    uint64_t chat_sent = 0;
    uint64_t chat_received = 0;
    // Composed text blocks accepted from the viewer for Unicode injection
    // (protocol v36). Gated by the input permission like InputEvent.
    uint64_t text_received = 0;
    // Viewer uploads that were verified and renamed into place, their total
    // size, and offers that were refused or failed before that.
    uint64_t files_received = 0;
    uint64_t file_bytes_received = 0;
    uint64_t file_failures = 0;
    // Operator downloads the viewer accepted and verified, their total size,
    // attempts that failed before the viewer confirmed the digest, and
    // downloads the viewer stopped itself. A cancel is not a send failure: it
    // is counted apart so the operator can tell “the viewer cancelled” from
    // “the host could not send”.
    uint64_t files_sent = 0;
    uint64_t file_bytes_sent = 0;
    uint64_t file_send_failures = 0;
    uint64_t file_send_cancels = 0;
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
    // Host-control actions (protocol v38): restart, console lock and local
    // input blocking. `block_input_active` is the state this session set; the
    // host always releases it when the session ends or the permission is
    // revoked.
    uint64_t host_actions = 0;
    uint64_t host_action_failures = 0;
    bool block_input_active = false;
    // OS login (protocol v39): passwords typed at the host's logon or lock
    // screen. `os_login_requests` counts every request, `os_login_failures`
    // the ones not answered Ok. The password itself is never kept here.
    uint64_t os_login_requests = 0;
    uint64_t os_login_failures = 0;
    // Session select (protocol v40): list and select requests handled, and the
    // accepted handoffs. A selection of the current session is answered Ok
    // without counting as a switch.
    uint64_t session_list_requests = 0;
    uint64_t session_select_requests = 0;
    uint64_t session_switches = 0;
    uint64_t session_select_failures = 0;
    // Near-end frames that passed through echo cancellation and how many times
    // its adaptive filter restarted (playback start/stop or endpoint reopen).
    uint64_t audio_dsp_frames = 0;
    uint64_t audio_dsp_resets = 0;
    bool audio_recovering = false;
    // Privacy mode: whether the local screen is masked right now, and how many
    // viewer requests changed it (refusals included).
    bool privacy_mode_active = false;
    uint64_t privacy_mode_changes = 0;
    // Ctrl+Alt+Del requests and how many were refused or failed. Every request
    // is answered, so a failed one never ends the session.
    uint64_t sas_requests = 0;
    uint64_t sas_failures = 0;
    // Viewer stream preferences: every request handled, the ones that changed
    // the encoder or its rate, and the refusals (unsupported codec, failed
    // rebuild or a busy cooldown). A request that already matched the stream
    // is counted only in requests.
    uint64_t stream_option_requests = 0;
    uint64_t stream_option_changes = 0;
    uint64_t stream_option_failures = 0;
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
    // Voice call (protocol v37): the host microphone uplink the viewer asked
    // for. requests counts every start request seen, accepted the calls that
    // answered Ok, completed the calls that stopped for any reason after being
    // accepted, and packets/bytes the Opus packets handed to the transport.
    bool voice_call_active = false;
    uint64_t voice_call_requests = 0;
    uint64_t voice_call_accepted = 0;
    uint64_t voice_call_completed = 0;
    uint64_t voice_call_packets = 0;
    uint64_t voice_call_bytes = 0;
    uint64_t voice_call_dropped = 0;
    uint64_t voice_call_errors = 0;
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
    // Viewers admitted right now. Above one, the fields below describe the
    // focused viewer (the most recently admitted) and stats.sessions counts
    // every viewer this host has admitted.
    size_t active_viewers = 0;
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
    // True when the stream is the merged union of every display on the
    // compositor's adapter. display_index is 0 and display_name is a synthetic
    // label in that case; stream.displays lists the members.
    bool display_merged = false;
    // The operator's most recent download to the viewer. `download_active`
    // stays true while the offer is pending or bytes are flowing; the tray
    // shows the name and progress while it does.
    bool download_active = false;
    std::string download_name;
    uint64_t download_size = 0;
    uint64_t download_sent = 0;
    std::string download_error;
    // TCP/WebSocket fallback. carrier_port is QUIC port + 1 and
    // carrier_error stays empty while it is healthy; a failed bind leaves the
    // host serving QUIC only. The counters are refreshed by status().
    bool carrier_listening = false;
    uint16_t carrier_port = 0;
    std::string carrier_error;
    uint64_t carrier_connections = 0;
    uint64_t carrier_active = 0;
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
    // Loopback silence gate: after this much digital silence the encoder stops
    // receiving samples. Zero encodes silence too, keeping the receiver's
    // playout fed so starvation measurements are not confused with host gating.
    uint32_t audio_silence_gate_ms = 2000;
    // Which hardware encoder family to use. Auto keeps oneVPL first and falls
    // back to FFmpeg's nvenc/amf/qsv; the command line can pin one for tests.
    EncoderBackend encoder_backend = EncoderBackend::Auto;
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
    // A successful password login replaced an older PBKDF2 hash with one at
    // the current cost. Called on the host thread; the receiver saves the new
    // encoding so the upgrade survives a restart.
    std::function<void(const std::string&)> password_hash_changed;
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
    // CF_HDROP access. Empty reads the real clipboard; the loopback check
    // substitutes a list so it never touches the operator's clipboard.
    std::function<bool(std::vector<std::string>&)> clipboard_read_files;
    // CF_HDROP write used when a viewer's clipboard file copy has been staged
    // (protocol v34). Empty writes the real clipboard; the loopback check
    // captures the list so an automated run never touches the operator's
    // clipboard. The staged files stay valid for the rest of the session.
    std::function<bool(const std::vector<std::string>&)> clipboard_write_files;
    // Called on the host thread for every ChatMessage the viewer sent. The
    // operator UI appends it to the chat window; the message is not echoed.
    std::function<void(const std::string&)> chat_message;
    // Called on the host thread for every composed text block the injector
    // accepted (protocol v36). The text has already been queued for Unicode
    // injection; the hook is for diagnostics and the loopback check.
    std::function<void(const std::string&)> text_input;
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
    // Creates the host microphone a viewer's voice call sends (protocol v37).
    // Empty opens the default communications capture device; the loopback
    // check substitutes a synthetic packet source. Called only while
    // settings.allow_microphone is set; returning nullptr (with `error`)
    // answers the call with Failed.
    std::function<std::unique_ptr<IMicrophoneCapture>(std::string*)> voice_capture;
    // Injects Ctrl+Alt+Del. Empty uses the platform SendSAS in service mode and
    // answers Unsupported otherwise (the tray host cannot call it). The
    // loopback check substitutes a callback so it never touches the real
    // secure desktop; returning false fills the reason and reports Failed.
    std::function<bool(std::string*)> send_sas;
    // Performs one host-control action: restart, console lock, or block/unblock
    // the local keyboard and pointer. Empty uses the Win32 backend. The
    // loopback check substitutes callbacks so a test never reboots or blocks
    // the operator's input; returning false fills the reason and reports
    // Failed.
    std::function<bool(HostAction action, bool block_input, std::string*)> host_control;
    // Reads what the host can type an OS password into. Empty uses the Win32
    // backend, which reports Unavailable when this process cannot open the
    // secure desktop (the tray application) and Unlocked unless a sign-in or
    // lock screen is actually current.
    std::function<OsLoginStatus()> os_login_state;
    // Types the OS password and Return. Empty uses the session's input
    // injector. Returning false fills the reason and reports Failed. The
    // password must never be logged or written anywhere by this callback.
    std::function<bool(const std::string& password, std::string*)> os_login;
    // Enumerates the Windows sessions the viewer can move the stream to. Only
    // active sessions with a signed-in user are valid targets, and `current`
    // marks the session this process runs in. Empty means the host cannot
    // switch: HelloAck withholds kPermissionSessionSelect and every request is
    // answered Unsupported. Returning false fills the reason and reports
    // Failed.
    std::function<bool(std::vector<RemoteSession>& sessions, std::string*)> list_sessions;
    // Asks the serving backend to move the worker to `session_id`. The host has
    // already re-validated the id against list_sessions and the control lease.
    // Returning false fills the reason and reports Failed; true means the
    // session is ending and the worker restarts in the target.
    std::function<bool(uint32_t session_id, std::string*)> select_session;
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

    // Queues one chat line for the current viewer. Returns false for empty or
    // oversized UTF-8, or when no viewer is streaming. Safe to call from the
    // UI thread; the frame loop sends it on the control stream.
    bool send_chat(const std::string& text);

private:
    explicit HostServer(HostServerConfig config);

    void run();
    // Sequential host: one viewer at a time, with the listener kept across
    // sessions. Selected when settings_.max_viewers is 1.
    void run_single();
    // Concurrent host: the listener accepts up to settings_.max_viewers
    // connections and each one runs on its own HostServer worker.
    void run_multi();
    // Worker entry point for one accepted connection. It waits for the Hello
    // (the policy runs here), serves the session and reports its own audit
    // record.
    void run_adopted();
    // Shared body of a connected session: publishes the viewer, runs the
    // streaming or files-only loop, finishes the transport and writes the
    // audit record. `rotate_token` is false for a viewer worker, whose parent
    // rotates the one-time token once no viewer is left.
    bool serve_current_session(HostServerStats& stats, bool rotate_token,
                               std::string* error, bool* peer_left);
    // Starts one worker for an accepted connection and makes it the focused
    // viewer for status() and the operator actions. `run_seconds_left` is what
    // remains of the listener's run budget (0 = unlimited): the worker ends its
    // own session at that deadline so the stream and audio finish with a FIN
    // instead of being torn down by the listener.
    void start_viewer(std::unique_ptr<QuicSession> connection, double run_seconds_left);
    // Joins and drops workers that finished. Runs on the parent host thread.
    void reap_viewers();
    // Stops and joins every viewer worker. Used when the listener goes away
    // and by the parent's shutdown path.
    void stop_viewers();
    // Copies the last served viewer's snapshot onto the listener, so a caller
    // that reads status() after wait() still sees the session counters and,
    // for a one-shot run, why the session ended.
    void remember_session(const HostStatus& session);
    // The viewer status(), send_chat() and the operator actions target: the
    // focused live worker, or null when nobody is streaming.
    std::shared_ptr<HostServer> focused_viewer() const;
    bool open_session(std::string* error);
    // Serves the connected viewer until the peer leaves, the run ends or an
    // error stops it. Returns false only for a failure, not for a normal
    // disconnect. `peer_left` distinguishes a vanished viewer.
    bool stream_session(HostServerStats& stats, std::string* error, bool* peer_left);
    // Serves a terminal, camera or port-forward-only session: no capturer or
    // encoder is started, and only that feature's control plane runs.
    bool control_session(SessionMode mode, HostServerStats& stats, std::string* error,
                         bool* peer_left);
    // The desktop stream itself. A multi-viewer host elects one publisher:
    // the first desktop viewer captures and encodes once and later viewers
    // receive the same frames (a second IDXGIOutputDuplication cannot exist
    // in one process). `already_announced` means this viewer followed before
    // and owns a live stream, so the description is refreshed, not announced.
    bool stream_desktop(HostServerStats& stats, std::string* error, bool* peer_left,
                        bool already_announced);
    enum class SharedFollowResult {
        Ended,    // the viewer left, the run stopped or following failed
        Promote,  // no publisher is left and this worker took the role over
    };
    // Serves a desktop viewer that is not the publisher: waits for the
    // publisher's stream description, announces (or refreshes) it and runs
    // this viewer's own control plane. `announced` says whether this session
    // already has a stream. Returns Promote after claiming the publisher role.
    SharedFollowResult follow_shared_desktop(HostServerStats& stats, std::string* error,
                                             bool* peer_left, bool* announced);
    void release_desktop_publisher();
    // Fans the publisher's output out to its followers. These run on the
    // publisher thread and never block, so a slow follower cannot stall the
    // capture loop. A follower that cannot keep up drops frames.
    void broadcast_video(const FrameHeader& header, const uint8_t* data, size_t size);
    void broadcast_audio(const AudioHeader& header, const uint8_t* data, size_t size);
    void broadcast_cursor(const CursorUpdate& update);
    void broadcast_desktop_state(DesktopKind kind);
    // Clipboard and microphone plumbing shared by the publisher and follower
    // paths; each worker owns its viewer's state.
    bool clipboard_allowed() const;
    bool read_local_clipboard_files(std::vector<ClipboardFileEntry>& entries,
                                    std::vector<std::string>& paths);
    void remember_local_clipboard();
    void apply_peer_clipboard(ClipboardContent content, HostServerStats& stats);
    void drain_peer(HostServerStats& stats);
    void poll_clipboard(std::chrono::steady_clock::time_point now, HostServerStats& stats);
    void stop_microphone(HostServerStats& stats, const std::string& reason);
    void drain_microphone(HostServerStats& stats);
    // Host microphone uplink of a voice call. handle_voice_call_requests()
    // answers the viewer's control messages and owns the capture lifetime;
    // drain_voice_call() forwards encoded packets. Both run per worker, so a
    // follower session can serve its own call without owning the desktop.
    void handle_voice_call_requests(HostServerStats& stats);
    void drain_voice_call(HostServerStats& stats);
    void stop_voice_call(HostServerStats& stats, const std::string& reason,
                         bool notify);
    bool send_sas_result(HostServerStats& stats, std::string* error, bool* peer_left);
    // Answers every pending HostActionRequest. The permission and the control
    // lease are checked here, not in the transport, so a viewer learns why an
    // action was refused. `notify` results also carry the resulting block
    // input state.
    bool handle_host_actions(HostServerStats& stats, std::string* error, bool* peer_left);
    // Answers every pending OsLoginRequest. The permission, the control lease,
    // the host setting and the desktop state are checked here, in that order,
    // immediately before the password is typed. The password is cleared as
    // soon as it has been handed over and is never logged.
    bool handle_os_login_requests(HostServerStats& stats, std::string* error,
                                  bool* peer_left);
    // Answers every pending SessionListRequest/SessionSelectRequest. The
    // permission, the control lease and the host setting are checked here, and
    // the session list is re-enumerated for every request so a stale id is
    // refused instead of switching into a session that is gone.
    bool handle_session_requests(HostServerStats& stats, std::string* error,
                                 bool* peer_left);
    // Releases local input blocking if this session owns it. `notify` answers
    // an unsolicited denial, used when the permission is revoked mid-session;
    // the session-end guard passes false because the connection is going away.
    bool release_block_input(HostServerStats& stats, std::string* error, bool* peer_left,
                             bool notify);
    void release_block_input_local();
    // Serves a viewer that asked for a file-transfer-only session: no capture,
    // encoder, display or audio exists. The same file machinery as a desktop
    // session runs in a short polling loop until the viewer leaves.
    bool file_session(HostServerStats& stats, std::string* error, bool* peer_left);
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
    // password survive a reconnect; a new HostServer replaces it. Viewer
    // workers share their parent's policy, which is thread-safe.
    std::shared_ptr<HostSecurity> security_;
    // The Result of the Hello currently being admitted. authenticate_client
    // and approve_client run on the same host thread, so committing after the
    // operator's decision cannot race another handshake.
    HostSecurity::Result pending_security_;
    // The mask the operator approved before the control lease stripped input.
    // The transport's permission ceiling is raised back to it once the stream
    // is announced, so a later lease transfer can grant input without ever
    // widening what was approved. `lease_stripped_input_` marks the temporal
    // strip (another viewer held control) as opposed to a policy exclusion.
    uint16_t approved_permissions_ = 0;
    bool lease_stripped_input_ = false;
    // A UI request is queued instead of touching the session from that
    // thread; the frame loop applies it. The pair is only meaningful together,
    // so both fields are read and written under permission_mutex_.
    std::mutex permission_mutex_;
    std::atomic<bool> permission_update_pending_{false};
    std::atomic<uint16_t> pending_permissions_{0};
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    bool started_ = false;
    // True while this session's block-input action is in effect. The OS
    // releases the block when the calling thread exits, but the session may
    // end first; the guard calls the platform again so the operator's input
    // returns before the worker exits.
    std::atomic<bool> block_input_active_{false};

    // Owned by the host thread after start(); created in create(). A viewer
    // worker owns the accepted connection instead and leaves this null.
    std::unique_ptr<QuicSession> session_;
    // Shared coordination between the listener and its viewer workers: the
    // live count, the focused viewer and the single control lease. Null on a
    // sequential host.
    std::shared_ptr<ViewerRegistry> registry_;
    // Viewer workers only. `self_` lets a worker register itself as focused;
    // `finished_` tells the parent its thread is about to end.
    std::weak_ptr<HostServer> self_;
    bool adopted_ = false;
    std::atomic<bool> finished_{false};
    std::vector<std::shared_ptr<HostServer>> viewers_;
    // The fallback listener feeds session_'s UDP port. Created once next to
    // the first session and kept across listener reopens because its target
    // address does not change; stopped before the session is destroyed.
    std::unique_ptr<CarrierServer> carrier_;
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
        // A stream write can fail because the viewer cancelled: its reset
        // closes the stream before its FileResult is drained. Such an error
        // stays uncounted until this deadline so the queued cancel can claim
        // it; an epoch value counts immediately.
        std::chrono::steady_clock::time_point failure_deadline;
        // Whether the deferred error still has to be reported to the viewer
        // when it is finally booked as a failure.
        bool report_failure = false;
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
    // Preassigned for a viewer-requested download so the answer can name the
    // transfer id before hashing starts; 0 for an operator download.
    uint32_t pending_download_transfer_id_ = 0;

    // The files named by the last FileList clipboard item the host announced
    // or received. The order is the viewer's ".chast-clipboard/<index>"
    // download key; the identity (ordered names, folder layout and sizes)
    // stops a staged CF_HDROP write from being announced straight back.
    mutable std::mutex clipboard_files_mutex_;
    std::vector<std::string> clipboard_file_paths_;
    std::vector<ClipboardFileEntry> clipboard_file_entries_;
    uint64_t last_file_identity_ = 0;
    std::vector<ClipboardFileEntry> last_file_entries_;
    std::vector<std::string> last_file_paths_;

    // The viewer's clipboard file list being staged (protocol v34). One list at
    // a time; the staging directory outlives the completed list because the
    // host's CF_HDROP points into it, and is removed when the next list starts
    // or the session ends.
    struct ClipboardIncoming {
        bool active = false;
        std::string directory;  // staging root (UTF-8)
        std::vector<ClipboardFileEntry> entries;
        std::vector<std::string> landed;  // final path per entry
        // Per entry: the staging directory below the root and the CF_HDROP
        // top-level item, both relative to the root.
        std::vector<std::string> relative;
        std::vector<std::string> top_relative;
        size_t next = 0;
        uint64_t identity = 0;
    };
    ClipboardIncoming clipboard_incoming_;
    uint64_t clipboard_stage_seq_ = 0;

    // ----- file transfer shared by both session kinds -----
    //
    // stream_session calls poll_files() once per frame; file_session calls it
    // from its own loop. Everything here runs on the host thread.

    // Picks up the receive directory and audit reference and resets the
    // per-session download throttle. Called once when a session starts.
    void begin_file_session();
    // Aborts an incomplete upload (keeping its .part file), joins the hashing
    // thread and leaves a truthful terminal state for the tray. Idempotent.
    void end_file_session();
    void poll_files(HostServerStats& stats);
    bool file_transfer_allowed() const;
    bool clipboard_transfer_allowed() const;
    // Records the viewer's announced clipboard list and creates its staging
    // directory; a previous list (and its in-flight upload) is dropped first.
    bool begin_clipboard_incoming(const std::vector<ClipboardFileEntry>& entries,
                                  HostServerStats& stats, std::string* error);
    void reset_clipboard_incoming(bool remove_directory);
    // All announced files landed: puts the top-level items on the host
    // clipboard and suppresses the echo the next local poll would see.
    void finish_clipboard_incoming(HostServerStats& stats);
    void drain_file_offers(HostServerStats& stats);
    void drain_file_data(HostServerStats& stats);
    void drain_file_cancel(HostServerStats& stats);
    void drain_file_browse(HostServerStats& stats);
    void drive_file_download(HostServerStats& stats);
    void end_upload(FileCode code, const std::string& reason, bool keep_partial,
                    bool report, HostServerStats& stats);
    void send_file_failure(const std::string& message, bool report,
                           HostServerStats& stats, bool cancelled = false);
    // A stream write failed, but the viewer may have cancelled first: keep the
    // error uncounted for a short grace so the queued cancel can claim it.
    void note_file_send_error(const std::string& message, bool report,
                              HostServerStats& stats);
    void audit_upload(const std::string& name, uint64_t bytes, bool success,
                      const std::string& detail);

    // The viewer's incoming upload. One transfer at a time: the receiver owns
    // the .part file and the answer goes out only after the name, free space
    // and resume state checked out.
    struct ActiveUpload {
        FileOffer offer;
        std::unique_ptr<FileReceiver> receiver;
        std::chrono::steady_clock::time_point answer_time;
        uint64_t written = 0;
        // A clipboard-marked offer stages into the announced list's directory
        // and never touches the receive directory (protocol v34).
        bool clipboard = false;
        size_t clipboard_entry = 0;
    };

    std::unique_ptr<ActiveUpload> upload_;
    std::string file_receive_directory_;
    // The id the current session's audit records carry: the viewer's
    // reference, or one the host generated when the viewer sent none.
    std::string session_audit_ref_;
    bool session_audit_ref_absent_ = false;
    std::string file_audit_ref_;
    bool file_audit_ref_absent_ = false;
    std::ifstream download_stream_;
    double download_tokens_ = 0.0;
    std::chrono::steady_clock::time_point download_token_time_;

    // Chat queued by the operator UI. Cleared when a session starts so a line
    // typed while nobody was connected never reaches the next viewer.
    mutable std::mutex chat_mutex_;
    std::deque<std::string> pending_chat_;

    // Multi-viewer desktop sharing. A multi-viewer host elects one publisher
    // that captures and encodes once; every later desktop viewer follows it.
    // The role flags belong to this worker's session thread; `session_mutex_`
    // guards the session handle against a publisher broadcasting into a
    // follower that is being torn down. The shared_* counters carry the
    // follower's broadcast traffic back to its own loop.
    bool desktop_publisher_ = false;
    bool desktop_info_ready_ = false;
    std::mutex session_mutex_;
    std::atomic<uint64_t> shared_frames_{0};
    std::atomic<uint64_t> shared_keyframes_{0};
    std::atomic<uint64_t> shared_bytes_{0};
    std::atomic<uint64_t> shared_frame_drops_{0};
    std::atomic<uint64_t> shared_send_errors_{0};
    std::atomic<uint64_t> shared_audio_packets_{0};
    std::atomic<uint64_t> shared_audio_bytes_{0};
    std::atomic<uint64_t> shared_audio_drops_{0};
    std::atomic<uint64_t> shared_cursor_sent_{0};
    std::atomic<uint64_t> shared_cursor_drops_{0};
    std::atomic<bool> shared_send_failed_{false};
    std::atomic<bool> shared_audio_ready_{false};

    // Clipboard baseline and microphone pipeline of this worker's viewer.
    // They live here instead of in stream_desktop() because the follower path
    // serves the same per-viewer features without owning capture.
    bool clipboard_was_allowed_ = false;
    ClipboardContent last_clipboard_;
    uint64_t last_clipboard_hash_ = 0;
    uint64_t clipboard_seq_ = 1;
    std::chrono::steady_clock::time_point next_clipboard_poll_{};
    std::unique_ptr<IAudioDecoder> microphone_decoder_;
    std::unique_ptr<IAudioPlayer> microphone_player_;
    AudioInfo microphone_info_;
    bool microphone_broken_ = false;
    // The publisher's loopback capture feeds the echo canceller; a follower
    // plays its microphone without one because the reference belongs to the
    // publisher's audio pipeline.
    std::shared_ptr<AudioReference> audio_reference_;

    // Voice call of this worker's viewer. The capture is created when the
    // viewer asks and stopped on close, permission loss or capture failure.
    std::unique_ptr<IMicrophoneCapture> voice_capture_;
    AudioInfo voice_info_;
    uint64_t voice_next_seq_ = 0;
    uint64_t voice_request_id_ = 0;
    uint64_t voice_send_drops_ = 0;

    mutable std::mutex status_mutex_;
    HostStatus status_;
};

}  // namespace chast
