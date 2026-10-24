// Checks for the TCP/WebSocket datagram carrier: the RFC 6455 accept vector,
// raw and WebSocket framing against a local UDP echo, the upgrade handshake,
// oversized/malformed frames and the connection limit. Everything stays on
// loopback, so the test needs no GPU, desktop or external network.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "chast/carrier.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void check_text(const std::string& text, const std::string& wanted, const char* what) {
    check(text == wanted, what);
    if (text != wanted) {
        std::printf("     got  %s\n     want %s\n", text.c_str(), wanted.c_str());
    }
}

bool set_timeout(SOCKET socket, DWORD milliseconds) {
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&milliseconds),
                      sizeof(milliseconds)) == 0;
}

bool send_bytes(SOCKET socket, const void* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int sent = send(socket, static_cast<const char*>(data) + offset,
                              static_cast<int>(size - offset), 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

SOCKET connect_loopback(uint16_t port) {
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket);
        return INVALID_SOCKET;
    }
    set_timeout(socket, 5000);
    return socket;
}

template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = 3000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

bool read_http_response(SOCKET socket, std::string* response) {
    char buffer[512];
    for (;;) {
        const int got = recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (got <= 0) {
            return false;
        }
        response->append(buffer, static_cast<size_t>(got));
        if (response->find("\r\n\r\n") != std::string::npos) {
            return true;
        }
        if (response->size() > 8192) {
            return false;
        }
    }
}

// Stands in for the host's QUIC listener: echoes every datagram back to its
// sender. The recv timeout lets close() stop the thread without closing the
// socket underneath a blocked call.
class UdpEcho {
public:
    UdpEcho() = default;
    ~UdpEcho() { close(); }
    UdpEcho(const UdpEcho&) = delete;
    UdpEcho& operator=(const UdpEcho&) = delete;

