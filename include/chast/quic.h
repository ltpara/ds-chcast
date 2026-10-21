#pragma once

// Minimal QUIC session built on MsQuic, carrying the chast protocol.
//
// One QuicSession is one connection: the server side accepts a single client,
// announces the stream in a HelloAck, and writes access units on a server-
// initiated unidirectional stream; the client side connects, performs the
// Hello handshake, and reads frames. Control messages share a client-initiated
// bidirectional stream. See chast/protocol.h for the wire format.
//
// The class is deliberately blocking and single-owner: MsQuic callbacks run on
// its worker threads, but every public method is called from one application
// thread. The methods that touch the peer wait on condition variables and
// return Error on timeout, transport failure, or protocol violation.
//
// TLS: the server presents a self-signed certificate generated on first use.
// The client disables certificate validation because there is no PKI yet and
// the first target is localhost; this is not suitable for an untrusted network
// and will be replaced by identity/key pinning in the rendezvous phase.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/error.h"
#include "chast/protocol.h"

namespace chast {

struct QuicServerConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 4433;
    // How long the connection may sit idle before the transport closes it.
    uint32_t idle_timeout_ms = 30000;
};

struct QuicClientConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 4433;
    uint32_t idle_timeout_ms = 30000;
    // Localhost development uses a self-signed server certificate; a later
    // phase pins the peer identity instead.
    bool skip_certificate_validation = true;
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

    // Waits for a client to connect and send Hello. A transport or protocol
    // failure ends the wait early.
    Error wait_for_client(std::chrono::milliseconds timeout);
    // Answers the Hello with the stream description and opens the video stream.
    // Must be called once, after wait_for_client() succeeded.
    Error announce(const ServerInfo& info);
    // Sends one access unit. The frame's sequence number, payload size and
    // CRC32 are filled in here; everything else comes from `header`. Blocks
    // when too much unacknowledged data is queued (backpressure), so the
    // caller can keep capturing without unbounded memory growth.
    Error send_frame(FrameHeader header, const uint8_t* payload, size_t size);
    // True when the client asked for a key frame since the last call.
    bool take_keyframe_request();
    // Ends the video stream, waits for the client's closing Stats/Bye, and
    // shuts the connection down. `stats_received` is false when the client
    // disappeared first; that alone is not an error.
    Error finish_server(std::chrono::milliseconds timeout, ClientStats& stats,
                        bool& stats_received);

    // ----- client -----

    // Starts the QUIC handshake and waits for it to complete.
    Error connect(std::chrono::milliseconds timeout);
    // Opens the control stream, sends Hello, and waits for HelloAck.
    Error handshake(ServerInfo& info, std::chrono::milliseconds timeout);
    // Waits for one complete access unit. Returns WaitTimeout when nothing
    // arrived in time, or Unavailable once the peer finished the video stream.
    Error receive_frame(FrameHeader& header, std::vector<uint8_t>& payload,
                        std::chrono::milliseconds timeout);
    // Asks the server for an IDR (sent on the control stream).
    Error request_keyframe();
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
