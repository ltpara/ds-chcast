// External, files-only viewer for the opt-in Windows process acceptance test.
// Uses the production QUIC handshake, never captures a desktop or injects input.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "chast/access_ticket.h"
#include "chast/quic.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: chast-live-viewer-probe PORT FINGERPRINT OUTPUT_DIRECTORY\n"
                             "The isolated test access token is read from stdin.\n");
        return 2;
    }
    const int port = std::atoi(argv[1]);
    if (port < 1 || port > 65535) return 2;
    const std::filesystem::path output(argv[3]);
    if (!std::filesystem::is_directory(output)) return 2;
    chast::QuicClientConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<uint16_t>(port);
    config.pinned_fingerprint = argv[2];
    config.session_mode = chast::SessionMode::FileTransfer;
    config.requested_permissions = chast::kPermissionFileTransfer;
    // QUIC sends keepalives every five seconds. Bound crash detection for
    // this probe without changing the product viewer's timeout.
    config.idle_timeout_ms = 15000;
    std::getline(std::cin, config.access_token);
    std::string encoded_ticket;
    std::string encoded_ticket_key;
    std::getline(std::cin, encoded_ticket);
    std::getline(std::cin, encoded_ticket_key);
    if (!encoded_ticket.empty() &&
        !chast::decode_ticket_base64url(encoded_ticket, &config.access_ticket)) {
        std::fprintf(stderr, "invalid access ticket encoding\n");
        return 2;
    }
    if (!encoded_ticket_key.empty() &&
        !chast::decode_ticket_base64url(encoded_ticket_key, &config.access_ticket_key)) {
        std::fprintf(stderr, "invalid access-ticket key encoding\n");
        return 2;
    }
    std::string error;
    auto session = chast::QuicSession::create_client(config, &error);
    if (!session) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    auto result = session->connect(std::chrono::seconds(10));
    if (!result.ok()) { std::fprintf(stderr, "%s\n", result.to_string().c_str()); return 1; }
    chast::ServerInfo info;
    result = session->handshake(info, std::chrono::seconds(10));
    if (!result.ok()) { std::fprintf(stderr, "%s\n", result.to_string().c_str()); return 1; }
    if (info.input_enabled) return 1;
    std::ofstream(output / "connected") << "files-only; certificate pinned; input disabled\n";
    std::puts("connected: real QUIC files-only session");
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!session->peer_alive()) {
            std::ofstream(output / "disconnected") << "transport peer no longer alive\n";
            std::puts("disconnected: transport peer no longer alive");
            return 0;
        }
        if (std::filesystem::exists(output / "stop")) {
            chast::ClientStats stats;
            return session->finish_client(stats, std::chrono::seconds(5)).ok() ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "probe deadline exceeded\n");
    return 1;
}
