#pragma once
#include <array>
#include <atomic>

// Minimal QUIC session built on MsQuic, carrying the chast protocol.
//
// One QuicSession is one connection: the server side accepts a single client,
// announces the stream in a HelloAck, and writes access units on a server-
// initiated unidirectional stream (plus a second audio stream when the host
// shares system sound); the client side connects, performs the Hello
// handshake, and reads both. Control messages share a client-initiated
// bidirectional stream. A viewer that was granted the microphone opens a
// client-initiated `CMIC` stream in the other direction; opening and closing
// that stream is the microphone switch. See chast/protocol.h for the wire
// format.
//
// A server created with QuicServerConfig::allow_input also injects the
// viewer's pointer and keyboard events locally (chast/input.h) and says so in
// the HelloAck; a client checks ServerInfo::input_enabled before sending.
//
// The class is deliberately blocking and single-owner: MsQuic callbacks run on
// its worker threads, but every public method is called from one application
// thread. The methods that touch the peer wait on condition variables and
// return Error on timeout, transport failure, or protocol violation.
//
// TLS: the server presents a persistent self-signed certificate from the
// user's certificate store (see chast/identity.h). A client must pin its
// SHA-256 fingerprint; MsQuic's validation callback compares the presented
// certificate and fails the handshake on a mismatch. allow_any_certificate is
// a development escape hatch, not a default.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chast/clipboard.h"
#include "chast/error.h"
#include "chast/input.h"
#include "chast/protocol.h"
#include "chast/security.h"

namespace chast {

// Why the host is asking the operator. The transport itself never decides
// this; HostServer annotates the request when it forwards a RequireApproval
// decision, so the approval window can explain the situation.
enum class ClientApprovalMethod {
    Unknown,     // a custom approval callback, no host annotation
    Click,       // the host's mode asks the operator for every viewer
    NoPassword,  // the viewer sent no password and asked for approval
};

// One Hello, handed to QuicServerConfig::authenticate and, when the host asks
// for a decision, to approve_client. Both run on the application thread (the
// one calling wait_for_client), never on a MsQuic worker, so they may hash a
// password, show a dialog or touch the settings file.
struct ClientRequest {
    uint16_t display = kDisplayAuto;
    // What the connection is for. A multi-viewer host uses it to decide which
    // session may hold the single control lease.
    SessionMode mode = SessionMode::Desktop;
    // "ip:port" of the connecting peer, empty when the transport would not
    // report it.
    std::string peer_address;
    // Everything the viewer sent, including the password, TOTP code and the
    // device proof. The signature has already been parsed but not verified:
    // only the policy knows which keys it trusts.
    ClientHello hello;
    // v62: the per-connection challenge the transport sent on the control
    // stream before this Hello. All zero when no challenge was sent. A managed
    // policy verifies the ticket possession proof against it.
    std::array<uint8_t, kTicketProofHostNonceSize> host_nonce{};
    // Authentication's permission ceiling: the host's defaults intersected
    // with what the viewer requested. The approval callback starts from this
    // mask and may only reduce it.
    PermissionMask permissions = 0;
    // Set by HostServer before it calls approve_client; Unknown for the raw
    // transport request.
    ClientApprovalMethod method = ClientApprovalMethod::Unknown;
    // The viewer proved its device key and asked to be remembered.
    bool device_proof = false;
    // True while the connection that sent this Hello is still usable. Set by
    // the transport and valid only for the duration of authenticate /
    // approve_client: those run on the application thread while the viewer
    // waits, and an operator prompt can poll it to close itself when the
    // viewer gives up. Admission is re-checked after the callback returns, so
    // approving a connection that died in the meantime cannot start a session.
    std::function<bool()> peer_alive;
};

enum class ClientAuthAction {
    Allow,            // admitted, with `permissions`
    RequireApproval,  // the operator has to decide (see approve_client)
    Deny,             // refused; `denial` and `reason` go to the viewer
};

struct ClientAuthResult {
    ClientAuthAction action = ClientAuthAction::Deny;
    AccessDenial denial = AccessDenial::Declined;
    std::string reason;
    // Used for Allow and as the ceiling for RequireApproval. Bits outside the
    // server's allow_* gates are dropped by the transport.
    PermissionMask permissions = 0;
};

struct ClientApproval {
    bool allow = false;
    // The operator's grant for this session. Bits outside the authentication
    // ceiling are dropped, so approval can only reduce.
    PermissionMask permissions = 0;
};

// Where the server keeps its key and certificate. The tray is per-user; the
// Windows service needs an identity that survives logins and belongs to the
// machine.
enum class IdentityScope {
    CurrentUser,
    LocalMachine,
};

struct QuicServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 4433;
    // How long the connection may sit idle before the transport closes it.
    uint32_t idle_timeout_ms = 30000;
    // How many viewers the listener connects at once. 1 keeps the legacy
    // single-session slot that wait_for_client() reuses; a value above 1 turns
    // the server into a listener that hands every accepted connection to the
    // application through accept(), each as its own QuicSession. The cap is
    // enforced before TLS completes, so an extra peer is refused immediately.
    //
    // Every accepted session runs its own Hello, permissions and streams. The
    // listener keeps the registration, configuration and certificate alive for
    // as long as any accepted session exists.
    uint32_t max_clients = 1;
    // Consumed after authentication and approval; max_clients separately
    // bounds all transports, including peers that have not sent Hello.
    uint32_t max_authenticated_clients = 0;
    // How long a connection may stay open without sending its Hello. A silent
    // peer would otherwise hold a session slot forever: the server keepalive
    // keeps the idle timeout from firing. On expiry the connection is shut
    // down and the slot is released for the next viewer. 0 disables the
    // pre-Hello timeout.
    uint32_t hello_timeout_ms = 10000;
    // Accept InputEvents from the viewer. Off by default: a client that
    // connects to a port bound to loopback is still a client, and the gate
    // belongs to the server, not to the binding.
    bool allow_input = false;
    // Accept ClipboardUpdates from the viewer and allow the server to send its
    // own. Off by default for the same reason as allow_input; HostServer turns
    // it on from the host settings.
    bool allow_clipboard = false;
    ClipboardDirection clipboard_direction = ClipboardDirection::Both;
    // Accept CCLB streams from the viewer and let the viewer upload files.
    // Same default and same reason as allow_clipboard.
    bool allow_file_transfer = false;
    // Open an audio stream when the announced ServerInfo carries an enabled
    // AudioInfo. Off by default; HostServer turns it on from the host
    // settings. When false, announce() clears audio.enabled so the viewer is
    // never told about a stream that will not exist.
    bool allow_audio = false;
    // Accept a viewer-initiated CMIC microphone stream. Off by default: the
    // microphone is a privacy feature, so a host that does not opt in never
    // sees a stream at all. announce() clears microphone_enabled when false,
    // and the transport aborts CMIC streams opened anyway.
    bool allow_microphone = false;
    // Accept SetPrivacyMode from the viewer. Off by default for the same
    // reason as allow_microphone: the overlay hides the operator's own screen.
    // announce() clears privacy_mode_enabled when false, and requests are
    // dropped by the transport.
    bool allow_privacy_mode = false;
    // Accept PortForwardRequest and CPFW streams from the viewer. Off by
    // default: forwarding reaches hosts the operator's own machine can dial,
    // so it is a firewall decision, not a viewer convenience. announce()
    // clears port_forward_enabled when false.
    bool allow_port_forward = false;
    // Accept TerminalRequest and CTTY streams from the viewer. Off by default:
    // a terminal is a shell on the operator's own account, so the host opts in
    // explicitly. announce() clears terminal_enabled when false.
    bool allow_terminal = false;
    // Accept ListCameras/StartCamera and open CCAM streams. Off by default: a
    // camera shows the operator's room, so the host opts in explicitly.
    // announce() clears camera_enabled when false.
    bool allow_camera = false;
    // Consent to the viewer recording the received stream locally. Advisory:
    // the host cannot observe or stop a recording on the viewer's machine, so
    // announce() only reports record_enabled and the viewer hides its record
    // button. On by default, matching RustDesk's "enable-record-session".
    bool allow_record = true;
    // Accept HostActionRequest for a remote restart or for blocking the local
    // keyboard and pointer. Off by default: both change the operator's machine
    // state, like port forwarding and the camera. announce() clears the
    // matching permission when false.
    bool allow_restart = false;
    bool allow_block_input = false;
    // Accept OsLoginRequest and grant kPermissionOsLogin. Off by default: it
    // lets the viewer type the OS password at the logon or lock screen. The
    // host still checks the desktop state for every request, so an unlocked
    // desktop or a UAC prompt refuses it even with this on.
    bool allow_os_login = false;
    // Accept SessionListRequest/SessionSelectRequest and grant
    // kPermissionSessionSelect. Off by default: the list names other signed-in
    // users and a selection moves the stream to another Windows session. The
    // host only sets this in service mode, where a backend can actually switch.
    bool allow_session_select = false;
    // Accept RemoteConfigRequest and grant kPermissionRemoteConfig. Off by
    // default: an accepted change is stored in the host settings and outlives
    // the session.
    bool allow_remote_config = false;
    // Accept set_permissions() during a session. Off freezes the grant made
    // at approval time; the operator can then only end the session.
    bool allow_permission_change = true;
    // Optional process-wide policy. A reduction gates transport operations
    // immediately, independently of a session's approval and UI permission lock.
    std::shared_ptr<std::atomic<PermissionMask>> permission_policy;
    // Walk the injection path without calling SendInput. Only for the
    // loopback check, which must not move the real pointer.
    bool dry_run_input = false;
    // Called once per Hello by wait_for_client(). This is the policy hook:
    // password/2FA/trusted-device checks and the permission defaults live
    // here. When empty the legacy token gate below applies instead.
    std::function<ClientAuthResult(const ClientRequest&)> authenticate;
    // When non-empty, Hello must carry exactly this token or the server
    // answers AccessDenied. The comparison is constant time and happens
    // inside the TLS-authenticated connection. Ignored when `authenticate`
    // is set: the policy then owns token authentication.
    std::string access_token;
    // false: the first accepted Hello consumes the token, so a second viewer
    // presenting it is refused with AccessDenial::TokenUsed. true: the token
    // stays valid for later sessions (a fixed passphrase).
    bool reuse_access_token = false;
    // Called when authenticate() returned RequireApproval. A refusal sends
    // AccessDenied and keeps the listener waiting for another client within
    // the same timeout, unless stop_after_denial is set. The callback runs on
    // the calling thread, not on a MsQuic thread, so it may prompt an
    // operator; it should still return promptly, because the peer is waiting.
    std::function<ClientApproval(const ClientRequest&)> approve_client;
    // End wait_for_client() with PermissionDenied after a refused Hello
    // instead of waiting for the next client. The command-line server uses
    // this so one wrong password is reported and the process exits; the
    // interactive host keeps listening.
    bool stop_after_denial = false;
    // LocalMachine requires the caller to be able to create machine keys and
    // write to the machine certificate store (a service running as SYSTEM).
    IdentityScope identity_scope = IdentityScope::CurrentUser;
    // Local UAC handoff only: retain the original user's certificate/DPAPI
    // identity even when UAC was approved by a different administrator.
    // Owns a token HANDLE with CloseHandle as its deleter; never sent on QUIC.
    std::shared_ptr<void> identity_user_token;
    // Local handoff must reuse this exact stored identity and must never
    // create a replacement certificate when the original cannot be opened.
    std::string expected_identity_fingerprint;
};

