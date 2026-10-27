// Winsock 2 must precede windows.h, which chast/quic.h pulls in through
// chast/error.h; otherwise the SDK sees winsock 1 first and ws2tcpip.h fails.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "chast/port_forward_client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace chast {

namespace {

constexpr size_t kRelayChunk = kMaxPortForwardChunkBytes;
constexpr size_t kMaxConnections = kMaxPortForwardConnections;

bool would_block() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
           error == WSAEALREADY;
}

std::string endpoint(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

}  // namespace

struct PortForwardClient::Impl {
    explicit Impl(QuicSession& quic) : session(quic) {}

    struct Conn {
        SOCKET socket = INVALID_SOCKET;
        uint32_t connection_id = 0;
        PortForwardHandle handle = 0;
        bool attached = false;
        // One chunk waiting for transport capacity and the transport bytes
        // waiting for the local socket. Keeping them bounded is what pushes
        // backpressure onto the endpoint that is reading too slowly.
        std::vector<uint8_t> from_tcp;
        std::vector<uint8_t> from_quic;
        size_t from_quic_offset = 0;
        bool tcp_eof = false;
        bool quic_eof = false;
    };

    enum class Step { Idle, Request, WaitResult, Accept, Closed };
    enum class Outcome { Keep, Done, Failed };

    void close_listener() {
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
    }

    void close_socket(Conn& conn) const {
        if (conn.socket != INVALID_SOCKET) {
            closesocket(conn.socket);
            conn.socket = INVALID_SOCKET;
        }
    }

    Outcome drive(Conn& conn, std::vector<std::string>& events) {
        if (!conn.attached) {
            const Error attached = session.wait_port_forward_connection(
                conn.handle, std::chrono::milliseconds(0));
            if (attached.status() == Status::WaitTimeout) {
                return Outcome::Keep;
            }
            if (!attached.ok()) {
                events.push_back("port forward: connection " +
                                 std::to_string(conn.connection_id) + " failed: " +
                                 attached.to_string());
                return Outcome::Failed;
            }
            conn.attached = true;
            ++accepted;
            events.push_back("port forward: forwarding connection " +
                             std::to_string(conn.connection_id));
        }

        // Transport -> local socket. A partial write keeps the rest here; the
        // host stops reading when this side falls behind.
        while (conn.from_quic_offset < conn.from_quic.size()) {
            const size_t remaining = conn.from_quic.size() - conn.from_quic_offset;
            const size_t span = remaining > (1u << 20) ? (1u << 20) : remaining;
            const int sent = ::send(
                conn.socket,
                reinterpret_cast<const char*>(conn.from_quic.data() +
                                              conn.from_quic_offset),
                static_cast<int>(span), 0);
            if (sent > 0) {
                conn.from_quic_offset += static_cast<size_t>(sent);
                down += static_cast<uint64_t>(sent);
                continue;
            }
            if (sent == SOCKET_ERROR && would_block()) {
                break;
            }
            events.push_back("port forward: connection " +
                             std::to_string(conn.connection_id) +
                             " was closed by the local application");
            return Outcome::Failed;
        }
        if (conn.from_quic_offset == conn.from_quic.size()) {
            conn.from_quic.clear();
            conn.from_quic_offset = 0;
            if (!conn.quic_eof) {
                std::vector<uint8_t> chunk;
                const Error next = session.receive_port_forward_data(
                    conn.handle, chunk, std::chrono::milliseconds(0));
                if (next.ok()) {
                    if (!chunk.empty()) {
                        conn.from_quic = std::move(chunk);
                    }
                } else if (next.status() != Status::WaitTimeout) {
                    // The host half-closed; pass EOF on and keep relaying the
                    // other direction until it ends too.
                    conn.quic_eof = true;
                    shutdown(conn.socket, SD_SEND);
                }
            }
        }

        // Local socket -> transport. Only read when the previous chunk made it
        // into the send queue, so a fast local application cannot build an
        // unbounded buffer here.
        if (!conn.from_tcp.empty()) {
            const Error sent = session.send_port_forward_data(
                conn.handle, conn.from_tcp.data(), conn.from_tcp.size(),
                std::chrono::milliseconds(0));
            if (sent.ok()) {
                up += conn.from_tcp.size();
                conn.from_tcp.clear();
            } else if (sent.status() != Status::WaitTimeout) {
                events.push_back("port forward: connection " +
                                 std::to_string(conn.connection_id) + " send failed: " +
                                 sent.to_string());
                return Outcome::Failed;
            }
        }
        if (conn.from_tcp.empty() && !conn.tcp_eof) {
            uint8_t buffer[kRelayChunk];
            const int got = ::recv(conn.socket, reinterpret_cast<char*>(buffer),
                                   static_cast<int>(sizeof(buffer)), 0);
            if (got > 0) {
                const Error sent = session.send_port_forward_data(
                    conn.handle, buffer, static_cast<size_t>(got),
                    std::chrono::milliseconds(0));
                if (sent.ok()) {
                    up += static_cast<uint64_t>(got);
                } else if (sent.status() == Status::WaitTimeout) {
                    conn.from_tcp.assign(buffer, buffer + got);
                } else {
                    events.push_back("port forward: connection " +
                                     std::to_string(conn.connection_id) +
                                     " send failed: " + sent.to_string());
                    return Outcome::Failed;
                }
            } else if (got == 0) {
                conn.tcp_eof = true;
                const Error finished = session.finish_port_forward(conn.handle);
                if (!finished.ok()) {
                    events.push_back("port forward: connection " +
                                     std::to_string(conn.connection_id) +
                                     " ended: " + finished.to_string());
                    return Outcome::Failed;
                }
            } else if (!would_block()) {
                events.push_back("port forward: connection " +
                                 std::to_string(conn.connection_id) +
                                 " reset the connection");
                return Outcome::Failed;
            }
        }

        if (conn.tcp_eof && conn.quic_eof && conn.from_tcp.empty() &&
            conn.from_quic.empty()) {
            events.push_back("port forward: connection " +
                             std::to_string(conn.connection_id) + " closed");
            return Outcome::Done;
        }
        return Outcome::Keep;
    }

