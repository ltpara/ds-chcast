#include "chast/carrier.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace chast {

namespace {

// One TCP connection carries one QUIC connection's datagrams. The framing is
// deliberately smaller than the QUIC path MTU ceiling so a single read never
// has to grow a buffer beyond one UDP datagram.
constexpr size_t kMaxDatagram = 65535;
constexpr char kRawMagic[] = "CHASTT1\n";
constexpr size_t kRawMagicSize = 8;
// A shared carrier multiplexes every host on one port, so the viewer must name
// its host and prove its lookup-issued token before any datagram is forwarded.
constexpr char kSharedMagic[] = "CHASTT-V2\n";
constexpr size_t kSharedMagicSize = 10;
constexpr DWORD kHandshakeTimeoutMs = 10000;
constexpr DWORD kIdleTimeoutMs = 180000;

// shared_hello_json builds the one-line hello both carriers use. host ids are
// [A-Za-z0-9_.-] and tokens are hex, so no escaping is needed.
std::string shared_hello_json(const std::string& host_id, const std::string& token) {
    return "{\"host_id\":\"" + host_id + "\",\"token\":\"" + token + "\"}";
}

void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        // MsQuic already starts Winsock in the product; the extra reference is
        // harmless and makes the codec test independent of it.
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    });
}

std::string socket_error(int code) {
    return "winsock error " + std::to_string(code);
}

int last_socket_error() {
    return WSAGetLastError();
}

bool set_timeout(SOCKET socket, int option, DWORD milliseconds) {
    return setsockopt(socket, SOL_SOCKET, option,
                      reinterpret_cast<const char*>(&milliseconds),
                      sizeof(milliseconds)) == 0;
}

// QUIC emits a keyframe as a burst of datagrams, and the hop between MsQuic
// and the carrier is still loopback UDP. Windows' default socket buffer drops
// the tail of such a burst while the carrier thread copies frames, which the
// sender's congestion control reads as network loss. Big buffers keep the
// carrier transparent on a clean link.
constexpr int kUdpBufferBytes = 4 * 1024 * 1024;

void set_udp_buffers(SOCKET socket) {
    const int bytes = kUdpBufferBytes;
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes),
               sizeof(bytes));
    setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes),
               sizeof(bytes));
}

