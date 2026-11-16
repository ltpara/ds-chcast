// Checks for the native live-connection control plane: id generation, the
// snapshot file the agent signs, and the command/receipt state machine that
// turns a server disconnect into one exact viewer disconnect. File-only, so
// no GPU, network or interactive desktop is involved.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "chast/host_settings.h"
#include "chast/live_connection.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

// Beside the test binary: some environments deny writes to the shared temp
// directory, and everything here is disposable either way.
std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    const std::filesystem::path module =
        length == 0 ? std::filesystem::path{} : std::filesystem::path(buffer).parent_path();
    const std::filesystem::path dir =
        (module.empty() ? std::filesystem::current_path() : module) /
        (L"host-live-connection-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A stand-in host: the tests choose exactly which viewers exist and whether a
// disconnect is accepted, without a QUIC listener.
struct FakeHost {
    std::vector<chast::host::LiveConnectionInfo> viewers;
    std::vector<std::pair<uint64_t, bool>> disconnects;
    bool refuse = false;
    bool removed_on_disconnect = true;

    chast::host::LiveConnectionSource source() {
        chast::host::LiveConnectionSource value;
        value.snapshot = [this] { return viewers; };
        value.disconnect = [this](uint64_t native_id) {
            disconnects.emplace_back(native_id, refuse);
            if (refuse) return false;
            if (removed_on_disconnect) {
                for (auto it = viewers.begin(); it != viewers.end(); ++it) {
                    if (it->native_connection_id == native_id) {
                        viewers.erase(it);
                        break;
                    }
                }
            }
            return true;
        };
        return value;
    }
};

chast::host::LiveConnectionInfo viewer(uint64_t native_id, const char* peer) {
    chast::host::LiveConnectionInfo info;
    info.native_connection_id = native_id;
    info.peer_address = peer;
    info.mode = "desktop";
    info.route = "direct";
    info.permissions = 0x3f;
    info.controller = native_id == 1;
    return info;
}

std::string command_json(const std::string& runtime_id, const std::string& command_id,
                         const std::string& connection_id, const std::string& operation,
                         int64_t deadline_ms) {
    return "{\"version\":1,\"runtime_id\":\"" + runtime_id + "\",\"commands\":[{"
           "\"command_id\":\"" + command_id + "\",\"host_id\":\"devbox\",\"runtime_id\":\"" +
           runtime_id + "\",\"connection_id\":\"" + connection_id +
           "\",\"operation\":\"" + operation + "\",\"request_key\":\"request-key-0001\","
           "\"status\":\"delivered\",\"issued_at_ms\":1,\"deadline_ms\":" +
           std::to_string(deadline_ms) + "}]}";
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t future_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() +
           60000;
}

void test_ids() {
    const std::string id = chast::host::generate_live_connection_id();
    check(chast::host::valid_live_connection_id(id), "a generated connection id is 32 lowercase hex");
    check(!chast::host::valid_live_connection_id(""), "an empty id is rejected");
    check(!chast::host::valid_live_connection_id(std::string(32, 'A')),
          "an uppercase id is rejected");
    check(!chast::host::valid_live_connection_id(std::string(31, 'a')),
          "a short id is rejected");
    check(!chast::host::valid_live_connection_id(std::string(32, 'g')),
          "a non-hex id is rejected");
    std::map<std::string, bool> seen;
    bool distinct = true;
    for (int i = 0; i < 512; ++i) {
        const std::string next = chast::host::generate_live_connection_id();
        if (!chast::host::valid_live_connection_id(next)) distinct = false;
        if (!seen.emplace(next, true).second) distinct = false;
    }
    check(distinct, "generated ids are valid and distinct");
    chast::host::LiveConnectionController first("ids-a.json");
    chast::host::LiveConnectionController second("ids-b.json");
    check(chast::host::valid_live_connection_id(first.runtime_id()),
          "a controller starts with a valid runtime id");
    check(first.runtime_id() == first.runtime_id(), "a runtime id is stable within one runtime");
    check(first.runtime_id() != second.runtime_id(),
          "two runtimes in one process never share a runtime id");
    first.start_runtime();
    check(first.runtime_id() != second.runtime_id() && !first.runtime_id().empty(),
          "starting a new runtime allocates a fresh runtime id");
}

void test_paths() {
    chast::HostSettings unmanaged;
    check(chast::host::live_connection_state_path(unmanaged).empty(),
          "an unmanaged host has no live connection state path");
    check(chast::host::live_connection_commands_path(unmanaged).empty(),
          "an unmanaged host has no command queue path");
    chast::HostSettings managed;
    managed.policy_state = "C:/data/host.policy.json";
    check(chast::host::live_connection_state_path(managed) ==
              "C:/data/host.policy.json.live-connections",
          "the state path derives from the policy file");
    check(chast::host::live_connection_commands_path(managed) ==
              "C:/data/host.policy.json.live-connections.commands",
          "the agent queues commands beside the snapshot");
}

void test_state_round_trip(const std::filesystem::path& dir) {
    chast::host::LiveConnectionState state;
    state.runtime_id = chast::host::generate_live_connection_id();
    state.sequence = 7;
    chast::host::LiveConnectionInfo info = viewer(9, "203.0.113.9:51000");
    info.connection_id = chast::host::generate_live_connection_id();
    state.connections.push_back(info);
    chast::host::LiveConnectionReceipt receipt;
    receipt.sequence = 2;
    receipt.command_id = "cmd-round-trip";
    receipt.status = "failed";
    receipt.detail = "still present";
    state.receipts.push_back(receipt);
    const std::filesystem::path path = dir / L"round-trip.json";
    std::string error;
    check(chast::host::write_live_connection_state(path.string(), state, &error),
          "the snapshot file is written");
    chast::host::LiveConnectionState loaded;
    check(chast::host::read_live_connection_state(path.string(), loaded, &error),
          "the snapshot file is read back");
    check(loaded.runtime_id == state.runtime_id && loaded.sequence == 7 && !loaded.terminal,
          "the runtime, sequence and terminal flag survive a round trip");
    check(loaded.connections.size() == 1 &&
              loaded.connections[0].connection_id == info.connection_id &&
              loaded.connections[0].peer_address == info.peer_address,
          "a connection survives a round trip");
    check(loaded.receipts.size() == 1 && loaded.receipts[0].sequence == 2 &&
              loaded.receipts[0].status == "failed" && loaded.receipts[0].detail == "still present",
          "a receipt survives a round trip");
    chast::host::LiveConnectionState missing;
    check(chast::host::read_live_connection_state((dir / L"absent.json").string(), missing, &error),
          "a missing snapshot file is empty state, not an error");
    check(missing.runtime_id.empty() && missing.connections.empty(),
          "the missing file reports no runtime");
    write_file(dir / L"corrupt.json", "{not json");
    check(!chast::host::read_live_connection_state((dir / L"corrupt.json").string(), missing, &error),
          "a corrupt snapshot file is an error");
}

void test_snapshot_and_sequence(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"snapshot.json";
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000"), viewer(2, "203.0.113.2:2000")};
    chast::host::LiveConnectionController controller(path.string());
    std::string error;
    check(controller.tick(host.source(), &error), "the first tick publishes a snapshot");
    chast::host::LiveConnectionState state;
    check(chast::host::read_live_connection_state(path.string(), state, &error),
          "the published snapshot is readable");
    check(state.runtime_id == controller.runtime_id(), "the snapshot carries the runtime id");
    check(state.sequence == 1, "the first snapshot has sequence 1");
    check(state.connections.size() == 2, "both viewers are reported");
    bool ids_ok = true;
    for (const auto& item : state.connections) {
        ids_ok &= chast::host::valid_live_connection_id(item.connection_id);
    }
    check(ids_ok, "every connection has a wire id");
    check(state.connections[0].connection_id != state.connections[1].connection_id,
          "two viewers get different wire ids");
    const std::string first_id = state.connections[0].connection_id;

    check(controller.tick(host.source(), &error), "a repeated tick succeeds");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.sequence == 1, "an unchanged viewer set keeps the sequence");
    check(state.connections.size() == 2 && state.connections[0].connection_id == first_id,
          "an unchanged viewer set keeps its ids");

    host.viewers.push_back(viewer(3, "203.0.113.3:3000"));
    check(controller.tick(host.source(), &error), "a new viewer is published");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.sequence == 2, "a new viewer advances the sequence");
    check(state.connections.size() == 3, "the new viewer is reported");
    std::string third_id;
    for (const auto& item : state.connections) {
        if (item.peer_address == "203.0.113.3:3000") third_id = item.connection_id;
    }
    check(!third_id.empty() && third_id != first_id, "a new viewer gets a fresh wire id");

    // The viewer leaves on its own: the next snapshot drops it by peer, not by
    // vector position (snapshots are sorted by wire id), and its id must not
    // reappear under a different viewer.
    const auto first_viewer = std::find_if(host.viewers.begin(), host.viewers.end(),
                                           [](const auto& item) {
                                               return item.peer_address == "203.0.113.1:1000";
                                           });
    check(first_viewer != host.viewers.end(), "the first viewer is found by peer");
    const uint64_t first_native = first_viewer == host.viewers.end() ? 0 : first_viewer->native_connection_id;
    std::string departed_id;
    for (const auto& item : state.connections) {
        if (item.peer_address == "203.0.113.1:1000") departed_id = item.connection_id;
    }
    check(!departed_id.empty(), "the departing viewer has a wire id");
    if (first_viewer != host.viewers.end()) host.viewers.erase(first_viewer);
    check(controller.tick(host.source(), &error), "a departed viewer is published");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.sequence == 3, "a departed viewer advances the sequence");
    check(state.connections.size() == 2, "the departed viewer is gone");
    for (const auto& item : state.connections) {
        if (item.connection_id == departed_id) check(false, "the departed wire id is not reused");
    }
    check(first_native != 0, "the departed native id was captured");
}

