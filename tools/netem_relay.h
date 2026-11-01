#pragma once

// Userspace weak-link relay for the QUIC harness.
//
// QUIC runs over UDP, so a transparent relay between the viewer and the host
// is enough to give the transport a path with delay, jitter, loss and a
// bandwidth bottleneck. Everything is applied to real datagrams: MsQuic's
// congestion controller, loss detection and the adaptive bitrate controller
// see the same signals they would see on a bad network.
//
// The impairment is a time window: clean before --degrade-at, impaired until
// --recover-at, clean again afterwards. That is what makes recovery testable
// without touching either process.
//
// Two users share this class: chast-netem wraps it in a process with the full
// option surface, and chast-quic's automated weak-network check runs it in a
// thread between its in-process host and viewer. The includer must include
// <winsock2.h> before <windows.h>; this header does it in that order.
//
// Windows-only (Winsock), IPv4-only, one relay per session.

// windows.h's min/max macros would break every std::max/std::clamp in this
// header and in the includer's translation unit.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace chast_tools {
namespace netem {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

constexpr size_t kMaxClients = 8;
constexpr size_t kMaxDatagram = 65536;
constexpr int kStatsIntervalMs = 5000;

struct Options {
    std::string bind_address = "127.0.0.1";
    std::string forward_host = "127.0.0.1";
    uint16_t listen_port = 0;
    uint16_t forward_port = 0;
    double delay_ms = 0.0;
    double jitter_ms = 0.0;
    double loss_percent = 0.0;
    uint32_t rate_kbps = 0;
    double queue_ms = 500.0;
    double degrade_at = 0.0;
    double recover_at = 0.0;  // 0 = never recover
    double seconds = 30.0;
    uint32_t seed = 1;
    bool quiet = false;
    // The phase times above normally count from start(). An automated harness
    // starts the relay before the session, so it can instead count from the
    // first datagram the viewer sends: that keeps the window anchored to the
    // session even when capture and encoder setup take a while.
    bool start_on_first_packet = false;
};

struct DirectionStats {
    uint64_t received = 0;
    uint64_t forwarded = 0;
    uint64_t loss_drops = 0;
    uint64_t queue_drops = 0;
    uint64_t send_errors = 0;
    uint64_t bytes = 0;
    double max_queue_ms = 0.0;
};

struct RelayStats {
    DirectionStats viewer_to_host;
    DirectionStats host_to_viewer;
};

inline std::string endpoint_text(const sockaddr_in& address) {
    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    return std::string(host) + ":" + std::to_string(ntohs(address.sin_port));
}

inline bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

inline SOCKET open_socket() {
    SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    // A keyframe burst can be hundreds of kilobytes in a few milliseconds.
    // The default 64 KiB receive buffer would silently drop part of it and
    // the tool would then report loss the link never had.
    const int buffer_bytes = 16 * 1024 * 1024;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&buffer_bytes), sizeof(buffer_bytes));
    setsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&buffer_bytes), sizeof(buffer_bytes));
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
        closesocket(socket_handle);
        return INVALID_SOCKET;
    }
    return socket_handle;
}

inline bool bind_socket(SOCKET socket_handle, const std::string& address, uint16_t port) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
        return false;
    }
    return bind(socket_handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
}

enum class Direction { ViewerToHost, HostToViewer };

struct Pending {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in destination{};
    Direction direction = Direction::ViewerToHost;
    std::vector<uint8_t> data;
};

struct Client {
    sockaddr_in address{};
    SOCKET upstream = INVALID_SOCKET;
};

class Relay {
public:
    explicit Relay(const Options& options)
        : options_(options),
          random_(options.seed),
          loss_(options.loss_percent / 100.0) {}

    ~Relay() {
        close();
    }

    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    bool start(std::string* error) {
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
            *error = "WSAStartup failed";
            return false;
        }
        wsa_started_ = true;
        // The shaper and the delay queue schedule in milliseconds; without the
        // higher timer resolution select() can oversleep by a whole tick.
        timeBeginPeriod(1);
        timer_period_ = true;

