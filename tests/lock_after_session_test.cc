// Workstation lock after the last session ends (the lock_after_session_end
// setting). A synthetic backend stands in for LockWorkStation, so regular
// CTest never locks the developer's workstation. The single-viewer host checks
// the default, a live enable, a live cancel, a failing platform call (logged,
// then retried by the next session) and the shutdown exemption; the shared
// host checks that a departing viewer cannot lock while another remains, that
// cancelling while connected wins, and that shutdown still suppresses the
// lock. Needs a desktop session, so it skips itself when no interactive
// desktop exists.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chast/host_server.h"

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

const std::string& access_token() {
    static const std::string value = "lock-session-test";
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

// Stands in for LockWorkStation: counts attempts and successes and lets one
// case fail so the error path is exercised without touching the desktop.
struct LockBackend {
    std::mutex mutex;
    int attempts = 0;
    int succeeded = 0;
    bool fail_next = false;

    std::function<bool(chast::HostAction, bool, std::string*)> hook() {
        return [this](chast::HostAction action, bool, std::string* error) {
            if (action != chast::HostAction::LockScreen) {
                return true;
            }
            std::lock_guard<std::mutex> lock(mutex);
            ++attempts;
            if (fail_next) {
                fail_next = false;
                *error = "injected lock failure";
                return false;
            }
            ++succeeded;
            return true;
        };
    }

    int attempt_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return attempts;
    }

    int success_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return succeeded;
    }

    void fail_once() {
        std::lock_guard<std::mutex> lock(mutex);
        fail_next = true;
    }
};

struct LogCapture {
    std::mutex mutex;
    std::vector<std::string> errors;

    size_t count(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t total = 0;
        for (const std::string& line : errors) {
            if (line.find(needle) != std::string::npos) {
                ++total;
            }
        }
        return total;
    }
};