void test_terminal(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"terminal.json";
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000")};
    chast::host::LiveConnectionController controller(path.string());
    std::string error;
    check(controller.tick(host.source(), &error), "a live tick succeeds");
    host.viewers.clear();
    check(controller.publish_terminal(host.source(), &error), "the terminal snapshot is written");
    chast::host::LiveConnectionState state;
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.terminal, "the terminal snapshot is marked terminal");
    check(state.connections.empty(), "the terminal snapshot has no connections");
    check(state.sequence == 2, "the terminal snapshot advances the sequence");
    check(controller.publish_terminal(host.source(), &error),
          "a repeated terminal publish succeeds");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.sequence == 2, "a repeated terminal publish does not advance the sequence");
}

// The tray restarts its listener in place when settings change. The server
// refuses to resurrect a runtime that already published its terminal snapshot,
// so a restart inside one process has to begin a new runtime instead of
// republishing the ended one.
void test_runtime_restart(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"restart.json";
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000")};
    chast::host::LiveConnectionController controller(path.string());
    std::string error;
    check(controller.tick(host.source(), &error), "the first runtime publishes a snapshot");
    const std::string first_runtime = controller.runtime_id();
    chast::host::LiveConnectionState state;
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.sequence == 1, "the first runtime starts at sequence 1");
    const std::string first_wire = state.connections.empty() ? std::string{}
                                                             : state.connections[0].connection_id;

    check(controller.publish_terminal(host.source(), &error),
          "the first runtime publishes its terminal snapshot");
    check(controller.ended(), "a terminal publish ends the runtime");
    check(!controller.tick(host.source(), &error),
          "an ended runtime refuses to publish a live snapshot");
    check(!controller.tick(host.source(), &error), "the refusal is stable across ticks");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.terminal && state.sequence == 2 && state.connections.empty(),
          "a refused tick leaves the terminal snapshot untouched");

    // The listener restarts: same process, same viewer, new runtime.
    host.viewers = {viewer(1, "203.0.113.1:1000")};
    controller.start_runtime();
    check(!controller.ended(), "a new runtime is not ended");
    check(controller.runtime_id() != first_runtime,
          "the new listener runtime gets a runtime id the server has not seen");
    check(controller.tick(host.source(), &error), "the new runtime publishes a snapshot");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.runtime_id == controller.runtime_id(), "the snapshot carries the new runtime id");
    check(!state.terminal, "the new runtime is live, not terminal");
    check(state.sequence == 1, "the new runtime restarts its sequence");
    check(state.connections.size() == 1, "the viewer of the new runtime is reported");
    check(!state.connections.empty() && state.connections[0].connection_id != first_wire,
          "a wire id from the previous runtime is never reissued");
    check(state.receipts.empty(), "receipts of the previous runtime do not carry over");

    // A command naming the old runtime's wire id cannot touch this runtime.
    host.disconnects.clear();
    write_file(controller.commands_path(),
               command_json(controller.runtime_id(), "cmd-old-wire", first_wire, "disconnect",
                            future_ms()));
    check(controller.tick(host.source(), &error), "a command naming an old wire id is consumed");
    check(host.disconnects.empty(), "an old wire id disconnects no viewer of the new runtime");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.receipts.size() == 1 && state.receipts[0].status == "applied" &&
              state.receipts[0].confirmed_absent,
          "the old wire id is answered as already ended");
}

