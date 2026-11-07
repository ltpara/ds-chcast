// Checks for the host controls (protocol v38): console lock, remote restart and
// blocking the operator's local input. A loopback viewer asks for each action,
// the host relays it to a synthetic backend (no real restart or input block),
// and the check follows the result, the counters and the automatic release
// when the permission is revoked or the session ends, even with a stalled
// clipboard reader. Also checks clipboard baselines, revocation and echo
// suppression across delayed reads. Needs an interactive
// desktop because the stream lives inside a desktop session, so it skips
// itself when no desktop exists.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chast/host_server.h"
#include "chast/security.h"
#include "clipboard_stub.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void skip(const char* what) {
    std::printf("skip %s\n", what);
}

const std::string& password() {
    static const std::string value = "host-control-password";
    return value;
}

uint16_t free_udp_port() {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    uint16_t port = 0;
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        sockaddr_in bound{};
        int length = sizeof(bound);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
            port = ntohs(bound.sin_port);
        }
    }
    closesocket(socket);
    return port;
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    const std::filesystem::path dir =
        std::filesystem::path(buffer) / L"chast-host-control-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Stands in for ExitWindowsEx, LockWorkStation and BlockInput: records every
// call and lets one case fail on demand.
struct ControlState {
    std::mutex mutex;
    std::vector<std::pair<chast::HostAction, bool>> calls;
    std::atomic<bool> fail_next{false};

    std::function<bool(chast::HostAction, bool, std::string*)> hook() {
        return [this](chast::HostAction action, bool block_input, std::string* error) {
            std::lock_guard<std::mutex> lock(mutex);
            calls.emplace_back(action, block_input);
            if (fail_next.exchange(false)) {
                *error = "synthetic platform failure";
                return false;
            }
            return true;
        };
    }

    size_t count(chast::HostAction action, bool block_input) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t total = 0;
        for (const auto& call : calls) {
            if (call.first == action && call.second == block_input) {
                ++total;
            }
        }
        return total;
    }
};

// Every read takes a snapshot, then waits for the test to let it return.
// This reproduces delayed rendering without touching the Windows clipboard.
struct ClipboardState {
    std::mutex mutex;
    std::condition_variable cv;
    size_t started = 0;
    size_t released = 0;
    size_t writes = 0;
    bool closing = false;
    bool timed_out = false;
    std::string text = "session baseline";

    bool read(chast::ClipboardContent& content) {
        std::unique_lock<std::mutex> lock(mutex);
        const size_t ticket = ++started;
        const std::string snapshot = text;
        if (!cv.wait_for(lock, std::chrono::seconds(30),
                         [&] { return closing || released >= ticket; })) {
            timed_out = true;
        }
        content = {};
        content.formats.push_back({chast::ClipboardFormat::Text,
                                   {snapshot.begin(), snapshot.end()}});
        return true;
    }

    bool write(const chast::ClipboardContent& content) {
        std::lock_guard<std::mutex> lock(mutex);
        text = content.text();
        ++writes;
        return true;
    }

    void set(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        text = value;
    }

    bool has_started(size_t ticket) {
        std::lock_guard<std::mutex> lock(mutex);
        return started >= ticket;
    }

    bool has_write() {
        std::lock_guard<std::mutex> lock(mutex);
        return writes != 0;
    }

    void release(size_t ticket) {
        std::lock_guard<std::mutex> lock(mutex);
        released = ticket;
        cv.notify_all();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true;
        cv.notify_all();
    }
};

std::unique_ptr<chast::HostServer> start_host(uint16_t port, const std::filesystem::path& dir,
                                              std::shared_ptr<ControlState> control,
                                              std::shared_ptr<ClipboardState> clipboard,
                                              std::string* error) {
    chast::HostServerConfig config;
    use_empty_clipboard(config);
    config.clipboard_read = [clipboard](chast::ClipboardContent& content) {
        return clipboard->read(content);
    };
    config.clipboard_write = [clipboard](const chast::ClipboardContent& content) {
        return clipboard->write(content);
    };
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 1;
    config.settings.allow_input = true;
    config.settings.allow_audio = false;
    config.settings.allow_microphone = false;
    config.settings.allow_restart = true;
    config.settings.allow_block_input = true;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(password());
    config.settings.audit_spool = (dir / L"control.audit.jsonl").string();
    config.settings.file_receive_directory = (dir / L"receive").string();
    config.first_client_timeout_ms = 0;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    config.log = [](const std::string& line) { std::printf("     host: %s\n", line.c_str()); };
    config.host_control = control->hook();
    std::unique_ptr<chast::HostServer> server =
        chast::HostServer::create(std::move(config), error);
    if (!server) {
        return nullptr;
    }
    server->start();
    if (!wait_for(
            [&server] { return server->status().state == chast::HostState::Listening; },
            std::chrono::seconds(5))) {
        *error = "the host did not start listening";
        server->stop();
        return nullptr;
    }
    return server;
}