bool send_all(SOCKET socket, const uint8_t* data, size_t size, std::string* error) {
    size_t offset = 0;
    while (offset < size) {
        const int chunk = static_cast<int>(
            (std::min)(size - offset, static_cast<size_t>(1u << 20)));
        const int sent = send(socket, reinterpret_cast<const char*>(data + offset), chunk, 0);
        if (sent <= 0) {
            *error = socket_error(last_socket_error());
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool recv_exact(SOCKET socket, uint8_t* data, size_t size, std::string* error) {
    size_t offset = 0;
    while (offset < size) {
        const int got = recv(socket, reinterpret_cast<char*>(data + offset),
                             static_cast<int>(size - offset), 0);
        if (got <= 0) {
            *error = socket_error(got == 0 ? WSAECONNRESET : last_socket_error());
            return false;
        }
        offset += static_cast<size_t>(got);
    }
    return true;
}

struct SocketHolder {
    explicit SocketHolder(SOCKET handle) : socket(handle) {}
    std::atomic<SOCKET> socket{INVALID_SOCKET};
    void close() {
        const SOCKET handle = socket.exchange(INVALID_SOCKET);
        if (handle != INVALID_SOCKET) {
            closesocket(handle);
        }
    }
};

std::string base64_encode(const uint8_t* data, size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < size ? data[i + 1] : 0;
        const uint32_t c = i + 2 < size ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? kAlphabet[triple & 0x3F] : '=');
    }
    return out;
}

std::string sha1_digest(const std::string& input) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr,
                                                    0)) ||
        algorithm == nullptr) {
        return {};
    }
    std::array<uint8_t, 20> digest{};
    const NTSTATUS status = BCryptHash(
        algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status)) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string random_bytes(size_t size) {
    std::string out(size, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(size),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
    return out;
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool header_value(const std::string& headers, const std::string& wanted, std::string* value) {
    const std::string needle = lowercase(wanted);
    size_t line_start = headers.find("\r\n");
    if (line_start == std::string::npos) {
        return false;
    }
    line_start += 2;
    while (line_start < headers.size()) {
        const size_t line_end = headers.find("\r\n", line_start);
        if (line_end == std::string::npos) {
            break;
        }
        const std::string line = headers.substr(line_start, line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && lowercase(trim(line.substr(0, colon))) == needle) {
            *value = trim(line.substr(colon + 1));
            return true;
        }
        line_start = line_end + 2;
    }
    return false;
}

bool read_http_headers(SOCKET socket, const std::string& prefix, std::string* headers,
                       std::string* error) {
    std::string buffer = prefix;
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        if (buffer.size() > 8192) {
            *error = "the HTTP handshake is too large";
            return false;
        }
        char chunk[512];
        const int got = recv(socket, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (got <= 0) {
            *error = socket_error(got == 0 ? WSAECONNRESET : last_socket_error());
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(got));
    }
    *headers = buffer;
    return true;
}

bool ws_write_frame(SOCKET socket, bool mask, std::mutex* write_mutex, uint8_t opcode,
                    const uint8_t* payload, size_t size, std::string* error) {
    if (size > 65535) {
        *error = "a WebSocket carrier message is larger than 65535 bytes";
        return false;
    }
    std::vector<uint8_t> frame;
    frame.reserve(size + 14);
    frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));
    const uint8_t mask_bit = mask ? 0x80 : 0x00;
    if (size <= 125) {
        frame.push_back(static_cast<uint8_t>(mask_bit | size));
    } else {
        frame.push_back(static_cast<uint8_t>(mask_bit | 126));
        frame.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(size & 0xFF));
    }
    if (mask) {
        const std::string key = random_bytes(4);
        if (key.size() != 4) {
            *error = "could not generate a WebSocket mask";
            return false;
        }
        frame.insert(frame.end(), key.begin(), key.end());
        const size_t payload_offset = frame.size();
        frame.resize(payload_offset + size);
        for (size_t i = 0; i < size; ++i) {
            frame[payload_offset + i] =
                payload[i] ^ static_cast<uint8_t>(key[i % 4]);
        }
    } else {
        frame.insert(frame.end(), payload, payload + size);
    }
    std::lock_guard<std::mutex> lock(*write_mutex);
    return send_all(socket, frame.data(), frame.size(), error);
}

bool ws_read_frame(SOCKET socket, bool expect_mask, bool send_mask, std::mutex* write_mutex,
                   std::vector<uint8_t>* payload, std::string* error) {
    for (;;) {
        uint8_t header[2];
        if (!recv_exact(socket, header, sizeof(header), error)) {
            return false;
        }
        const bool fin = (header[0] & 0x80) != 0;
        const uint8_t opcode = header[0] & 0x0F;
        const bool masked = (header[1] & 0x80) != 0;
        uint64_t length = header[1] & 0x7F;
        if (length == 126) {
            uint8_t extended[2];
            if (!recv_exact(socket, extended, sizeof(extended), error)) {
                return false;
            }
            length = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
        } else if (length == 127) {
            uint8_t extended[8];
            if (!recv_exact(socket, extended, sizeof(extended), error)) {
                return false;
            }
            length = 0;
            for (uint8_t byte : extended) {
                length = (length << 8) | byte;
            }
            if (length > kMaxDatagram) {
                *error = "a WebSocket carrier message is larger than 65535 bytes";
                return false;
            }
        }
        if (length > kMaxDatagram) {
            *error = "a WebSocket carrier message is larger than 65535 bytes";
            return false;
        }
        if (masked != expect_mask) {
            *error = masked ? "a server sent a masked frame" : "a client sent an unmasked frame";
            return false;
        }
        uint8_t mask_key[4]{};
        if (masked && !recv_exact(socket, mask_key, sizeof(mask_key), error)) {
            return false;
        }
        std::vector<uint8_t> body(static_cast<size_t>(length));
        if (!body.empty() && !recv_exact(socket, body.data(), body.size(), error)) {
            return false;
        }
        if (masked) {
            for (size_t i = 0; i < body.size(); ++i) {
                body[i] ^= mask_key[i % 4];
            }
        }
        switch (opcode) {
            case 0x8:  // close
                *error = "the carrier peer closed the connection";
                return false;
            case 0x9: {  // ping
                std::string pong_error;
                if (!ws_write_frame(socket, send_mask, write_mutex, 0xA, body.data(),
                                    body.size(), &pong_error)) {
                    *error = pong_error;
                    return false;
                }
                continue;
            }
            case 0xA:  // pong
                continue;
            case 0x2:  // binary
                if (!fin) {
                    *error = "fragmented WebSocket messages are not supported";
                    return false;
                }
                *payload = std::move(body);
                return true;
            case 0x0:
                *error = "WebSocket continuation frames are not supported";
                return false;
            default:
                *error = "the carrier only accepts WebSocket binary messages";
                return false;
        }
    }
}

