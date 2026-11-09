// Two desktop viewers on one shared host. The second viewer joins view-only
// because the first holds the control lease; this test checks that it receives
// the shared GPU stream but cannot inject gestures, that its privacy and
// host-action requests are refused with the shared-stream reasons, that the
// operator's chat reaches only the focused viewer, and that the remaining
// viewer keeps running when the first leaves. Needs an interactive desktop,
// so it skips itself when no desktop stream is available.

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
    static const std::string value = "multi-session-test";
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

struct InputCapture {
    std::mutex mutex;
    std::vector<chast::InputEvent> events;

    std::function<void(const chast::InputEvent&)> callback() {
        return [this](const chast::InputEvent& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        };
    }

    size_t gestures() {
        std::lock_guard<std::mutex> lock(mutex);
        size_t total = 0;
        for (const chast::InputEvent& event : events) {
            if (event.kind == chast::InputKind::Gesture) {
                ++total;
            }
        }
        return total;
    }

    chast::GestureAction last_gesture() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->kind == chast::InputKind::Gesture) {
                return it->gesture.action;
            }
        }
        return chast::GestureAction::Pan;
    }
};

std::unique_ptr<chast::HostServer> start_host(uint16_t port,
                                              const std::shared_ptr<InputCapture>& input,
                                              std::string* error) {
    chast::HostServerConfig config;
    config.settings.bind_address = "127.0.0.1";
    config.settings.port = port;
    config.settings.carrier = false;
    config.settings.tunnel = false;
    config.settings.max_viewers = 2;
    config.settings.allow_input = true;
    config.settings.allow_audio = false;
    config.settings.allow_privacy_mode = true;
    config.settings.allow_restart = true;
    config.settings.allow_block_input = true;
    config.settings.token_auth = true;
    config.settings.access_token = access_token();
    config.settings.token_mode = chast::HostTokenMode::Reusable;
    config.dry_run_input = true;
    config.first_client_timeout_ms = 0;
    config.finish_timeout_ms = 200;
    config.serve_once = false;
    config.max_consecutive_failures = 3;
    config.input_event = input->callback();
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

bool finish_viewer(chast::QuicSession& viewer) {
    chast::ClientStats stats;
    return viewer.finish_client(stats, std::chrono::seconds(5)).ok();
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

bool take_privacy_state(chast::QuicSession& viewer, bool& enabled,
                        chast::PrivacyModeCode& code, std::string& reason,
                        std::chrono::milliseconds timeout) {
    return wait_for([&] { return viewer.take_privacy_mode_state(enabled, code, reason); },
                    timeout);
}

bool take_host_action(chast::QuicSession& viewer, chast::HostAction action,
                      chast::HostControlResult& result) {
    return wait_for(
        [&] {
            chast::HostControlResult next;
            while (viewer.take_host_action_result(next)) {
                if (next.action == action) {
                    result = next;
                    return true;
                }
            }
            return false;
        },
        std::chrono::seconds(5));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("FAIL WSAStartup\n");
        return 1;
    }
    const uint16_t port = free_udp_port();
    check(port != 0, "a loopback port is available");
    std::string error;
    auto input = std::make_shared<InputCapture>();
    std::unique_ptr<chast::HostServer> host =
        port == 0 ? nullptr : start_host(port, input, &error);
    check(host != nullptr, "the shared host starts");
    if (!host) {
        std::printf("     %s\n", error.c_str());
        WSACleanup();
        return 1;
    }
    const std::string fingerprint = host->status().fingerprint;
    chast::ServerInfo first_info;
    std::unique_ptr<chast::QuicSession> first =
        connect_viewer(port, fingerprint, &first_info, &error);
    check(first != nullptr, "the first desktop viewer is admitted");
    if (!first) {
        std::printf("     %s\n", error.c_str());
        host->stop();
        WSACleanup();
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (host->status().state != chast::HostState::Streaming) {
        skip("the multi-session check needs a desktop stream");
        finish_viewer(*first);
        first.reset();
        host->stop();
        WSACleanup();
        return failures == 0 ? 0 : 1;
    }
    check(first_info.input_enabled, "the first viewer holds the control lease");

    chast::ServerInfo second_info;
    std::unique_ptr<chast::QuicSession> second =
        connect_viewer(port, fingerprint, &second_info, &error);
    check(second != nullptr, "the second desktop viewer is admitted");
    if (!second) {
        std::printf("     %s\n", error.c_str());
        finish_viewer(*first);
        first.reset();
        host->stop();
        WSACleanup();
        return 1;
    }
    check(!second_info.input_enabled, "the second viewer joins view-only");
    check(wait_for([&] { return host->status().active_viewers == 2; },
                   std::chrono::seconds(5)),
          "both desktop sessions coexist");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    chast::FrameHeader first_header;
    std::vector<uint8_t> first_payload;
    check(receive_frame_with_activity(*first, first_header, first_payload,
                                      std::chrono::seconds(10)),
          "the publisher delivers the shared desktop stream");
    chast::FrameHeader second_header;
    std::vector<uint8_t> second_payload;
    check(receive_frame_with_activity(*second, second_header, second_payload,
                                      std::chrono::seconds(10)),
          "the view-only viewer receives the same shared stream");

    check(second->set_privacy_mode(true).ok(),
          "the view-only viewer asks for virtual display privacy");
    bool enabled = true;
    chast::PrivacyModeCode privacy_code = chast::PrivacyModeCode::Ok;
    std::string reason;
    check(take_privacy_state(*second, enabled, privacy_code, reason,
                             std::chrono::seconds(5)) &&
              !enabled && privacy_code == chast::PrivacyModeCode::Denied &&
              reason == "another viewer controls this host",
          "privacy is refused with the shared-stream reason");

    check(second->request_host_action(chast::HostAction::Restart).ok(),
          "the view-only viewer queues a restart request");
    chast::HostControlResult action_result;
    check(take_host_action(*second, chast::HostAction::Restart, action_result) &&
              action_result.code == chast::HostActionCode::Busy &&
              action_result.reason == "another viewer controls this host",
          "host actions are refused with the shared-stream reason");

    check(host->send_chat("only-focused"), "the operator sends a chat line");
    std::string chat;
    check(wait_for([&] { return second->take_chat(chat); }, std::chrono::seconds(5)) &&
              chat == "only-focused",
          "the focused viewer receives the chat line");
    check(!first->take_chat(chat), "the chat line does not leak to the other viewer");

    chast::InputEvent zoom;
    zoom.kind = chast::InputKind::Gesture;
    zoom.gesture = {chast::GestureAction::Zoom, 0, 120};
    const chast::Error refused = second->send_input(zoom);
    check(!refused.ok() && refused.status() == chast::Status::PermissionDenied,
          "the view-only viewer cannot inject a gesture");
    chast::InputEvent pan;
    pan.kind = chast::InputKind::Gesture;
    pan.gesture = {chast::GestureAction::Pan, 40, 0};
    check(first->send_input(pan).ok(), "the controlling viewer sends a gesture");
    check(wait_for([&] { return input->gestures() == 1; }, std::chrono::seconds(5)) &&
              input->last_gesture() == chast::GestureAction::Pan,
          "only the controlling viewer's gesture reaches the host");

    const uint64_t close_mark = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count()) &
        0xFFFFFFFFFFFFull;
    check(finish_viewer(*first), "the controlling viewer closes cleanly");
    first.reset();
    check(wait_for([&] { return host->status().active_viewers == 1; },
                   std::chrono::seconds(5)),
          "the remaining viewer keeps the host alive");

    bool continued = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline && !continued) {
        chast::FrameHeader header;
        std::vector<uint8_t> payload;
        if (!receive_frame_with_activity(*second, header, payload,
                                         std::chrono::seconds(5))) {
            continue;
        }
        continued = header.capture_ms >= close_mark;
    }
    check(continued, "the remaining viewer continues the desktop stream");

    check(finish_viewer(*second), "the remaining viewer closes cleanly");
    second.reset();
    check(wait_for([&] { return host->status().active_viewers == 0; },
                   std::chrono::seconds(5)),
          "the host drains after both viewers leave");
    host->stop();
    host.reset();
    WSACleanup();
    if (failures != 0) {
        std::printf("multi-session checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("multi-session checks passed\n");
    return 0;
}