void test_disabled_controller() {
    chast::host::LiveConnectionController controller{std::string()};
    FakeHost host;
    std::string error;
    check(!controller.enabled(), "an empty path disables the controller");
    check(controller.runtime_id().empty(), "a disabled controller has no runtime id");
    check(controller.tick(host.source(), &error), "a disabled controller is a no-op");
    check(host.disconnects.empty(), "a disabled controller disconnects nothing");
}

void test_command_applies_only_the_target(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"command.json";
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000"), viewer(2, "203.0.113.2:2000")};
    chast::host::LiveConnectionController controller(path.string());
    std::string error;
    check(controller.tick(host.source(), &error), "the viewer set is published");
    chast::host::LiveConnectionState state;
    chast::host::read_live_connection_state(path.string(), state, &error);
    std::string target_id;
    for (const auto& item : state.connections) {
        if (item.peer_address == "203.0.113.2:2000") target_id = item.connection_id;
    }
    check(!target_id.empty(), "the second viewer has a wire id");
    write_file(controller.commands_path(),
               command_json(controller.runtime_id(), "cmd-1", target_id, "disconnect", future_ms()));
    check(controller.tick(host.source(), &error), "the queued command is consumed");
    check(host.disconnects.size() == 1 && host.disconnects[0].first == 2,
          "exactly the addressed viewer was asked to disconnect");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.receipts.empty(),
          "no receipt is written before the connection is verifiably gone");
    check(state.connections.size() == 1, "the snapshot already reflects the disconnect");
    check(controller.tick(host.source(), &error), "the next tick settles the command");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.receipts.size() == 1, "exactly one receipt is written");
    check(state.receipts[0].command_id == "cmd-1", "the receipt names the command");
    check(state.receipts[0].status == "applied", "the receipt reports applied");
    check(state.receipts[0].confirmed_absent, "applied is bound to confirmed_absent");
    check(state.receipts[0].sequence == 1, "the first receipt has sequence 1");
    check(host.viewers.size() == 1 && host.viewers[0].native_connection_id == 1,
          "the other viewer is untouched");
    const uint64_t sequence_after = state.sequence;
    check(controller.tick(host.source(), &error), "a later tick is still fine");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.receipts.size() == 1 && state.receipts[0].sequence == 1,
          "a settled command is not receipted twice");
    check(state.sequence == sequence_after, "a settled command does not bump the sequence");
    check(host.disconnects.size() == 1, "a settled command is not applied twice");
}