class CarrierStream {
public:
    virtual ~CarrierStream() = default;
    virtual bool read_datagram(std::vector<uint8_t>* payload, std::string* error) = 0;
    virtual bool write_datagram(const uint8_t* data, size_t size, std::string* error) = 0;
    virtual CarrierTransport transport() const = 0;
};

class RawStream final : public CarrierStream {
public:
    explicit RawStream(SOCKET socket) : socket_(socket) {}
    CarrierTransport transport() const override { return CarrierTransport::kTcp; }

    bool read_datagram(std::vector<uint8_t>* payload, std::string* error) override {
        uint8_t size_bytes[4];
        if (!recv_exact(socket_, size_bytes, sizeof(size_bytes), error)) {
            return false;
        }
        const uint32_t size = static_cast<uint32_t>(size_bytes[0]) |
                              (static_cast<uint32_t>(size_bytes[1]) << 8) |
                              (static_cast<uint32_t>(size_bytes[2]) << 16) |
                              (static_cast<uint32_t>(size_bytes[3]) << 24);
        if (size > kMaxDatagram) {
            *error = "a raw carrier frame is larger than 65535 bytes";
            return false;
        }
        payload->resize(size);
        return size == 0 || recv_exact(socket_, payload->data(), size, error);
    }

    bool write_datagram(const uint8_t* data, size_t size, std::string* error) override {
        if (size > kMaxDatagram) {
            *error = "a raw carrier frame is larger than 65535 bytes";
            return false;
        }
        std::array<uint8_t, 4> header{
            static_cast<uint8_t>(size & 0xFF), static_cast<uint8_t>((size >> 8) & 0xFF),
            static_cast<uint8_t>((size >> 16) & 0xFF),
            static_cast<uint8_t>((size >> 24) & 0xFF)};
        std::lock_guard<std::mutex> lock(write_mutex_);
        return send_all(socket_, header.data(), header.size(), error) &&
               send_all(socket_, data, size, error);
    }

private:
    SOCKET socket_;
    std::mutex write_mutex_;
};

class WebSocketStream final : public CarrierStream {
public:
    WebSocketStream(SOCKET socket, bool client_side)
        : socket_(socket), client_side_(client_side) {}
    CarrierTransport transport() const override { return CarrierTransport::kWebSocket; }

    bool read_datagram(std::vector<uint8_t>* payload, std::string* error) override {
        return ws_read_frame(socket_, !client_side_, client_side_, &write_mutex_, payload,
                             error);
    }

    bool write_datagram(const uint8_t* data, size_t size, std::string* error) override {
        return ws_write_frame(socket_, client_side_, &write_mutex_, 0x2, data, size, error);
    }

private:
    SOCKET socket_;
    bool client_side_;
    std::mutex write_mutex_;
};

