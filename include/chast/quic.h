#pragma once

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
    // "ip:port" of the connecting peer, empty when the transport would not
    // report it.
    std::string peer_address;
    // Everything the viewer sent, including the password, TOTP code and the
    // device proof. The signature has already been parsed but not verified:
    // only the policy knows which keys it trusts.
    ClientHello hello;
    // Authentication's permission ceiling: the host's defaults intersected
    // with what the viewer requested. The approval callback starts from this
    // mask and may only reduce it.
    uint16_t permissions = 0;
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
    uint16_t permissions = 0;
};

struct ClientApproval {
    bool allow = false;
    // The operator's grant for this session. Bits outside the authentication
    // ceiling are dropped, so approval can only reduce.
    uint16_t permissions = 0;
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
    // Accept InputEvents from the viewer. Off by default: a client that
    // connects to a port bound to loopback is still a client, and the gate
    // belongs to the server, not to the binding.
    bool allow_input = false;
    // Accept ClipboardUpdates from the viewer and allow the server to send its
    // own. Off by default for the same reason as allow_input; HostServer turns
    // it on from the host settings.
    bool allow_clipboard = false;
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
    // Accept set_permissions() during a session. Off freezes the grant made
    // at approval time; the operator can then only end the session.
    bool allow_permission_change = true;
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
    bool finished = false;       // stream ended and every chunk was taken
    std::string error;           // set when the stream broke
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
    // The permissions the viewer asks for; the host intersects them with its
    // defaults and the operator's approval.
    uint16_t requested_permissions = kPermissionAll;
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
    // The display index the client asked for in Hello, or kDisplayAuto. Valid
    // once wait_for_client() succeeded; the server creates its capturer after
    // that so the request can pick the output.
    uint16_t requested_display() const;
    // SHA-256 fingerprint of the certificate this server presents, uppercase
    // hex. Stable across restarts because the certificate is stored in the
    // user's certificate store; this is what a client pins.
    std::string server_fingerprint() const;
    // "ip:port" of the client whose Hello this session accepted, empty when
    // the transport did not report it. Valid after wait_for_client().
    std::string client_address() const;
    // The permission mask in force: what the accepted viewer was granted on
    // the server, or what the host last announced on the client.
    uint16_t permissions() const;
    // Server: replaces the mask mid-session and tells the viewer. The new
    // mask is clamped to the grant approved at connect time, so a permission
    // the operator did not allow cannot be restored later. Revoking input
    // also lifts anything the injector still holds. Requires an announced
    // stream.
    Error set_permissions(uint16_t permissions);
    // Replaces the token the next client has to send. Call between sessions:
    // a token spent by the previous one becomes valid again under its new
    // value, which is how a host rotates a one-time token without rebinding.
    void set_access_token(std::string token);
    // Closes the finished connection and clears the per-session state, leaving
    // the listener up. Call after finish_server() so wait_for_client() can
    // accept the next client.
    void reset_for_next_client();
    // Answers the Hello with the stream description and opens the media
    // streams. Must be called once, after wait_for_client() succeeded. The
    // audio stream is opened too when `info.audio.enabled` is set and the
    // server allows audio; if the stream cannot be opened the announcement
    // falls back to video only instead of failing the session.
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
    // Pops the newest permission mask the host sent with PermissionUpdate.
    // permissions() already reflects it, so this is the "something changed"
    // edge for a UI that wants to explain the revocation.
    bool take_permission_update(uint16_t& permissions);
    uint64_t permission_updates() const;
    // Waits for one complete access unit. Returns WaitTimeout when nothing
    // arrived in time, or Unavailable once the peer finished the video stream.
    Error receive_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                        std::chrono::milliseconds timeout);
    // Waits for one complete audio access unit. Returns WaitTimeout when
    // nothing arrived in time, or Unavailable once the server finished the
    // audio stream; either is normal when audio is optional. A malformed
    // packet returns CaptureFailed, which the caller may treat as "audio
    // broken" without ending the video session.
    Error receive_audio(AudioHeader& header, std::vector<uint8_t>& payload,
                        std::chrono::milliseconds timeout);
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
    // Sends one LatencyProbe carrying this process's steady clock. The server
    // answers automatically; the result appears in latency(). Call about once
    // a second while streaming.
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
    // Requires the handshake and input permission; the answer arrives through
    // take_display_switch_result(). On Ok a StreamInfo describing the new
    // output follows on the same control stream.
    Error request_display_switch(uint16_t display_index);
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
    // Asks the host to change this session's video preferences (encoder,
    // frame-rate ceiling and quality preset). The host can only lower its own
    // budget; the answer arrives through take_stream_options_result(). No
    // permission is required because the change only affects this viewer's
    // picture. Unlike display switches, a changed preference survives a
    // reconnect only by being sent again.
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

    // ----- file transfer (both directions) -----
    //
    // The side that offers a file is the sender; the side that answers is the
    // receiver and writes the bytes. A viewer upload has the host as receiver;
    // a host download has the viewer as receiver. Every call below works on
    // both roles, and `session_permissions` must carry
    // kPermissionFileTransfer.

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

    // ----- pointer (server to client) -----

    // Sends the host's pointer state. Only the newest update matters to a
    // viewer, so a send never blocks: when the queue is full the update is
    // dropped and WaitTimeout returned, like audio.
    Error send_cursor(const CursorUpdate& update);
    // Pops the newest CursorUpdate received since the last call, discarding
    // any older ones that were never taken. Returns false when nothing new
    // arrived.
    bool take_cursor_update(CursorUpdate& update);
    // Total CursorUpdates received since the connection started.
    uint64_t cursor_updates_received() const;

    // True after the server shut down its send direction of the video stream.
    bool video_finished() const;
    // True after the server shut down its send direction of the audio stream.
    bool audio_finished() const;
    // Asynchronous transport/protocol error, if one was recorded.
    Error async_error() const;

private:
    QuicSession() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
