// Winsock 2 must precede windows.h, which chast/quic.h pulls in through
// chast/error.h; otherwise the SDK sees winsock 1 first and ws2tcpip.h fails.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chast/port_forward_relay.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace chast {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kDialTimeout = std::chrono::seconds(5);
constexpr size_t kRelayChunk = kMaxPortForwardChunkBytes;

bool would_block() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
           error == WSAEALREADY;
}

std::string endpoint(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

}  // namespace

struct PortForwardRelay::Conn {
    PortForwardHandle handle = 0;
    std::string host;
    uint16_t port = 0;
    SOCKET socket = INVALID_SOCKET;
    bool dialing = true;
    bool accepted = false;
    Clock::time_point dial_deadline;
    // TCP bytes waiting for transport send capacity, and transport bytes
    // waiting for the local socket to accept them. Keeping one chunk on this
    // side is how backpressure reaches the other endpoint: while either queue
    // is full the corresponding socket is not read.
    std::vector<uint8_t> from_tcp;
    std::vector<uint8_t> from_quic;
    size_t from_quic_offset = 0;
    bool tcp_eof = false;
    bool quic_eof = false;
};

PortForwardRelay::PortForwardRelay(QuicSession& session) : session_(session) {}

PortForwardRelay::~PortForwardRelay() {
    for (const auto& conn : live_) {
        close_socket(*conn);
    }
}

void PortForwardRelay::close_socket(Conn& conn) const {
    if (conn.socket != INVALID_SOCKET) {
        closesocket(conn.socket);
        conn.socket = INVALID_SOCKET;
    }
}