    bool start() {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            return false;
        }
        int size = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return false;
        }
        port_ = ntohs(address.sin_port);
        DWORD timeout = 200;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        thread_ = std::thread([this] {
            std::vector<char> buffer(65536);
            while (!stopping_.load(std::memory_order_relaxed)) {
                sockaddr_in peer{};
                int peer_size = sizeof(peer);
                const int got =
                    recvfrom(socket_, buffer.data(), static_cast<int>(buffer.size()), 0,
                             reinterpret_cast<sockaddr*>(&peer), &peer_size);
                if (got <= 0) {
                    continue;
                }
                sendto(socket_, buffer.data(), got, 0,
                       reinterpret_cast<const sockaddr*>(&peer), peer_size);
            }
        });
        return true;
    }

    void close() {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    uint16_t port() const { return port_; }

private:
    SOCKET socket_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

// Sends one datagram to the bridge's loopback port and waits for the echo
// that travelled through the carrier and back.
bool echo_round_trip(uint16_t bridge_port, const std::string& message,
                     std::string* received) {
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        return false;
    }
    set_timeout(socket, 5000);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(bridge_port);
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int sent = sendto(socket, message.data(), static_cast<int>(message.size()), 0,
                            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    if (sent != static_cast<int>(message.size())) {
        closesocket(socket);
        return false;
    }
    std::vector<char> buffer(65536);
    const int got = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    closesocket(socket);
    if (got <= 0) {
        return false;
    }
    received->assign(buffer.data(), static_cast<size_t>(got));
    return true;
}

std::unique_ptr<chast::CarrierServer> start_server(uint16_t quic_port, size_t max_connections,
                                                   std::string* error) {
    chast::CarrierServerConfig config;
    config.bind_address = "127.0.0.1";
    config.port = 0;
    config.quic_address = "127.0.0.1";
    config.quic_port = quic_port;
    config.max_connections = max_connections;
    return chast::CarrierServer::create(config, error);
}

void test_accept_vector() {
    check_text(chast::carrier_websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="),
               "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "the RFC 6455 accept vector matches");
    check_text(chast::carrier_transport_name(chast::CarrierTransport::kTcp), "tcp",
               "tcp transport name");
    check_text(chast::carrier_transport_name(chast::CarrierTransport::kWebSocket), "ws",
               "websocket transport name");
}

void test_raw_round_trip() {
    UdpEcho echo;
    check(echo.start(), "the UDP echo starts");
    std::string error;
    std::unique_ptr<chast::CarrierServer> server = start_server(echo.port(), 16, &error);
    check(server != nullptr, "the raw carrier listener starts");
    if (!server) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    check(server->port() != 0, "the listener reports its bound port");

    chast::CarrierBridgeConfig config;
    config.host = "127.0.0.1";
    config.port = server->port();
    config.transport = chast::CarrierTransport::kTcp;
    config.connect_timeout = std::chrono::milliseconds(3000);
    std::unique_ptr<chast::CarrierBridge> bridge = chast::CarrierBridge::open(config, &error);
    check(bridge != nullptr, "the raw carrier bridge opens");
    if (!bridge) {
        std::printf("     %s\n", error.c_str());
        server->stop();
        return;
    }
    check(bridge->transport() == chast::CarrierTransport::kTcp, "the bridge reports tcp");
    check(bridge->local_port() != 0, "the bridge exposes a loopback UDP port");

    std::string received;
    check(echo_round_trip(bridge->local_port(), "hello-carrier", &received),
          "a raw datagram makes the round trip");
    check_text(received, "hello-carrier", "the raw payload is unchanged");

    std::string pattern;
    for (int i = 0; i < 3000; ++i) {
        pattern.push_back(static_cast<char>(i * 7 + 3));
    }
    received.clear();
    check(echo_round_trip(bridge->local_port(), pattern, &received),
          "a 3000-byte raw datagram makes the round trip");
    check(received == pattern, "the large raw payload is unchanged");

    // A zero-length frame is the raw keepalive; it must be skipped rather
    // than echoed as an empty datagram.
    const SOCKET raw = connect_loopback(server->port());
    check(raw != INVALID_SOCKET, "a raw control connection opens");
    if (raw != INVALID_SOCKET) {
        const unsigned char magic[8] = {'C', 'H', 'A', 'S', 'T', 'T', '1', '\n'};
        const unsigned char keepalive[4] = {0, 0, 0, 0};
        check(send_bytes(raw, magic, sizeof(magic)), "the raw magic is sent");
        check(send_bytes(raw, keepalive, sizeof(keepalive)), "a raw keepalive is sent");
        closesocket(raw);
    }

    check(wait_for([&] { return bridge->stats().to_carrier_datagrams >= 2; }),
          "the bridge counts datagrams towards the carrier");
    check(wait_for([&] { return bridge->stats().from_carrier_datagrams >= 2; }),
          "the bridge counts datagrams from the carrier");
    check(wait_for([&] { return server->stats().datagrams_to_quic >= 2; }),
          "the carrier counts datagrams towards QUIC");
    check(wait_for([&] { return server->stats().datagrams_from_quic >= 2; }),
          "the carrier counts datagrams from QUIC");
    check(bridge->error().empty(), "the raw bridge reports no error while healthy");

    bridge->stop();
    server->stop();
}

void test_websocket_round_trip() {
    UdpEcho echo;
    check(echo.start(), "the UDP echo starts (websocket)");
    std::string error;
    std::unique_ptr<chast::CarrierServer> server = start_server(echo.port(), 16, &error);
    check(server != nullptr, "the websocket carrier listener starts");
    if (!server) {
        std::printf("     %s\n", error.c_str());
        return;
    }
    chast::CarrierBridgeConfig config;
    config.host = "127.0.0.1";
    config.port = server->port();
    config.transport = chast::CarrierTransport::kWebSocket;
    config.connect_timeout = std::chrono::milliseconds(3000);
    std::unique_ptr<chast::CarrierBridge> bridge = chast::CarrierBridge::open(config, &error);
    check(bridge != nullptr, "the websocket carrier bridge opens");
    if (!bridge) {
        std::printf("     %s\n", error.c_str());
        server->stop();
        return;
    }
    check(bridge->transport() == chast::CarrierTransport::kWebSocket,
          "the bridge reports websocket");

    std::string received;
    check(echo_round_trip(bridge->local_port(), "hello-websocket", &received),
          "a websocket datagram makes the round trip");
    check_text(received, "hello-websocket", "the websocket payload is unchanged");

    std::string pattern(4000, '\0');
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<char>((i * 31 + 5) & 0xFF);
    }
    received.clear();
    check(echo_round_trip(bridge->local_port(), pattern, &received),
          "a 4000-byte websocket message makes the round trip");
    check(received == pattern, "the large websocket payload is unchanged");
    check(bridge->error().empty(), "the websocket bridge reports no error while healthy");

    bridge->stop();
    server->stop();
}

void test_websocket_handshake() {
    UdpEcho echo;
    check(echo.start(), "the UDP echo starts (handshake)");
    std::string error;
    std::unique_ptr<chast::CarrierServer> server = start_server(echo.port(), 16, &error);
    check(server != nullptr, "the handshake listener starts");
    if (!server) {
        return;
    }
    const SOCKET socket = connect_loopback(server->port());
    check(socket != INVALID_SOCKET, "a handshake connection opens");
    if (socket != INVALID_SOCKET) {
        const std::string request =
            "GET /chast HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: chast-carrier.v1\r\n"
            "\r\n";
        check(send_bytes(socket, request.data(), request.size()), "the upgrade request is sent");
        std::string response;
        check(read_http_response(socket, &response), "the upgrade is answered");
        check(response.find(" 101 ") != std::string::npos,
              "the answer switches protocols");
        check(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
                  std::string::npos,
              "the answer carries the accept value");
        check(response.find("Sec-WebSocket-Protocol: chast-carrier.v1") != std::string::npos,
              "the answer selects the carrier subprotocol");
        closesocket(socket);
    }
    server->stop();
}

void test_bad_input_is_closed() {
    UdpEcho echo;
    check(echo.start(), "the UDP echo starts (bad input)");
    std::string error;
    std::unique_ptr<chast::CarrierServer> server = start_server(echo.port(), 16, &error);
    check(server != nullptr, "the rejection listener starts");
    if (!server) {
        return;
    }

    const SOCKET bad_magic = connect_loopback(server->port());
    check(bad_magic != INVALID_SOCKET, "a connection with the wrong magic opens");
    if (bad_magic != INVALID_SOCKET) {
        check(send_bytes(bad_magic, "BADMAGIC", 8), "the wrong magic is sent");
        char byte = 0;
        check(recv(bad_magic, &byte, 1, 0) <= 0, "the wrong magic is closed");
        closesocket(bad_magic);
    }

    const SOCKET oversized = connect_loopback(server->port());
    check(oversized != INVALID_SOCKET, "an oversized-frame connection opens");
    if (oversized != INVALID_SOCKET) {
        const unsigned char magic[8] = {'C', 'H', 'A', 'S', 'T', 'T', '1', '\n'};
        // 70000 > 65535, the largest datagram a carrier frame may hold.
        const unsigned char length[4] = {0x70, 0x11, 0x01, 0x00};
        check(send_bytes(oversized, magic, sizeof(magic)), "the oversized magic is sent");
        check(send_bytes(oversized, length, sizeof(length)), "the oversized length is sent");
        char byte = 0;
        check(recv(oversized, &byte, 1, 0) <= 0, "an oversized frame is closed");
        closesocket(oversized);
    }
    server->stop();
}

void test_connection_limit() {
    UdpEcho echo;
    check(echo.start(), "the UDP echo starts (limit)");
    std::string error;
    std::unique_ptr<chast::CarrierServer> server = start_server(echo.port(), 1, &error);
    check(server != nullptr, "the limited listener starts");
    if (!server) {
        return;
    }
    const SOCKET first = connect_loopback(server->port());
    check(first != INVALID_SOCKET, "the first connection opens");
    if (first == INVALID_SOCKET) {
        server->stop();
        return;
    }
    const unsigned char magic[8] = {'C', 'H', 'A', 'S', 'T', 'T', '1', '\n'};
    check(send_bytes(first, magic, sizeof(magic)), "the first magic is sent");
    check(wait_for([&] {
              const chast::CarrierServerStats stats = server->stats();
              return stats.connections == 1 && stats.active == 1;
          }),
          "the first connection is active");

    const SOCKET second = connect_loopback(server->port());
    check(second != INVALID_SOCKET, "the second connection reaches the listener");
    if (second != INVALID_SOCKET) {
        check(wait_for([&] { return server->stats().rejected >= 1; }),
              "the extra connection is counted as rejected");
        char byte = 0;
        check(recv(second, &byte, 1, 0) <= 0, "the extra connection is closed");
        closesocket(second);
    }
    closesocket(first);
    server->stop();
}

void test_shared_hello() {
    // The shared carrier authenticates viewers from the hello they send before
    // any frame: the V2 magic and one JSON line on raw TCP.
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(listener != INVALID_SOCKET, "the shared listener socket opens");
    if (listener == INVALID_SOCKET) {
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        check(false, "the shared hearer binds");
        closesocket(listener);
        return;
    }
    int size = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
    const uint16_t port = ntohs(address.sin_port);

    std::string seen;
    std::thread hearer([&] {
        const SOCKET peer = accept(listener, nullptr, nullptr);
        if (peer == INVALID_SOCKET) {
            return;
        }
        set_timeout(peer, 5000);
        char buffer[512];
        // The V2 magic ends in a newline and the hello is the following line,
        // so wait for the second newline before hanging up.
        auto newlines = [&] {
            return static_cast<size_t>(
                std::count(seen.begin(), seen.end(), '\n'));
        };
        while (newlines() < 2 && seen.size() < sizeof(buffer) - 1) {
            const int got = recv(peer, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
            if (got <= 0) {
                break;
            }
            seen.append(buffer, static_cast<size_t>(got));
        }
        closesocket(peer);
    });

    chast::CarrierBridgeConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.transport = chast::CarrierTransport::kTcp;
    config.connect_timeout = std::chrono::milliseconds(3000);
    config.host_id = "devbox";
    config.token = "0123456789abcdef";
    std::string error;
    std::unique_ptr<chast::CarrierBridge> bridge = chast::CarrierBridge::open(config, &error);
    check(bridge != nullptr, "the shared carrier bridge opens");
    if (!bridge) {
        std::printf("     %s\n", error.c_str());
    }
    // The hello went out during open; the hearer returns once it has the line.
    closesocket(listener);
    hearer.join();
    check(seen.rfind("CHASTT-V2\n", 0) == 0, "the shared bridge sends the V2 magic");
    check(seen.find("{\"host_id\":\"devbox\",\"token\":\"0123456789abcdef\"}") !=
              std::string::npos,
          "the shared bridge sends its host id and token");
    if (bridge) {
        bridge->stop();
    }

    // A shared endpoint without a token is refused before any socket work.
    chast::CarrierBridgeConfig missing;
    missing.host = "127.0.0.1";
    missing.port = 1;
    missing.host_id = "devbox";
    error.clear();
    check(chast::CarrierBridge::open(missing, &error) == nullptr,
          "a shared bridge without a token is refused");
    check(error.find("token") != std::string::npos, "the refusal names the missing token");
}

void test_unreachable_bridge() {
    chast::CarrierBridgeConfig config;
    config.host = "127.0.0.1";
    // Port 1 on loopback is not a carrier; connect is refused immediately.
    config.port = 1;
    config.transport = chast::CarrierTransport::kTcp;
    config.connect_timeout = std::chrono::milliseconds(500);
    std::string error;
    std::unique_ptr<chast::CarrierBridge> bridge = chast::CarrierBridge::open(config, &error);
    check(bridge == nullptr, "an unreachable carrier bridge fails");
    check(!error.empty(), "the failed bridge reports an error");
}

}  // namespace

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::printf("FAIL winsock did not start\n");
        return 1;
    }
    test_accept_vector();
    test_raw_round_trip();
    test_websocket_round_trip();
    test_websocket_handshake();
    test_bad_input_is_closed();
    test_connection_limit();
    test_shared_hello();
    test_unreachable_bridge();
    WSACleanup();
    std::printf("%s %d failure(s)\n", failures == 0 ? "PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