// What the QUIC stack says about the connection. RTT and loss come from the
// transport's own packet accounting; queued_* are this session's application
// sends that MsQuic has not completed yet, which is what tells an adaptive
// controller that the host is outrunning the path rather than the desktop.
struct TransportStats {
    bool available = false;  // false when no connection existed or the query failed
    uint32_t rtt_us = 0;
    uint32_t min_rtt_us = 0;
    uint32_t max_rtt_us = 0;
    uint64_t send_bytes = 0;   // UDP payload bytes
    uint64_t recv_bytes = 0;
    uint64_t sent_packets = 0;
    uint64_t lost_packets = 0;  // suspected minus spurious, cumulative
    uint32_t congestion_window = 0;
    size_t queued_bytes = 0;
    size_t queued_sends = 0;
};

// The client's clock-sync result. offset_us maps a client steady-clock reading
// to the host's: host_us = client_us + offset_us. `valid` stays false until
// the first LatencyReply; values are raw samples, not smoothed.
struct LatencySample {
    bool valid = false;
    uint64_t rtt_us = 0;
    int64_t offset_us = 0;
    uint64_t probes_sent = 0;
    uint64_t replies = 0;
    // Server probes measure application round trips, including reply handling.
    // Nonzero while its one outstanding probe has not been answered.
    uint64_t pending_us = 0;
    // Server: capture-to-application-receipt round trip. This includes video
    // backlog that a probe on the independent control stream cannot observe.
    uint64_t video_delay_us = 0;
    uint64_t video_pending_us = 0;
    uint64_t video_replies = 0;
    size_t video_pending_frames = 0;
    size_t video_pending_bytes = 0;
};

// One piece of a CFIL upload. `offset` is the absolute file offset of the
// first byte, so the host can check contiguity and its resume position.
struct FileChunk {
    uint32_t transfer_id = 0;
    uint64_t offset = 0;
    std::vector<uint8_t> data;
};

// What the transport knows about one incoming upload.
struct FileUploadState {
    uint64_t resume_offset = 0;  // where the CFIL stream started
    uint64_t received = 0;       // content bytes, resume offset excluded
    uint64_t wire_bytes = 0;     // CFIL body bytes, compressed framing included
    bool compressed = false;     // the sender picked the v2 block stream
    bool finished = false;       // stream ended and every chunk was taken
    std::string error;           // set when the stream broke
};

// A completed host screenshot. `png` is the file the host encoded from its raw
// frame; a refusal leaves it empty and explains itself in `reason`.
struct ScreenshotResult {
    uint32_t stream_id = 0;
    ScreenshotCode code = ScreenshotCode::Ok;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string reason;
    std::vector<uint8_t> png;
};

// One forwarded TCP connection, named on this side by an opaque handle. The
// server only ever hands out a handle through take_port_forward_open(); the
// client gets one from open_port_forward_connection().
using PortForwardHandle = uint64_t;

// A connection the viewer opened and the host has not answered yet.
struct PortForwardOpen {
    PortForwardHandle handle = 0;
    uint32_t forward_id = 0;
    uint32_t connection_id = 0;
    // The registered target the viewer dialed. Only the host knows it,
    // because it is the side that dials.
    std::string host;
    uint16_t port = 0;
};

struct QuicClientConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    uint32_t idle_timeout_ms = 30000;
    // Sent in Hello. Empty when the server accepts connections without one.
    std::string access_token;
    // Interactive authentication. An empty password asks the host for click
    // approval when its mode allows that. `totp` is the six-digit code the
    // viewer collected after a TwoFactorRequired denial.
    std::string password;
    std::string totp;
    // Lowercase hex reference for this session. The host copies it into the
    // inbound audit records it forwards, so the server can merge the viewer's
    // report with the host's. Empty is allowed for tools.
    std::string audit_ref;
    // The permissions the viewer asks for; the host intersects them with its
    // defaults and the operator's approval.
    PermissionMask requested_permissions = kPermissionAll;
    // What this connection is for. Desktop captures the remote desktop;
    // FileTransfer asks for a control-only session that browses the shared
    // directories and moves files. The host may refuse the mode.
    SessionMode session_mode = SessionMode::Desktop;
    // Non-owning, must outlive the session. When set, Hello carries the public
    // key and name and is signed with the host fingerprint, which lets the
    // host recognize this device and skip 2FA. The nonce and timestamp are
    // generated inside handshake().
    const DeviceIdentity* device_identity = nullptr;
    DevicePlatform device_platform = DevicePlatform::Windows;
    // Ask the host to remember this device after 2FA succeeds.
    bool trust_device = false;
    // SHA-256 fingerprint of the server certificate (uppercase hex; colons,
    // spaces and lowercase are accepted). The client completes the TLS
    // validation callback itself and fails the handshake when the presented
    // certificate names a different fingerprint. An empty pin is an error
    // unless allow_any_certificate is set, so an unauthenticated connection is
    // always an explicit choice.
    std::string pinned_fingerprint;
    // Development escape hatch: accept any certificate. Never use it on an
    // untrusted network; there is no identity check behind it.
    bool allow_any_certificate = false;
    // v35: advertise the datagram-audio capability in Hello so the host sends
    // system audio as QUIC DATAGRAM frames. On by default; false asks for the
    // reliable audio stream (A/B measurements and very small datagram paths).
    bool audio_datagram = true;
    // Tool receivers support all compiled formats by default. Product GPU
    // receivers must populate this from the rendering device's decoder caps.
    VideoCodecMask decoding_codecs = kVideoCodecAll;
    StreamCodecPreference codec_preference = StreamCodecPreference::Auto;
    VideoCodecMask decoding_444 = 0;
    VideoChroma prefer_chroma = VideoChroma::Yuv420;
    // v62 managed access: the signed access ticket (envelope plus Ed25519
    // signature) and the 32-byte Ed25519 seed whose public key the ticket
    // names. Both empty on unmanaged hosts. When set, handshake() waits for
    // the host's AuthChallenge before sending Hello and attaches the
    // possession proof; a managed host refuses a Hello without one.
    std::vector<uint8_t> access_ticket;
    std::vector<uint8_t> access_ticket_key;
};

class QuicSession {
public:
    // Server: opens a registration, configuration and listener on
    // config.bind_address:config.port. Fails when the port is taken or
    // MsQuic/the certificate cannot be initialised.
    static std::unique_ptr<QuicSession> create_server(const QuicServerConfig& config,
                                                      std::string* error);
    // Client: opens the registration and configuration; the connection is
    // started by connect().
    static std::unique_ptr<QuicSession> create_client(const QuicClientConfig& config,
                                                      std::string* error);

    ~QuicSession();
    QuicSession(const QuicSession&) = delete;
    QuicSession& operator=(const QuicSession&) = delete;

    // ----- server -----

