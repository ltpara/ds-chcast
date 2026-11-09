// v62 managed admission over loopback QUIC. A managed HostSecurity policy
// admits a viewer only when Hello carries a valid access ticket and the
// possession proof over the host's per-connection challenge; the same host
// refuses a Hello with no ticket or with a proof from another client key.
// FileTransfer sessions carry no video stream, so no GPU or desktop is used.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chast/host_security.h"
#include "chast/quic.h"
#include "chast/security.h"

#include "access_ticket_vector.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

constexpr const char* kIssuer = "chast.example";
constexpr const char* kKid = "kid-2026-11";
// The public half of ticket_test::sequential_seed(0x00), base64url without
// padding. Same issuer pin the access-ticket tests use.
constexpr const char* kIssuerPublicKey = "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg";
constexpr const char* kHostID = "devbox";
constexpr const char* kPassword = "managed-test-password";
constexpr uint8_t kTicketClientSeedFirst = 0x80;
constexpr uint32_t kPolicyVersion = 2;
constexpr uint32_t kTicketPermissions =
    chast::kPermissionInput | chast::kPermissionClipboard | chast::kPermissionFileTransfer;

int64_t now_seconds() { return static_cast<int64_t>(std::time(nullptr)); }

ticket_test::Seed issuer_seed() { return ticket_test::sequential_seed(0x00); }
ticket_test::Seed client_seed() { return ticket_test::sequential_seed(kTicketClientSeedFirst); }

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

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    const std::filesystem::path dir =
        std::filesystem::path(buffer) / L"chast-managed-access-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path write_policy(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / L"host.policy.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{\n  \"managed\": true,\n  \"mode\": \"restricted\",\n"
        << "  \"permission_ceiling\": " << chast::kPermissionAll << ",\n"
        << "  \"policy_version\": " << kPolicyVersion << "\n}\n";
    return path;
}

std::vector<uint8_t> mint_ticket(const std::string& host_fingerprint, const std::string& id,
                                 int64_t now) {
    chast::AccessTicket ticket;
    ticket.issuer = kIssuer;
    ticket.audience = std::string("device:") + kHostID;
    ticket.key_id = kKid;
    ticket.host_id = kHostID;
    ticket.host_public_key_fingerprint = host_fingerprint;
    ticket.subject = "alice";
    ticket.permissions = kTicketPermissions;
    ticket.policy_version = kPolicyVersion;
    ticket.issued_at = now - 1;
    ticket.not_before = now - 1;
    ticket.expires_at = now + 50;
    ticket.id = id;
    std::vector<uint8_t> client_key;
    if (!ticket_test::ed25519_public_key(client_seed(), &client_key)) {
        return {};
    }
    std::memcpy(ticket.client_public_key.data(), client_key.data(),
                chast::kTicketClientKeySize);
    return ticket_test::sign_ticket(issuer_seed(), ticket_test::encode(ticket));
}

struct TestServer {
    std::unique_ptr<chast::QuicSession> session;
    std::shared_ptr<chast::HostSecurity> security;
    std::string fingerprint;
    uint16_t port = 0;
};

std::unique_ptr<TestServer> start_server(const std::string& policy_path, std::string* error) {
    auto server = std::make_unique<TestServer>();
    server->port = free_udp_port();
    if (server->port == 0) {
        *error = "no free loopback port";
        return nullptr;
    }
    chast::HostSecurity::Config config;
    config.mode = chast::HostAuthMode::Password;
    // A verified ticket is an additional gate: local password policy still
    // applies afterwards, so the host carries one and the viewer presents it.
    config.password_hash = chast::hash_password(kPassword);
    config.default_permissions = chast::kPermissionAll;
    config.policy_state = policy_path;
    config.device_id = kHostID;
    config.access_issuer = kIssuer;
    config.access_kid = kKid;
    config.access_public_key = kIssuerPublicKey;
    server->security = std::make_shared<chast::HostSecurity>(std::move(config));

    chast::QuicServerConfig quic;
    quic.bind_address = "127.0.0.1";
    quic.port = server->port;
    quic.idle_timeout_ms = 10000;
    quic.hello_timeout_ms = 5000;
    quic.stop_after_denial = true;
    quic.authenticate = [state = server->security](const chast::ClientRequest& request) {
        chast::HostSecurity::TicketRequest ticket;
        chast::HostSecurity::TicketRequest* ticket_request = nullptr;
        if (!request.hello.access_ticket.empty()) {
            ticket.signed_ticket = request.hello.access_ticket;
            ticket.host_nonce = request.host_nonce;
            ticket.proof = request.hello.ticket_proof;
            ticket_request = &ticket;
        }
        const chast::HostSecurity::Result result =
            state->evaluate(request.hello, request.peer_address, now_seconds(),
                            ticket_request);
        chast::ClientAuthResult auth;
        auth.permissions = result.permissions;
        if (result.decision == chast::HostSecurity::Decision::Allow) {
            auth.action = chast::ClientAuthAction::Allow;
        } else {
            auth.action = chast::ClientAuthAction::Deny;
            auth.denial = result.denial;
            auth.reason = result.reason;
        }
        return auth;
    };
    server->session = chast::QuicSession::create_server(quic, error);
    if (!server->session) {
        return nullptr;
    }
    server->fingerprint = server->session->server_fingerprint();
    // Tickets name the certificate digest of the host they were issued for.
    chast::HostSecurity::Config bound = server->security->config();
    bound.host_fingerprint = server->fingerprint;
    server->security->set_config(std::move(bound));
    return server;
}

