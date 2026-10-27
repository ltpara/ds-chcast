#pragma once

// The host half of remote terminals: it spawns a console under ConPTY for an
// approved TerminalRequest and relays bytes between that console and one CTTY
// stream pair. It is driven from the host thread by pump(), which never
// blocks. A console whose far end is busy falls back to "try again next
// frame"; the QUIC receive window and a bounded queue absorb the burst.
//
// The relay owns the child process: ending a session, revoking the terminal
// permission or losing the viewer kills the console before the next session
// starts, so no shell survives its session.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/quic.h"

namespace chast {

class TerminalRelay {
public:
    explicit TerminalRelay(QuicSession& session);
    ~TerminalRelay();
    TerminalRelay(const TerminalRelay&) = delete;
    TerminalRelay& operator=(const TerminalRelay&) = delete;

    // Takes every queued request, resize and stop, moves bytes in both
    // directions and reaps exited shells. Returns human-readable state changes
    // for the host log.
    std::vector<std::string> pump();

    uint64_t terminals() const { return started_; }
    uint64_t failures() const { return failures_; }

private:
    struct Console;
    enum class Outcome { Keep, Done, Failed };

    void start_console(const TerminalRequest& request, std::vector<std::string>& events);
    Outcome drive(Console& console, std::vector<std::string>& events);
    void apply_resize(Console& console, const TerminalSize& size,
                      std::vector<std::string>& events);
    void stop_console(Console& console);
    void finish(Console& console, TerminalCode code, const std::string& reason,
                std::vector<std::string>& events);

    QuicSession& session_;
    std::vector<std::unique_ptr<Console>> live_;
    uint64_t started_ = 0;
    uint64_t failures_ = 0;
};

}  // namespace chast