        if (inet_pton(AF_INET, options_.forward_host.c_str(), &forward_address_.sin_addr) != 1) {
            *error = "invalid --forward-host " + options_.forward_host;
            close();
            return false;
        }
        forward_address_.sin_family = AF_INET;
        forward_address_.sin_port = htons(options_.forward_port);

        listener_ = open_socket();
        if (listener_ == INVALID_SOCKET) {
            *error = "cannot create the listening socket: " + std::to_string(WSAGetLastError());
            close();
            return false;
        }
        if (!bind_socket(listener_, options_.bind_address, options_.listen_port)) {
            *error = "cannot bind " + options_.bind_address + ":" +
                     std::to_string(options_.listen_port) + ": " +
                     std::to_string(WSAGetLastError());
            close();
            return false;
        }
        start_ = Clock::now();
        std::printf(
            "netem: %s:%u -> %s, delay %.0f+0..%.0f ms, loss %.1f%%, rate %u kbps, "
            "impairment %.1f..%.1f s, %.1f s\n",
            options_.bind_address.c_str(), options_.listen_port,
            endpoint_text(forward_address_).c_str(), options_.delay_ms, options_.jitter_ms,
            options_.loss_percent, options_.rate_kbps, options_.degrade_at,
            options_.recover_at == 0.0 ? options_.seconds : options_.recover_at,
            options_.seconds);
        std::fflush(stdout);
        was_impaired_ = is_impaired(0.0);
        std::printf("netem: link %s\n", was_impaired_ ? "degraded" : "clean");
        std::fflush(stdout);
        return true;
    }

    std::atomic<bool>& stop_flag() { return stop_; }

    void request_stop() { stop_.store(true); }

    bool run() {
        auto last_stats = Clock::now();
        while (!stop_.load()) {
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - start_).count();
            if (elapsed >= options_.seconds) {
                break;
            }
            update_phase(elapsed);
            send_due(now);
            if (!options_.quiet && now - last_stats >= std::chrono::milliseconds(kStatsIntervalMs)) {
                print_stats(elapsed, now);
                last_stats = now;
            }

            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            for (const Client& client : clients_) {
                FD_SET(client.upstream, &readable);
            }
            int wait_ms = 20;
            if (!queue_.empty()) {
                const auto left =
                    std::chrono::duration_cast<Milliseconds>(queue_.begin()->first - now);
                wait_ms = (std::clamp)(static_cast<int>(std::ceil(left.count())), 0, 20);
            }
            timeval timeout{wait_ms / 1000, (wait_ms % 1000) * 1000};
            const int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) {
                std::fprintf(stderr, "netem: select failed: %d\n", WSAGetLastError());
                return false;
            }
            if (ready == 0) {
                continue;
            }
            if (FD_ISSET(listener_, &readable)) {
                receive_viewer();
            }
            for (Client& client : clients_) {
                if (FD_ISSET(client.upstream, &readable)) {
                    receive_host(client);
                }
            }
        }
        send_due(Clock::now());
        print_summary();
        return true;
    }

    // Safe to call after run() returned; the copy keeps the counters stable
    // while the caller inspects them.
    RelayStats stats() const {
        RelayStats copy;
        copy.viewer_to_host = viewer_to_host_;
        copy.host_to_viewer = host_to_viewer_;
        return copy;
    }

