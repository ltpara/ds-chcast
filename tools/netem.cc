// Userspace weak-link relay for the QUIC harness.
//
//   chast-netem --listen-port 46000 --forward-port 46001 [options]
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
// Windows-only (Winsock), IPv4-only, one relay per session.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
#include <cstring>
#include <exception>
#include <map>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

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
};

void usage() {
    std::fprintf(stderr,
                 "usage: chast-netem --listen-port P --forward-port P [options]\n"
                 "  --bind ADDR           address the viewer connects to (default 127.0.0.1)\n"
                 "  --forward-host ADDR   host address to relay to (default 127.0.0.1)\n"
                 "  --delay-ms N          one-way delay while degraded (default 0)\n"
                 "  --jitter-ms N         extra uniform 0..N ms while degraded (default 0)\n"
                 "  --loss-percent X      per-packet loss while degraded (default 0)\n"
                 "  --rate-kbps N         per-direction bottleneck, 0 = none (default 0)\n"
                 "  --queue-ms N          max serialization backlog before tail drop (default 500)\n"
                 "  --degrade-at S        start impairment this many seconds in (default 0)\n"
                 "  --recover-at S        stop impairment this many seconds in (default: never)\n"
                 "  --seconds S           run for this long (default 30)\n"
                 "  --seed N              impairment RNG seed (default 1)\n"
                 "  --quiet               no periodic counters\n");
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        try {
            if (arg == "--bind" && has_value) {
                options->bind_address = argv[++i];
            } else if (arg == "--forward-host" && has_value) {
                options->forward_host = argv[++i];
            } else if (arg == "--listen-port" && has_value) {
                options->listen_port = static_cast<uint16_t>(std::stoul(argv[++i]));
            } else if (arg == "--forward-port" && has_value) {
                options->forward_port = static_cast<uint16_t>(std::stoul(argv[++i]));
            } else if (arg == "--delay-ms" && has_value) {
                options->delay_ms = std::stod(argv[++i]);
            } else if (arg == "--jitter-ms" && has_value) {
                options->jitter_ms = std::stod(argv[++i]);
            } else if (arg == "--loss-percent" && has_value) {
                options->loss_percent = std::stod(argv[++i]);
            } else if (arg == "--rate-kbps" && has_value) {
                options->rate_kbps = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--queue-ms" && has_value) {
                options->queue_ms = std::stod(argv[++i]);
            } else if (arg == "--degrade-at" && has_value) {
                options->degrade_at = std::stod(argv[++i]);
            } else if (arg == "--recover-at" && has_value) {
                options->recover_at = std::stod(argv[++i]);
            } else if (arg == "--seconds" && has_value) {
                options->seconds = std::stod(argv[++i]);
            } else if (arg == "--seed" && has_value) {
                options->seed = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--quiet") {
                options->quiet = true;
            } else if (arg == "--help" || arg == "-h") {
                usage();
                return false;
            } else {
                std::fprintf(stderr, "chast-netem: unknown or incomplete option %s\n", arg.c_str());
                usage();
                return false;
            }
        } catch (const std::exception&) {
            std::fprintf(stderr, "chast-netem: %s wants a number\n", arg.c_str());
            return false;
        }
    }
    if (options->listen_port == 0 || options->forward_port == 0) {
        std::fprintf(stderr, "chast-netem: --listen-port and --forward-port are required\n");
        return false;
    }
    if (options->delay_ms < 0.0 || options->jitter_ms < 0.0 || options->delay_ms > 60000.0 ||
        options->jitter_ms > 60000.0) {
        std::fprintf(stderr, "chast-netem: delay and jitter must be 0..60000 ms\n");
        return false;
    }
    if (options->loss_percent < 0.0 || options->loss_percent > 100.0) {
        std::fprintf(stderr, "chast-netem: --loss-percent must be 0..100\n");
        return false;
    }
    if (options->queue_ms < 1.0 || options->queue_ms > 60000.0) {
        std::fprintf(stderr, "chast-netem: --queue-ms must be 1..60000\n");
        return false;
    }
    if (options->degrade_at < 0.0 || options->recover_at < 0.0) {
        std::fprintf(stderr, "chast-netem: phase times cannot be negative\n");
        return false;
    }
    if (options->recover_at != 0.0 && options->recover_at <= options->degrade_at) {
        std::fprintf(stderr, "chast-netem: --recover-at must be after --degrade-at\n");
        return false;
    }
    if (options->seconds <= 0.0) {
        std::fprintf(stderr, "chast-netem: --seconds must be positive\n");
        return false;
    }
    if (options->recover_at > options->seconds) {
        std::fprintf(stderr, "chast-netem: --recover-at is past the end of the run\n");
        return false;
    }
    return true;
}

