// v62 access-ticket lease over a real HostServer session. A managed host
// admits a file-transfer viewer on a valid ticket and must close that session
// at the ticket's signed expiry even while the viewer is idle (traffic would
// otherwise keep it alive). A renewal with a fresh ticket moves the deadline;
// a renewal naming another client key is refused and ends the session; a
// renewal that drops a permission takes the capability away immediately. The
// ticket lifetimes are a few seconds, well inside the 60-second format limit,
// so the checks run quickly without a GPU.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "chast/host_server.h"
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
constexpr const char* kIssuerPublicKey = "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg";
constexpr const char* kHostID = "devbox";
constexpr const char* kPassword = "ticket-lease-password";
constexpr uint8_t kClientSeedFirst = 0x80;
constexpr uint8_t kClientSeedForeign = 0x81;
constexpr uint32_t kPolicyVersion = 2;
constexpr uint32_t kTicketPermissions = chast::kPermissionFileTransfer;

int64_t now_seconds() { return static_cast<int64_t>(std::time(nullptr)); }

uint16_t free_udp_port() {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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
        std::filesystem::path(buffer) / L"chast-host-ticket-lease-test";
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

std::vector<uint8_t> mint_ticket(const std::string& fingerprint, int64_t issued_at,
                                 int64_t expires_at, const std::string& id,
                                 uint8_t client_seed = kClientSeedFirst,
                                 uint32_t permissions = kTicketPermissions) {
    chast::AccessTicket ticket;
    ticket.issuer = kIssuer;
    ticket.audience = std::string("device:") + kHostID;
    ticket.key_id = kKid;
    ticket.host_id = kHostID;
    ticket.host_public_key_fingerprint = fingerprint;
    ticket.subject = "alice";
    ticket.permissions = permissions;
    ticket.policy_version = kPolicyVersion;
    ticket.issued_at = issued_at;
    ticket.not_before = issued_at;
    ticket.expires_at = expires_at;
    ticket.id = id;
    std::vector<uint8_t> client_key;
    if (!ticket_test::ed25519_public_key(ticket_test::sequential_seed(client_seed),
                                         &client_key)) {
        return {};
    }
    std::memcpy(ticket.client_public_key.data(), client_key.data(),
                chast::kTicketClientKeySize);
    return ticket_test::sign_ticket(ticket_test::sequential_seed(0x00),
                                    ticket_test::encode(ticket));
}

struct HostFixture {
    std::unique_ptr<chast::HostServer> server;
    std::string fingerprint;
    uint16_t port = 0;
    std::mutex log_mutex;
    std::vector<std::string> log_lines;
};

std::unique_ptr<HostFixture> start_host(const std::filesystem::path& dir,
                                        const std::filesystem::path& policy) {
    auto fixture = std::make_unique<HostFixture>();
    fixture->port = free_udp_port();
    if (fixture->port == 0) {
        return nullptr;
    }
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = fixture->port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 1;
    config.settings.allow_file_transfer = true;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(kPassword);
    config.settings.audit_spool = (dir / L"host.audit.jsonl").string();
    config.settings.file_receive_directory = (dir / L"receive").string();
    config.settings.policy_state = policy.string();
    config.settings.tunnel_id = kHostID;
    config.settings.access_issuer = kIssuer;
    config.settings.access_kid = kKid;
    config.settings.access_public_key = kIssuerPublicKey;
    config.first_client_timeout_ms = 0;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    HostFixture* raw = fixture.get();
    config.log = [raw](const std::string& line) {
        std::printf("     host: %s\n", line.c_str());
        std::lock_guard<std::mutex> lock(raw->log_mutex);
        raw->log_lines.push_back(line);
    };
    std::string error;
    fixture->server = chast::HostServer::create(std::move(config), &error);
    if (!fixture->server) {
        std::printf("host create: %s\n", error.c_str());
        return nullptr;
    }
    fixture->server->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (fixture->server->status().state == chast::HostState::Listening) {
            fixture->fingerprint = fixture->server->status().fingerprint;
            if (!fixture->fingerprint.empty()) {
                return fixture;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return nullptr;
}

bool log_contains(HostFixture& fixture, const std::string& text) {
    std::lock_guard<std::mutex> lock(fixture.log_mutex);
    for (const std::string& line : fixture.log_lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<chast::QuicSession> connect_ticket_viewer(const HostFixture& fixture,
                                                          const std::vector<uint8_t>& ticket,
                                                          uint8_t client_seed,
                                                          std::string* error) {
    chast::QuicClientConfig client;
    client.host = "127.0.0.1";
    client.port = fixture.port;
    client.idle_timeout_ms = 15000;
    client.pinned_fingerprint = fixture.fingerprint;
    client.password = kPassword;
    client.session_mode = chast::SessionMode::FileTransfer;
    client.access_ticket = ticket;
    const ticket_test::Seed seed = ticket_test::sequential_seed(client_seed);
    client.access_ticket_key = std::vector<uint8_t>(seed.begin(), seed.end());
    std::unique_ptr<chast::QuicSession> viewer =
        chast::QuicSession::create_client(client, error);
    if (!viewer) {
        return nullptr;
    }
    const chast::Error connected = viewer->connect(std::chrono::seconds(8));
    if (!connected.ok()) {
        *error = "connect: " + connected.to_string();
        return nullptr;
    }
    chast::ServerInfo info;
    const chast::Error handshaken = viewer->handshake(info, std::chrono::seconds(8));
    if (!handshaken.ok()) {
        *error = "handshake: " + handshaken.to_string();
        return nullptr;
    }
    return viewer;
}

bool wait_listening(HostFixture& fixture, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fixture.server->status().state == chast::HostState::Listening) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return fixture.server->status().state == chast::HostState::Listening;
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

    {
        std::unique_ptr<HostFixture> fixture = start_host(dir, policy);
        check(fixture != nullptr, "the managed host listens");
        if (fixture) {
            const int64_t issued = now_seconds();
            const int64_t expires = issued + 3;
            const std::vector<uint8_t> ticket =
                mint_ticket(fixture->fingerprint, issued, expires, "jti-lease-1");
            check(!ticket.empty(), "the lease ticket is minted");

            std::string error;
            std::unique_ptr<chast::QuicSession> viewer =
                connect_ticket_viewer(*fixture, ticket, kClientSeedFirst, &error);
            check(viewer != nullptr, "the host admits the ticket and possession proof");
            if (!viewer) {
                std::printf("     client: %s\n", error.c_str());
            } else {
                // The lease must outlive the first moments of the session and
                // then end on its own, without the viewer sending anything.
                const auto connected_at = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                check(viewer->peer_alive(), "the session is alive before the ticket expires");

                bool closed = false;
                while (std::chrono::steady_clock::now() - connected_at < std::chrono::seconds(10)) {
                    if (!viewer->peer_alive()) {
                        closed = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - connected_at);
                check(closed, "the host closes the session at the ticket expiry");
                check(elapsed >= std::chrono::milliseconds(2000) &&
                          elapsed <= std::chrono::seconds(8),
                      "the session ends near the signed deadline, not before it");
                check(log_contains(*fixture, "access ticket expired"),
                      "the host names the ticket expiry as the reason");

                chast::ClientStats close_stats;
                viewer->finish_client(close_stats, std::chrono::seconds(1));
                check(wait_listening(*fixture, std::chrono::seconds(5)),
                      "the host returns to listening after the lease ends");
            }

            // Renewal: a fresh ticket for the same key moves the deadline to
            // its own expiry. The first ticket ends at +3 s; after a renewal at
            // +1.2 s with a +6 s ticket the session must still be alive at
            // +4 s and end only near +7 s.
            {
                const int64_t issued2 = now_seconds();
                const std::vector<uint8_t> ticket2 =
                    mint_ticket(fixture->fingerprint, issued2, issued2 + 3, "jti-renew-1");
                std::string renew_error;
                std::unique_ptr<chast::QuicSession> renewed_viewer =
                    connect_ticket_viewer(*fixture, ticket2, kClientSeedFirst, &renew_error);
                check(renewed_viewer != nullptr, "the renewal viewer is admitted");
                if (!renewed_viewer) {
                    std::printf("     client: %s\n", renew_error.c_str());
                } else {
                    const auto connected_at = std::chrono::steady_clock::now();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                    const int64_t renewed_at = now_seconds();
                    const std::vector<uint8_t> replacement =
                        mint_ticket(fixture->fingerprint, renewed_at, renewed_at + 6,
                                    "jti-renew-2");
                    const chast::Error renewal = renewed_viewer->renew_access_ticket(
                        replacement, std::chrono::seconds(5));
                    check(renewal.ok(), "the host accepts a fresh ticket and extends the lease");
                    if (!renewal.ok()) {
                        std::printf("     renewal: %s\n", renewal.to_string().c_str());
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
                    check(renewed_viewer->peer_alive(),
                          "the session outlives the first ticket's deadline");
                    bool renewed_closed = false;
                    while (std::chrono::steady_clock::now() - connected_at <
                           std::chrono::seconds(12)) {
                        if (!renewed_viewer->peer_alive()) {
                            renewed_closed = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    const auto renewed_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - connected_at);
                    check(renewed_closed, "the renewed lease still ends at its own deadline");
                    check(renewed_elapsed >= std::chrono::milliseconds(5500) &&
                              renewed_elapsed <= std::chrono::seconds(10),
                          "the session ends near the renewed deadline, not the first one");
                    check(log_contains(*fixture, "access ticket was renewed"),
                          "the host records the renewal");
                    chast::ClientStats close_stats;
                    renewed_viewer->finish_client(close_stats, std::chrono::seconds(1));
                    wait_listening(*fixture, std::chrono::seconds(5));
                }
            }

            // A renewal naming another client key is refused and ends the
            // session, even though the foreign ticket itself verifies.
            {
                const int64_t issued3 = now_seconds();
                const std::vector<uint8_t> ticket3 =
                    mint_ticket(fixture->fingerprint, issued3, issued3 + 20, "jti-renew-3");
                std::string refuse_error;
                std::unique_ptr<chast::QuicSession> refused_viewer =
                    connect_ticket_viewer(*fixture, ticket3, kClientSeedFirst, &refuse_error);
                check(refused_viewer != nullptr, "the refusal viewer is admitted");
                if (!refused_viewer) {
                    std::printf("     client: %s\n", refuse_error.c_str());
                } else {
                    const std::vector<uint8_t> foreign =
                        mint_ticket(fixture->fingerprint, issued3, issued3 + 20,
                                    "jti-renew-4", kClientSeedForeign);
                    const chast::Error renewal = refused_viewer->renew_access_ticket(
                        foreign, std::chrono::seconds(5));
                    check(renewal.status() == chast::Status::PermissionDenied,
                          "a renewal for another client key is refused");
                    check(log_contains(*fixture, "ticket renewal was refused"),
                          "the host records the refused renewal");
                    bool closed = false;
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (std::chrono::steady_clock::now() < deadline) {
                        if (!refused_viewer->peer_alive()) {
                            closed = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    check(closed, "the host closes a session whose renewal was refused");
                    chast::ClientStats close_stats;
                    refused_viewer->finish_client(close_stats, std::chrono::seconds(1));
                }
            }

            wait_listening(*fixture, std::chrono::seconds(5));

            // Narrowing: a renewal whose ticket no longer carries file transfer
            // reaches the live session as a PermissionUpdate, and a new file
            // operation is refused instead of waiting for the lease to end.
            {
                const int64_t issued4 = now_seconds();
                const std::vector<uint8_t> ticket4 =
                    mint_ticket(fixture->fingerprint, issued4, issued4 + 20, "jti-narrow-1");
                std::string narrow_error;
                std::unique_ptr<chast::QuicSession> narrow_viewer =
                    connect_ticket_viewer(*fixture, ticket4, kClientSeedFirst, &narrow_error);
                check(narrow_viewer != nullptr, "the narrowing viewer is admitted");
                if (!narrow_viewer) {
                    std::printf("     client: %s\n", narrow_error.c_str());
                } else {
                    check((narrow_viewer->permissions() & chast::kPermissionFileTransfer) != 0,
                          "the admitted viewer holds file transfer");
                    check(narrow_viewer->request_file_list(9, "").ok(),
                          "the authorized file listing is accepted");
                    const int64_t narrowed_at = now_seconds();
                    const std::vector<uint8_t> narrowed_ticket =
                        mint_ticket(fixture->fingerprint, narrowed_at, narrowed_at + 20,
                                    "jti-narrow-2", kClientSeedFirst, 0);
                    const chast::Error renewal = narrow_viewer->renew_access_ticket(
                        narrowed_ticket, std::chrono::seconds(5));
                    check(renewal.ok(), "the host accepts a ticket that only narrows the mask");
                    bool told = false;
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (std::chrono::steady_clock::now() < deadline) {
                        chast::PermissionMask update = 0;
                        while (narrow_viewer->take_permission_update(update)) {
                        }
                        if ((narrow_viewer->permissions() & chast::kPermissionFileTransfer) == 0) {
                            told = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    check(told, "the viewer is told the file-transfer grant was removed");
                    check(narrow_viewer->request_file_list(10, "").status() ==
                              chast::Status::PermissionDenied,
                          "a file listing is refused once the grant is gone");
                    check(narrow_viewer->peer_alive(),
                          "the narrowed session stays connected until its own deadline");
                    chast::ClientStats narrow_stats;
                    narrow_viewer->finish_client(narrow_stats, std::chrono::seconds(1));
                }
            }
        }
        if (fixture) {
            fixture->server->stop();
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    WSACleanup();
    std::printf("host ticket lease checks %s\n", failures == 0 ? "passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
