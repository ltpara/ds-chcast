// Userspace weak-link relay for the QUIC harness.
//
//   chast-netem --listen-port 46000 --forward-port 46001 [options]
//
// The relay itself lives in netem_relay.h so chast-quic's automated
// weak-network check runs the same code in-process. This file is the
// command-line wrapper: option parsing, the console handler and the run loop.
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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

#include "netem_relay.h"

namespace {

using Options = chast_tools::netem::Options;

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
    chast_tools::netem::Relay relay(options);
    g_stop_flag = &relay.stop_flag();
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::string error;
    const bool started = relay.start(&error);
    const bool ok = started && relay.run();
    if (!started) {
        std::fprintf(stderr, "chast-netem: %s\n", error.c_str());
    }

    g_stop_flag = nullptr;
    SetConsoleCtrlHandler(console_handler, FALSE);
    return ok ? 0 : 1;
}
