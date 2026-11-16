#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "chast/host_server.h"
#include "chast/host_settings.h"

namespace chast::host {

// Viewer connections as the server control plane sees them. The operator UI
// names these "connections"; the wire form needs two ids that survive being
// copied out of this process:
//
//   runtime id     one per native listener runtime, never reused. Every
//                  listener start - including one inside the same process -
//                  allocates a fresh id, because a runtime that published its
//                  terminal snapshot is over for good: the server refuses to
//                  resurrect it, so commands queued for the old runtime can
//                  never be applied to a connection of the new one.
//   connection id  one per viewer, 32 lowercase hex characters, generated when
//                  the viewer is first observed. The process-local
//                  HostServer connection id grows monotonically and is only
//                  valid inside this process, so it is never exposed.
constexpr size_t kLiveConnectionIdBytes = 16;
constexpr size_t kLiveConnectionIdLength = 2 * kLiveConnectionIdBytes;

// Exactly 32 lowercase hex characters.
bool valid_live_connection_id(const std::string& value);
std::string generate_live_connection_id();

struct LiveConnectionInfo {
    std::string connection_id;
    std::string peer_address;
    std::string viewer;
    std::string mode;
    std::string route;
    uint32_t permissions = 0;
    bool controller = false;
    int64_t started_at_ms = 0;
    // Process-local HostServer connection id. Never serialized to the wire.
    uint64_t native_connection_id = 0;
};

// One queued command as the Go agent persisted it. The agent writes the
// command file; the native side only reads it and answers with a receipt.
struct LiveConnectionCommand {
    std::string command_id;
    std::string connection_id;
    std::string operation;
    std::string reason;
    int64_t issued_at_ms = 0;
    int64_t deadline_ms = 0;
};

// A native result. "applied" is only ever recorded with confirmed_absent set,
// after the connection is verifiably gone from the host's viewer set.
struct LiveConnectionReceipt {
    uint64_t sequence = 0;
    std::string command_id;
    std::string status;
    bool confirmed_absent = false;
    std::string detail;
};

// The native snapshot file. `<file>` is written by this process and read by
// chast-tunnel, which signs and uploads it. The agent owns `<file>.commands`;
// the two directions never share a writer.
struct LiveConnectionState {
    int version = 1;
    std::string runtime_id;
    uint64_t sequence = 0;
    bool terminal = false;
    std::vector<LiveConnectionInfo> connections;
    std::vector<LiveConnectionReceipt> receipts;
};

// <policy state>.live-connections and <policy state>.live-connections.commands.
// Empty when the host has no managed policy file, which keeps every managed
// control path disabled until the deployment configures one.
std::string live_connection_state_path(const HostSettings& local);
std::string live_connection_commands_path(const HostSettings& local);

// Reads the native snapshot. A missing file reports an empty state and
// succeeds; a file this build cannot parse is an error.
bool read_live_connection_state(const std::string& path, LiveConnectionState& state,
                               std::string* error);
// Atomic replacement: a temporary file in the same directory, flushed, then
// renamed over the target.
bool write_live_connection_state(const std::string& path, const LiveConnectionState& state,
                                std::string* error);

// Reads the agent's command inbox. Commands naming another runtime or an
// unknown shape are dropped rather than applied. A missing file is not an
// error.
bool read_live_connection_commands(const std::string& path, const std::string& runtime_id,
                                   std::vector<LiveConnectionCommand>& out,
                                   std::string* error);

// The host this controller acts on. The interface keeps the command state
// machine testable without a live QUIC host; the HostServer overloads below
// bind it to the real viewer set.
struct LiveConnectionSource {
    std::function<std::vector<LiveConnectionInfo>()> snapshot;
    std::function<bool(uint64_t native_connection_id)> disconnect;
};

LiveConnectionSource live_connection_source(HostServer& server);

// Owns the runtime id, the connection-id mapping and the on-disk snapshot.
// One instance per host process: the tray and the service both hold exactly
// one, and both report through the same file. A listener restart inside that
// process is a new runtime, not a continuation of the old one.
class LiveConnectionController {
public:
    // `state_path` empty disables the controller.
    explicit LiveConnectionController(std::string state_path);
    LiveConnectionController(const LiveConnectionController&) = delete;
    LiveConnectionController& operator=(const LiveConnectionController&) = delete;

    bool enabled() const { return !state_path_.empty(); }
    const std::string& state_path() const { return state_path_; }
    const std::string& commands_path() const { return commands_path_; }
    const std::string& runtime_id() const { return runtime_id_; }
    // Diagnostics for the operator UI and logs; the last tick error.
    const std::string& last_error() const { return last_error_; }

    // Publishes the current viewer set and resolves queued commands. Safe to
    // call repeatedly: an unchanged set keeps its sequence, so the agent's
    // repeated report is a heartbeat rather than a new snapshot.
    bool tick(HostServer& server, std::string* error);
    bool tick(const LiveConnectionSource& source, std::string* error);

    // Publishes the final snapshot of a stopping runtime: terminal, no
    // connections. The runtime is over once this succeeds: only start_runtime()
    // makes ticks publish again.
    bool publish_terminal(HostServer& server, std::string* error);
    bool publish_terminal(const LiveConnectionSource& source, std::string* error);

    // Begins a new native runtime under a fresh id. The tray restarts its
    // listener in place when settings change, so the id of a runtime that
    // already published its terminal snapshot must never be published again.
    void start_runtime();
    // True once this runtime published its terminal snapshot.
    bool ended() const { return ended_; }

private:
    struct PendingCommand {
        std::string connection_id;
        int64_t deadline_ms = 0;
    };

    bool run(const LiveConnectionSource& source, bool terminal, std::string* error);
    bool settle(const std::string& command_id, const std::string& status, bool confirmed_absent,
                const std::string& detail);
    std::vector<LiveConnectionInfo> map_connections(const std::vector<LiveConnectionInfo>& raw);

    std::string state_path_;
    std::string commands_path_;
    std::string runtime_id_;
    std::string last_error_;
    // Native id -> wire id of the viewers seen right now, and every wire id
    // this runtime ever issued. The second set is what distinguishes "already
    // gone" from "never existed here".
    std::map<uint64_t, std::string> wire_by_native_;
    std::set<std::string> issued_;
    std::map<std::string, int64_t> started_at_ms_;
    std::map<std::string, PendingCommand> pending_;
    LiveConnectionState state_;
    bool ended_ = false;
    uint64_t next_receipt_sequence_ = 0;
};

}  // namespace chast::host