void test_command_rejections(const std::filesystem::path& dir) {
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000")};
    {
        const std::filesystem::path path = dir / L"reject-expired.json";
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        const std::string id = state.connections[0].connection_id;
        write_file(controller.commands_path(),
                   command_json(controller.runtime_id(), "cmd-expired", id, "disconnect", 1));
        check(controller.tick(host.source(), &error), "an expired command is consumed");
        check(host.disconnects.empty(), "an expired command disconnects nothing");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.size() == 1 && state.receipts[0].status == "failed",
              "an expired command is receipted as failed");
        check(!state.receipts[0].confirmed_absent, "a failed receipt never claims absence");
    }
    {
        const std::filesystem::path path = dir / L"reject-unknown.json";
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        const std::string foreign = chast::host::generate_live_connection_id();
        write_file(controller.commands_path(),
                   command_json(controller.runtime_id(), "cmd-unknown", foreign, "disconnect",
                                future_ms()));
        check(controller.tick(host.source(), &error), "an unknown id is consumed");
        check(host.disconnects.empty(), "an unknown connection id disconnects nothing");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.size() == 1 && state.receipts[0].status == "failed",
              "an unknown connection id is receipted as failed");
    }
    {
        const std::filesystem::path path = dir / L"reject-runtime.json";
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        const std::string id = state.connections[0].connection_id;
        write_file(controller.commands_path(),
                   command_json(chast::host::generate_live_connection_id(), "cmd-foreign", id,
                                "disconnect", future_ms()));
        check(controller.tick(host.source(), &error), "a foreign runtime queue is ignored");
        check(host.disconnects.empty(), "another runtime's command disconnects nothing");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.empty(), "another runtime's command is not receipted here");
    }
    {
        const std::filesystem::path path = dir / L"reject-operation.json";
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        const std::string id = state.connections[0].connection_id;
        write_file(controller.commands_path(),
                   command_json(controller.runtime_id(), "cmd-op", id, "reboot", future_ms()));
        check(controller.tick(host.source(), &error), "an unsupported operation is consumed");
        check(host.disconnects.empty(), "an unsupported operation disconnects nothing");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.size() == 1 && state.receipts[0].status == "failed",
              "an unsupported operation is receipted as failed");
    }
}