std::unique_ptr<chast::QuicSession> connect_viewer(uint16_t port,
                                                   const std::string& fingerprint,
                                                   chast::ServerInfo* info,
                                                   std::string* error) {
    chast::QuicClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.idle_timeout_ms = 8000;
    config.pinned_fingerprint = fingerprint;
    config.password = password();
    config.session_mode = chast::SessionMode::Desktop;
    std::unique_ptr<chast::QuicSession> session =
        chast::QuicSession::create_client(config, error);
    if (!session) {
        return nullptr;
    }
    const chast::Error connected = session->connect(std::chrono::seconds(8));
    if (!connected.ok()) {
        *error = "connect: " + connected.to_string();
        return nullptr;
    }
    const chast::Error handshaken = session->handshake(*info, std::chrono::seconds(8));
    if (!handshaken.ok()) {
        *error = "handshake: " + handshaken.to_string();
        return nullptr;
    }
    return session;
}

bool take_result(chast::QuicSession& session, chast::HostAction action,
                 chast::HostControlResult& result, std::chrono::milliseconds timeout) {
    return wait_for(
        [&] {
            chast::HostControlResult next;
            while (session.take_host_action_result(next)) {
                if (next.action == action) {
                    result = next;
                    return true;
                }
            }
            return false;
        },
        timeout);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("FAIL WSAStartup\n");
        return 1;
    }
    const std::filesystem::path dir = temp_dir();
    const uint16_t port = free_udp_port();
    check(port != 0, "a loopback port is available");
    std::string error;
    auto control = std::make_shared<ControlState>();
    auto clipboard = std::make_shared<ClipboardState>();
    std::unique_ptr<chast::HostServer> host =
        port == 0 ? nullptr : start_host(port, dir, control, clipboard, &error);
    check(host != nullptr, "the host-control host starts");
    if (host) {
        const std::string fingerprint = host->status().fingerprint;
        chast::ServerInfo info;
        std::unique_ptr<chast::QuicSession> viewer =
            connect_viewer(port, fingerprint, &info, &error);
        if (!viewer) {
            std::printf("     viewer: %s\n", error.c_str());
        }
        check(viewer != nullptr, "the desktop viewer is admitted");
        if (viewer) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            if (host->status().state != chast::HostState::Streaming) {
                skip("the host-control check needs a desktop stream");
            } else {
                check(wait_for([&] { return clipboard->has_started(1); },
                               std::chrono::seconds(5)),
                      "the clipboard baseline stays blocked during host controls");
                const chast::PermissionMask permissions = viewer->permissions();
                check((permissions & chast::kPermissionRestart) != 0,
                      "the host offers the restart permission");
                check((permissions & chast::kPermissionBlockInput) != 0,
                      "the host offers the block-input permission");

                chast::HostControlResult result;
                check(viewer->request_host_action(chast::HostAction::LockScreen).ok(),
                      "the viewer locks the console");
                check(take_result(*viewer, chast::HostAction::LockScreen, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok && !result.block_input,
                      "the console lock is acknowledged");
                check(control->count(chast::HostAction::LockScreen, false) == 1,
                      "the lock reached the platform backend");

                check(viewer->request_host_action(chast::HostAction::Restart).ok(),
                      "the viewer restarts the host");
                check(take_result(*viewer, chast::HostAction::Restart, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok,
                      "the restart is acknowledged");
                check(control->count(chast::HostAction::Restart, false) == 1,
                      "the restart reached the platform backend");

                control->fail_next.store(true);
                check(viewer->request_host_action(chast::HostAction::LockScreen).ok(),
                      "the viewer asks for a lock the backend refuses");
                check(take_result(*viewer, chast::HostAction::LockScreen, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Failed &&
                          result.reason == "synthetic platform failure",
                      "a platform failure is reported with its reason");
                check(wait_for(
                          [&host] { return host->status().stats.host_action_failures == 1; },
                          std::chrono::seconds(5)),
                      "the host counts the failed action");

                check(viewer->request_host_action(chast::HostAction::BlockInput, true)
                          .ok(),
                      "the viewer blocks local input");
                check(take_result(*viewer, chast::HostAction::BlockInput, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok && result.block_input,
                      "the block is acknowledged with its state");
                check(wait_for([&host] { return host->status().stats.block_input_active; },
                               std::chrono::seconds(5)),
                      "the host reports local input as blocked");
                check(control->count(chast::HostAction::BlockInput, true) == 1,
                      "the block reached the platform backend");

                check(viewer->request_host_action(chast::HostAction::BlockInput, true)
                          .ok(),
                      "the viewer repeats the same block");
                check(take_result(*viewer, chast::HostAction::BlockInput, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok && result.block_input,
                      "repeating a block is idempotent");
                check(control->count(chast::HostAction::BlockInput, true) == 1,
                      "the backend is not called twice for the same state");

                check(viewer->request_host_action(chast::HostAction::BlockInput, false)
                          .ok(),
                      "the viewer unblocks local input");
                check(take_result(*viewer, chast::HostAction::BlockInput, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok && !result.block_input,
                      "the unblock is acknowledged");
                check(wait_for([&host] { return !host->status().stats.block_input_active; },
                               std::chrono::seconds(5)),
                      "the host reports local input as released");

                check(viewer->request_host_action(chast::HostAction::BlockInput, true)
                          .ok(),
                      "the viewer blocks local input again");
                check(take_result(*viewer, chast::HostAction::BlockInput, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok && result.block_input,
                      "the second block is acknowledged");
                host->set_permissions(chast::kPermissionAll & ~chast::kPermissionBlockInput);
                check(take_result(*viewer, chast::HostAction::BlockInput, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Denied &&
                          !result.block_input &&
                          result.reason == "the host released local input",
                      "revoking the permission releases local input and tells the viewer");
                check(wait_for([&host] { return !host->status().stats.block_input_active; },
                               std::chrono::seconds(5)),
                      "the host clears the block after the revocation");
                check(control->count(chast::HostAction::BlockInput, false) == 2,
                      "the revocation released the backend block");
                check(wait_for(
                          [&viewer] {
                              return (viewer->permissions() & chast::kPermissionBlockInput) == 0;
                          },
                          std::chrono::seconds(5)),
                      "the viewer learns that block input was revoked");
                const chast::Error refused =
                    viewer->request_host_action(chast::HostAction::BlockInput, true);
                check(!refused.ok() && refused.status() == chast::Status::PermissionDenied,
                      "the viewer refuses to queue a revoked block");
                check(wait_for(
                          [&host] { return host->status().stats.host_actions == 7; },
                          std::chrono::seconds(5)),
                      "no host action was sent for the revoked block");

                host->set_permissions(chast::kPermissionAll);
                check(wait_for(
                          [&viewer] {
                              return (viewer->permissions() & chast::kPermissionBlockInput) != 0;
                          },
                          std::chrono::seconds(5)),
                      "the viewer sees the restored block-input permission");
                check(viewer->request_host_action(chast::HostAction::BlockInput, true)
                          .ok(),
                      "the viewer blocks local input once more");
                check(take_result(*viewer, chast::HostAction::BlockInput, result,
                                  std::chrono::seconds(5)) &&
                          result.code == chast::HostActionCode::Ok && result.block_input,
                      "the restored block is acknowledged");
                check(wait_for([&host] { return host->status().stats.block_input_active; },
                               std::chrono::seconds(5)),
                      "the host reports the restored block");

                auto await_read = [&](size_t ticket) {
                    return wait_for([&] { return clipboard->has_started(ticket); },
                                    std::chrono::seconds(5));
                };
                clipboard->release(1);
                check(await_read(2), "clipboard polling resumes after the slow baseline");
                check(viewer->clipboard_contents_received() == 0,
                      "the delayed baseline is never sent to the viewer");

                clipboard->set("first local copy");
                clipboard->release(2);
                check(await_read(3), "the next clipboard read observes the local copy");
                clipboard->release(3);
                chast::ClipboardContent received;
                check(wait_for([&] { return viewer->take_clipboard_content(received); },
                               std::chrono::seconds(5)) && received.text() == "first local copy",
                      "a local clipboard change is sent after the baseline");

                check(await_read(4), "a clipboard poll is blocked before revocation");
                host->set_permissions(chast::kPermissionAll & ~chast::kPermissionClipboard);
                check(wait_for([&] {
                          return (viewer->permissions() & chast::kPermissionClipboard) == 0;
                      }, std::chrono::seconds(5)),
                      "clipboard revocation responds while a read is blocked");
                clipboard->set("copy while permission was off");
                clipboard->release(4);
                host->set_permissions(chast::kPermissionAll);
                check(wait_for([&] {
                          return (viewer->permissions() & chast::kPermissionClipboard) != 0;
                      }, std::chrono::seconds(5)), "clipboard permission is restored");
                check(await_read(5), "restoration starts a fresh clipboard baseline");
                clipboard->release(5);
                check(await_read(6), "polling follows the restored baseline");
                check(viewer->clipboard_contents_received() == 1,
                      "neither the stale read nor copies made while revoked are sent");

                chast::ClipboardContent remote;
                const std::string remote_text = "viewer copy";
                remote.formats.push_back({chast::ClipboardFormat::Text,
                                          {remote_text.begin(), remote_text.end()}});
                check(viewer->send_clipboard_content(std::move(remote)).ok(),
                      "a viewer write is queued while a local read is blocked");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                check(!clipboard->has_write(), "clipboard writes do not race a blocked read");
                clipboard->release(6);
                check(wait_for([&] { return clipboard->has_write(); }, std::chrono::seconds(5)),
                      "the queued write completes after the read is released");
                check(await_read(7), "a stale read is replaced after the viewer write");
                clipboard->release(7);
                check(await_read(8), "polling resumes after the viewer write");
                check(viewer->clipboard_contents_received() == 1,
                      "the old snapshot and viewer write are not echoed back");

                clipboard->set("copy after restoration");
                clipboard->release(8);
                check(await_read(9), "local copies continue after restoration");
                clipboard->release(9);
                check(wait_for([&] { return viewer->take_clipboard_content(received); },
                               std::chrono::seconds(5)) &&
                          received.text() == "copy after restoration",
                      "a new local copy is delivered after restoration");
                check(await_read(10), "a clipboard read remains blocked at disconnect");
            }
            chast::ClientStats stats;
            viewer->finish_client(stats, std::chrono::seconds(5));
            viewer.reset();
            if (host->status().state == chast::HostState::Streaming ||
                host->status().stats.block_input_active) {
                check(wait_for([&host] { return !host->status().stats.block_input_active; },
                               std::chrono::seconds(15)),
                      "ending the session releases local input");
            } else {
                check(true, "ending the session releases local input");
            }
            check(control->count(chast::HostAction::BlockInput, false) >= 3,
                  "the session end unblocked the platform backend");
            if (clipboard->has_started(10)) {
                viewer = connect_viewer(port, fingerprint, &info, &error);
                check(viewer != nullptr,
                      "a new viewer connects while the old clipboard read is blocked");
                if (viewer) {
                    chast::HostControlResult result;
                    check(viewer->request_host_action(chast::HostAction::LockScreen).ok() &&
                              take_result(*viewer, chast::HostAction::LockScreen, result,
                                          std::chrono::seconds(5)) &&
                              result.code == chast::HostActionCode::Ok,
                          "the new viewer's controls do not wait for the old read");
                    clipboard->release(10);
                    check(wait_for([&] { return clipboard->has_started(11); },
                                   std::chrono::seconds(5)),
                          "the new session takes its own clipboard baseline");
                    clipboard->release(11);
                    check(wait_for([&] { return clipboard->has_started(12); },
                                   std::chrono::seconds(5)),
                          "the new session starts polling after its baseline");
                    check(viewer->clipboard_contents_received() == 0,
                          "an old session's clipboard result never reaches the new viewer");
                    viewer->finish_client({}, std::chrono::seconds(5));
                    viewer.reset();
                }
            }
        }
        clipboard->close();
        host->stop();
        check(!clipboard->timed_out, "all slow clipboard reads were released by the test");
        host.reset();
    }
    WSACleanup();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("host-control checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host-control checks passed\n");
    return 0;
}
