// Blocking the operator's local input over the v38 host-action channel.
//
// Two loopback hosts run the same checks: one has allow_block_input on and one
// leaves it off. The host relays every block/release to a synthetic backend
// (BlockInput is never called for real), so the test cannot lock the
// developer's keyboard or mouse. The allowed host checks the handshake
// consent, the idempotent toggle and the automatic release when either the
// input or the block-input permission is revoked; the refusing host checks
// that the viewer never sends a block at all. Needs a desktop session, so it
// skips itself when no interactive desktop exists.

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

const std::string& access_token() {
    static const std::string value = "local-block-input-test";
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

// Stands in for BlockInput: records every call so the test can prove that a
// repeated request is ignored and that a revocation really released the
// operator's input.
struct BlockBackend {
    std::mutex mutex;
    std::vector<bool> calls;

    std::function<bool(chast::HostAction, bool, std::string*)> hook() {
        return [this](chast::HostAction action, bool block_input, std::string*) {
            if (action != chast::HostAction::BlockInput) {
                return true;
            }
            std::lock_guard<std::mutex> lock(mutex);
            calls.push_back(block_input);
            return true;
        };
    }

    size_t count(bool blocked) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t total = 0;
        for (const bool call : calls) {
            if (call == blocked) {
                ++total;
            }
        }
        return total;
    }
};

std::unique_ptr<chast::HostServer> start_host(uint16_t port, bool allow_block_input,
                                              const std::shared_ptr<BlockBackend>& backend,
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
    config.settings.allow_block_input = allow_block_input;
    config.settings.token_auth = true;
    config.settings.access_token = access_token();
    config.settings.token_mode = chast::HostTokenMode::Reusable;
    config.dry_run_input = true;
    config.first_client_timeout_ms = 0;
    config.finish_timeout_ms = 200;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    config.host_control = backend->hook();
    config.log = [](const std::string& line) { std::printf("     host: %s\n", line.c_str()); };
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

bool take_action(chast::QuicSession& session, chast::HostControlResult& result,
                 std::chrono::milliseconds timeout) {
    return wait_for(
        [&] {
            chast::HostControlResult next;
            while (session.take_host_action_result(next)) {
                if (next.action == chast::HostAction::BlockInput) {
                    result = next;
                    return true;
                }
            }
            return false;
        },
        timeout);
}

bool wait_permissions(chast::QuicSession& session, chast::PermissionMask mask,
                      std::chrono::milliseconds timeout) {
    return wait_for([&] { return (session.permissions() & mask) == mask; }, timeout);
}

void run_allowed(uint16_t port) {
    std::string error;
    auto backend = std::make_shared<BlockBackend>();
    std::unique_ptr<chast::HostServer> host = start_host(port, true, backend, &error);
    check(host != nullptr, "the consenting host starts");
    if (!host) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    chast::ServerInfo info;
    std::unique_ptr<chast::QuicSession> viewer =
        connect_viewer(port, host->status().fingerprint, &info, &error);
    check(viewer != nullptr, "the desktop viewer is admitted");
    if (!viewer) {
        std::printf("     %s\n", error.c_str());
        host->stop();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (host->status().state != chast::HostState::Streaming) {
        skip("the block-input check needs a desktop stream");
        viewer->finish_client({}, std::chrono::seconds(5));
        host->stop();
        return;
    }
    check(info.block_input_enabled, "the handshake reports the host's consent");
    const chast::PermissionMask both = chast::kPermissionInput | chast::kPermissionBlockInput;
    check(wait_permissions(*viewer, both, std::chrono::seconds(5)),
          "the viewer is granted input and block-input");

    chast::HostControlResult result;
    check(viewer->request_host_action(chast::HostAction::BlockInput, true).ok(),
          "the viewer blocks local input");
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Ok && result.block_input,
          "the block is acknowledged with its state");
    check(wait_for([&] { return backend->count(true) == 1; }, std::chrono::seconds(5)),
          "the block reached the platform backend");
    check(wait_for([&] { return host->status().stats.block_input_active; },
                   std::chrono::seconds(5)),
          "the host reports local input as blocked");

    check(viewer->request_host_action(chast::HostAction::BlockInput, true).ok(),
          "the viewer repeats the same block");
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Ok && result.block_input,
          "the repeated block is acknowledged");
    check(backend->count(true) == 1, "the backend is not called twice for the same state");

    check(viewer->request_host_action(chast::HostAction::BlockInput, false).ok(),
          "the viewer releases local input");
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Ok && !result.block_input,
          "the release is acknowledged");
    check(wait_for([&] { return backend->count(false) == 1; }, std::chrono::seconds(5)),
          "the release reached the platform backend");

    check(viewer->request_host_action(chast::HostAction::BlockInput, true).ok(),
          "the viewer blocks local input before the revocation");
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Ok && result.block_input,
          "the block before the revocation is acknowledged");
    check(wait_for([&] { return host->status().stats.host_actions == 4; },
                   std::chrono::seconds(5)),
          "the host counted four actions");

    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionInput);
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Denied && !result.block_input &&
              result.reason == "the host released local input",
          "revoking input releases the block and tells the viewer");
    check(wait_for([&] { return backend->count(false) == 2; }, std::chrono::seconds(5)),
          "the revocation released the backend block");
    check(wait_for([&] { return (viewer->permissions() & both) != both; },
                   std::chrono::seconds(5)),
          "the viewer learns that the permissions were revoked");
    const chast::Error refused =
        viewer->request_host_action(chast::HostAction::BlockInput, true);
    check(!refused.ok() && refused.status() == chast::Status::PermissionDenied,
          "the viewer refuses to queue a revoked block");
    check(host->status().stats.host_actions == 4,
          "a locally refused block never reaches the host");

    host->set_permissions(chast::kPermissionAll);
    check(wait_permissions(*viewer, both, std::chrono::seconds(5)),
          "the restored permissions reach the viewer");
    check(viewer->request_host_action(chast::HostAction::BlockInput, true).ok(),
          "the viewer blocks local input again");
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Ok && result.block_input,
          "the restored block is acknowledged");
    check(wait_for([&] { return backend->count(true) == 3; }, std::chrono::seconds(5)),
          "the restored block reached the platform backend");

    host->set_permissions(chast::kPermissionAll & ~chast::kPermissionBlockInput);
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Denied && !result.block_input &&
              result.reason == "the host released local input",
          "revoking only block-input also releases the block");
    check(wait_for([&] { return backend->count(false) == 3; }, std::chrono::seconds(5)),
          "the second revocation released the backend block");
    check(wait_for(
              [&] { return (viewer->permissions() & chast::kPermissionBlockInput) == 0; },
              std::chrono::seconds(5)),
          "the viewer loses the block-input permission");
    check(!viewer->request_host_action(chast::HostAction::BlockInput, true).ok(),
          "a viewer without block-input cannot block");

    host->set_permissions(chast::kPermissionAll);
    check(wait_permissions(*viewer, both, std::chrono::seconds(5)),
          "the permissions are restored again");
    check(viewer->request_host_action(chast::HostAction::BlockInput, true).ok(),
          "the viewer blocks local input once more");
    check(take_action(*viewer, result, std::chrono::seconds(5)) &&
              result.code == chast::HostActionCode::Ok && result.block_input,
          "the final block is acknowledged");

    chast::ClientStats stats;
    viewer->finish_client(stats, std::chrono::seconds(5));
    viewer.reset();
    check(wait_for([&] { return backend->count(false) == 4; }, std::chrono::seconds(10)),
          "ending the session releases local input");
    host->stop();
}