private:
    void close() {
        for (const Client& client : clients_) {
            if (client.upstream != INVALID_SOCKET) {
                closesocket(client.upstream);
            }
        }
        clients_.clear();
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (timer_period_) {
            timeEndPeriod(1);
            timer_period_ = false;
        }
        if (wsa_started_) {
            WSACleanup();
            wsa_started_ = false;
        }
    }

    bool is_impaired(double elapsed) const {
        return has_impairment() && elapsed >= options_.degrade_at &&
               (options_.recover_at == 0.0 || elapsed < options_.recover_at);
    }

    bool has_impairment() const {
        return options_.delay_ms > 0.0 || options_.jitter_ms > 0.0 ||
               options_.loss_percent > 0.0 || options_.rate_kbps > 0;
    }

    void update_phase(double elapsed) {
        const bool impaired = is_impaired(elapsed);
        if (impaired == was_impaired_) {
            return;
        }
        was_impaired_ = impaired;
        std::printf("netem: t=%.1f s link %s\n", elapsed, impaired ? "degraded" : "clean");
        std::fflush(stdout);
    }

    void receive_viewer() {
        for (;;) {
            sockaddr_in from{};
            int from_length = sizeof(from);
            const int received =
                recvfrom(listener_, reinterpret_cast<char*>(buffer_.data()),
                         static_cast<int>(buffer_.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_length);
            if (received == SOCKET_ERROR) {
                return;
            }
            Client* client = find_client(from);
            if (!client) {
                continue;
            }
            queue_or_send(Direction::ViewerToHost, client->upstream, forward_address_,
                          buffer_.data(), static_cast<size_t>(received));
        }
    }

    void receive_host(Client& client) {
        for (;;) {
            const int received = recv(client.upstream, reinterpret_cast<char*>(buffer_.data()),
                                      static_cast<int>(buffer_.size()), 0);
            if (received == SOCKET_ERROR) {
                return;
            }
            queue_or_send(Direction::HostToViewer, listener_, client.address, buffer_.data(),
                          static_cast<size_t>(received));
        }
    }

    Client* find_client(const sockaddr_in& from) {
        for (Client& client : clients_) {
            if (same_endpoint(client.address, from)) {
                return &client;
            }
        }
        if (clients_.size() >= kMaxClients) {
            return nullptr;
        }
        Client client;
        client.address = from;
        client.upstream = open_socket();
        if (client.upstream == INVALID_SOCKET) {
            std::fprintf(stderr, "netem: cannot create an upstream socket: %d\n",
                         WSAGetLastError());
            return nullptr;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        if (bind(client.upstream, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            std::fprintf(stderr, "netem: cannot bind an upstream socket: %d\n",
                         WSAGetLastError());
            closesocket(client.upstream);
            return nullptr;
        }
        if (clients_.empty() && options_.start_on_first_packet) {
            start_ = Clock::now();
            was_impaired_ = is_impaired(0.0);
        }
        std::printf("netem: viewer %s\n", endpoint_text(from).c_str());
        std::fflush(stdout);
        clients_.push_back(client);
        return &clients_.back();
    }

    struct Shaper {
        Clock::time_point next_free{};
        bool started = false;
    };

    void queue_or_send(Direction direction, SOCKET socket_handle, const sockaddr_in& destination,
                       const uint8_t* data, size_t size) {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        DirectionStats& stats =
            direction == Direction::ViewerToHost ? viewer_to_host_ : host_to_viewer_;
        ++stats.received;
        stats.bytes += size;

        if (!is_impaired(elapsed)) {
            send_packet(socket_handle, destination, data, size, stats);
            return;
        }
        if (options_.loss_percent > 0.0 && loss_(random_)) {
            ++stats.loss_drops;
            return;
        }

        Shaper& shaper = direction == Direction::ViewerToHost ? viewer_shaper_ : host_shaper_;
        auto transmit_at = now;
        if (options_.rate_kbps > 0) {
            const double bytes_per_ms = static_cast<double>(options_.rate_kbps) * 1000.0 / 8.0 / 1000.0;
            if (!shaper.started || shaper.next_free < now) {
                shaper.next_free = now;
                shaper.started = true;
            }
            transmit_at = shaper.next_free;
            const auto finish =
                transmit_at + std::chrono::duration_cast<Clock::duration>(
                                  Milliseconds(static_cast<double>(size) / bytes_per_ms));
            if (std::chrono::duration<double, std::milli>(finish - now).count() > options_.queue_ms) {
                ++stats.queue_drops;
                return;
            }
            shaper.next_free = finish;
        }

        double wait_ms = options_.delay_ms;
        if (options_.jitter_ms > 0.0) {
            std::uniform_real_distribution<double> jitter(0.0, options_.jitter_ms);
            wait_ms += jitter(random_);
        }
        const Clock::time_point due =
            transmit_at +
            std::chrono::duration_cast<Clock::duration>(Milliseconds(wait_ms));
        const double queue_ms = std::chrono::duration<double, std::milli>(due - now).count();
        stats.max_queue_ms = (std::max)(stats.max_queue_ms, queue_ms);

        Pending pending;
        pending.socket = socket_handle;
        pending.destination = destination;
        pending.direction = direction;
        pending.data.assign(data, data + size);
        // multimap: two packets can be due at the same steady-clock tick.
        queue_.emplace(due, std::move(pending));
    }

    void send_due(Clock::time_point now) {
        while (!queue_.empty() && queue_.begin()->first <= now) {
            auto packet = queue_.begin();
            Pending pending = std::move(packet->second);
            queue_.erase(packet);
            DirectionStats& stats = pending.direction == Direction::ViewerToHost
                                        ? viewer_to_host_
                                        : host_to_viewer_;
            send_packet(pending.socket, pending.destination, pending.data.data(),
                        pending.data.size(), stats);
        }
    }

    void send_packet(SOCKET socket_handle, const sockaddr_in& destination, const uint8_t* data,
                     size_t size, DirectionStats& stats) {
        const int sent =
            sendto(socket_handle, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        if (sent == SOCKET_ERROR) {
            ++stats.send_errors;
            return;
        }
        ++stats.forwarded;
    }

    void print_stats(double elapsed, Clock::time_point now) {
        const double backlog_ms =
            queue_.empty()
                ? 0.0
                : (std::max)(0.0, std::chrono::duration<double, std::milli>(
                                      queue_.begin()->first - now)
                                      .count());
        std::printf(
            "netem: t=%.1f s %s v2h rx=%llu fwd=%llu loss=%llu queue=%llu | h2v rx=%llu "
            "fwd=%llu loss=%llu queue=%llu | pending=%zu backlog=%.0f ms\n",
            elapsed, is_impaired(elapsed) ? "degraded" : "clean",
            static_cast<unsigned long long>(viewer_to_host_.received),
            static_cast<unsigned long long>(viewer_to_host_.forwarded),
            static_cast<unsigned long long>(viewer_to_host_.loss_drops),
            static_cast<unsigned long long>(viewer_to_host_.queue_drops),
            static_cast<unsigned long long>(host_to_viewer_.received),
            static_cast<unsigned long long>(host_to_viewer_.forwarded),
            static_cast<unsigned long long>(host_to_viewer_.loss_drops),
            static_cast<unsigned long long>(host_to_viewer_.queue_drops), queue_.size(),
            backlog_ms);
        std::fflush(stdout);
    }

    void print_summary() {
        const auto line = [](const char* label, const DirectionStats& stats) {
            std::printf(
                "netem: %s received=%llu forwarded=%llu loss-dropped=%llu queue-dropped=%llu "
                "send-errors=%llu bytes=%llu max-queue=%.0f ms\n",
                label, static_cast<unsigned long long>(stats.received),
                static_cast<unsigned long long>(stats.forwarded),
                static_cast<unsigned long long>(stats.loss_drops),
                static_cast<unsigned long long>(stats.queue_drops),
                static_cast<unsigned long long>(stats.send_errors),
                static_cast<unsigned long long>(stats.bytes), stats.max_queue_ms);
        };
        line("viewer->host", viewer_to_host_);
        line("host->viewer", host_to_viewer_);
        std::fflush(stdout);
    }

    Options options_;
    SOCKET listener_ = INVALID_SOCKET;
    sockaddr_in forward_address_{};
    std::vector<Client> clients_;
    std::array<uint8_t, kMaxDatagram> buffer_{};
    std::multimap<Clock::time_point, Pending> queue_;
    DirectionStats viewer_to_host_;
    DirectionStats host_to_viewer_;
    Shaper viewer_shaper_;
    Shaper host_shaper_;
    Clock::time_point start_{};
    std::mt19937 random_;
    std::bernoulli_distribution loss_;
    bool was_impaired_ = false;
    bool wsa_started_ = false;
    bool timer_period_ = false;
    std::atomic<bool> stop_{false};
};

}  // namespace netem
}  // namespace chast_tools
