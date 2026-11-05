#pragma once

// The host half of remote terminals: it spawns a console under ConPTY for an
// approved TerminalRequest and relays bytes between that console and one CTTY
// stream pair. It is driven from the host thread by pump(), which never
// blocks. A console whose far end is busy falls back to "try again next
// frame"; the QUIC receive window and a bounded queue absorb the burst.
//
// With terminal persistence enabled, a console created with the persist flag
// becomes a host-level service: when the session ends the relay detaches it
// instead of killing it, keeps the last kTerminalScrollbackBytes of output and
// replays them to the next session that attaches by service id. A service no
// viewer attaches to within kTerminalServiceIdleSeconds is reaped. Services
// live in a process-wide registry, so a shell survives the HostServer that
// created it. Every other console is killed when its session ends.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "chast/quic.h"

namespace chast {

struct TerminalConsole;

class TerminalRelay {
public:
    explicit TerminalRelay(QuicSession& session, bool allow_persist = false);
    ~TerminalRelay();
    TerminalRelay(const TerminalRelay&) = delete;
    TerminalRelay& operator=(const TerminalRelay&) = delete;

    // Takes every queued request, resize and stop, moves bytes in both
    // directions and reaps exited shells. Returns human-readable state changes
    // for the host log.
    std::vector<std::string> pump();

    uint64_t terminals() const { return started_; }
    uint64_t failures() const { return failures_; }
    uint64_t resumes() const { return resumed_; }

private:
    enum class Outcome { Keep, Done, Failed };

    void start_console(const TerminalRequest& request, std::vector<std::string>& events);
    void attach_console(const TerminalRequest& request, std::vector<std::string>& events);
    Outcome drive(const std::shared_ptr<TerminalConsole>& console,
                  std::vector<std::string>& events);
    void apply_resize(TerminalConsole& console, const TerminalSize& size,
                      std::vector<std::string>& events);
    void stop_console(TerminalConsole& console);
    void detach_console(const std::shared_ptr<TerminalConsole>& console);
    void finish(const std::shared_ptr<TerminalConsole>& console, TerminalCode code,
                const std::string& reason, std::vector<std::string>& events);

    QuicSession& session_;
    bool allow_persist_ = false;
    std::vector<std::shared_ptr<TerminalConsole>> live_;
    uint64_t started_ = 0;
    uint64_t failures_ = 0;
    uint64_t resumed_ = 0;
};

}  // namespace chast
