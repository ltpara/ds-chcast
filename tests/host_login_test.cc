// Checks for OS login (protocol v39): the viewer sends the host's password and
// the host types it at its logon or lock screen. A loopback viewer sends the
// password, the host asks a synthetic backend for its desktop state and to type
// the text (no real desktop change), and the check follows the answer, the
// counters and the password the backend received. Needs an interactive desktop
// because the stream lives inside a desktop session, so it skips itself when no
// desktop exists.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
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
    static const std::string value = "host-login-password";
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
        std::filesystem::path(buffer) / L"chast-host-login-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Stands in for the session state probe and the injector: records every typed
// password, lets the probe report any state, and lets one injection fail.
struct LoginState {
    std::mutex mutex;
    chast::OsLoginState state = chast::OsLoginState::Locked;
    std::string state_error;
    std::vector<std::string> typed;
    std::atomic<bool> fail_next{false};

    std::function<chast::OsLoginStatus()> probe() {
        return [this] {
            std::lock_guard<std::mutex> lock(mutex);
            chast::OsLoginStatus status;
            status.state = state;
            status.error = state_error;
            return status;
        };
    }

    std::function<bool(const std::string&, std::string*)> hook() {
        return [this](const std::string& candidate, std::string* error) {
            std::lock_guard<std::mutex> lock(mutex);
            typed.push_back(candidate);
            if (fail_next.exchange(false)) {
                *error = "synthetic injection failure";
                return false;
            }
            return true;
        };
    }

    void set(chast::OsLoginState next, std::string error_text = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        state = next;
        state_error = std::move(error_text);
    }

    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return typed.size();
    }

    std::string last() {
        std::lock_guard<std::mutex> lock(mutex);
        return typed.empty() ? std::string() : typed.back();
    }
};

