// Real QUIC, authentication and GPU desktop streams. The lock API is replaced
// by a counter: regular CTest must never lock the developer's workstation.
#include "chast/host_server.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
using namespace std::chrono_literals;
namespace {
int failures = 0;
bool check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", text);
    if (!ok) ++failures;
    return ok;
}
bool until(const std::function<bool()>& test) {
    const auto end = std::chrono::steady_clock::now() + 8s;
    do { if (test()) return true; std::this_thread::sleep_for(20ms); }
    while (std::chrono::steady_clock::now() < end);
    return false;
}
bool state(chast::QuicSession& viewer, bool enabled) {
    return until([&] {
        bool actual = !enabled;
        chast::LockAfterSessionCode code = chast::LockAfterSessionCode::Failed;
        std::string error;
        if (!viewer.take_lock_after_session_state(actual, code, error)) return false;
        if (code != chast::LockAfterSessionCode::Ok) std::printf("state: %s\n", error.c_str());
        return actual == enabled && code == chast::LockAfterSessionCode::Ok;
    });
}
void run() {
    std::atomic<int> locks{0}, lock_errors{0};
    std::atomic<bool> fail_lock{false};
    chast::HostServerConfig cfg;
    cfg.max_sessions = 4;
    cfg.settings.bind_address = "127.0.0.1";
    cfg.settings.port = 45540;
    cfg.settings.carrier = false;
    cfg.settings.token_auth = true;
    cfg.settings.token_mode = chast::HostTokenMode::Reusable;
    cfg.settings.access_token = "lock-session-test";
    cfg.settings.allow_input = true;
    cfg.settings.allow_audio = false;
    cfg.dry_run_input = true;
    cfg.finish_timeout_ms = 100;
    cfg.lock_workstation = [&](std::string* error) {
        ++locks;
        if (fail_lock) { *error = "injected lock API failure"; return false; }
        return true;
    };
    cfg.log = [&](const std::string& text) {
        if (text.find("workstation lock request failed: injected lock API failure") != std::string::npos) ++lock_errors;
        std::printf("host: %s\n", text.c_str());
    };
    std::string error;
    auto host = chast::HostServer::create(cfg, &error);
    if (!check(host != nullptr, "listener created")) return;
    host->start();
    const auto connect = [&](chast::SessionMode mode = chast::SessionMode::Desktop) {
        chast::QuicClientConfig cc;
        cc.port = cfg.settings.port;
        cc.access_token = cfg.settings.access_token;
        cc.pinned_fingerprint = host->status().fingerprint;
        cc.session_mode = mode;
        auto viewer = chast::QuicSession::create_client(cc, &error);
        chast::ServerInfo info;
        if (!viewer || !viewer->connect(5s).ok() || !viewer->handshake(info, 30s).ok()) {
            check(false, "viewer connects and completes authentication");
            viewer.reset();
        }
        return viewer;
    };
    const auto empty = [&] { return until([&] { return host->sessions().empty(); }); };
    auto first = connect();
    if (!first) return;
    first.reset();
    check(empty() && locks == 0, "default desktop disconnect never locks");
    first = connect(); if (!first) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "authorized viewer opts in and receives acknowledgement");
    check(first->set_lock_after_session(false).ok() && state(*first, false), "viewer cancels its own request");
    first.reset();
    check(empty() && locks == 0, "cancelled request does not lock");
    first = connect(); if (!first) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "arm before permission revocation");
    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionInput);
    check(state(*first, false), "revocation cancels lock intent and announces actual state");
    check(!first->set_lock_after_session(true).ok(), "view-only connection cannot arm locking");
    check(first->set_lock_after_session(false).ok() && state(*first, false), "cancellation stays available after revocation");
    host->set_permissions(chast::kPermissionAll);
    check(until([&] { return (first->permissions() & chast::kPermissionInput) != 0; }), "input permission returns");
    first.reset();
    check(empty() && locks == 0, "restoring permission does not restore cancelled lock intent");
    first = connect(); if (!first) return;
    auto second = connect(); if (!second) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "one of two viewers opts in");
    first.reset();
    check(until([&] { return host->sessions().size() == 1; }) && locks == 0 && second->peer_alive(),
          "departing viewer cannot lock out another desktop viewer");
    auto files = connect(chast::SessionMode::FileTransfer); if (!files) return;
    check(!files->set_lock_after_session(true).ok() && !files->set_lock_after_session(false).ok(),
          "file-only connection cannot configure workstation locking");
    second.reset();
    check(until([&] { return locks == 1; }) && files->peer_alive(),
          "last desktop exit executes deferred lock without waiting for file sessions");
    files.reset();
    check(empty() && locks == 1, "deferred request executes exactly once");
    first = connect(); if (!first) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "arm before immediate revoke/disconnect race");
    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionInput);
    first.reset();
    check(empty() && locks == 1, "synchronous revocation wins even when no further frame can run");
    host->set_permissions(chast::kPermissionAll);
    first = connect(); if (!first) return;
    second = connect(); if (!second) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "arm deferred request before global revocation");
    first.reset();
    check(until([&] { return host->sessions().size() == 1; }), "requesting viewer departs first");
    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionInput);
    second.reset();
    check(empty() && locks == 1, "global revocation cancels already deferred lock requests");
    host->set_permissions(chast::kPermissionAll);
    first = connect(); if (!first) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "arm before operator disconnect");
    const auto sessions = host->sessions();
    check(sessions.size() == 1 && host->disconnect_session(sessions.front().session_id), "operator closes requested session");
    check(empty() && locks == 2, "operator disconnect honors authorized lock intent");
    first.reset();
    fail_lock = true;
    first = connect(); if (!first) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "arm before lock API failure");
    first.reset();
    check(empty() && locks == 3 && lock_errors == 1, "OS lock failure is recorded rather than reported as success");
    fail_lock = false;
    first = connect(); if (!first) return;
    first.reset();
    check(empty() && locks == 3, "failed request is not inherited by a later unrelated viewer");
    first = connect(); if (!first) return;
    check(first->set_lock_after_session(true).ok() && state(*first, true), "arm before host shutdown");
    host->stop();
    check(locks == 4, "graceful host shutdown honors the final desktop lock intent");
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run();
    return failures ? 1 : 0;
}