std::string endpoint_text(const sockaddr_in& address) {
    char host[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    return std::string(host) + ":" + std::to_string(ntohs(address.sin_port));
}

bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

SOCKET open_socket() {
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

bool bind_socket(SOCKET socket_handle, const std::string& address, uint16_t port) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
        return false;
    }
    return bind(socket_handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
}

struct DirectionStats {
    uint64_t received = 0;
    uint64_t forwarded = 0;
    uint64_t loss_drops = 0;
    uint64_t queue_drops = 0;
    uint64_t send_errors = 0;
    uint64_t bytes = 0;
    double max_queue_ms = 0.0;
};

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
        for (const Client& client : clients_) {
            if (client.upstream != INVALID_SOCKET) {
                closesocket(client.upstream);
            }
        }
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
        }
    }

    bool start(std::string* error) {
        if (inet_pton(AF_INET, options_.forward_host.c_str(), &forward_address_.sin_addr) != 1) {
            *error = "invalid --forward-host " + options_.forward_host;
            return false;
        }
        forward_address_.sin_family = AF_INET;
        forward_address_.sin_port = htons(options_.forward_port);

        listener_ = open_socket();
        if (listener_ == INVALID_SOCKET) {
            *error = "cannot create the listening socket: " + std::to_string(WSAGetLastError());
            return false;
        }
        if (!bind_socket(listener_, options_.bind_address, options_.listen_port)) {
            *error = "cannot bind " + options_.bind_address + ":" +
                     std::to_string(options_.listen_port) + ": " +
                     std::to_string(WSAGetLastError());
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
                const auto left = std::chrono::duration_cast<Milliseconds>(queue_.begin()->first - now);
                wait_ms = std::clamp(static_cast<int>(std::ceil(left.count())), 0, 20);
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

private:
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
        stats.max_queue_ms = std::max(stats.max_queue_ms, queue_ms);

        Pending pending;
        pending.socket = socket_handle;
        pending.destination = destination;
        pending.direction = direction;
        pending.data.assign(data, data + size);
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
                : std::max(0.0, std::chrono::duration<double, std::milli>(
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
    std::map<Clock::time_point, Pending> queue_;
    DirectionStats viewer_to_host_;
    DirectionStats host_to_viewer_;
    Shaper viewer_shaper_;
    Shaper host_shaper_;
    Clock::time_point start_{};
    std::mt19937 random_;
    std::bernoulli_distribution loss_;
    bool was_impaired_ = false;
    std::atomic<bool> stop_{false};
};

std::atomic<bool>* g_stop_flag = nullptr;

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        if (g_stop_flag) {
            g_stop_flag->store(true);
        }
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        return 1;
    }
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::fprintf(stderr, "chast-netem: WSAStartup failed\n");
        return 1;
    }
    timeBeginPeriod(1);
    SetConsoleCtrlHandler(console_handler, TRUE);

    Relay relay(options);
    g_stop_flag = &relay.stop_flag();
    std::string error;
    const bool started = relay.start(&error);
    const bool ok = started && relay.run();
    if (!started) {
        std::fprintf(stderr, "chast-netem: %s\n", error.c_str());
    }
    g_stop_flag = nullptr;
    SetConsoleCtrlHandler(console_handler, FALSE);
    timeEndPeriod(1);
    WSACleanup();
    return ok ? 0 : 1;
}