void test_refusal_and_deadline(const std::filesystem::path& dir) {
    {
        const std::filesystem::path path = dir / L"refused.json";
        FakeHost host;
        host.viewers = {viewer(1, "203.0.113.1:1000")};
        host.refuse = true;
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        write_file(controller.commands_path(),
                   command_json(controller.runtime_id(), "cmd-refused",
                                state.connections[0].connection_id, "disconnect", future_ms()));
        check(controller.tick(host.source(), &error), "a refused command is consumed");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.size() == 1 && state.receipts[0].status == "failed",
              "a refused disconnect is receipted as failed, not applied");
        check(!state.receipts[0].confirmed_absent,
              "a refused disconnect never claims the viewer is gone");
    }
    {
        const std::filesystem::path path = dir / L"still-present.json";
        FakeHost host;
        host.viewers = {viewer(1, "203.0.113.1:1000")};
        host.removed_on_disconnect = false;
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        // A short deadline models a host that accepts the request but never
        // finishes closing the viewer.
        const int64_t deadline = now_ms() + 400;
        write_file(controller.commands_path(),
                   command_json(controller.runtime_id(), "cmd-stubborn",
                                state.connections[0].connection_id, "disconnect", deadline));
        check(controller.tick(host.source(), &error), "the disconnect is requested");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.empty(), "no receipt while the viewer is still listed");
        while (now_ms() <= deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(controller.tick(host.source(), &error), "the deadline tick succeeds");
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.receipts.size() == 1 && state.receipts[0].status == "failed" &&
                  !state.receipts[0].confirmed_absent,
              "a viewer that survives the deadline is receipted as failed");
        check(host.disconnects.size() == 1, "the pending command was not re-issued");
    }
}

void test_stale_id_is_not_retargeted(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"stale.json";
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000")};
    chast::host::LiveConnectionController controller(path.string());
    std::string error;
    check(controller.tick(host.source(), &error), "the first viewer is published");
    chast::host::LiveConnectionState state;
    chast::host::read_live_connection_state(path.string(), state, &error);
    const std::string first_id = state.connections[0].connection_id;
    host.viewers.clear();
    check(controller.tick(host.source(), &error), "the viewer leaving is published");
    host.viewers = {viewer(2, "203.0.113.2:2000")};
    check(controller.tick(host.source(), &error), "a later viewer is published");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.connections.size() == 1 && state.connections[0].connection_id != first_id,
          "the later viewer never inherits the departed wire id");
    write_file(controller.commands_path(),
               command_json(controller.runtime_id(), "cmd-stale", first_id, "disconnect",
                            future_ms()));
    check(controller.tick(host.source(), &error), "a command for the stale id is consumed");
    check(host.disconnects.empty(), "a stale wire id does not disconnect the new viewer");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.receipts.size() == 1 && state.receipts[0].status == "applied" &&
              state.receipts[0].confirmed_absent,
          "the stale command resolves as applied because that connection had ended");
    check(state.connections.size() == 1, "the new viewer is still connected");
}