struct ViewerResult {
    bool handshaken = false;
    std::string detail;
};

ViewerResult run_viewer(TestServer& server, const std::vector<uint8_t>& ticket,
                        const std::vector<uint8_t>& client_key) {
    ViewerResult result;
    std::thread host_thread([&server] {
        if (!server.session->wait_for_client(std::chrono::seconds(8)).ok()) {
            return;
        }
        chast::ServerInfo info;
        info.width = 64;
        info.height = 64;
        info.source_width = 64;
        info.source_height = 64;
        info.framerate = 30;
        info.bitrate = 100000;
        info.encoder = "probe";
        server.session->announce(info);
    });

    std::string error;
    chast::QuicClientConfig config;
    config.host = "127.0.0.1";
    config.port = server.port;
    config.idle_timeout_ms = 8000;
    config.pinned_fingerprint = server.fingerprint;
    config.session_mode = chast::SessionMode::FileTransfer;
    config.password = kPassword;
    config.access_ticket = ticket;
    config.access_ticket_key = client_key;
    std::unique_ptr<chast::QuicSession> viewer =
        chast::QuicSession::create_client(config, &error);
    if (!viewer) {
        result.detail = "client: " + error;
    } else {
        chast::ServerInfo info;
        const chast::Error connected = viewer->connect(std::chrono::seconds(8));
        const chast::Error handshaken =
            connected.ok() ? viewer->handshake(info, std::chrono::seconds(8)) : connected;
        result.handshaken = handshaken.ok();
        if (!handshaken.ok()) {
            result.detail = handshaken.to_string();
        }
        viewer.reset();
    }
    host_thread.join();
    return result;
}

std::vector<uint8_t> seed_bytes(const ticket_test::Seed& seed) {
    return std::vector<uint8_t>(seed.begin(), seed.end());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::printf("FAIL WSAStartup failed\n");
        return 1;
    }
    const std::filesystem::path dir = temp_dir();
    const std::filesystem::path policy = write_policy(dir);
    const int64_t now = now_seconds();

    {
        std::string error;
        std::unique_ptr<TestServer> server = start_server(policy.string(), &error);
        if (!server) {
            std::printf("FAIL cannot start the managed listener: %s\n", error.c_str());
            return 1;
        }
        const std::vector<uint8_t> ticket =
            mint_ticket(server->fingerprint, "jti-managed-1", now);
        check(!ticket.empty(), "the managed ticket is minted");
        const ViewerResult result = run_viewer(*server, ticket, seed_bytes(client_seed()));
        check(result.handshaken, "a managed host admits a valid ticket and possession proof");
        if (!result.handshaken) {
            std::printf("     %s\n", result.detail.c_str());
        }
    }

    {
        std::string error;
        std::unique_ptr<TestServer> server = start_server(policy.string(), &error);
        if (!server) {
            std::printf("FAIL cannot start the managed listener: %s\n", error.c_str());
            return 1;
        }
        const ViewerResult result = run_viewer(*server, {}, seed_bytes(client_seed()));
        check(!result.handshaken && result.detail.find("ticket") != std::string::npos,
              "a managed host refuses a Hello without an access ticket");
    }

    {
        std::string error;
        std::unique_ptr<TestServer> server = start_server(policy.string(), &error);
        if (!server) {
            std::printf("FAIL cannot start the managed listener: %s\n", error.c_str());
            return 1;
        }
        const std::vector<uint8_t> ticket =
            mint_ticket(server->fingerprint, "jti-managed-2", now);
        // The ticket names sequential_seed(0x80); a proof signed by another
        // key must not be accepted even though the ticket itself verifies.
        const ViewerResult result =
            run_viewer(*server, ticket, seed_bytes(ticket_test::sequential_seed(0x90)));
        check(!result.handshaken && result.detail.find("possession proof") != std::string::npos,
              "a proof from a key the ticket does not name is refused");
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    WSACleanup();
    std::printf("managed access checks %s\n", failures == 0 ? "passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