bool connect_tcp(const std::string& host, uint16_t port,
                 std::chrono::milliseconds timeout, SOCKET* out, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int resolved = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0 || addresses == nullptr) {
        *error = "cannot resolve " + host + ": " + socket_error(resolved);
        return false;
    }
    SOCKET chosen = INVALID_SOCKET;
    std::string last_error;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        SOCKET socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == INVALID_SOCKET) {
            last_error = socket_error(last_socket_error());
            continue;
        }
        u_long non_blocking = 1;
        ioctlsocket(socket, FIONBIO, &non_blocking);
        const int connected = connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (connected != 0) {
            const int code = last_socket_error();
            if (code == WSAEWOULDBLOCK || code == WSAEINPROGRESS) {
                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(socket, &writable);
                timeval wait{};
                wait.tv_sec = static_cast<long>(timeout.count() / 1000);
                wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
                const int ready = select(0, nullptr, &writable, nullptr, &wait);
                int so_error = 0;
                int so_size = sizeof(so_error);
                if (ready > 0 &&
                    getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                               &so_size) == 0 &&
                    so_error == 0) {
                    chosen = socket;
                } else {
                    last_error = socket_error(so_error != 0 ? so_error : WSAETIMEDOUT);
                }
            } else {
                last_error = socket_error(code);
            }
        } else {
            chosen = socket;
        }
        if (chosen != INVALID_SOCKET) {
            non_blocking = 0;
            ioctlsocket(chosen, FIONBIO, &non_blocking);
            break;
        }
        closesocket(socket);
    }
    freeaddrinfo(addresses);
    if (chosen == INVALID_SOCKET) {
        *error = last_error.empty() ? "could not connect to the carrier" : last_error;
        return false;
    }
    int nodelay = 1;
    setsockopt(chosen, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
               sizeof(nodelay));
    set_timeout(chosen, SO_RCVTIMEO, kIdleTimeoutMs);
    set_timeout(chosen, SO_SNDTIMEO, kIdleTimeoutMs);
    *out = chosen;
    return true;
}

bool websocket_handshake_server(SOCKET socket, const std::string& prefix,
                                std::string* error) {
    std::string headers;
    if (!read_http_headers(socket, prefix, &headers, error)) {
        return false;
    }
    std::string upgrade;
    std::string key;
    if (!header_value(headers, "Upgrade", &upgrade) ||
        lowercase(upgrade).find("websocket") == std::string::npos ||
        !header_value(headers, "Sec-WebSocket-Key", &key) || key.empty()) {
        *error = "the HTTP request is not a WebSocket upgrade";
        return false;
    }
    const std::string accept = carrier_websocket_accept(key);
    if (accept.empty()) {
        *error = "could not compute Sec-WebSocket-Accept";
        return false;
    }
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "Sec-WebSocket-Protocol: chast-carrier.v1\r\n"
        "\r\n";
    return send_all(socket, reinterpret_cast<const uint8_t*>(response.data()), response.size(),
                    error);
}

bool websocket_handshake_client(SOCKET socket, const std::string& host, uint16_t port,
                                std::string* error) {
    const std::string key_bytes = random_bytes(16);
    if (key_bytes.size() != 16) {
        *error = "could not generate a WebSocket key";
        return false;
    }
    const std::string nonce = base64_encode(
        reinterpret_cast<const uint8_t*>(key_bytes.data()), key_bytes.size());
    const std::string request =
        "GET /chast HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + nonce + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chast-carrier.v1\r\n"
        "\r\n";
    if (!send_all(socket, reinterpret_cast<const uint8_t*>(request.data()), request.size(),
                  error)) {
        return false;
    }
    std::string headers;
    if (!read_http_headers(socket, {}, &headers, error)) {
        return false;
    }
    const size_t line_end = headers.find("\r\n");
    if (line_end == std::string::npos || headers.substr(0, line_end).find(" 101 ") ==
                                             std::string::npos) {
        *error = "the carrier did not accept the WebSocket upgrade";
        return false;
    }
    std::string accept;
    if (!header_value(headers, "Sec-WebSocket-Accept", &accept) ||
        accept != carrier_websocket_accept(nonce)) {
        *error = "the carrier returned the wrong Sec-WebSocket-Accept";
        return false;
    }
    return true;
}

bool parse_ipv4(const std::string& text, sockaddr_in* address, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const bool any = text.empty() || text == "0.0.0.0" || text == "::";
    if (any) {
        hints.ai_flags = AI_PASSIVE;
    }
    addrinfo* addresses = nullptr;
    const char* name = any ? nullptr : text.c_str();
    const int resolved = getaddrinfo(name, nullptr, &hints, &addresses);
    if (resolved != 0 || addresses == nullptr) {
        *error = "cannot resolve " + (text.empty() ? std::string("0.0.0.0") : text) + ": " +
                 socket_error(resolved);
        return false;
    }
    *address = *reinterpret_cast<sockaddr_in*>(addresses->ai_addr);
    freeaddrinfo(addresses);
    return true;
}

}  // namespace

