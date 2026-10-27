#pragma once

// The viewer half of port forwarding: it listens on 127.0.0.1 and maps every
// accepted local TCP connection onto one CPFW connection that the host relays
// to `remote_host:remote_port`. It is driven by pump() from the session
// thread, which never blocks: an unanswered request, a slow local endpoint and
// a full transport send queue all turn into "try again next poll".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/error.h"
#include "chast/quic.h"

namespace chast {

class PortForwardClient {
public:
    explicit PortForwardClient(QuicSession& session);
    ~PortForwardClient();
    PortForwardClient(const PortForwardClient&) = delete;
    PortForwardClient& operator=(const PortForwardClient&) = delete;

    // Binds the listener (127.0.0.1:local_port, 0 picks a free port) and
    // reports the bound port. The request for the remote target goes out on
    // the first pump(); connections are only accepted once the host answered
    // Ok, so a refused target leaves nothing relayed.
    Error start(const std::string& remote_host, uint16_t remote_port,
                uint16_t local_port, uint16_t* bound_port);
    // Closes the listener and every connection, and tells the host to drop
    // the target. Safe to call when nothing is running.
    void stop();

    // Non-blocking; call once per session loop iteration.
    std::vector<std::string> pump();

    bool running() const;
    std::string remote_host() const;
    uint16_t remote_port() const;
    uint16_t local_port() const;
    // Connections accepted and relayed so far this session, and attempts that
    // failed (target refused, transport error, local reset).
    uint64_t connections() const;
    uint64_t failures() const;
    uint64_t bytes_up() const;
    uint64_t bytes_down() const;
    // Empty while healthy; the host's reason after a refused target.
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chast