std::vector<std::string> PortForwardRelay::pump() {
    std::vector<std::string> events;
    PortForwardOpen open;
    while (session_.take_port_forward_open(open)) {
        start_dial(open, events);
    }
    for (size_t i = 0; i < live_.size();) {
        Conn& conn = *live_[i];
        const Outcome outcome = drive(conn, events);
        if (outcome == Outcome::Keep) {
            ++i;
            continue;
        }
        close_socket(conn);
        if (outcome == Outcome::Failed) {
            ++failures_;
        }
        // Failed: aborting both directions is how the viewer learns the local
        // side is gone (a dial failure already rejected the connection, so it
        // is a no-op there). Done: both streams ended cleanly and the handle is
        // free to forget once the last byte has been relayed.
        session_.close_port_forward(conn.handle);
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return events;
}

void PortForwardRelay::start_dial(const PortForwardOpen& open,
                                  std::vector<std::string>& events) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(open.port);
    if (getaddrinfo(open.host.c_str(), service.c_str(), &hints, &results) != 0 ||
        !results) {
        session_.reject_port_forward_connection(open.handle);
        events.push_back("port forward: cannot resolve " + endpoint(open.host, open.port));
        ++failures_;
        return;
    }
    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* it = results; it; it = it->ai_next) {
        sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(sock, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        if (::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        if (would_block()) {
            break;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(results);
    if (sock == INVALID_SOCKET) {
        session_.reject_port_forward_connection(open.handle);
        events.push_back("port forward: cannot connect to " +
                         endpoint(open.host, open.port));
        ++failures_;
        return;
    }
    auto conn = std::make_unique<Conn>();
    conn->handle = open.handle;
    conn->host = open.host;
    conn->port = open.port;
    conn->socket = sock;
    conn->dial_deadline = Clock::now() + kDialTimeout;
    live_.push_back(std::move(conn));
    events.push_back("port forward: dialing " + endpoint(open.host, open.port));
}

PortForwardRelay::Outcome PortForwardRelay::drive(Conn& conn,
                                                  std::vector<std::string>& events) {
    if (conn.dialing) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(conn.socket, &writable);
        timeval immediate{0, 0};
        const int ready = select(0, nullptr, &writable, nullptr, &immediate);
        if (ready == SOCKET_ERROR) {
            events.push_back("port forward: select failed for " +
                             endpoint(conn.host, conn.port));
            return Outcome::Failed;
        }
        if (ready == 0) {
            if (Clock::now() >= conn.dial_deadline) {
                session_.reject_port_forward_connection(conn.handle);
                events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                                 " did not answer the dial");
                return Outcome::Failed;
            }
            return Outcome::Keep;
        }
        int error = 0;
        int length = sizeof(error);
        if (getsockopt(conn.socket, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&error), &length) != 0 ||
            error != 0) {
            session_.reject_port_forward_connection(conn.handle);
            events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                             " refused the connection");
            return Outcome::Failed;
        }
        conn.dialing = false;
        const Error accepted = session_.accept_port_forward_connection(conn.handle);
        if (!accepted.ok()) {
            events.push_back("port forward: cannot accept " +
                             endpoint(conn.host, conn.port) + ": " +
                             accepted.to_string());
            return Outcome::Failed;
        }
        conn.accepted = true;
        ++accepted_;
        events.push_back("port forward: connected " + endpoint(conn.host, conn.port));
    }

    // Transport -> local socket. A partial write keeps the rest here; the
    // local endpoint's receive window is the backpressure for the viewer.
    while (conn.from_quic_offset < conn.from_quic.size()) {
        const size_t remaining = conn.from_quic.size() - conn.from_quic_offset;
        const size_t span = remaining > (1u << 20) ? (1u << 20) : remaining;
        const int sent = ::send(
            conn.socket,
            reinterpret_cast<const char*>(conn.from_quic.data() + conn.from_quic_offset),
            static_cast<int>(span), 0);
        if (sent > 0) {
            conn.from_quic_offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent == SOCKET_ERROR && would_block()) {
            break;
        }
        events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                         " stopped reading");
        return Outcome::Failed;
    }
    if (conn.from_quic_offset == conn.from_quic.size()) {
        conn.from_quic.clear();
        conn.from_quic_offset = 0;
        if (!conn.quic_eof) {
            std::vector<uint8_t> chunk;
            const Error next = session_.receive_port_forward_data(
                conn.handle, chunk, std::chrono::milliseconds(0));
            if (next.ok()) {
                conn.from_quic = std::move(chunk);
            } else if (next.status() != Status::WaitTimeout) {
                // The viewer half-closed or the connection broke; pass EOF on
                // and keep relaying the other direction until it ends too.
                conn.quic_eof = true;
                shutdown(conn.socket, SD_SEND);
            }
        }
    }

    // Local socket -> transport. Only read when the previous chunk made it
    // into the send queue, so a fast local endpoint cannot build an unbounded
    // buffer here.
    if (!conn.from_tcp.empty()) {
        const Error sent = session_.send_port_forward_data(
            conn.handle, conn.from_tcp.data(), conn.from_tcp.size(),
            std::chrono::milliseconds(0));
        if (sent.ok()) {
            conn.from_tcp.clear();
        } else if (sent.status() != Status::WaitTimeout) {
            events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                             " send failed: " + sent.to_string());
            return Outcome::Failed;
        }
    }
    if (conn.from_tcp.empty() && !conn.tcp_eof) {
        uint8_t buffer[kRelayChunk];
        const int got = ::recv(conn.socket, reinterpret_cast<char*>(buffer),
                               static_cast<int>(sizeof(buffer)), 0);
        if (got > 0) {
            const Error sent = session_.send_port_forward_data(
                conn.handle, buffer, static_cast<size_t>(got),
                std::chrono::milliseconds(0));
            if (sent.ok()) {
                // Queued completely.
            } else if (sent.status() == Status::WaitTimeout) {
                conn.from_tcp.assign(buffer, buffer + got);
            } else {
                events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                                 " send failed: " + sent.to_string());
                return Outcome::Failed;
            }
        } else if (got == 0) {
            conn.tcp_eof = true;
            const Error finished = session_.finish_port_forward(conn.handle);
            if (!finished.ok()) {
                events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                                 " ended: " + finished.to_string());
                return Outcome::Failed;
            }
        } else if (!would_block()) {
            events.push_back("port forward: " + endpoint(conn.host, conn.port) +
                             " reset the connection");
            return Outcome::Failed;
        }
    }

    if (conn.tcp_eof && conn.quic_eof && conn.from_tcp.empty() &&
        conn.from_quic.empty()) {
        events.push_back("port forward: closed " + endpoint(conn.host, conn.port));
        return Outcome::Done;
    }
    return Outcome::Keep;
}

}  // namespace chast