const char* carrier_transport_name(CarrierTransport transport) {
    return transport == CarrierTransport::kWebSocket ? "ws" : "tcp";
}

std::string carrier_websocket_accept(const std::string& key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string digest = sha1_digest(key + kGuid);
    if (digest.empty()) {
        return {};
    }
    return base64_encode(reinterpret_cast<const uint8_t*>(digest.data()), digest.size());
}

// ---------------------------------------------------------------------------
// Host listener
// ---------------------------------------------------------------------------

struct CarrierServer::Impl {
    CarrierServerConfig config;
    SOCKET listener = INVALID_SOCKET;
    sockaddr_in quic{};
    uint16_t bound_port = 0;
    std::thread accept_thread;
    std::atomic<bool> stopping{false};
    std::atomic<size_t> active{0};
    mutable std::mutex stats_mutex;
    CarrierServerStats stats;
    std::mutex sockets_mutex;
    std::set<SocketHolder*> sockets;
    std::mutex threads_mutex;
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<std::atomic<bool>>> finished;

    void log(const std::string& message) {
        if (config.log) {
            config.log("carrier: " + message);
        }
    }

    void track(SocketHolder* holder) {
        std::lock_guard<std::mutex> lock(sockets_mutex);
        sockets.insert(holder);
    }

    void untrack(SocketHolder* holder) {
        std::lock_guard<std::mutex> lock(sockets_mutex);
        sockets.erase(holder);
    }

    void close_sockets() {
        std::lock_guard<std::mutex> lock(sockets_mutex);
        for (SocketHolder* holder : sockets) {
            holder->close();
        }
    }

    void note_datagram(bool to_quic, size_t size) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        if (to_quic) {
            ++stats.datagrams_to_quic;
            stats.bytes_to_quic += size;
        } else {
            ++stats.datagrams_from_quic;
            stats.bytes_from_quic += size;
        }
    }

    void run_accept();
    void handle_connection(SOCKET client);
    void reap_finished();
};

void CarrierServer::Impl::run_accept() {
    while (!stopping.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        int peer_size = sizeof(peer);
        const SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client == INVALID_SOCKET) {
            if (!stopping.load(std::memory_order_relaxed)) {
                log("accept failed: " + socket_error(last_socket_error()));
            }
            break;
        }
        if (active.load() >= config.max_connections) {
            std::lock_guard<std::mutex> lock(stats_mutex);
            ++stats.rejected;
            closesocket(client);
            continue;
        }
        ++active;
        {
            std::lock_guard<std::mutex> lock(stats_mutex);
            ++stats.connections;
        }
        auto done_flag = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(threads_mutex);
            threads.emplace_back([this, client, done_flag] {
                handle_connection(client);
                --active;
                done_flag->store(true);
            });
            finished.push_back(done_flag);
        }
        reap_finished();
    }
}

