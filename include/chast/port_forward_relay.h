#pragma once

// The host half of port forwarding: it dials the target a viewer asked for and
// relays bytes between that TCP socket and one CPFW connection. It is driven
// from the host thread by pump(), which never blocks; a dial in progress, a
// local endpoint that stopped reading and a full transport send queue all turn
// into "try again next frame" instead of a stalled capture loop.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/quic.h"

namespace chast {

class PortForwardRelay {
public:
    explicit PortForwardRelay(QuicSession& session);
    ~PortForwardRelay();
    PortForwardRelay(const PortForwardRelay&) = delete;
    PortForwardRelay& operator=(const PortForwardRelay&) = delete;

    // Takes every queued open, advances each dial and relays every live
    // connection. Returns human-readable state changes for the host log.
    std::vector<std::string> pump();

    // Connections that finished dialing and were accepted, and relay attempts
    // that ended in an error (a refused dial included).
    uint64_t connections() const { return accepted_; }
    uint64_t failures() const { return failures_; }

private:
    struct Conn;
    enum class Outcome { Keep, Done, Failed };

    void start_dial(const PortForwardOpen& open, std::vector<std::string>& events);
    Outcome drive(Conn& conn, std::vector<std::string>& events);
    void close_socket(Conn& conn) const;

    QuicSession& session_;
    std::vector<std::unique_ptr<Conn>> live_;
    uint64_t accepted_ = 0;
    uint64_t failures_ = 0;
};

}  // namespace chast