    QuicSession& session;
    Step step = Step::Idle;
    std::string host;
    uint16_t remote_port = 0;
    uint16_t local_port = 0;
    SOCKET listener = INVALID_SOCKET;
    uint32_t forward_id = 1;
    uint32_t next_connection_id = 1;
    std::vector<Conn> live;
    uint64_t accepted = 0;
    uint64_t failures = 0;
    uint64_t up = 0;
    uint64_t down = 0;
    std::string error;
    bool wsa = false;
};

PortForwardClient::PortForwardClient(QuicSession& session)
    : impl_(std::make_unique<Impl>(session)) {
    WSADATA data{};
    impl_->wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

PortForwardClient::~PortForwardClient() {
    stop();
    if (impl_->wsa) {
        WSACleanup();
        impl_->wsa = false;
    }
}

Error PortForwardClient::start(const std::string& remote_host, uint16_t remote_port,
                               uint16_t local_port, uint16_t* bound_port) {
    Impl& impl = *impl_;
    if (impl.step != Impl::Step::Idle && impl.step != Impl::Step::Closed) {
        return Error(Status::InvalidCall, "a port forward is already running");
    }
    if (!impl.wsa) {
        WSADATA data{};
        impl.wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!impl.wsa) {
            return Error(Status::Unavailable, "Winsock is not available");
        }
    }
    if (remote_host.empty() || remote_port == 0) {
        return Error(Status::InvalidCall, "the target is not a valid host:port pair");
    }
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return Error(Status::Unavailable, "cannot create the port forward listener");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(local_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
        closesocket(listener);
        return Error(Status::Unavailable, "cannot bind 127.0.0.1:" +
                                              std::to_string(local_port));
    }
    if (::listen(listener, 8) != 0) {
        closesocket(listener);
        return Error(Status::Unavailable, "cannot listen for forwarded connections");
    }
    u_long nonblocking = 1;
    if (ioctlsocket(listener, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(listener);
        return Error(Status::Unavailable, "cannot make the listener non-blocking");
    }
    int length = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(listener);
        return Error(Status::Unavailable, "cannot read the listener's port");
    }
    impl.host = remote_host;
    impl.remote_port = remote_port;
    impl.local_port = ntohs(address.sin_port);
    impl.listener = listener;
    impl.step = Impl::Step::Request;
    impl.error.clear();
    if (bound_port) {
        *bound_port = impl.local_port;
    }
    return Error::success();
}