std::unique_ptr<chast::HostServer> start_host(uint16_t port, const std::filesystem::path& dir,
                                              const std::shared_ptr<LoginState>& login,
                                              std::string* error) {
    chast::HostServerConfig config;
    use_empty_clipboard(config);
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 1;
    config.settings.allow_input = true;
    config.settings.allow_audio = false;
    config.settings.allow_microphone = false;
    config.settings.allow_os_login = true;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(password());
    config.settings.audit_spool = (dir / L"login.audit.jsonl").string();
    config.settings.file_receive_directory = (dir / L"receive").string();
    config.first_client_timeout_ms = 0;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    config.log = [](const std::string& line) { std::printf("     host: %s\n", line.c_str()); };
    config.os_login_state = login->probe();
    config.os_login = login->hook();
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

bool take_answer(chast::QuicSession& session, chast::OsLoginCode& code, std::string& reason,
                 std::chrono::milliseconds timeout) {
    return wait_for(
        [&] {
            chast::OsLoginCode next_code = chast::OsLoginCode::Ok;
            std::string next_reason;
            bool seen = false;
            while (session.take_os_login_result(next_code, next_reason)) {
                code = next_code;
                reason = std::move(next_reason);
                seen = true;
            }
            return seen;
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
    auto login = std::make_shared<LoginState>();
    std::unique_ptr<chast::HostServer> host =
        port == 0 ? nullptr : start_host(port, dir, login, &error);
    check(host != nullptr, "the OS-login host starts");
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
                skip("the OS-login check needs a desktop stream");
            } else {
                const chast::PermissionMask permissions = viewer->permissions();
                check((permissions & chast::kPermissionInput) != 0,
                      "the host offers the input permission");
                check((permissions & chast::kPermissionOsLogin) != 0,
                      "the host offers the OS-login permission");
                check(info.os_login_enabled,
                      "HelloAck announces OS login");

                chast::OsLoginCode code = chast::OsLoginCode::Denied;
                std::string reason;
                const std::string first = "s3cret \xE5\xAF\x86\xE7\xA0\x81";
                check(viewer->request_os_login(first).ok(),
                      "the viewer queues an OS password");
                check(take_answer(*viewer, code, reason, std::chrono::seconds(5)) &&
                          code == chast::OsLoginCode::Ok && reason.empty(),
                      "a lock-screen password is acknowledged");
                check(login->count() == 1 && login->last() == first,
                      "the backend received the exact password");
                check(wait_for(
                          [&host] {
                              return host->status().stats.os_login_requests == 1 &&
                                     host->status().stats.os_login_failures == 0;
                          },
                          std::chrono::seconds(5)),
                      "the host counts the accepted request");

                login->set(chast::OsLoginState::Logon);
                check(viewer->request_os_login("logon-pass").ok(),
                      "the viewer queues a password at the logon screen");
                check(take_answer(*viewer, code, reason, std::chrono::seconds(5)) &&
                          code == chast::OsLoginCode::Ok,
                      "a logon-screen password is acknowledged");
                check(login->count() == 2 && login->last() == "logon-pass",
                      "the logon-screen password reached the backend");

                login->set(chast::OsLoginState::Unlocked);
                check(viewer->request_os_login("not-typed").ok(),
                      "the viewer queues a password at an unlocked desktop");
                check(take_answer(*viewer, code, reason, std::chrono::seconds(5)) &&
                          code == chast::OsLoginCode::Unsupported &&
                          reason == "the host is not at a logon or lock screen",
                      "an unlocked desktop refuses the password without typing it");
                check(login->count() == 2,
                      "the unlocked desktop left the backend untouched");

                login->set(chast::OsLoginState::Unavailable, "no input desktop");
                check(viewer->request_os_login("still-not-typed").ok(),
                      "the viewer queues a password without a logon desktop");
                check(take_answer(*viewer, code, reason, std::chrono::seconds(5)) &&
                          code == chast::OsLoginCode::Unsupported &&
                          reason == "no input desktop",
                      "an unavailable desktop explains itself");
                check(login->count() == 2,
                      "the unavailable desktop left the backend untouched");

                login->set(chast::OsLoginState::Locked);
                login->fail_next.store(true);
                check(viewer->request_os_login("failing-pass").ok(),
                      "the viewer queues a password the backend cannot type");
                check(take_answer(*viewer, code, reason, std::chrono::seconds(5)) &&
                          code == chast::OsLoginCode::Failed &&
                          reason == "synthetic injection failure",
                      "an injection failure is reported with its reason");
                check(wait_for(
                          [&host] {
                              return host->status().stats.os_login_requests == 5 &&
                                     host->status().stats.os_login_failures == 3;
                          },
                          std::chrono::seconds(5)),
                      "the host counts every refusal");

                const chast::Error blank = viewer->request_os_login({});
                check(!blank.ok() && blank.status() == chast::Status::InvalidCall,
                      "an empty password never reaches the transport");
                const chast::Error multiline = viewer->request_os_login("two\nlines");
                check(!multiline.ok() && multiline.status() == chast::Status::InvalidCall,
                      "a password with a line break never reaches the transport");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                check(host->status().stats.os_login_requests == 5,
                      "a local rejection adds no host request");

                host->set_permissions(chast::kPermissionAll & ~chast::kPermissionOsLogin);
                check(wait_for(
                          [&viewer] {
                              return (viewer->permissions() & chast::kPermissionOsLogin) == 0;
                          },
                          std::chrono::seconds(5)),
                      "the viewer learns that OS login was revoked");
                const chast::Error revoked = viewer->request_os_login("revoked-pass");
                check(!revoked.ok() && revoked.status() == chast::Status::PermissionDenied,
                      "a revoked OS-login grant refuses the password locally");
                check(login->count() == 3,
                      "a revoked grant adds no backend call");
            }
            chast::ClientStats stats;
            viewer->finish_client(stats, std::chrono::seconds(5));
            viewer.reset();
        }
        host->stop();
        host.reset();
    }
    WSACleanup();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all OS-login checks passed\n");
    return 0;
}