    // Waits for a client to connect and send a Hello that the access token
    // and the approval callback accept. A refused Hello is answered with
    // AccessDenied and the wait continues with the next client, so one wrong
    // token does not take the server down; a client that leaves before the
    // handshake frees its slot the same way. A transport or protocol failure
    // ends the wait early.
    Error wait_for_client(std::chrono::milliseconds timeout);
    // Multi-client listener (max_clients > 1) only: pops one accepted
    // connection into `client`. The returned session is in the server role,
    // has its own MsQuic registration for callbacks, and reports hello errors
    // through wait_for_client() exactly like a single-client server. Its
    // configuration and identity belong to the listener, which must outlive
    // every accepted session. Returns WaitTimeout when no viewer arrived.
    Error accept(std::unique_ptr<QuicSession>& client, std::chrono::milliseconds timeout);
    // Replaces the policy hooks of an accepted session before its Hello is
    // evaluated. The listener's own hooks run on the listener object, so a
    // host that serves several viewers must point each accepted session at the
    // state that belongs to that viewer.
    void set_server_callbacks(
        std::function<ClientAuthResult(const ClientRequest&)> authenticate,
        std::function<ClientApproval(const ClientRequest&)> approve_client);
    // The display index the client asked for in Hello, or kDisplayAuto. Valid
    // once wait_for_client() succeeded; the server creates its capturer after
    // that so the request can pick the output.
    uint16_t requested_display() const;
    VideoCodecMask decoding_codecs() const;
    StreamCodecPreference codec_preference() const;
    VideoCodecMask decoding_444() const;
    VideoChroma chroma_preference() const;
    // The session mode the client asked for in Hello. Valid once
    // wait_for_client() succeeded; the host decides whether to serve it.
    SessionMode requested_mode() const;
    // True while the transport still believes the connection is up. A session
    // without a media stream (files only) uses it to notice a viewer that
    // closed without sending a Bye; a video session learns the same from
    // receive_frame().
    bool peer_alive() const;
    // Server: true once the viewer sent its closing Stats or Bye, or the
    // connection dropped. A control-only session polls this to end its loop;
    // finish_server() still performs the shutdown and collects the stats.
    bool viewer_finished() const;
    // The session's audit reference from Hello, empty when the viewer did not
    // send one. Valid once wait_for_client() succeeded; the host copies it
    // into its own audit records.
    std::string audit_ref() const;
    // SHA-256 fingerprint of the certificate this server presents, uppercase
    // hex. Stable across restarts because the certificate is stored in the
    // user's certificate store; this is what a client pins.
    std::string server_fingerprint() const;
    // "ip:port" of the client whose Hello this session accepted, empty when
    // the transport did not report it. Valid after wait_for_client().
    std::string client_address() const;
    // The permission mask in force: what the accepted viewer was granted on
    // the server, or what the host last announced on the client.
    PermissionMask permissions() const;
    // Server: replaces the mask mid-session and tells the viewer. The new
    // mask is clamped to the grant approved at connect time, so a permission
    // the operator did not allow cannot be restored later. Revoking input
    // also lifts anything the injector still holds. Requires an announced
    // stream.
    Error set_permissions(PermissionMask permissions);
    // Thread-safe policy revocation: closes the gate now; the serving thread
    // calls apply_permission_policy() to abort streams and notify the peer.
    void revoke_policy_permissions(PermissionMask permissions);
    Error apply_permission_policy();
    // Server: raises or lowers what a later set_permissions() may grant,
    // without changing the mask in force. The new ceiling is clamped to what
    // the configuration allows and never exceeds the mask the operator
    // approved; the multi-viewer host uses it so a viewer that joined
    // view-only can receive input once the control lease frees. Requires an
    // announced stream.
    Error set_permission_ceiling(PermissionMask permissions);
    // Replaces the token the next client has to send. Call between sessions:
    // a token spent by the previous one becomes valid again under its new
    // value, which is how a host rotates a one-time token without rebinding.
    void set_access_token(std::string token);
    // Server (v62 managed access): pops the newest ticket renewal the viewer
    // sent. Verify it with HostSecurity::renew_ticket(); on acceptance the
    // host extends its monotonic lease to RenewalResult::expires_at and
    // narrows the session mask, and it answers with send_ticket_renew_result().
    // A refused renewal must end the session after the answer is queued.
    bool take_ticket_renew(TicketRenewRequest& request);
    Error send_ticket_renew_result(const TicketRenewResult& result);
    // Closes the finished connection and clears the per-session state, leaving
    // the listener up. Call after finish_server() so wait_for_client() can
    // accept the next client.
    void reset_for_next_client();
    // Answers the Hello with the stream description and opens the media
    // streams. Must be called once, after wait_for_client() succeeded. The
    // audio stream is opened too when `info.audio.enabled` is set and the
    // server allows audio; if the stream cannot be opened the announcement
    // falls back to video only instead of failing the session. When
    // `info.files_only` is set no media streams are opened and the picture
    // and source sizes are zeroed in the message.
    Error announce(const ServerInfo& info);
    // Replaces the stream description mid-session: the captured output's size,
    // origin, rotation or codec changed after a capture rebuild. Sent on the
    // control stream; the viewer applies it to its pointer mapping and
    // decoder. The video stream, its sequence numbers and its frames continue
    // unchanged, so the caller should force an IDR around the update.
    Error refresh_stream(const ServerInfo& info);
    // Sends one access unit. The frame's sequence number, payload size and
    // CRC32 are filled in here; everything else comes from `header`. Blocks
    // when too much unacknowledged data is queued (backpressure), so the
    // caller can keep capturing without unbounded memory growth.
    Error send_frame(FrameHeader header, const uint8_t* payload, size_t size);
    // The same send without backpressure: WaitTimeout means the queue was full
    // and the frame was dropped, any other error means the stream is unusable.
    // A host that shares one encoded stream between viewers uses this so a
    // slow viewer cannot stall the capture loop.
    Error send_frame_nowait(FrameHeader header, const uint8_t* payload, size_t size);
    // A static-desktop refinement: best effort like RustDesk's per-subscriber
    // repeat send. It claims no delivery-window credit, so refinements never
    // look like a stalled viewer and never pause the shared capture loop.
    Error send_frame_best_effort(FrameHeader header, const uint8_t* payload, size_t size);
    // Advisory: bounded video delivery window and transport capacity. A
    // refused nowait send consumes neither a frame nor a sequence number.
    bool video_send_ready() const;
    // Sends one audio access unit on the audio stream. Like send_frame it
    // fills in the sequence number, size and CRC. Unlike it, the send never
    // waits: when the shared video/audio queue is full the packet is dropped
    // and WaitTimeout is returned, so audio can never stall video. A dropped
    // packet is a gap in the AAC stream and shows up as a short glitch.
    Error send_audio(AudioHeader header, const uint8_t* payload, size_t size);
    // Ends the audio stream gracefully (host setting turned audio off). The
    // stream cannot be reopened on this connection; a StreamInfo announcing
    // audio disabled tells the viewer why it ended.
    Error stop_audio();
    // Pops the next microphone packet the viewer sent. `format` describes the
    // current CMIC stream and `format_changed` is true on the first packet
    // after one opened, so the host can rebuild its decoder and player.
    // WaitTimeout means silence; Unavailable means the viewer ended the
    // stream, which may later be followed by a new one. Microphone problems
    // never end the video session: a malformed packet returns CaptureFailed.
    Error receive_microphone(AudioHeader& header, std::vector<uint8_t>& payload,
                             AudioInfo& format, bool& format_changed,
                             std::chrono::milliseconds timeout);
    // True after the viewer closed or aborted its current microphone stream.
    // A later start on the viewer clears it again.
    bool microphone_finished() const;
    // Number of CMIC streams accepted since the connection started.
    uint64_t microphone_streams() const;
    // True when the client asked for a key frame since the last call.
    bool take_keyframe_request();
    // Pops the newest SwitchDisplay request the viewer sent. Only the newest
    // is kept: the host switches once, to where the viewer last pointed.
    // Requires input permission on both ends; a request without it is
    // dropped by the transport. The host answers each taken request with
    // send_display_switch_result().
    bool take_display_switch_request(uint16_t& display_index);
    // Answers a SwitchDisplay. Ok tells the viewer a StreamInfo follows (or
    // that the stream already shows that display); every other code leaves
    // the current stream up. `reason` is UTF-8 and is truncated to
    // kMaxDisplaySwitchReason bytes on a character boundary.
    Error send_display_switch_result(uint16_t display_index, DisplaySwitchCode code,
                                     const std::string& reason = {});
    // Pops the newest ListDisplayModes request. The host answers it with
    // send_display_modes(). Only the newest request is kept, like the switch
    // slot above.
    bool take_list_display_modes_request(uint16_t& display_index);
    // Sends one mode list answer. A refusal must carry no modes; the maker
    // drops them if the caller passes some.
    Error send_display_modes(const DisplayModeList& list);
    // Pops the newest SetDisplayMode request. The host answers each taken
    // request with send_display_mode_result(). Requires input permission on
    // both ends; a request without it is dropped by the transport.
    bool take_set_display_mode_request(uint16_t& display_index, uint16_t& width,
                                       uint16_t& height, uint16_t& refresh_hz);
    // Answers a SetDisplayMode. Ok tells the viewer a StreamInfo follows; every
    // other code leaves the current mode and stream in place. The mode is
    // echoed so the viewer can pair the answer with its request.
    Error send_display_mode_result(uint16_t display_index, uint16_t width, uint16_t height,
                                   uint16_t refresh_hz, DisplayModeCode code,
                                   const std::string& reason = {});
    // Pops the newest SetDisplayFollow request. Only the newest is kept, like
    // the switch slot: the viewer's toggles describe the state it wants now.
    // Requires input permission on both ends; a request without it is dropped
    // by the transport. The host answers each taken request with
    // send_display_follow_state().
    bool take_display_follow_request(uint8_t& flags);
    // Asks the host for the follow modes this viewer wants. Returns false when
    // the handshake is not complete or the host did not grant input.
    Error request_display_follow(uint8_t flags);
    // Pops the newest show-cursor hint. Only the newest is kept: the host
    // overlay only needs the latest pointer position. Hints need no input
    // permission; they are draw-only and never reach the injector.
    bool take_show_cursor_hint(ShowCursorHint& hint);
    // Number of show-cursor hints received. Observation hook for tools and
    // the host's own counters.
    uint64_t show_cursor_hints() const;
    // Asks the host to draw the viewer pointer. No permission is required
    // beyond a completed handshake; the host may ignore hints at its own
    // policy. Returns InvalidCall before the handshake completes.
    Error request_show_cursor(const ShowCursorHint& hint);
    // Pops the newest DisplayFollowState. The host sends it as the answer and
    // unsolicited when it turns a mode off itself.
    bool take_display_follow_state(DisplayFollowState& state);
    // Answers SetDisplayFollow, or announces a host-initiated change. The
    // flags describe the modes in effect after the call; a code other than Ok
    // travels with flags=0.
    Error send_display_follow_state(const DisplayFollowState& state);
    // Pops the newest SetPrivacyMode request. Only the newest is kept: a viewer
    // that toggles twice wants the final state. The host answers each taken
    // request with send_privacy_mode_state(). Requires the privacy-mode
    // permission; a request without it is dropped by the transport.
    bool take_privacy_mode_request(bool& enabled);
    // Answers a privacy-mode request or announces a state the host changed on
    // its own (permission revoked, desktop switch). `enabled` is the resulting
    // overlay state; a code other than Ok always travels with enabled=false.
    Error send_privacy_mode_state(bool enabled, PrivacyModeCode code,
                                  const std::string& reason = {});
    // Tells the viewer which desktop the video stream is capturing. Sent when
    // the host switches between the user desktop and a secure desktop.
    Error send_desktop_state(DesktopKind kind);
    // Pops one pending SendSas request. The host answers every request with
    // send_sas_result(); the permission check is the host's, so a viewer
    // without input control gets Denied instead of silence.
    bool take_sas_request();
    // Number of SendSas requests received. Observation hook for tools and the
    // host's own counters.
    uint64_t sas_requests() const;
    // Answers SendSas. A reason is kept for every code so the viewer can show
    // why the secure desktop was not reached.
    Error send_sas_result(SasCode code, const std::string& reason = {});
    // Pops one pending HostActionRequest. The host checks the permission and
    // its own settings, performs the action, and answers every request with
    // send_host_action_result(). A viewer without the matching permission gets
    // Denied instead of silence.
    bool take_host_action_request(HostControlRequest& request);
    bool take_elevation_request(uint32_t& request_id);
    Error send_elevation_state(const ElevationState& state);
    // Number of HostActionRequests received. Observation hook for tools and
    // the host's own counters.
    uint64_t host_action_requests() const;
    // Answers a HostActionRequest, or announces that the host released block
    // input on its own (permission revoked, session ending). `block_input` is
    // the state in effect after the call and is ignored for other actions.
    Error send_host_action_result(const HostControlResult& result);
    // Pops one pending OsLoginRequest password. The host checks its
    // permissions, settings and desktop state, types the password with
    // inject_login_text() and answers every request with send_os_login_result().
    bool take_os_login_request(std::string& password);
    // Number of OsLoginRequests received. Observation hook for tools and the
    // host's own counters.
    uint64_t os_login_requests() const;
    // Answers an OsLoginRequest. Ok only when the password was handed to the
    // injector while the logon or lock screen was current.
    Error send_os_login_result(OsLoginCode code, const std::string& reason = {});
    // Pops the pending SessionListRequest, coalesced: only the newest request
    // matters, and the host enumerates the sessions when it drains it. The
    // host checks the permission, its setting and the backend, then answers
    // send_session_list_result().
    bool take_session_list_request();
    // Number of SessionListRequests received. Observation hook for tools and
    // the host's own counters.
    uint64_t session_list_requests() const;
    // Answers a SessionListRequest. A non-Ok code must carry no sessions; the
    // maker drops them if the caller passes some.
    Error send_session_list_result(const SessionListResult& result);
    // Pops the pending SessionSelectRequest. A newer request replaces an older
    // one: the viewer can only be heading to one session. The host answers
    // every taken request with send_session_select_result().
    bool take_session_select_request(uint32_t& session_id);
    // Number of SessionSelectRequests received. Observation hook for tools.
    uint64_t session_select_requests() const;
    // Answers a SessionSelectRequest. Ok means the host accepted the handoff
    // and is ending this session so the worker can restart in the target.
    Error send_session_select_result(const SessionSelectResult& result);
    // Pops the newest RemoteConfigRequest. Only the newest is kept: a viewer
    // that toggles several switches in a row wants the final combination.
    // The host checks kPermissionRemoteConfig and its allow_remote_config
    // setting, applies the change and answers with send_remote_config_state().
    bool take_remote_config_request(RemoteConfigRequest& request);
    // Number of RemoteConfigRequests received. Observation hook for tools and
    // the host's own counters.
    uint64_t remote_config_requests() const;
    // Answers a RemoteConfigRequest with the switch state now stored on the
    // host. A non-Ok code must carry flags=0; the maker refuses anything else.
    Error send_remote_config_state(const RemoteConfigState& state);
    // Pops the newest DisplayControl request. Only the newest is kept: a
    // viewer that toggles several outputs in a row wants the final topology.
    // The host checks the permissions itself so a refusal still gets an
    // answer, then replies with send_display_control_state().
    bool take_display_control_request(DisplayControlRequest& request);
    // Number of DisplayControl requests received. Observation hook for tools
    // and the host's own counters.
    uint64_t display_control_requests() const;
    // Answers a DisplayControl request with the state now in effect. The
    // maker refuses more than four outputs, duplicate ids and an unknown code.
    Error send_display_control_state(const DisplayControlState& state);
    // Asks the host for its display state or to change it. Returns
    // PermissionDenied for a desktop action when the host did not grant the
    // privacy permission; Query and Restore need no permission so a viewer can
    // always read the state and undo its own change after a revocation.
    Error request_display_control(const DisplayControlRequest& request);
    // Pops the newest DisplayControlState. The host sends it as the answer and
    // unsolicited when it releases a topology itself.
    bool take_display_control_state(DisplayControlState& state);
    // Releases anything the viewer still holds, then types the password and
    // Return through the server's input injector. The injection thread checks
    // the input desktop before applying, but the caller must still confirm the
    // logon state right before this call: a UAC prompt must not receive it.
    Error inject_login_text(const std::string& password);
    // Pops the newest SetStreamOptions request. Only the newest is kept: a
    // viewer that changes several menus in a row wants the final combination.
    // The host answers each taken request with send_stream_options_result().
    // No permission gates this: the preferences only change the viewer's own
    // picture.
    bool take_stream_options_request(StreamOptions& options);
    // Number of SetStreamOptions requests received. Observation hook for tools
    // and the host's own counters.
    uint64_t stream_options_requests() const;
    // Answers SetStreamOptions. The effective fields must describe the stream
    // that is actually running, including when the request was refused, so the
    // viewer can restore its menu to reality.
    Error send_stream_options_result(const StreamOptionsResult& result);
    // Drains the input events received since the last call, in order. They
    // were already injected by the session's own InputInjector when
    // QuicServerConfig::allow_input is set; this is the observation hook for
    // tests and for callers that want their own policy.
    std::vector<InputEvent> take_input_events();
    // Injection counters (events applied, rejected, failed, dropped).
    InputStats input_stats() const;
    // Number of InputRelease messages received (viewer gave up control, lost
    // focus or is closing). Observation hook for tests and tools.
    uint64_t input_release_requests() const;
    // Ends the video stream, waits for the client's closing Stats/Bye, and
    // shuts the connection down. `stats_received` is false when the client
    // disappeared first; that alone is not an error.
    Error finish_server(std::chrono::milliseconds timeout, ClientStats& stats,
                        bool& stats_received);