void PortForwardClient::stop() {
    Impl& impl = *impl_;
    const bool had_target = impl.step == Impl::Step::Request ||
                            impl.step == Impl::Step::WaitResult ||
                            impl.step == Impl::Step::Accept;
    impl.close_listener();
    for (Impl::Conn& conn : impl.live) {
        impl.close_socket(conn);
        if (conn.handle != 0) {
            impl.session.close_port_forward(conn.handle);
        }
    }
    impl.live.clear();
    if (had_target) {
        impl.session.send_port_forward_stop(impl.forward_id,
                                            PortForwardCode::Cancelled,
                                            "the viewer stopped port forwarding");
    }
    impl.step = Impl::Step::Closed;
}

std::vector<std::string> PortForwardClient::pump() {
    Impl& impl = *impl_;
    std::vector<std::string> events;
    if (impl.step == Impl::Step::Request) {
        const Error requested = impl.session.request_port_forward(
            impl.forward_id, impl.host, impl.remote_port);
        if (!requested.ok()) {
            impl.error = requested.to_string();
            impl.close_listener();
            impl.step = Impl::Step::Closed;
            events.push_back("port forward: " + impl.error);
            return events;
        }
        impl.step = Impl::Step::WaitResult;
    }
    if (impl.step == Impl::Step::WaitResult) {
        PortForwardResult result;
        while (impl.session.take_port_forward_result(result)) {
            if (result.forward_id != impl.forward_id) {
                continue;
            }
            if (result.code != PortForwardCode::Ok) {
                impl.error = result.reason.empty()
                                 ? "the host refused the forward target"
                                 : result.reason;
                impl.close_listener();
                impl.step = Impl::Step::Closed;
                events.push_back("port forward: " + impl.error);
                return events;
            }
            impl.step = Impl::Step::Accept;
            events.push_back("port forward: forwarded " +
                             endpoint(impl.host, impl.remote_port) +
                             " over 127.0.0.1:" + std::to_string(impl.local_port));
            break;
        }
    }
    if (impl.step == Impl::Step::Accept) {
        while (impl.live.size() < kMaxConnections) {
            sockaddr_in peer{};
            int length = sizeof(peer);
            const SOCKET socket =
                ::accept(impl.listener, reinterpret_cast<sockaddr*>(&peer), &length);
            if (socket == INVALID_SOCKET) {
                if (!would_block()) {
                    events.push_back("port forward: the listener failed");
                    impl.close_listener();
                    impl.step = Impl::Step::Closed;
                    impl.error = "the port forward listener failed";
                }
                break;
            }
            u_long nonblocking = 1;
            if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                closesocket(socket);
                continue;
            }
            Impl::Conn conn;
            conn.socket = socket;
            conn.connection_id = impl.next_connection_id++;
            const Error opened = impl.session.open_port_forward_connection(
                impl.forward_id, conn.connection_id, conn.handle);
            if (!opened.ok()) {
                closesocket(socket);
                ++impl.failures;
                events.push_back("port forward: cannot open a connection: " +
                                 opened.to_string());
                continue;
            }
            events.push_back("port forward: accepted connection " +
                             std::to_string(conn.connection_id));
            impl.live.push_back(std::move(conn));
        }
    }
    for (size_t i = 0; i < impl.live.size();) {
        Impl::Conn& conn = impl.live[i];
        const Impl::Outcome outcome = impl.drive(conn, events);
        if (outcome == Impl::Outcome::Keep) {
            ++i;
            continue;
        }
        impl.close_socket(conn);
        if (outcome == Impl::Outcome::Failed) {
            ++impl.failures;
        }
        impl.session.close_port_forward(conn.handle);
        impl.live.erase(impl.live.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return events;
}

bool PortForwardClient::running() const {
    const Impl& impl = *impl_;
    return impl.step == Impl::Step::Request || impl.step == Impl::Step::WaitResult ||
           impl.step == Impl::Step::Accept;
}

std::string PortForwardClient::remote_host() const { return impl_->host; }

uint16_t PortForwardClient::remote_port() const { return impl_->remote_port; }

uint16_t PortForwardClient::local_port() const { return impl_->local_port; }

uint64_t PortForwardClient::connections() const { return impl_->accepted; }

uint64_t PortForwardClient::failures() const { return impl_->failures; }

uint64_t PortForwardClient::bytes_up() const { return impl_->up; }

uint64_t PortForwardClient::bytes_down() const { return impl_->down; }

std::string PortForwardClient::error() const { return impl_->error; }

}  // namespace chast