void CarrierServer::Impl::reap_finished() {
    std::lock_guard<std::mutex> lock(threads_mutex);
    for (size_t i = 0; i < threads.size() && i < finished.size();) {
        if (finished[i]->load()) {
            if (threads[i].joinable()) {
                threads[i].join();
            }
            threads.erase(threads.begin() + static_cast<std::ptrdiff_t>(i));
            finished.erase(finished.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void CarrierServer::Impl::handle_connection(SOCKET client) {
    SocketHolder client_holder(client);
    track(&client_holder);
    set_timeout(client, SO_RCVTIMEO, kHandshakeTimeoutMs);
    set_timeout(client, SO_SNDTIMEO, kIdleTimeoutMs);

    std::string error;
    std::unique_ptr<CarrierStream> stream;
    uint8_t first[kRawMagicSize];
    if (!recv_exact(client, first, sizeof(first), &error)) {
        untrack(&client_holder);
        client_holder.close();
        return;
    }
    const std::string prefix(reinterpret_cast<char*>(first), sizeof(first));
    if (prefix.compare(0, 4, "GET ") == 0) {
        // The prefix holds the first eight bytes of the upgrade request;
        // handshake_server reads the rest and answers with 101 + accept.
        if (!websocket_handshake_server(client, prefix, &error)) {
            log("websocket handshake failed: " + error);
            untrack(&client_holder);
            client_holder.close();
            return;
        }
        stream = std::make_unique<WebSocketStream>(client, false);
    } else if (prefix == std::string(kRawMagic, kRawMagicSize)) {
        stream = std::make_unique<RawStream>(client);
    } else {
        untrack(&client_holder);
        client_holder.close();
        return;
    }

    SOCKET udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET) {
        log("UDP socket failed: " + socket_error(last_socket_error()));
        untrack(&client_holder);
        client_holder.close();
        return;
    }
    SocketHolder udp_holder(udp);
    track(&udp_holder);
    sockaddr_in quic_address = this->quic;
    if (connect(udp, reinterpret_cast<const sockaddr*>(&quic_address), sizeof(quic_address)) !=
        0) {
        log("carrier UDP connect failed: " + socket_error(last_socket_error()));
        untrack(&udp_holder);
        untrack(&client_holder);
        udp_holder.close();
        client_holder.close();
        return;
    }
    set_timeout(udp, SO_RCVTIMEO, kIdleTimeoutMs);
    set_timeout(udp, SO_SNDTIMEO, kIdleTimeoutMs);
    set_udp_buffers(udp);

    CarrierStream* carrier = stream.get();
    std::atomic<bool> done{false};
    auto stop_both = [&] {
        if (!done.exchange(true)) {
            client_holder.close();
            udp_holder.close();
        }
    };

    std::thread up([&] {
        std::vector<uint8_t> payload;
        std::string read_error;
        while (carrier->read_datagram(&payload, &read_error)) {
            if (payload.empty()) {
                continue;
            }
            const int sent = send(udp, reinterpret_cast<const char*>(payload.data()),
                                  static_cast<int>(payload.size()), 0);
            if (sent <= 0) {
                break;
            }
            note_datagram(true, payload.size());
        }
        stop_both();
    });
    std::thread down([&] {
        std::vector<uint8_t> buffer(kMaxDatagram);
        std::string write_error;
        for (;;) {
            const int got = recv(udp, reinterpret_cast<char*>(buffer.data()),
                                 static_cast<int>(buffer.size()), 0);
            if (got <= 0) {
                break;
            }
            if (!carrier->write_datagram(buffer.data(), static_cast<size_t>(got),
                                         &write_error)) {
                break;
            }
            note_datagram(false, static_cast<size_t>(got));
        }
        stop_both();
    });
    up.join();
    down.join();
    untrack(&udp_holder);
    untrack(&client_holder);
    stream.reset();
}

std::unique_ptr<CarrierServer> CarrierServer::create(const CarrierServerConfig& config,
                                                     std::string* error) {
    ensure_winsock();
    if (config.quic_port == 0) {
        *error = "the carrier needs a QUIC port";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    if (!parse_ipv4(config.quic_address, &impl->quic, error)) {
        return nullptr;
    }
    impl->quic.sin_port = htons(config.quic_port);
    impl->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl->listener == INVALID_SOCKET) {
        *error = "carrier listener: " + socket_error(last_socket_error());
        return nullptr;
    }
    BOOL exclusive = TRUE;
    setsockopt(impl->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in bind_address{};
    if (!parse_ipv4(config.bind_address, &bind_address, error)) {
        closesocket(impl->listener);
        return nullptr;
    }
    bind_address.sin_port = htons(config.port);
    if (bind(impl->listener, reinterpret_cast<const sockaddr*>(&bind_address),
             sizeof(bind_address)) != 0) {
        *error = "carrier bind " + config.bind_address + ":" + std::to_string(config.port) +
                 ": " + socket_error(last_socket_error());
        closesocket(impl->listener);
        return nullptr;
    }
    if (listen(impl->listener, 16) != 0) {
        *error = "carrier listen: " + socket_error(last_socket_error());
        closesocket(impl->listener);
        return nullptr;
    }
    sockaddr_in actual{};
    int actual_size = sizeof(actual);
    if (getsockname(impl->listener, reinterpret_cast<sockaddr*>(&actual), &actual_size) != 0) {
        *error = "carrier getsockname: " + socket_error(last_socket_error());
        closesocket(impl->listener);
        return nullptr;
    }
    impl->bound_port = ntohs(actual.sin_port);
    impl->accept_thread = std::thread([raw = impl.get()] { raw->run_accept(); });
    return std::unique_ptr<CarrierServer>(new CarrierServer(std::move(impl)));
}

CarrierServer::CarrierServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CarrierServer::~CarrierServer() {
    stop();
}

uint16_t CarrierServer::port() const {
    return impl_->bound_port;
}

void CarrierServer::stop() {
    if (!impl_ || impl_->stopping.exchange(true)) {
        return;
    }
    if (impl_->listener != INVALID_SOCKET) {
        closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
    }
    impl_->close_sockets();
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(impl_->threads_mutex);
        threads.swap(impl_->threads);
        impl_->finished.clear();
    }
    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

CarrierServerStats CarrierServer::stats() const {
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    CarrierServerStats snapshot = impl_->stats;
    snapshot.active = impl_->active.load();
    return snapshot;
}

// ---------------------------------------------------------------------------
// Viewer bridge
// ---------------------------------------------------------------------------

struct CarrierBridge::Impl {
    CarrierBridgeConfig config;
    SOCKET tcp = INVALID_SOCKET;
    SOCKET udp = INVALID_SOCKET;
    std::unique_ptr<CarrierStream> stream;
    std::thread uplink;
    std::thread downlink;
    std::atomic<bool> stopping{false};
    std::atomic<bool> done{false};
    std::mutex client_mutex;
    sockaddr_in client{};
    bool has_client = false;
    mutable std::mutex error_mutex;
    std::string error;
    mutable std::mutex stats_mutex;
    CarrierBridgeStats stats;
    uint16_t local_port = 0;

    void note_to_carrier(size_t size) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        ++stats.to_carrier_datagrams;
        stats.to_carrier_bytes += size;
    }

    void note_from_carrier(size_t size) {
        std::lock_guard<std::mutex> lock(stats_mutex);
        ++stats.from_carrier_datagrams;
        stats.from_carrier_bytes += size;
    }

    void shutdown() {
        if (done.exchange(true)) {
            return;
        }
        if (tcp != INVALID_SOCKET) {
            closesocket(tcp);
            tcp = INVALID_SOCKET;
        }
        if (udp != INVALID_SOCKET) {
            closesocket(udp);
            udp = INVALID_SOCKET;
        }
    }
};

std::unique_ptr<CarrierBridge> CarrierBridge::open(const CarrierBridgeConfig& config,
                                                   std::string* error) {
    ensure_winsock();
    if (config.host.empty() || config.port == 0) {
        *error = "the carrier needs a host and a port";
        return nullptr;
    }
    const bool shared = !config.host_id.empty();
    if (shared && config.token.empty()) {
        *error = "a shared carrier needs the token issued by the host lookup";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    if (!connect_tcp(config.host, config.port, config.connect_timeout, &impl->tcp, error)) {
        return nullptr;
    }
    if (config.transport == CarrierTransport::kWebSocket) {
        if (!websocket_handshake_client(impl->tcp, config.host, config.port, error)) {
            closesocket(impl->tcp);
            return nullptr;
        }
        impl->stream = std::make_unique<WebSocketStream>(impl->tcp, true);
        if (shared) {
            // The shared carrier reads its hello as the first binary message;
            // there is no separate line protocol on the WebSocket path.
            const std::string hello = shared_hello_json(config.host_id, config.token);
            if (!impl->stream->write_datagram(reinterpret_cast<const uint8_t*>(hello.data()),
                                              hello.size(), error)) {
                closesocket(impl->tcp);
                return nullptr;
            }
        }
    } else {
        const std::string magic = shared ? std::string(kSharedMagic, kSharedMagicSize)
                                         : std::string(kRawMagic, kRawMagicSize);
        if (!send_all(impl->tcp, reinterpret_cast<const uint8_t*>(magic.data()), magic.size(),
                      error)) {
            closesocket(impl->tcp);
            return nullptr;
        }
        if (shared) {
            const std::string hello = shared_hello_json(config.host_id, config.token) + "\n";
            if (!send_all(impl->tcp, reinterpret_cast<const uint8_t*>(hello.data()),
                          hello.size(), error)) {
                closesocket(impl->tcp);
                return nullptr;
            }
        }
        impl->stream = std::make_unique<RawStream>(impl->tcp);
    }

    impl->udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->udp == INVALID_SOCKET) {
        *error = "carrier bridge UDP: " + socket_error(last_socket_error());
        closesocket(impl->tcp);
        return nullptr;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    if (bind(impl->udp, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        *error = "carrier bridge bind: " + socket_error(last_socket_error());
        closesocket(impl->udp);
        closesocket(impl->tcp);
        return nullptr;
    }
    sockaddr_in actual{};
    int actual_size = sizeof(actual);
    getsockname(impl->udp, reinterpret_cast<sockaddr*>(&actual), &actual_size);
    impl->local_port = ntohs(actual.sin_port);
    set_timeout(impl->udp, SO_RCVTIMEO, kIdleTimeoutMs);
    set_timeout(impl->udp, SO_SNDTIMEO, kIdleTimeoutMs);
    set_udp_buffers(impl->udp);

    Impl* raw = impl.get();
    impl->uplink = std::thread([raw] {
        std::vector<uint8_t> buffer(kMaxDatagram);
        for (;;) {
            sockaddr_in source{};
            int source_size = sizeof(source);
            const int got = recvfrom(raw->udp, reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()), 0,
                                     reinterpret_cast<sockaddr*>(&source), &source_size);
            if (got <= 0) {
                break;
            }
            if (source.sin_family != AF_INET ||
                source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(raw->client_mutex);
                raw->client = source;
                raw->has_client = true;
            }
            std::string write_error;
            if (!raw->stream->write_datagram(buffer.data(), static_cast<size_t>(got),
                                             &write_error)) {
                std::lock_guard<std::mutex> lock(raw->error_mutex);
                raw->error = write_error;
                break;
            }
            raw->note_to_carrier(static_cast<size_t>(got));
        }
        raw->shutdown();
    });
    impl->downlink = std::thread([raw] {
        std::vector<uint8_t> payload;
        std::string read_error;
        while (raw->stream->read_datagram(&payload, &read_error)) {
            if (payload.empty()) {
                continue;
            }
            sockaddr_in target{};
            bool have_target = false;
            {
                std::lock_guard<std::mutex> lock(raw->client_mutex);
                if (raw->has_client) {
                    target = raw->client;
                    have_target = true;
                }
            }
            if (!have_target) {
                continue;
            }
            const int sent = sendto(raw->udp, reinterpret_cast<const char*>(payload.data()),
                                    static_cast<int>(payload.size()), 0,
                                    reinterpret_cast<const sockaddr*>(&target),
                                    sizeof(target));
            if (sent <= 0) {
                break;
            }
            raw->note_from_carrier(payload.size());
        }
        {
            std::lock_guard<std::mutex> lock(raw->error_mutex);
            if (raw->error.empty()) {
                raw->error = read_error;
            }
        }
        raw->shutdown();
    });
    return std::unique_ptr<CarrierBridge>(new CarrierBridge(std::move(impl)));
}

CarrierBridge::CarrierBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

CarrierBridge::~CarrierBridge() {
    stop();
}

uint16_t CarrierBridge::local_port() const {
    return impl_->local_port;
}

CarrierTransport CarrierBridge::transport() const {
    return impl_->config.transport;
}

CarrierBridgeStats CarrierBridge::stats() const {
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    return impl_->stats;
}

std::string CarrierBridge::error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    return impl_->error;
}

void CarrierBridge::stop() {
    if (!impl_) {
        return;
    }
    impl_->shutdown();
    if (impl_->uplink.joinable()) {
        impl_->uplink.join();
    }
    if (impl_->downlink.joinable()) {
        impl_->downlink.join();
    }
}

}  // namespace chast
