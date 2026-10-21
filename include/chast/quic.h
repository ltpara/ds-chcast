#pragma once

// Minimal QUIC session built on MsQuic, carrying the chast protocol.
//
// One QuicSession is one connection: the server side accepts a single client,
// announces the stream in a HelloAck, and writes access units on a server-
// initiated unidirectional stream; the client side connects, performs the
// Hello handshake, and reads frames. Control messages share a client-initiated
// bidirectional stream. See chast/protocol.h for the wire format.
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

#include "chast/error.h"
#include "chast/input.h"
#include "chast/protocol.h"

namespace chast {

// A Hello that passed the access-token check, handed to
// QuicServerConfig::approve_client so the operator can decide.
struct ClientRequest {
    uint16_t display = kDisplayAuto;
    // "ip:port" of the connecting peer, empty when the transport would not
    // report it.
    std::string peer_address;
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
    // Walk the injection path without calling SendInput. Only for the
    // loopback check, which must not move the real pointer.
    bool dry_run_input = false;
    // When non-empty, Hello must carry exactly this token or the server
    // answers AccessDenied. The comparison is constant time and happens
    // inside the TLS-authenticated connection.
    std::string access_token;
    // false: the first accepted Hello consumes the token, so a second viewer
    // presenting it is refused with AccessDenial::TokenUsed. true: the token
    // stays valid for later sessions (a fixed passphrase).
    bool reuse_access_token = false;
    // Called by wait_for_client() after a Hello passed the token check.
    // Returning false sends AccessDenied and keeps the listener waiting for
    // another client within the same timeout. The callback runs on the
    // calling thread, not on a MsQuic thread, so it may prompt an operator;
    // it should still return promptly, because the peer is waiting.
    std::function<bool(const ClientRequest&)> approve_client;
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

struct QuicClientConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    uint32_t idle_timeout_ms = 30000;
    // Sent in Hello. Empty when the server accepts connections without one.
    std::string access_token;
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
    // Replaces the token the next client has to send. Call between sessions:
    // a token spent by the previous one becomes valid again under its new
    // value, which is how a host rotates a one-time token without rebinding.
    void set_access_token(std::string token);
    // Closes the finished connection and clears the per-session state, leaving
    // the listener up. Call after finish_server() so wait_for_client() can
    // accept the next client.
    void reset_for_next_client();
    // Answers the Hello with the stream description and opens the video stream.
    // Must be called once, after wait_for_client() succeeded.
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
    // True when the client asked for a key frame since the last call.
    bool take_keyframe_request();
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
    // Waits for one complete access unit. Returns WaitTimeout when nothing
    // arrived in time, or Unavailable once the peer finished the video stream.
    Error receive_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                        std::chrono::milliseconds timeout);
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

    // True after the server shut down its send direction of the video stream.
    bool video_finished() const;
    // Asynchronous transport/protocol error, if one was recorded.
    Error async_error() const;

private:
    QuicSession() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
