// Real authenticated QUIC and host loop; input injection is dry-run so this
// test never blocks the operator's keyboard or mouse. Requires a capture GPU.
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chast/host_server.h"
#include "chast/quic.h"

using namespace std::chrono_literals;

namespace {
int failures = 0;
void check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "ok  " : "FAIL", message);
    if (!value) ++failures;
}

bool await_state(chast::QuicSession& viewer, bool wanted) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        bool enabled = false;
        chast::BlockInputCode code = chast::BlockInputCode::Failed;
        std::string reason;
        if (viewer.take_block_input_state(enabled, code, reason)) {
            if (code != chast::BlockInputCode::Ok) {
                std::printf("block input response: %s\n", reason.c_str());
                return false;
            }
            if (enabled == wanted) return true;
        }
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        (void)viewer.receive_frame(header, payload, 20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void run(bool allowed, uint16_t port) {
    chast::HostServerConfig config;
    config.settings.port = port;
    config.settings.token_auth = true;
    config.settings.access_token = "local-block-input-test";
    config.settings.token_mode = chast::HostTokenMode::Reusable;
    config.settings.allow_input = true;
    config.settings.allow_block_input = allowed;
    config.settings.allow_audio = false;
    config.dry_run_input = true;
    config.serve_once = true;
    config.run_seconds = 15;
    config.log = [](const std::string& message) { std::printf("host: %s\n", message.c_str()); };
    std::string error;
    auto host = chast::HostServer::create(std::move(config), &error);
    check(host != nullptr, "host listener starts");
    if (!host) {
        std::printf("%s\n", error.c_str());
        return;
    }
    host->start();
    chast::QuicClientConfig client;
    client.port = port;
    client.access_token = "local-block-input-test";
    client.pinned_fingerprint = host->status().fingerprint;
    auto viewer = chast::QuicSession::create_client(client, &error);
    check(viewer != nullptr, "viewer starts");
    if (!viewer) return;
    auto connected = viewer->connect(5s);
    chast::ServerInfo info;
    auto handshake = connected.ok() ? viewer->handshake(info, 30s) : connected;
    check(handshake.ok(), "authenticated GPU session opens");
    if (!handshake.ok()) {
        std::printf("%s\n", handshake.to_string().c_str());
        host->stop();
        return;
    }
    check(info.block_input_enabled == allowed, "handshake reports host consent");
    if (allowed) {
        check(viewer->set_block_input(true).ok() && await_state(*viewer, true),
              "viewer enables local input block and receives acknowledgement");
        check(viewer->set_block_input(false).ok() && await_state(*viewer, false),
              "viewer restores local input");
        check(viewer->set_block_input(true).ok() && await_state(*viewer, true),
              "viewer enables local block before revocation");
        host->set_permissions(static_cast<uint16_t>(chast::kPermissionAll &
                                                    ~chast::kPermissionBlockInput));
        check(await_state(*viewer, false), "host revocation restores local input");
        check(!viewer->set_block_input(true).ok(), "revoked viewer cannot re-enable block");
        check(viewer->set_block_input(false).ok() && await_state(*viewer, false),
              "release stays available after revocation");
        host->set_permissions(chast::kPermissionAll);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        const uint16_t need = chast::kPermissionInput | chast::kPermissionBlockInput;
        while ((viewer->permissions() & need) != need &&
               std::chrono::steady_clock::now() < deadline) {
            chast::FrameHeader header;
            std::vector<uint8_t> payload;
            (void)viewer->receive_frame(header, payload, 20ms);
        }
        check(viewer->set_block_input(true).ok() && await_state(*viewer, true),
              "a fresh host grant permits blocking again");
        host->set_permissions(static_cast<uint16_t>(chast::kPermissionAll &
                                                    ~chast::kPermissionInput));
        check(await_state(*viewer, false), "revoking keyboard/mouse input also restores local input");
        check(!viewer->set_block_input(true).ok(), "block permission alone cannot enable blocking");
    } else {
        check(!viewer->set_block_input(true).ok(), "host default refuses local input block");
    }
    (void)viewer->finish_client({}, 2s);
    host->stop();
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run(true, 45524);
    run(false, 45525);
    return failures == 0 ? 0 : 1;
}