std::unique_ptr<chast::HostServer> start_host(uint16_t port, uint32_t max_viewers,
                                              const std::shared_ptr<LockBackend>& backend,
                                              const std::shared_ptr<LogCapture>& logs,
                                              std::string* error) {
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = max_viewers;
    config.settings.allow_input = true;
    config.settings.allow_audio = false;
    config.settings.token_auth = true;
    config.settings.access_token = access_token();
    config.settings.token_mode = chast::HostTokenMode::Reusable;
    config.dry_run_input = true;
    config.settings.lock_after_session_end = false;
    config.first_client_timeout_ms = 0;
    config.finish_timeout_ms = 200;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    config.host_control = backend->hook();
    config.log = [logs](const std::string& line) {
        std::printf("     host: %s\n", line.c_str());
        if (line.find("cannot lock the workstation after the session") != std::string::npos) {
            std::lock_guard<std::mutex> lock(logs->mutex);
            logs->errors.push_back(line);
        }
    };
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
    config.access_token = access_token();
    config.pinned_fingerprint = fingerprint;
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

bool wait_streaming(const std::unique_ptr<chast::HostServer>& host,
                    std::chrono::milliseconds timeout) {
    return wait_for([&host] { return host->status().state == chast::HostState::Streaming; },
                    timeout);
}

bool wait_listening(const std::unique_ptr<chast::HostServer>& host,
                    std::chrono::milliseconds timeout) {
    return wait_for([&host] { return host->status().state == chast::HostState::Listening; },
                    timeout);
}

void finish_viewer(chast::QuicSession& viewer) {
    chast::ClientStats stats;
    viewer.finish_client(stats, std::chrono::seconds(5));
}

void run_single(uint16_t port) {
    std::string error;
    auto backend = std::make_shared<LockBackend>();
    auto logs = std::make_shared<LogCapture>();
    std::unique_ptr<chast::HostServer> host = start_host(port, 1, backend, logs, &error);
    check(host != nullptr, "the single-viewer host starts");
    if (!host) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    const std::string fingerprint = host->status().fingerprint;
    chast::ServerInfo info;
    std::unique_ptr<chast::QuicSession> viewer =
        connect_viewer(port, fingerprint, &info, &error);
    if (!viewer || !wait_streaming(host, std::chrono::seconds(8))) {
        skip("the lock-after-session check needs a desktop stream");
        if (viewer) {
            finish_viewer(*viewer);
            viewer.reset();
        }
        host->stop();
        return;
    }
    finish_viewer(*viewer);
    viewer.reset();
    check(wait_listening(host, std::chrono::seconds(5)),
          "the host returns to listening after the first session");
    check(backend->attempt_count() == 0,
          "the default setting never locks the workstation");

    host->set_lock_after_session_end(true);
    viewer = connect_viewer(port, fingerprint, &info, &error);
    check(viewer != nullptr && wait_streaming(host, std::chrono::seconds(8)),
          "a session starts with the setting enabled");
    if (!viewer) {
        host->stop();
        return;
    }
    finish_viewer(*viewer);
    viewer.reset();
    check(wait_listening(host, std::chrono::seconds(5)) &&
              wait_for([&] { return backend->attempt_count() == 1; },
                       std::chrono::seconds(5)),
          "the enabled setting locks after the session ends");
    check(backend->success_count() == 1, "the lock reached the platform backend");

    viewer = connect_viewer(port, fingerprint, &info, &error);
    check(viewer != nullptr && wait_streaming(host, std::chrono::seconds(8)),
          "a session starts before a live cancel");
    if (!viewer) {
        host->stop();
        return;
    }
    host->set_lock_after_session_end(false);
    finish_viewer(*viewer);
    viewer.reset();
    check(wait_listening(host, std::chrono::seconds(5)), "the cancelled host returns to listening");
    check(backend->attempt_count() == 1, "cancelling while connected prevents the lock");

    host->set_lock_after_session_end(true);
    backend->fail_once();
    viewer = connect_viewer(port, fingerprint, &info, &error);
    check(viewer != nullptr && wait_streaming(host, std::chrono::seconds(8)),
          "a session starts before the failing lock");
    if (!viewer) {
        host->stop();
        return;
    }
    finish_viewer(*viewer);
    viewer.reset();
    check(wait_listening(host, std::chrono::seconds(5)) &&
              wait_for([&] { return backend->attempt_count() == 2; },
                       std::chrono::seconds(5)),
          "a failing lock is attempted once per session end");
    check(backend->success_count() == 1, "the failure is not reported as a success");
    check(logs->count("cannot lock the workstation after the session: injected lock failure") == 1,
          "the platform failure is logged with its reason");

    viewer = connect_viewer(port, fingerprint, &info, &error);
    check(viewer != nullptr && wait_streaming(host, std::chrono::seconds(8)),
          "a later session starts after the failed lock");
    if (!viewer) {
        host->stop();
        return;
    }
    finish_viewer(*viewer);
    viewer.reset();
    check(wait_listening(host, std::chrono::seconds(5)) &&
              wait_for([&] { return backend->attempt_count() == 3; },
                       std::chrono::seconds(5)),
          "the still-enabled setting locks the later session");
    check(backend->success_count() == 2, "the retry succeeds");

    const int before_stop = backend->attempt_count();
    viewer = connect_viewer(port, fingerprint, &info, &error);
    check(viewer != nullptr && wait_streaming(host, std::chrono::seconds(8)),
          "a session is connected when the host shuts down");
    host->stop();
    check(backend->attempt_count() == before_stop,
          "a graceful shutdown does not lock the workstation");
    viewer.reset();
    host.reset();
}

void run_shared(uint16_t port) {
    std::string error;
    auto backend = std::make_shared<LockBackend>();
    auto logs = std::make_shared<LogCapture>();
    std::unique_ptr<chast::HostServer> host = start_host(port, 2, backend, logs, &error);
    check(host != nullptr, "the two-viewer host starts");
    if (!host) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    const std::string fingerprint = host->status().fingerprint;
    chast::ServerInfo info;
    std::unique_ptr<chast::QuicSession> first =
        connect_viewer(port, fingerprint, &info, &error);
    if (!first || !wait_streaming(host, std::chrono::seconds(8))) {
        skip("the shared lock check needs a desktop stream");
        if (first) {
            finish_viewer(*first);
            first.reset();
        }
        host->stop();
        return;
    }
    std::unique_ptr<chast::QuicSession> second =
        connect_viewer(port, fingerprint, &info, &error);
    check(second != nullptr, "the second viewer is admitted");
    if (!second) {
        finish_viewer(*first);
        first.reset();
        host->stop();
        return;
    }
    check(wait_for([&] { return host->status().active_viewers == 2; },
                   std::chrono::seconds(5)),
          "both viewers are active");
    finish_viewer(*first);
    first.reset();
    check(wait_for([&] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "the first viewer leaves");
    finish_viewer(*second);
    second.reset();
    check(wait_for([&] { return host->status().active_viewers == 0; },
                   std::chrono::seconds(5)) &&
              wait_listening(host, std::chrono::seconds(5)),
          "the shared host drains");
    check(backend->attempt_count() == 0,
          "the default setting never locks after the last shared session");

    host->set_lock_after_session_end(true);
    first = connect_viewer(port, fingerprint, &info, &error);
    second = connect_viewer(port, fingerprint, &info, &error);
    check(first != nullptr && second != nullptr &&
              wait_for([&] { return host->status().active_viewers == 2; },
                       std::chrono::seconds(5)),
          "two viewers are active with the setting enabled");
    if (!first || !second) {
        if (first) {
            first.reset();
        }
        if (second) {
            second.reset();
        }
        host->stop();
        return;
    }
    finish_viewer(*first);
    first.reset();
    check(wait_for([&] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "the first of two viewers leaves");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(backend->attempt_count() == 0,
          "a departing viewer cannot lock out the remaining one");
    check(second->peer_alive(), "the remaining viewer stays connected");
    finish_viewer(*second);
    second.reset();
    check(wait_for([&] { return host->status().active_viewers == 0; },
                   std::chrono::seconds(5)) &&
              wait_for([&] { return backend->attempt_count() == 1; },
                       std::chrono::seconds(5)),
          "the last shared session locks exactly once");
    check(backend->success_count() == 1, "the shared-host lock reached the backend");

    first = connect_viewer(port, fingerprint, &info, &error);
    second = connect_viewer(port, fingerprint, &info, &error);
    check(first != nullptr && second != nullptr &&
              wait_for([&] { return host->status().active_viewers == 2; },
                       std::chrono::seconds(5)),
          "two viewers are active before a live cancel");
    if (first && second) {
        host->set_lock_after_session_end(false);
        finish_viewer(*first);
        first.reset();
        finish_viewer(*second);
        second.reset();
        check(wait_for([&] { return host->status().active_viewers == 0; },
                       std::chrono::seconds(5)),
              "the cancelled shared host drains");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(backend->attempt_count() == 1,
              "cancelling while connected prevents the deferred lock");
    } else {
        if (first) {
            first.reset();
        }
        if (second) {
            second.reset();
        }
        check(false, "two viewers are active before a live cancel");
    }

    host->set_lock_after_session_end(true);
    backend->fail_once();
    first = connect_viewer(port, fingerprint, &info, &error);
    second = connect_viewer(port, fingerprint, &info, &error);
    check(first != nullptr && second != nullptr &&
              wait_for([&] { return host->status().active_viewers == 2; },
                       std::chrono::seconds(5)),
          "two viewers are active before the failing lock");
    if (first && second) {
        finish_viewer(*first);
        first.reset();
        finish_viewer(*second);
        second.reset();
        check(wait_for([&] { return host->status().active_viewers == 0; },
                       std::chrono::seconds(5)) &&
                  wait_for([&] { return backend->attempt_count() == 2; },
                           std::chrono::seconds(5)),
              "the last shared session attempts the failing lock");
        check(backend->success_count() == 1, "the shared failure is not a success");
        check(logs->count("cannot lock the workstation after the session: injected lock failure") == 1,
              "the shared failure is logged with its reason");
    } else {
        if (first) {
            first.reset();
        }
        if (second) {
            second.reset();
        }
        check(false, "two viewers are active before the failing lock");
    }

    const int before_stop = backend->attempt_count();
    first = connect_viewer(port, fingerprint, &info, &error);
    second = connect_viewer(port, fingerprint, &info, &error);
    check(first != nullptr && second != nullptr &&
              wait_for([&] { return host->status().active_viewers == 2; },
                       std::chrono::seconds(5)),
          "two viewers are active when the shared host shuts down");
    host->stop();
    check(backend->attempt_count() == before_stop,
          "a graceful shared shutdown does not lock the workstation");
    first.reset();
    second.reset();
    host.reset();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("FAIL WSAStartup\n");
        return 1;
    }
    const uint16_t single_port = free_udp_port();
    const uint16_t shared_port = free_udp_port();
    check(single_port != 0 && shared_port != 0 && single_port != shared_port,
          "two loopback ports are available");
    if (single_port != 0 && shared_port != 0 && single_port != shared_port) {
        run_single(single_port);
        run_shared(shared_port);
    }
    WSACleanup();
    if (failures != 0) {
        std::printf("lock-after-session checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("lock-after-session checks passed\n");
    return 0;
}
