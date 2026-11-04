// Checks for the multi-viewer host: two viewers admitted at once, one leaving
// without disturbing the other, the connection cap and its slot release, the
// per-session audit records (including two viewers that present the same
// reference), and the single control lease. The file-transfer session keeps
// the concurrency cases off the capture path; only the control-lease case
// opens a desktop stream and skips itself when no interactive desktop exists.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chast/host_server.h"
#include "chast/security.h"

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
    static const std::string value = "multi-viewer-password";
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
        std::filesystem::path(buffer) / L"chast-host-multi-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::string audit_ref_of(const std::string& record) {
    const std::string key = "\"audit_ref\":\"";
    const size_t start = record.find(key);
    if (start == std::string::npos) {
        return {};
    }
    const size_t begin = start + key.size();
    const size_t end = record.find('"', begin);
    return end == std::string::npos ? std::string() : record.substr(begin, end - begin);
}

std::unique_ptr<chast::HostServer> start_host(uint16_t port, uint32_t max_viewers,
                                              bool allow_input,
                                              const std::filesystem::path& dir,
                                              std::string* error) {
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = max_viewers;
    config.settings.allow_input = allow_input;
    config.settings.allow_file_transfer = true;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(password());
    config.settings.audit_spool = (dir / L"host.audit.jsonl").string();
    config.settings.file_receive_directory = (dir / L"receive").string();
    config.first_client_timeout_ms = 0;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
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
                                                   chast::SessionMode mode,
                                                   const std::string& audit_ref,
                                                   chast::ServerInfo* info,
                                                   std::string* error) {
    chast::QuicClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.idle_timeout_ms = 8000;
    config.pinned_fingerprint = fingerprint;
    config.password = password();
    config.session_mode = mode;
    config.audit_ref = audit_ref;
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

bool finish_viewer(chast::QuicSession& session) {
    chast::ClientStats stats;
    return session.finish_client(stats, std::chrono::seconds(5)).ok();
}

void stop_host(std::unique_ptr<chast::HostServer>& host) {
    if (host) {
        host->stop();
        host.reset();
    }
}

size_t session_records(const std::vector<std::string>& lines) {
    size_t count = 0;
    for (const std::string& line : lines) {
        if (line.find("\"kind\":\"session\"") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

// Desktop Duplication only produces a frame when something changed. A
// one-pixel pointer nudge is the cheapest change a test can make, and the
// cursor is put back where it was before returning.
bool receive_frame_with_activity(chast::QuicSession& session, chast::FrameHeader& header,
                                 std::vector<uint8_t>& payload,
                                 std::chrono::milliseconds timeout) {
    POINT original{};
    GetCursorPos(&original);
    bool toggled = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const chast::Error frame =
            session.receive_frame(header, payload, std::chrono::milliseconds(200));
        if (frame.ok()) {
            SetCursorPos(original.x, original.y);
            return true;
        }
        if (frame.status() != chast::Status::WaitTimeout) {
            SetCursorPos(original.x, original.y);
            return false;
        }
        toggled = !toggled;
        SetCursorPos(original.x + (toggled ? 1 : 0), original.y);
    }
    SetCursorPos(original.x, original.y);
    return false;
}

void test_two_viewers(const std::filesystem::path& dir) {
    const uint16_t port = free_udp_port();
    check(port != 0, "a loopback port is available");
    if (port == 0) {
        return;
    }
    std::string error;
    std::unique_ptr<chast::HostServer> host = start_host(port, 2, false, dir, &error);
    if (!host) {
        check(false, "the multi-viewer host starts");
        std::printf("     %s\n", error.c_str());
        return;
    }
    const std::string fingerprint = host->status().fingerprint;
    check(!fingerprint.empty(), "the host reports its certificate");

    chast::ServerInfo first_info;
    std::unique_ptr<chast::QuicSession> first = connect_viewer(
        port, fingerprint, chast::SessionMode::FileTransfer, "a1a1a1a1a1a1a1a1", &first_info,
        &error);
    check(first != nullptr, "the first viewer is admitted");
    if (!first) {
        std::printf("     %s\n", error.c_str());
        stop_host(host);
        return;
    }
    check(wait_for([&host] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "the host counts one active viewer");

    chast::ServerInfo second_info;
    std::unique_ptr<chast::QuicSession> second = connect_viewer(
        port, fingerprint, chast::SessionMode::FileTransfer, "b2b2b2b2b2b2b2b2", &second_info,
        &error);
    check(second != nullptr, "the second viewer is admitted while the first stays");
    if (!second) {
        std::printf("     %s\n", error.c_str());
        first.reset();
        stop_host(host);
        return;
    }
    check(wait_for([&host] { return host->status().active_viewers == 2; },
                   std::chrono::seconds(5)),
          "both viewers are active at once");
    check(host->status().stats.sessions == 2, "the session count covers both viewers");
    check(first->peer_alive() && second->peer_alive(), "both connections are alive");
    // A files-only Hello never receives input, whatever it asked for.
    check(!first_info.input_enabled && !second_info.input_enabled,
          "file-transfer viewers are announced without input");

    check(finish_viewer(*first), "the first viewer closes cleanly");
    first.reset();
    check(wait_for([&host] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "the first viewer leaving drops the count to one");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(second->peer_alive(), "the second viewer keeps running after the first leaves");
    check(host->status().state == chast::HostState::Streaming,
          "the host keeps streaming for the remaining viewer");

    check(finish_viewer(*second), "the second viewer closes cleanly");
    second.reset();
    check(wait_for([&host] { return host->status().active_viewers == 0; },
                   std::chrono::seconds(5)),
          "the host is idle once the last viewer leaves");
    check(host->status().state == chast::HostState::Listening,
          "the host returns to listening");

    const std::filesystem::path spool = dir / L"host.audit.jsonl";
    check(wait_for([&spool] { return session_records(read_lines(spool)) == 2; },
                   std::chrono::seconds(5)),
          "both sessions leave an audit record");
    const std::vector<std::string> lines = read_lines(spool);
    check(session_records(lines) == 2, "exactly two session records are written");
    bool saw_first = false;
    bool saw_second = false;
    for (const std::string& line : lines) {
        const std::string ref = audit_ref_of(line);
        saw_first = saw_first || ref == "a1a1a1a1a1a1a1a1";
        saw_second = saw_second || ref == "b2b2b2b2b2b2b2b2";
        check(line.find("\"success\":true") != std::string::npos,
              "the session record reports success");
        check(line.find("\"viewer_ref_absent\":false") != std::string::npos,
              "the session record carries the viewer's reference");
    }
    check(saw_first && saw_second, "the two audit records are distinguishable");
    stop_host(host);
}

void test_connection_cap(const std::filesystem::path& dir) {
    const uint16_t port = free_udp_port();
    check(port != 0, "a second loopback port is available");
    if (port == 0) {
        return;
    }
    std::string error;
    std::unique_ptr<chast::HostServer> host = start_host(port, 2, false, dir, &error);
    if (!host) {
        check(false, "the capped host starts");
        std::printf("     %s\n", error.c_str());
        return;
    }
    const std::string fingerprint = host->status().fingerprint;
    chast::ServerInfo info;
    std::unique_ptr<chast::QuicSession> first = connect_viewer(
        port, fingerprint, chast::SessionMode::FileTransfer, "c3c3c3c3c3c3c3c3", &info, &error);
    std::unique_ptr<chast::QuicSession> second = connect_viewer(
        port, fingerprint, chast::SessionMode::FileTransfer, "d4d4d4d4d4d4d4d4", &info, &error);
    check(first && second, "the cap admits exactly two viewers");
    if (!first || !second) {
        first.reset();
        second.reset();
        stop_host(host);
        return;
    }

    chast::ServerInfo third_info;
    std::string refused;
    std::unique_ptr<chast::QuicSession> third = connect_viewer(
        port, fingerprint, chast::SessionMode::FileTransfer, "e5e5e5e5e5e5e5e5", &third_info,
        &refused);
    check(!third, "a viewer above the cap is refused");
    third.reset();

    check(finish_viewer(*first), "the first viewer leaves the capped host");
    first.reset();
    check(wait_for([&host] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "one viewer remains after the close");

    // The transport releases the slot in the worker's destructor, which runs
    // when the listener reaps it; retry briefly instead of assuming the order.
    std::unique_ptr<chast::QuicSession> replacement;
    bool admitted = false;
    for (int attempt = 0; attempt < 15 && !admitted; ++attempt) {
        std::string retry_error;
        replacement = connect_viewer(port, fingerprint, chast::SessionMode::FileTransfer,
                                     "f6f6f6f6f6f6f6f6", &info, &retry_error);
        admitted = replacement != nullptr;
        if (!admitted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    check(admitted, "the freed slot admits a replacement viewer");
    if (replacement) {
        finish_viewer(*replacement);
        replacement.reset();
    }
    finish_viewer(*second);
    second.reset();
    check(wait_for([&host] { return host->status().active_viewers == 0; },
                   std::chrono::seconds(5)),
          "the capped host drains after the replacements");
    stop_host(host);
}

void test_duplicate_audit_ref(const std::filesystem::path& dir) {
    const std::filesystem::path spool = dir / L"duplicate.audit.jsonl";
    const uint16_t port = free_udp_port();
    check(port != 0, "a third loopback port is available");
    if (port == 0) {
        return;
    }
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 2;
    config.settings.allow_file_transfer = true;
    config.settings.auth_mode = chast::HostAuthMode::Password;
    config.settings.password_hash = chast::hash_password(password());
    config.settings.audit_spool = spool.string();
    config.settings.file_receive_directory = (dir / L"receive").string();
    config.first_client_timeout_ms = 0;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    std::string error;
    std::unique_ptr<chast::HostServer> host =
        chast::HostServer::create(std::move(config), &error);
    if (!host) {
        check(false, "the duplicate-reference host starts");
        std::printf("     %s\n", error.c_str());
        return;
    }
    host->start();
    check(wait_for([&host] { return host->status().state == chast::HostState::Listening; },
                   std::chrono::seconds(5)),
          "the duplicate-reference host listens");

    const std::string fingerprint = host->status().fingerprint;
    const std::string shared_ref = "9a9a9a9a9a9a9a9a";
    chast::ServerInfo info;
    std::unique_ptr<chast::QuicSession> first =
        connect_viewer(port, fingerprint, chast::SessionMode::FileTransfer, shared_ref, &info,
                       &error);
    std::unique_ptr<chast::QuicSession> second =
        connect_viewer(port, fingerprint, chast::SessionMode::FileTransfer, shared_ref, &info,
                       &error);
    check(first && second, "both viewers present the same reference and are admitted");
    if (!first || !second) {
        first.reset();
        second.reset();
        stop_host(host);
        return;
    }
    finish_viewer(*first);
    first.reset();
    finish_viewer(*second);
    second.reset();
    check(wait_for([&spool] { return session_records(read_lines(spool)) == 2; },
                   std::chrono::seconds(5)),
          "both colliding sessions leave a record");

    size_t matched = 0;
    size_t renamed = 0;
    for (const std::string& line : read_lines(spool)) {
        const std::string ref = audit_ref_of(line);
        if (ref == shared_ref) {
            ++matched;
            check(line.find("\"viewer_ref_absent\":false") != std::string::npos,
                  "the first connection keeps the viewer reference");
        } else if (!ref.empty()) {
            ++renamed;
            check(line.find("\"viewer_ref_absent\":true") != std::string::npos,
                  "the colliding connection is marked as host-generated");
        }
    }
    check(matched == 1, "only one session owns the viewer reference");
    check(renamed == 1, "the colliding session is recorded under a host id");
    stop_host(host);
}

void test_input_lease(const std::filesystem::path& dir) {
    const uint16_t port = free_udp_port();
    check(port != 0, "a fourth loopback port is available");
    if (port == 0) {
        return;
    }
    std::string error;
    std::unique_ptr<chast::HostServer> host = start_host(port, 2, true, dir, &error);
    if (!host) {
        check(false, "the control-lease host starts");
        std::printf("     %s\n", error.c_str());
        return;
    }
    const std::string fingerprint = host->status().fingerprint;
    chast::ServerInfo first_info;
    std::unique_ptr<chast::QuicSession> first = connect_viewer(
        port, fingerprint, chast::SessionMode::Desktop, "a7a7a7a7a7a7a7a7", &first_info, &error);
    if (!first) {
        skip("the control-lease check needs a desktop stream");
        std::printf("     %s\n", error.c_str());
        stop_host(host);
        return;
    }
    check(first_info.input_enabled, "the first desktop viewer is granted input");
    // Capture failure is not what this check is about; without a desktop the
    // lease has no session to arbitrate.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (host->status().active_viewers != 1 ||
        host->status().state != chast::HostState::Streaming) {
        skip("the control-lease check needs a desktop stream");
        first.reset();
        stop_host(host);
        return;
    }

    chast::ServerInfo second_info;
    std::unique_ptr<chast::QuicSession> second = connect_viewer(
        port, fingerprint, chast::SessionMode::Desktop, "b8b8b8b8b8b8b8b8", &second_info, &error);
    check(second != nullptr, "the second desktop viewer is admitted");
    if (!second) {
        std::printf("     %s\n", error.c_str());
        first.reset();
        stop_host(host);
        return;
    }
    check(!second_info.input_enabled, "the second desktop viewer joins view-only");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (host->status().active_viewers != 2) {
        skip("the second desktop stream did not stay up");
        second.reset();
        first.reset();
        stop_host(host);
        return;
    }

    // One capture pipeline serves both viewers: the second one follows the
    // publisher's stream instead of opening its own duplication.
    chast::FrameHeader shared_header;
    std::vector<uint8_t> shared_payload;
    check(receive_frame_with_activity(*second, shared_header, shared_payload,
                                      std::chrono::seconds(10)),
          "the view-only viewer receives the shared desktop stream");
    check(shared_header.width > 0 && shared_header.height > 0,
          "the shared frame describes a picture");

    // The operator's grant targets the focused viewer (the second one, which
    // does not hold the lease), so the input bit must still be stripped. The
    // mask does not change, so no PermissionUpdate is due: what matters is
    // that the viewer still has no input.
    host->set_permissions(chast::kPermissionAll);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    uint16_t second_mask = 0;
    while (second->take_permission_update(second_mask)) {
    }
    second_mask = second->permissions();
    check((second_mask & chast::kPermissionInput) == 0,
          "the grant cannot give a second viewer input");

    const uint64_t close_mark = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count()) &
        0xFFFFFFFFFFFFull;
    check(finish_viewer(*first), "the controlling viewer closes cleanly");
    first.reset();
    check(wait_for([&host] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "the remaining viewer keeps the host alive");

    // The publisher left; the remaining viewer takes over capture and keeps
    // sending frames on its own session.
    bool promoted_frames = false;
    const auto promote_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < promote_deadline && !promoted_frames) {
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        if (!receive_frame_with_activity(*second, header, payload,
                                         std::chrono::seconds(5))) {
            continue;
        }
        promoted_frames = header.capture_ms >= close_mark;
    }
    check(promoted_frames, "the remaining viewer takes over the shared capture");

    bool input_restored = false;
    uint16_t restored_mask = 0;
    wait_for(
        [&] {
            host->set_permissions(chast::kPermissionAll);
            uint16_t mask = 0;
            if (second->take_permission_update(mask)) {
                restored_mask = mask;
                input_restored = (mask & chast::kPermissionInput) != 0;
            }
            return input_restored;
        },
        std::chrono::seconds(5));
    check(input_restored, "the freed lease lets the operator grant input again");
    check((restored_mask & chast::kPermissionInput) != 0,
          "the remaining viewer is told about its new input grant");

    finish_viewer(*second);
    second.reset();
    wait_for([&host] { return host->status().active_viewers == 0; },
             std::chrono::seconds(5));
    stop_host(host);
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
    test_two_viewers(dir);
    test_connection_cap(dir);
    test_duplicate_audit_ref(dir);
    test_input_lease(dir);
    WSACleanup();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (failures != 0) {
        std::printf("host multi-viewer checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("host multi-viewer checks passed\n");
    return 0;
}