void test_degraded_inputs(const std::filesystem::path& dir) {
    {
        const std::filesystem::path path = dir / L"corrupt-inbox.json";
        FakeHost host;
        host.viewers = {viewer(1, "203.0.113.1:1000")};
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "the viewer set is published");
        write_file(controller.commands_path(), "{\"version\":1,\"commands\":[");
        check(!controller.tick(host.source(), &error), "a corrupt command queue is reported");
        check(!controller.last_error().empty(), "the corrupt queue has a diagnostic");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.connections.size() == 1, "a corrupt queue keeps the last known snapshot");
        check(host.disconnects.empty(), "a corrupt queue disconnects nothing");
    }
    {
        const std::filesystem::path path = dir / L"foreign-state.json";
        chast::HostSettings settings;
        chast::host::LiveConnectionState foreign;
        foreign.runtime_id = chast::host::generate_live_connection_id();
        foreign.sequence = 41;
        foreign.terminal = true;
        chast::host::LiveConnectionReceipt receipt;
        receipt.sequence = 9;
        receipt.command_id = "cmd-other-process";
        receipt.status = "failed";
        foreign.receipts.push_back(receipt);
        std::string error;
        check(chast::host::write_live_connection_state(path.string(), foreign, &error),
              "a foreign runtime file is written");
        FakeHost host;
        host.viewers = {viewer(1, "203.0.113.1:1000")};
        chast::host::LiveConnectionController controller(path.string());
        check(controller.tick(host.source(), &error), "a foreign runtime file is replaced");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.runtime_id == controller.runtime_id(),
              "the new process publishes under its own runtime id");
        check(state.terminal == false, "the new runtime is not terminal");
        check(state.receipts.empty(), "the previous runtime's receipts are not carried over");
        check(state.sequence == 1, "the new runtime restarts at the first sequence");
    }
    {
        const std::filesystem::path path = dir / L"unreadable.json";
        write_file(path, "{\"version\":99}");
        FakeHost host;
        host.viewers = {viewer(1, "203.0.113.1:1000")};
        chast::host::LiveConnectionController controller(path.string());
        std::string error;
        check(controller.tick(host.source(), &error), "an unreadable snapshot file is replaced");
        chast::host::LiveConnectionState state;
        chast::host::read_live_connection_state(path.string(), state, &error);
        check(state.sequence == 1 && state.connections.size() == 1,
              "the replacement snapshot reports the current viewers");
    }
}

void test_two_viewers_independently(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"two.json";
    FakeHost host;
    host.viewers = {viewer(1, "203.0.113.1:1000"), viewer(2, "203.0.113.2:2000"),
                    viewer(3, "203.0.113.3:3000")};
    chast::host::LiveConnectionController controller(path.string());
    std::string error;
    check(controller.tick(host.source(), &error), "three viewers are published");
    chast::host::LiveConnectionState state;
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.connections.size() == 3, "all three viewers are reported");
    std::map<std::string, std::string> wire;
    for (const auto& item : state.connections) wire[item.peer_address] = item.connection_id;
    write_file(controller.commands_path(),
               command_json(controller.runtime_id(), "cmd-second", wire["203.0.113.2:2000"],
                            "disconnect", future_ms()));
    check(controller.tick(host.source(), &error), "the targeted command is consumed");
    check(controller.tick(host.source(), &error), "the receipt tick succeeds");
    chast::host::read_live_connection_state(path.string(), state, &error);
    check(state.receipts.size() == 1 && state.receipts[0].status == "applied",
          "the targeted disconnect is applied");
    bool other_present = true, target_present = false;
    for (const auto& item : host.viewers) {
        if (item.peer_address == "203.0.113.2:2000") target_present = true;
        if (item.peer_address != "203.0.113.1:1000" && item.peer_address != "203.0.113.3:3000")
            other_present = false;
    }
    check(other_present && !target_present,
          "only the targeted viewer left the other two alone");
    bool wired = true;
    for (const auto& item : state.connections) {
        wired &= wire[item.peer_address] == item.connection_id;
    }
    check(wired, "the surviving viewers keep their wire ids");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::filesystem::path dir = temp_dir();
    test_ids();
    test_paths();
    test_state_round_trip(dir);
    test_snapshot_and_sequence(dir);
    test_terminal(dir);
    test_runtime_restart(dir);
    test_disabled_controller();
    test_command_applies_only_the_target(dir);
    test_command_rejections(dir);
    test_refusal_and_deadline(dir);
    test_stale_id_is_not_retargeted(dir);
    test_degraded_inputs(dir);
    test_two_viewers_independently(dir);
    std::printf("%s\n", failures == 0 ? "all live connection checks passed"
                                      : "live connection checks failed");
    return failures == 0 ? 0 : 1;
}