void run_refused(uint16_t port) {
    std::string error;
    auto backend = std::make_shared<BlockBackend>();
    std::unique_ptr<chast::HostServer> host = start_host(port, false, backend, &error);
    check(host != nullptr, "the refusing host starts");
    if (!host) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    chast::ServerInfo info;
    std::unique_ptr<chast::QuicSession> viewer =
        connect_viewer(port, host->status().fingerprint, &info, &error);
    check(viewer != nullptr, "the viewer is admitted without block-input consent");
    if (!viewer) {
        std::printf("     %s\n", error.c_str());
        host->stop();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (host->status().state != chast::HostState::Streaming) {
        skip("the block-input refusal needs a desktop stream");
        viewer->finish_client({}, std::chrono::seconds(5));
        host->stop();
        return;
    }
    check(!info.block_input_enabled, "the handshake reports that the host declined");
    check(wait_permissions(*viewer, chast::kPermissionInput, std::chrono::seconds(5)),
          "the viewer still holds the input permission");
    check((viewer->permissions() & chast::kPermissionBlockInput) == 0,
          "the viewer never receives the block-input permission");
    const chast::Error blocked =
        viewer->request_host_action(chast::HostAction::BlockInput, true);
    check(!blocked.ok() && blocked.status() == chast::Status::PermissionDenied,
          "the viewer refuses to block without the host's consent");
    const chast::Error released =
        viewer->request_host_action(chast::HostAction::BlockInput, false);
    check(!released.ok() && released.status() == chast::Status::PermissionDenied,
          "the viewer also refuses to queue a release");
    check(backend->count(true) == 0 && backend->count(false) == 0,
          "the backend was never called");
    check(host->status().stats.host_actions == 0,
          "no block request reached the host");

    viewer->finish_client({}, std::chrono::seconds(5));
    viewer.reset();
    host->stop();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("FAIL WSAStartup\n");
        return 1;
    }
    const uint16_t allowed_port = free_udp_port();
    const uint16_t refused_port = free_udp_port();
    check(allowed_port != 0 && refused_port != 0 && allowed_port != refused_port,
          "two loopback ports are available");
    if (allowed_port != 0 && refused_port != 0 && allowed_port != refused_port) {
        run_allowed(allowed_port);
        run_refused(refused_port);
    }
    WSACleanup();
    if (failures != 0) {
        std::printf("block-input checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("block-input checks passed\n");
    return 0;
}