    // ----- client -----

    // Starts the QUIC handshake and waits for it to complete.
    Error connect(std::chrono::milliseconds timeout);
    // Opens the control stream, sends Hello (asking for `display`, an index
    // into the server's display list or kDisplayAuto), and waits for HelloAck.
    // Returns Status::PermissionDenied (with the server's reason) when the
    // server refuses the Hello.
    Error handshake(ServerInfo& info, std::chrono::milliseconds timeout,
                    uint16_t display = kDisplayAuto);
    // The AccessDenied the server answered Hello with, once handshake() failed
    // with Status::PermissionDenied. Empty when the server never refused one.
    // handshake() already carries the server's reason; the code lets a caller
    // choose its own wording without matching text.
    std::optional<AccessDenial> denial() const;
    // Client (v62 managed access): presents a freshly issued ticket for this
    // same connection and waits for the host's TicketRenewResult. The ticket
    // must name the client key the handshake used; the proof covers this
    // session's Hello nonce and the host challenge. Returns PermissionDenied
    // (with the host's reason) when the renewal was refused; the host closes
    // the session in that case. Requires a completed handshake that used a
    // ticket.
    Error renew_access_ticket(const std::vector<uint8_t>& signed_ticket,
                              std::chrono::milliseconds timeout);
    // Pops the newest permission mask the host sent with PermissionUpdate.
    // permissions() already reflects it, so this is the "something changed"
    // edge for a UI that wants to explain the revocation.
    bool take_permission_update(PermissionMask& permissions);
    uint64_t permission_updates() const;
    // Waits for one complete access unit. Returns WaitTimeout when nothing
    // arrived in time, or Unavailable once the peer finished the video stream.
    Error receive_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                        std::chrono::milliseconds timeout);
    // Waits for one complete audio access unit. Returns WaitTimeout when
    // nothing arrived in time, or Unavailable once the server finished the
    // audio stream; either is normal when audio is optional. A malformed
    // packet returns CaptureFailed, which the caller may treat as "audio
    // broken" without ending the video session. When audio_datagram() is
    // true the bytes come from QUIC DATAGRAM frames, so packets can be lost
    // or reordered and the header sequence number is the only ordering clue.
    Error receive_audio(AudioHeader& header, std::vector<uint8_t>& payload,
                        std::chrono::milliseconds timeout);
    // True when the host announced audio over QUIC DATAGRAM in HelloAck (v35).
    // Valid once the handshake completed; the audio stream is still open and
    // carries the FIN that ends the session's audio.
    bool audio_datagram() const;
    // Opens the viewer-initiated CMIC stream and writes its format header.
    // Opening the stream is the microphone switch: the host starts decoding
    // on the header and stops when the stream ends, so there is no control
    // message. Requires kPermissionMicrophone in the last HelloAck or
    // PermissionUpdate, and a complete 48 kHz Opus description with OpusHead
    // extradata. One stream at a time; stop_microphone() ends it.
    Error start_microphone(const AudioInfo& info);
    // Sends one Opus packet on the open CMIC stream. Like send_audio it never
    // waits: a full queue drops the packet and returns WaitTimeout, so the
    // microphone can never stall video. `capture_ms` stays on this viewer's
    // steady clock; the host treats it as a relative timeline.
    Error send_microphone(AudioHeader header, const uint8_t* payload, size_t size);
    // Ends the microphone stream gracefully. A later start_microphone() opens
    // a fresh stream. Revoking kPermissionMicrophone aborts it from the host
    // side as well, so the viewer stops sending either way.
    Error stop_microphone();
    // Asks the server for an IDR (sent on the control stream).
    Error request_keyframe();
    // Sends a LatencyProbe. Both endpoints answer automatically. Client probes
    // synchronize clocks; server probes measure application RTT for shared
    // video QoS. The server keeps at most one probe outstanding.
    Error send_latency_probe();
    // Last clock-sync result and probe/reply counters.
    LatencySample latency() const;
    // Transport-level RTT, loss and queue state. Safe on both ends; on the
    // server it is what the adaptive bitrate controller samples.
    TransportStats transport_stats() const;
    // Copies the description from the last StreamInfo the server sent, if one
    // arrived since the previous call. The same struct as the HelloAck: the
    // viewer replaces its mapping and decoder parameters with it.
    bool take_stream_update(ServerInfo& info);
    // Number of StreamInfo messages received. Observation hook for tests.
    uint64_t stream_updates() const;
    // Asks the host to capture another display mid-session. `display_index`
    // names an entry of the last announced display list, or kDisplayAuto.
    // Requires a desktop handshake, including view-only; the answer arrives through
    // take_display_switch_result(). On Ok a StreamInfo describing the new
    // output follows on the same control stream.
    Error request_display_switch(uint16_t display_index);
    // v54 ordered add/sub/set subscription requests, available to view-only
    // desktop sessions too. Empty set stops all desktop video subscriptions.
    Error request_capture_displays(const CaptureDisplaysRequest& request);
    bool take_capture_displays_request(CaptureDisplaysRequest& request);
    Error send_capture_displays_result(const CaptureDisplaysResult& result);
    bool take_capture_displays_result(CaptureDisplaysResult& result);
    // Stream metadata never changes connection-wide permissions or audio.
    Error refresh_display_stream(const DisplayStreamInfo& info);
    bool take_display_stream_update(DisplayStreamInfo& info);
    Error request_display_keyframe(uint32_t stream_id);
    bool take_display_keyframe_request(uint32_t& stream_id);
    // Host side (v70): pops the next ScreenshotRequest the viewer queued. The
    // host answers with PNG slices from the pre-encode frame of `stream_id`
    // (kScreenshotCurrentStream selects the focused stream).
    bool take_screenshot_request(ScreenshotRequest& request);
    // Sends one ScreenshotResponse slice. Never waits: WaitTimeout means the
    // send queue was full and the caller should retry the same slice on a
    // later pass, Unavailable means the session ended.
    Error send_screenshot_response(const ScreenshotResponse& response);
    // Viewer side (v70): asks the host for the raw frame of `stream_id`
    // (kScreenshotCurrentStream selects current). One request at a time;
    // InvalidCall while another is outstanding. The answer arrives through
    // take_screenshot().
    Error request_screenshot(uint32_t stream_id = kScreenshotCurrentStream);
    // Pops a completed answer. The transport assembles the slices into the
    // full PNG before queueing it, and bounds the assembly by
    // kMaxScreenshotBytes.
    bool take_screenshot(ScreenshotResult& result);
    // Forgets an outstanding request, for a viewer that gave up waiting. Late
    // slices for it are ignored.
    void cancel_screenshot();
    // Pops the newest DisplaySwitchResult. The reason is the host's UTF-8
    // explanation, empty when it has none.
    bool take_display_switch_result(uint16_t& display_index, DisplaySwitchCode& code,
                                    std::string& reason);
    // Total DisplaySwitchResults received. Observation hook for tools.
    uint64_t display_switch_results() const;
    // Asks the host for the modes of one announced display. The answer
    // arrives through take_display_modes(). Listing works without input
    // permission; only request_set_display_mode() is gated.
    Error request_display_modes(uint16_t display_index);
    // Pops the newest DisplayModes answer. Like the switch answer, only the
    // newest matters; `list.display` says which output it describes.
    bool take_display_modes(DisplayModeList& list);
    // Total DisplayModes answers received. Observation hook for tools.
    uint64_t display_mode_lists() const;
    // Asks the host to change the streamed output's mode for this session.
    // Requires the handshake and input permission. The answer arrives through
    // take_display_mode_result(); on Ok a StreamInfo describing the new source
    // size follows on the same control stream.
    Error request_set_display_mode(uint16_t display_index, uint16_t width, uint16_t height,
                                   uint16_t refresh_hz);
    // Pops the newest DisplayModeResult, echoing the requested mode.
    bool take_display_mode_result(uint16_t& display_index, uint16_t& width, uint16_t& height,
                                  uint16_t& refresh_hz, DisplayModeCode& code,
                                  std::string& reason);
    // Total DisplayModeResults received. Observation hook for tools.
    uint64_t display_mode_results() const;
    // Asks the host to hide or restore its physical display. Requires the
    // handshake and kPermissionPrivacyMode; the host answers through
    // take_privacy_mode_state(). Privacy mode is per session and always starts
    // off after a reconnect.
    Error set_privacy_mode(bool enabled);
    // Pops the newest privacy-mode state. `enabled` is the resulting overlay
    // state; a refusal carries the host's UTF-8 reason.
    bool take_privacy_mode_state(bool& enabled, PrivacyModeCode& code, std::string& reason);
    // Pops the newest desktop state the host sent. The first message follows
    // HelloAck, so a viewer that connects while the host is locked learns it
    // without waiting for a switch.
    bool take_desktop_state(DesktopKind& kind);
    // Asks the host to inject Ctrl+Alt+Del. Requires the handshake and input
    // permission; the answer arrives through take_sas_result(). Unlike a key
    // event this is one explicit request: Windows only accepts SAS through
    // SendSAS(), which the service worker can call.
    Error request_sas();
    // Pops the newest SAS answer. `reason` is the host's UTF-8 explanation,
    // empty when it has none.
    bool take_sas_result(SasCode& code, std::string& reason);
    // Total SAS answers received. Observation hook for tools.
    uint64_t sas_results() const;
    // Asks the host to restart, lock its console or change local block input.
    // Requires the handshake and the action's permission: input for the lock,
    // the restart bit for a restart, and input plus the block-input bit for
    // block input. The answer arrives through take_host_action_result().
    Error request_host_action(HostAction action, bool block_input = false);
    // Pops the oldest host-control answer. Results are a short ordered queue
    // because each block-input toggle and each action produces one.
    bool take_host_action_result(HostControlResult& result);
    Error request_elevation(uint32_t request_id);
    bool take_elevation_state(ElevationState& state);
    // Total host-control answers received. Observation hook for tools.
    uint64_t host_action_results() const;
    // Asks the host to read or change its exposed policy switches. Requires
    // the handshake and kPermissionRemoteConfig; mask=0 queries the current
    // state. The answer arrives through take_remote_config_state().
    Error request_remote_config(uint32_t keys, uint32_t values);
    // Pops the newest remote-config answer. `flags` carries the state stored
    // on the host; a refusal carries the host's UTF-8 reason.
    bool take_remote_config_state(RemoteConfigState& state);
    // Total remote-config answers received. Observation hook for tools.
    uint64_t remote_config_states() const;
    // Asks the host to type the operating-system password at its logon or lock
    // screen. Requires the handshake, kPermissionInput and kPermissionOsLogin.
    // The answer arrives through take_os_login_result().
    Error request_os_login(const std::string& password);
    // Pops the newest OS-login answer, which carries the reason on refusal.
    bool take_os_login_result(OsLoginCode& code, std::string& reason);
    // Total OS-login answers received. Observation hook for tools.
    uint64_t os_login_results() const;
    // Asks the host which Windows sessions the stream can move to. Requires
    // the handshake and kPermissionSessionSelect; the answer arrives through
    // take_session_list_result().
    Error request_session_list();
    // Pops the newest session list answer. A refusal carries the host's
    // UTF-8 reason and no sessions.
    bool take_session_list_result(SessionListResult& result);
    // Total session list answers received. Observation hook for tools.
    uint64_t session_list_results() const;
    // Asks the host to move the stream to another Windows session. Requires
    // the handshake and kPermissionSessionSelect; an Ok answer means this
    // session is about to end, so the caller should wait for the disconnect
    // and reconnect. The answer arrives through take_session_select_result().
    Error request_session_select(uint32_t session_id);
    // Pops the newest session select answer.
    bool take_session_select_result(SessionSelectResult& result);
    // Total session select answers received. Observation hook for tools.
    uint64_t session_select_results() const;
    // Sets this session's codec, frame-rate ceiling and quality preferences.
    // Input control is not required. Each viewer contributes to shared codec
    // and quality policies, so the effective result can differ from its request.
    // The answer arrives through take_stream_options_result(); preferences
    // must be sent again when reconnecting.
    Error request_stream_options(const StreamOptions& options);
    // Pops the newest StreamOptionsResult. The effective fields describe the
    // running stream even on a refusal, so a menu can roll back to reality.
    bool take_stream_options_result(StreamOptionsResult& result);
    // Total StreamOptionsResults received. Observation hook for tools.
    uint64_t stream_options_results() const;
    // SHA-256 fingerprint of the certificate the server presented (uppercase
    // hex). Empty until the TLS handshake reached the validation callback;
    // available even when the pin rejected it, which is what makes a mismatch
    // report readable.
    std::string peer_fingerprint() const;
    // Sends one pointer or keyboard event to the server. The caller is
    // responsible for the coordinate space (virtual-desktop pixels) and for
    // not sending when the HelloAck reported input disabled.
    Error send_input(const InputEvent& event);
    // Tells the server to lift everything its injector still holds. Sent when
    // the viewer gives up control or loses focus, so a key or button whose
    // release never made it out cannot stay down on the host.
    Error release_input();
    // Reports the closing statistics, says Bye, and waits for the server to
    // shut the connection down.
    Error finish_client(const ClientStats& stats, std::chrono::milliseconds timeout);

    // ----- clipboard (both roles) -----

    // Sends one text change. `seq` is the sender's running counter. Returns
    // InvalidCall for text that fails valid_clipboard_text() and, on a server
    // whose config forbids clipboard, for every call.
    Error send_clipboard(uint64_t seq, const std::string& text);
    // Pops the oldest clipboard update received since the last call. On the
    // server the peer is the viewer; on the client, the host.
    bool take_clipboard_update(ClipboardUpdate& update);
    // Total ClipboardUpdates received since the connection started.
    uint64_t clipboard_updates_received() const;

    // ----- rich clipboard and files (protocol v16, both roles) -----

    // Sends one rich clipboard change as a CCLB blob on its own
    // unidirectional stream. Requires clipboard permission; on a server whose
    // config forbids clipboard every call returns PermissionDenied. Returns
    // InvalidCall for content that fails valid_clipboard_content().
    Error send_clipboard_content(ClipboardContent content);
    // Sends an already-encoded CCLB blob on its own unidirectional stream.
    // Callers normally use send_clipboard_content(); this exists for content
    // that is already encoded and for the loopback limit probe. The receiver
    // validates the blob.
    Error send_clipboard_blob(std::vector<uint8_t> blob);
    // Pops the oldest rich clipboard change the peer sent. Legacy
    // ClipboardUpdate control messages have their own taker above.
    bool take_clipboard_content(ClipboardContent& content);
    // Total CCLB blobs received since the connection started.
    uint64_t clipboard_contents_received() const;

    Error request_clipboard_file(const ClipboardFileRead& request);
    Error send_clipboard_file_data(const ClipboardFileData& response);
    bool take_clipboard_file_read(ClipboardFileRead& request);
    bool take_clipboard_file_data(ClipboardFileData& response);
    void cancel_clipboard_file_reads(uint64_t generation);

    // ----- session chat (both roles, protocol v24) -----

    // Sends one chat line. Empty, oversized or invalid UTF-8 text returns
    // InvalidCall. The client requires the completed handshake; no permission
    // bit gates chat, since an admitted session is the authorization.
    Error send_chat(const std::string& text);
    // Pops the oldest chat line the peer sent since the last call.
    bool take_chat(std::string& text);
    // Total ChatMessages received since the connection started.
    uint64_t chats_received() const;

    // ----- voice call (protocol v37, both roles) -----
    //
    // A voice call streams the host's microphone to the viewer. It is started
    // by the viewer, answered by the host, and then carries encoded Opus units
    // as VoiceAudio control messages. Both directions are gated by
    // kPermissionMicrophone; a revocation ends the call and drops queued
    // audio. The system-audio and viewer-microphone paths are unaffected.

    // Server: pops one request. `connect` true asks to start a call; false
    // tells the host to stop the one it runs. The transport already refuses a
    // start without the microphone permission and answers a duplicate start,
    // so every request the host sees can be acted on.
    bool take_voice_call_request(VoiceCallRequest& request);
    // Server: answers a start request. Ok opens the call and makes
    // send_voice_audio() valid; the response carries the microphone format
    // the host actually opened. A response for an id that is not waiting is
    // refused, and a revocation turns an Ok answer into Refused.
    Error send_voice_call_response(const VoiceCallResponse& response);
    // Server: sends one encoded microphone unit. InvalidCall when no call is
    // open or the header is not a voice header; PermissionDenied once the
    // microphone permission was revoked. The sequence number is assigned here.
    Error send_voice_audio(const AudioHeader& header, const uint8_t* payload, size_t size);
    // Server: closes the call from the host side. Idempotent.
    Error send_voice_call_close();
    // True while this side considers the call open.
    bool voice_call_active() const;

    // Client: asks the host to start a call. Requires the completed handshake
    // and kPermissionMicrophone; a pending or running call returns InvalidCall.
    Error request_voice_call();
    // Client: stops a pending or running call. The host closes its capture.
    Error request_voice_call_stop();
    // Client: pops the oldest answer. A later answer supersedes none: each
    // request id is answered once.
    bool take_voice_call_response(VoiceCallResponse& response);
    // Client: true once when the host closed the call on its own (permission
    // revoked, device failed, operator stopped it). Cleared by the call.
    bool take_voice_call_closed();
    // Client: pops one microphone unit. The header is validated and the
    // payload copied. Empty when no audio is queued.
    bool take_voice_audio(AudioHeader& header, std::vector<uint8_t>& payload);

    // ----- composed text input (viewer -> host, protocol v36) -----
    //
    // The viewer's own input method composes the text and sends the result;
    // the host types it as Unicode, so a layout the host lacks cannot mangle
    // it. The server injects it like InputEvent and keeps a copy for tools.

    // Sends one block of composed text. Requires the input permission and, on
    // the client, the completed handshake; the server cannot send it. Empty,
    // oversized or invalid UTF-8 text returns InvalidCall.
    Error send_text_input(const std::string& text);
    // Server: pops the oldest text block the viewer sent since the last call.
    bool take_text_input(std::string& text);
    // Total TextInputs received since the connection started.
    uint64_t text_inputs_received() const;

    // ----- port forwarding (protocol v26, both roles) -----
    //
    // The viewer registers a TCP target, then turns each accepted local
    // connection into one CPFW uplink; the host dials the target and answers
    // with a CPFW downlink. Both directions use the same data calls below;
    // the server additionally approves targets and accepts each connection.
    // Every call requires kPermissionPortForward, so a host whose config
    // disables forwarding never sees a request or a stream.

    // Server: approves or refuses a target the viewer registered. Ok records
    // `request.host:request.port` for the rest of the session and is the only
    // way a CPFW uplink naming that forward_id becomes acceptable; any other
    // code forgets the request. The reason is UTF-8 and truncated on send.
    bool take_port_forward_request(PortForwardRequest& request);
    Error send_port_forward_result(uint32_t forward_id, PortForwardCode code,
                                   const std::string& reason = {});
    // Server: pops the oldest viewer connection that wants the host to dial.
    // Nothing is relayed until accept_port_forward_connection() succeeds; the
    // uplink's QUIC receive stays paused until then, so an ignored handle
    // backpressures the viewer instead of buffering without bound.
    bool take_port_forward_open(PortForwardOpen& open);
    // Server: opens the downlink stream after the host dialed the target.
    // Until this succeeds the viewer reads nothing and the relay is idle.
    Error accept_port_forward_connection(PortForwardHandle handle);
    // Server: refuses one connection (dial failed, or the host is full). The
    // viewer sees its uplink reset and closes the local socket; close the
    // handle afterwards.
    Error reject_port_forward_connection(PortForwardHandle handle);

    // Client: sends PortForwardRequest and waits for the answer through
    // take_port_forward_result(). Requires the completed handshake and the
    // permission; at most kMaxPortForwardTargets ids are tracked per session.
    Error request_port_forward(uint32_t forward_id, const std::string& host, uint16_t port);
    // Client: pops the oldest target answer. Ok makes forward_id usable in
    // open_port_forward_connection(); every other code forgets the request.
    bool take_port_forward_result(PortForwardResult& result);
    // Client: starts relaying one accepted local TCP connection. Opens the
    // uplink and writes the CPFW header; the local reader may start sending
    // at once. The handle stays valid until close_port_forward().
    Error open_port_forward_connection(uint32_t forward_id, uint32_t connection_id,
                                       PortForwardHandle& handle);
    // Client: waits until the host attached its downlink, which means the
    // dial succeeded. Unavailable reports a failure (the viewer's uplink was
    // reset); WaitTimeout means the host has not answered yet.
    Error wait_port_forward_connection(PortForwardHandle handle,
                                       std::chrono::milliseconds timeout);

    // Both: tells the peer to stop forwarding a registered target. Every
    // connection under it is torn down on both sides; the answer is silence,
    // because the message is already the verdict. A reason is truncated on
    // send, like every other port-forward reason.
    Error send_port_forward_stop(uint32_t forward_id, PortForwardCode code,
                                 const std::string& reason = {});
    // Both: pops the oldest stop the peer sent. The transport has already
    // torn down the matching connections, so this explains why they ended.
    bool take_port_forward_stop(PortForwardStop& stop);
    // Both: sends one piece of raw TCP data on the connection's sending
    // stream (the uplink on the viewer, the downlink on the host). Waits for
    // send-queue capacity at most `timeout`; WaitTimeout means nothing was
    // sent and the caller keeps the bytes and retries. An aborted stream
    // returns Unavailable.
    Error send_port_forward_data(PortForwardHandle handle, const uint8_t* data, size_t size,
                                 std::chrono::milliseconds timeout);
    // Both: pops the next piece of received TCP data. WaitTimeout means no
    // bytes arrived in time (normal for an idle connection); Unavailable
    // means the peer ended or broke the stream and the queue is drained.
    Error receive_port_forward_data(PortForwardHandle handle, std::vector<uint8_t>& data,
                                    std::chrono::milliseconds timeout);
    // Both: half-closes the sending direction (TCP FIN) without touching the
    // other direction. Ending the whole connection is close_port_forward().
    Error finish_port_forward(PortForwardHandle handle);
    // Both: aborts both streams and forgets the handle. Idempotent. The peer
    // sees a reset rather than a FIN, which is what a local socket failure
    // means.
    void close_port_forward(PortForwardHandle handle);

    // ----- remote terminal (protocol v27, both roles) -----
    //
    // The viewer asks for a console; the host runs it under ConPTY and
    // answers. The viewer then opens a CTTY uplink and sends keystrokes, and
    // the host opens a CTTY downlink the first time the console prints. A
    // session console ends with the session; a persistent console (v42) keeps
    // its service and a 1 MiB scrollback, and a later session attaches to it
    // by service id. Every call requires kPermissionTerminal.

    // Server: pops the oldest TerminalRequest accepted by the permission and
    // settings filters. The console is created by the host, which then answers
    // with send_terminal_result().
    bool take_terminal_request(TerminalRequest& request);
    // Server: answers one request. Ok registers the terminal id and makes
    // CTTY streams naming it acceptable; every other code forgets the request.
    // The reason is UTF-8 and truncated on send.
    Error send_terminal_result(const TerminalResult& result);

    // Client: asks the host for a console of the given size. The answer comes
    // through take_terminal_result(); at most kMaxTerminals are tracked.
    // `flags` is kTerminalRequestPersist to ask for a service that outlives
    // the session (the host answers with its service id) or
    // kTerminalRequestAttach with `service_id` set to reattach to one.
    Error request_terminal(uint32_t terminal_id, uint16_t columns, uint16_t rows,
                           uint8_t flags = 0, uint32_t service_id = 0);
    // Client: pops the oldest answer. Ok means open_terminal_input() may run.
    bool take_terminal_result(TerminalResult& result);
    // Client: opens the CTTY uplink for an approved terminal. Keystrokes are
    // then sent with send_terminal_input(); the stream stays open until
    // finish_terminal_input() or close_terminal().
    Error open_terminal_input(uint32_t terminal_id);
    // Client: sends raw console input. Non-blocking semantics match
    // send_port_forward_data(): WaitTimeout means the bytes were kept by the
    // caller and should be retried once the send queue has room.
    Error send_terminal_input(uint32_t terminal_id, const uint8_t* data, size_t size,
                              std::chrono::milliseconds timeout);
    // Client: half-closes the input direction (the console sees EOF).
    Error finish_terminal_input(uint32_t terminal_id);
    // Client: asks the host to resize the console. Best effort; an unknown or
    // already closed terminal is not an error at this layer.
    Error send_terminal_resize(uint32_t terminal_id, uint16_t columns, uint16_t rows);
    // Client: pops the next piece of console output. Unavailable once the host
    // ended the output stream and the queue is drained.
    bool take_terminal_output(uint32_t terminal_id, std::vector<uint8_t>& data, bool& ended);

    // Server: pops the oldest resize request.
    bool take_terminal_resize(TerminalSize& size);
    // Server: pops the next piece of viewer input. `ended` reports the
    // viewer's half-close; the queue may still hold bytes from before it.
    bool take_terminal_input(uint32_t terminal_id, std::vector<uint8_t>& data, bool& ended);
    // Server: sends console output. Opens the CTTY downlink on the first call.
    // WaitTimeout means nothing was sent and the caller keeps the bytes.
    Error send_terminal_output(uint32_t terminal_id, const uint8_t* data, size_t size,
                               std::chrono::milliseconds timeout);
    // Server: half-closes the output direction after the child process exited.
    Error finish_terminal_output(uint32_t terminal_id);

    // Both: tells the peer the terminal is over. The host must close the
    // console and the child process; the viewer drops its streams. A
    // direction that already finished cleanly keeps its FIN, so the stop never
    // resets a stream whose bytes the peer has not acknowledged yet.
    Error send_terminal_stop(uint32_t terminal_id, TerminalCode code,
                             const std::string& reason = {});
    // Both: pops the oldest stop the peer sent. On the host the matching
    // streams are gone; on the viewer the entry survives the stop until
    // take_terminal_output() reports ended, so output that was already in
    // flight is still delivered. close_terminal() then forgets it.
    bool take_terminal_stop(TerminalStop& stop);
    // Both: aborts both streams and forgets the terminal. Idempotent. A clean
    // ending uses finish_terminal_output()/finish_terminal_input() first; this
    // is the failure or session-end path.
    void close_terminal(uint32_t terminal_id);

    // ----- camera (protocol v29, both roles) -----
    //
    // The viewer lists the host's cameras and asks for one; the host opens a
    // server-initiated CCAM stream carrying encoded access units. The stream
    // runs beside the desktop picture and ends with a FIN when the viewer
    // stops it or the host's device fails. Every call requires
    // kPermissionCamera, so a host whose config disables cameras never sees a
    // request or a stream.

    // Server: pops one ListCameras request. The host answers with
    // send_camera_list(); a request is a flag rather than a queue because the
    // answer is a snapshot and a second list supersedes the first.
    bool take_camera_list_request();
    Error send_camera_list(const CameraList& list);
    // Server: pops the oldest camera the viewer asked for. The device is
    // opened by the host, which then answers with send_camera_result().
    bool take_camera_start(uint16_t& camera_id);
    // Server: answers one start. Ok opens the CCAM stream (the header names
    // the id) and makes send_camera_frame() valid; every other code means no
    // stream will follow. The reason is UTF-8 and truncated on send.
    Error send_camera_result(const CameraResult& result);
    // Server: sends one encoded access unit on the CCAM stream. Like
    // send_frame it never waits: a full queue drops the frame and returns
    // WaitTimeout, so camera traffic can never stall the desktop stream.
    Error send_camera_frame(FrameHeader header, const uint8_t* payload, size_t size);
    // Server: ends the CCAM stream with a FIN. Idempotent.
    Error finish_camera();
    // Server: true while a CCAM stream is open.
    bool camera_open() const;
    // Server: pops the camera id the viewer stopped. The transport already
    // finished the stream; this is the "stop capturing" edge.
    bool take_camera_stop(uint16_t& camera_id);

    // Client: asks for the camera list. Requires the handshake and
    // kPermissionCamera; the answer arrives through take_camera_list().
    Error request_camera_list();
    // Client: pops the newest list. A later answer supersedes an older one.
    bool take_camera_list(CameraList& list);
    // Total CameraList answers received. Observation hook for tools.
    uint64_t camera_lists() const;
    // Client: asks the host to open one camera. The answer arrives through
    // take_camera_result(); Ok makes receive_camera_frame() valid. One camera
    // at a time: a pending or open camera refuses a second start.
    Error request_camera_start(uint16_t camera_id);
    // Client: pops the oldest start answer.
    bool take_camera_result(CameraResult& result);
    // Client: waits for one camera access unit. Unavailable once the host
    // finished the stream (or the viewer called request_camera_stop()).
    Error receive_camera_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                               std::chrono::milliseconds timeout);
    // Client: stops the running camera. The host finishes the stream; a later
    // start opens a fresh one. An already stopped camera is a no-op.
    Error request_camera_stop();

    // ----- file transfer (both directions) -----
    //
    // The side that offers a file is the sender; the side that answers is the
    // receiver and writes the bytes. A viewer upload has the host as receiver;
    // a host download has the viewer as receiver. Every call below works on
    // both roles. Ordinary offers require kPermissionFileTransfer; marked
    // clipboard offers require kPermissionClipboard and the allowed direction.

    // Receiver: pops the oldest FileOffer the peer sent. Nothing is accepted
    // until send_file_answer() says so.
    bool take_file_offer(FileOffer& offer);
    // Receiver: answers an offer on the control stream. Ok tells the sender to
    // open the CFIL stream at `resume_offset`; every other code must pass 0
    // and aborts a stream the sender opened early.
    Error send_file_answer(uint32_t transfer_id, FileCode code, uint64_t resume_offset = 0,
                           const std::string& reason = {});
    // Receiver: waits for the next piece of any incoming transfer. WaitTimeout
    // means no bytes yet, Unavailable means no transfer is active any more. A
    // broken transfer returns its transport error after its bytes were drained.
    Error receive_file_chunk(FileChunk& chunk, std::chrono::milliseconds timeout);
    // Receiver: transport state of one incoming transfer; false when the id is
    // unknown.
    bool file_upload_state(uint32_t transfer_id, FileUploadState& state) const;
    // Receiver: reports the final outcome on the control stream. A code other
    // than Ok aborts the data stream and discards its buffered bytes.
    Error send_file_result(uint32_t transfer_id, FileCode code, uint64_t received,
                           const std::string& reason = {});
    // Either side: pops the oldest FileCancel. The receiver owns the partial
    // file and answers with send_file_result() when it wants to; the sender
    // drops its stream and local read state.
    bool take_file_cancel(FileCancel& cancel);
    // Total FileOffers received since the connection started.
    uint64_t file_offers_received() const;

    // Sender: announces a file on the control stream. The transfer id has to
    // be new for this connection.
    Error offer_file(const FileOffer& offer);
    // Sender: pops the oldest FileAnswer. On Ok the caller reads the local
    // file from the answer's resume offset.
    bool take_file_answer(FileAnswer& answer);
    // Sender: sends one piece of an accepted transfer. The first call opens
    // the CFIL stream with the answer's resume offset; pieces then follow on
    // exactly. Blocks on the shared send queue like video.
    Error send_file_data(uint32_t transfer_id, const uint8_t* data, size_t size);
    // Sender: send_file_data() without waiting for queue capacity. Returns
    // WaitTimeout, having sent nothing, when the shared queue is full; the
    // caller keeps the piece and retries. A bulk transfer uses this so it
    // never stalls the receive loop that feeds video, audio and input.
    Error try_send_file_data(uint32_t transfer_id, const uint8_t* data, size_t size);
    // Sender: half-closes the data stream after the last byte. A zero-byte
    // file calls this without send_file_data().
    Error finish_file_upload(uint32_t transfer_id);
    // Sender: aborts the data stream and forgets it (local read failure,
    // rejected offer or a receiver cancel).
    void cancel_file_upload(uint32_t transfer_id);
    // Sender: pops the oldest FileResult. Taking it releases the transfer
    // state.
    bool take_file_result(FileResult& result);
    // Sender: tells the receiver to drop the transfer and its partial file.
    // The local stream is aborted through cancel_file_upload().
    Error send_file_cancel(uint32_t transfer_id, FileCode code, const std::string& reason);
    // Host only: the held offer's name already exists, so the offer waits for
    // the viewer to decide. Sent instead of send_file_answer(); the answer
    // arrives through take_file_conflict_answer(). The offer stays in
    // incoming_file_offers, unreserved by timeouts, until the caller answers
    // it through send_file_answer().
    Error send_file_conflict(const FileConflict& conflict);
    // Viewer only: pops the oldest held-offer announcement.
    bool take_file_conflict(FileConflict& conflict);
    // Viewer only: sends the decision for a held offer, in both directions.
    // In the upload direction this answers the host's announcement; in the
    // download direction it follows the FileAnswer with FileCode::Conflict.
    Error send_file_conflict_answer(const FileConflictAnswer& answer);
    // Host only: pops the oldest decision. The caller then either starts the
    // transfer (send_file_answer Ok in the upload direction, stream start in
    // the download direction), sends FileAnswer{Skipped}, or cancels.
    bool take_file_conflict_answer(FileConflictAnswer& answer);
    // Host only: accepts a held download after the viewer's overwrite
    // decision. No FileAnswer{Ok} follows in that direction, so the offer
    // must be marked accepted here before its stream can start.
    Error accept_held_download(uint32_t transfer_id);

    // ----- shared-folder browsing -----
    //
    // The viewer lists and downloads files below the host's receive directory.
    // Every call requires the handshake and kPermissionFileTransfer, like
    // FileOffer.

    // Server: pops the oldest browse request (the viewer's request id, a
    // relative directory and whether hidden entries were requested). The host
    // answers with send_file_list_result().
    bool take_file_list_request(uint32_t& request_id, std::string& directory,
                                std::string* cursor = nullptr, bool* show_hidden = nullptr);
    // Server: answers one browse request on the control stream.
    Error send_file_list_result(const FileListResult& result);
    // Server: pops the oldest download request (the viewer's request id and a
    // relative file path). The host answers with send_file_download_result();
    // on Ok the file is offered through the regular transfer machinery.
    bool take_file_download_request(uint32_t& request_id, std::string& path);
    // Server: answers one download request. Ok must name the transfer id the
    // host is about to offer; every other code must pass 0.
    Error send_file_download_result(const FileDownloadResult& result, bool clipboard = false);
    // Client: asks for one directory listing; `show_hidden` keeps entries
    // with FILE_ATTRIBUTE_HIDDEN. The answer arrives through
    // take_file_list_result().
    Error request_file_list(uint32_t request_id, const std::string& directory,
                            const std::string& cursor = {}, bool show_hidden = false);
    // Client: pops the oldest listing.
    bool take_file_list_result(FileListResult& result);
    // Client: asks the host to offer one file. The answer arrives through
    // take_file_download_result(); Ok means a FileOffer with the same transfer
    // id follows.
    Error request_file_download(uint32_t request_id, const std::string& path);
    // Client: pops the oldest download answer.
    bool take_file_download_result(FileDownloadResult& result);
    // Server: pops the oldest file operation (create, rename or remove one
    // entry below the receive root). The host answers with
    // send_file_op_result().
    bool take_file_op_request(uint32_t& request_id, FileOperation& op,
                              std::string& path, std::string& new_name);
    // Server: answers one file operation on the control stream.
    Error send_file_op_result(const FileOpResult& result);
    // Client: asks the host to apply one file operation. The answer arrives
    // through take_file_op_result().
    Error request_file_op(uint32_t request_id, FileOperation op,
                          const std::string& path, const std::string& new_name);
    // Client: pops the oldest file operation answer.
    bool take_file_op_result(FileOpResult& result);

    // ----- pointer (server to client) -----

    // Sends the host's pointer state. Only the newest update matters to a
    // viewer, so a send never blocks: when the queue is full the update is
    // dropped and WaitTimeout returned, like audio.
    Error send_cursor(const CursorUpdate& update);
    // Pops the newest CursorUpdate received since the last call, discarding
    // any older ones that were never taken. Returns false when nothing new
    // arrived.
    bool take_cursor_update(CursorUpdate& update);
    // Independent pending state per subscribed source. Late updates for a
    // removed stream are discarded; surviving streams retain unread sprites.
    Error send_display_cursor(const DisplayCursorUpdate& update);
    bool take_display_cursor_update(DisplayCursorUpdate& update);
    // Total CursorUpdates received since the connection started.
    uint64_t cursor_updates_received() const;
    // Host side (v73): pops the next sprite re-request the viewer queued.
    // kCursorCurrentStream selects the focused stream; shape_id 0 selects the
    // shape the host holds now. The host answers on the same stream with a
    // full CursorUpdate or DisplayCursorUpdate.
    bool take_cursor_shape_request(CursorShapeRequest& request);
    // Viewer side (v73): asks the host to send a cursor sprite again after the
    // viewer's cache lost it. The request is advisory; a host that no longer
    // holds that shape ignores it and the next pointer sample carries the
    // current one.
    Error request_cursor_shape(uint32_t stream_id = kCursorCurrentStream,
                               uint64_t shape_id = 0);

    // True after the server shut down its send direction of the video stream.
    bool video_finished() const;
    // True after the server shut down its send direction of the audio stream.
    bool audio_finished() const;
    // Asynchronous transport/protocol error, if one was recorded.
    Error async_error() const;

private:
    QuicSession() = default;

    Error send_frame_impl(FrameHeader header, const uint8_t* payload, size_t size, bool wait,
                          bool track = true);
    Error set_permissions_impl(PermissionMask permissions, bool policy_change);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
